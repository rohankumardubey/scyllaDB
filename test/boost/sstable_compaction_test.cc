/*
 * Copyright (C) 2021-present ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <seastar/core/sstring.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/align.hh>
#include <seastar/core/aligned_buffer.hh>
#include <seastar/util/closeable.hh>
#include <seastar/core/coroutine.hh>

#include "sstables/sstables.hh"
#include "sstables/key.hh"
#include "sstables/compress.hh"
#include "compaction/compaction.hh"
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include "schema.hh"
#include "schema_builder.hh"
#include "replica/database.hh"
#include "compaction/leveled_manifest.hh"
#include "sstables/metadata_collector.hh"
#include "sstables/sstable_writer.hh"
#include <memory>
#include "test/boost/sstable_test.hh"
#include <seastar/core/seastar.hh>
#include <seastar/core/do_with.hh>
#include "compaction/compaction_manager.hh"
#include "test/lib/tmpdir.hh"
#include "dht/i_partitioner.hh"
#include "dht/murmur3_partitioner.hh"
#include "range.hh"
#include "partition_slice_builder.hh"
#include "compaction/compaction_strategy_impl.hh"
#include "compaction/date_tiered_compaction_strategy.hh"
#include "compaction/time_window_compaction_strategy.hh"
#include "test/lib/mutation_assertions.hh"
#include "counters.hh"
#include "cell_locking.hh"
#include "test/lib/simple_schema.hh"
#include "memtable-sstable.hh"
#include "test/lib/index_reader_assertions.hh"
#include "test/lib/flat_mutation_reader_assertions.hh"
#include "test/lib/make_random_string.hh"
#include "test/lib/sstable_run_based_compaction_strategy_for_tests.hh"
#include "compatible_ring_position.hh"
#include "mutation_compactor.hh"
#include "service/priority_manager.hh"
#include "db/config.hh"
#include "mutation_writer/partition_based_splitting_writer.hh"
#include "compaction/table_state.hh"

#include <stdio.h>
#include <ftw.h>
#include <unistd.h>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/algorithm/cxx11/all_of.hpp>
#include <boost/algorithm/cxx11/is_sorted.hpp>
#include <boost/icl/interval_map.hpp>
#include "test/lib/test_services.hh"
#include "test/lib/cql_test_env.hh"
#include "test/lib/reader_concurrency_semaphore.hh"
#include "test/lib/sstable_utils.hh"
#include "test/lib/random_utils.hh"

namespace fs = std::filesystem;

using namespace sstables;

static const sstring some_keyspace("ks");
static const sstring some_column_family("cf");

atomic_cell make_atomic_cell(data_type dt, bytes_view value, uint32_t ttl = 0, uint32_t expiration = 0) {
    if (ttl) {
        return atomic_cell::make_live(*dt, 0, value,
            gc_clock::time_point(gc_clock::duration(expiration)), gc_clock::duration(ttl));
    } else {
        return atomic_cell::make_live(*dt, 0, value);
    }
}

////////////////////////////////  Test basic compaction support

// open_sstable() opens the requested sstable for reading only (sstables are
// immutable, so an existing sstable cannot be opened for writing).
// It returns a future because opening requires reading from disk, and
// therefore may block. The future value is a shared sstable - a reference-
// counting pointer to an sstable - allowing for the returned handle to
// be passed around until no longer needed.
static future<sstables::shared_sstable> open_sstable(test_env& env, schema_ptr schema, sstring dir, unsigned long generation) {
    return env.reusable_sst(std::move(schema), dir, generation);
}

// open_sstables() opens several generations of the same sstable, returning,
// after all the tables have been open, their vector.
static future<std::vector<sstables::shared_sstable>> open_sstables(test_env& env, schema_ptr s, sstring dir, std::vector<unsigned long> generations) {
    return do_with(std::vector<sstables::shared_sstable>(),
            [&env, dir = std::move(dir), generations = std::move(generations), s] (auto& ret) mutable {
        return parallel_for_each(generations, [&env, &ret, &dir, s] (unsigned long generation) {
            return open_sstable(env, s, dir, generation).then([&ret] (sstables::shared_sstable sst) {
                ret.push_back(std::move(sst));
            });
        }).then([&ret] {
            return std::move(ret);
        });
    });
}

// mutation_reader for sstable keeping all the required objects alive.
static flat_mutation_reader sstable_reader(shared_sstable sst, schema_ptr s, reader_permit permit) {
    return sst->as_mutation_source().make_reader(s, std::move(permit), query::full_partition_range, s->full_slice());
}

class table_state_for_test : public table_state {
    column_family_for_tests& _t;
    test_env& _env;
    std::vector<sstables::shared_sstable> _compacted_undeleted;
public:
    explicit table_state_for_test(column_family_for_tests& t, test_env& env)
        : _t(t)
        , _env(env)
    {
    }
    const schema_ptr& schema() const noexcept override {
        return _t->schema();
    }
    unsigned min_compaction_threshold() const noexcept override {
        return _t.schema()->min_compaction_threshold();
    }
    bool compaction_enforce_min_threshold() const noexcept override {
        return true;
    }
    const sstables::sstable_set& get_sstable_set() const override {
        return _t->get_sstable_set();
    }
    std::unordered_set<sstables::shared_sstable> fully_expired_sstables(const std::vector<sstables::shared_sstable>& sstables, gc_clock::time_point query_time) const override {
        return sstables::get_fully_expired_sstables(_t->as_table_state(), sstables, query_time);
    }
    const std::vector<sstables::shared_sstable>& compacted_undeleted_sstables() const noexcept override {
        return _compacted_undeleted;
    }
    sstables::compaction_strategy& get_compaction_strategy() const noexcept override {
        return _t->get_compaction_strategy();
    }
    reader_permit make_compaction_reader_permit() const override {
        return _env.make_reader_permit();
    }
    sstables::sstable_writer_config configure_writer(sstring origin) const override {
        return _env.manager().configure_writer(std::move(origin));
    }

    api::timestamp_type min_memtable_timestamp() const override {
        return _t->min_memtable_timestamp();
    }
};

static std::unique_ptr<table_state> make_table_state_for_test(column_family_for_tests& t, test_env& env) {
    return std::make_unique<table_state_for_test>(t, env);
}

class strategy_control_for_test : public strategy_control {
    bool _has_ongoing_compaction;
public:
    explicit strategy_control_for_test(bool has_ongoing_compaction) noexcept : _has_ongoing_compaction(has_ongoing_compaction) {}

    bool has_ongoing_compaction(table_state& table_s) const noexcept override {
        return _has_ongoing_compaction;
    }
};

static std::unique_ptr<strategy_control> make_strategy_control_for_test(bool has_ongoing_compaction) {
    return std::make_unique<strategy_control_for_test>(has_ongoing_compaction);
}

SEASTAR_TEST_CASE(compaction_manager_basic_test) {
  return test_env::do_with_async([] (test_env& env) {
    BOOST_REQUIRE(smp::count == 1);
    auto s = make_shared_schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", int32_type}}, {}, utf8_type);

    auto cm = make_lw_shared<compaction_manager>();
    cm->enable();
    auto stop_cm = defer([&cm] {
        cm->stop().get();
    });

    auto tmp = tmpdir();
    replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
    cfg.datadir = tmp.path().string();
    cfg.enable_commitlog = false;
    cfg.enable_incremental_backups = false;
    auto cl_stats = make_lw_shared<cell_locker_stats>();
    auto tracker = make_lw_shared<cache_tracker>();
    auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), *cm, *cl_stats, *tracker);
    cf->start();
    cf->mark_ready_for_writes();
    cf->set_compaction_strategy(sstables::compaction_strategy_type::size_tiered);

    auto generations = std::vector<unsigned long>({1, 2, 3, 4});
    for (auto generation : generations) {
        // create 4 sstables of similar size to be compacted later on.

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        sstring k = "key" + to_sstring(generation);
        auto key = partition_key::from_exploded(*s, {to_bytes(k)});
        auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

        mutation m(s, key);
        m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type, int32_type->decompose(1)));
        mt->apply(std::move(m));

        auto sst = env.make_sstable(s, tmp.path().string(), column_family_test::calculate_generation_for_new_table(*cf), sstables::get_highest_sstable_version(), big);

        write_memtable_to_sstable_for_test(*mt, sst).get();
        sst->load().get();
        column_family_test(cf).add_sstable(sst);
    }

    BOOST_REQUIRE(cf->sstables_count() == generations.size());
    cf->trigger_compaction();
    BOOST_REQUIRE(cm->get_stats().pending_tasks == 1 || cm->get_stats().active_tasks == 1);

    // wait for submitted job to finish.
    auto end = [cm] { return cm->get_stats().pending_tasks == 0 && cm->get_stats().active_tasks == 0; };
    while (!end()) {
        // sleep until compaction manager selects cf for compaction.
        sleep(std::chrono::milliseconds(100)).get();
    }
    BOOST_REQUIRE(cm->get_stats().completed_tasks == 1);
    BOOST_REQUIRE(cm->get_stats().errors == 0);

    // expect sstables of cf to be compacted.
    BOOST_REQUIRE(cf->sstables_count() == 1);

    cf->stop().get();
  });
}

SEASTAR_TEST_CASE(compact) {
  return sstables::test_env::do_with([] (sstables::test_env& env) {
    BOOST_REQUIRE(smp::count == 1);
    constexpr int generation = 17;
    // The "compaction" sstable was created with the following schema:
    // CREATE TABLE compaction (
    //        name text,
    //        age int,
    //        height int,
    //        PRIMARY KEY (name)
    //);
    auto builder = schema_builder("tests", "compaction")
        .with_column("name", utf8_type, column_kind::partition_key)
        .with_column("age", int32_type)
        .with_column("height", int32_type);
    builder.set_comment("Example table for compaction");
    builder.set_gc_grace_seconds(std::numeric_limits<int32_t>::max());
    auto s = builder.build();
    auto cm = make_lw_shared<compaction_manager>();
    auto cl_stats = make_lw_shared<cell_locker_stats>();
    auto tracker = make_lw_shared<cache_tracker>();
    auto cf = make_lw_shared<replica::column_family>(s, column_family_test_config(env.manager(), env.semaphore()), replica::column_family::no_commitlog(), *cm, *cl_stats, *tracker);
    cf->mark_ready_for_writes();

    return test_setup::do_with_tmp_directory([s, generation, cf, cm] (test_env& env, sstring tmpdir_path) {
        return open_sstables(env, s, "test/resource/sstables/compaction", {1,2,3}).then([&env, tmpdir_path, s, cf, cm, generation] (auto sstables) {
            auto new_sstable = [&env, gen = make_lw_shared<unsigned>(generation), s, tmpdir_path] {
                return env.make_sstable(s, tmpdir_path,
                        (*gen)++, sstables::get_highest_sstable_version(), sstables::sstable::format_types::big);
            };
            return compact_sstables(sstables::compaction_descriptor(std::move(sstables), cf->get_sstable_set(), default_priority_class()), *cf, new_sstable).then([&env, s, generation, cf, cm, tmpdir_path] (auto) {
                // Verify that the compacted sstable has the right content. We expect to see:
                //  name  | age | height
                // -------+-----+--------
                //  jerry |  40 |    170
                //    tom |  20 |    180
                //   john |  20 |   deleted
                //   nadav - deleted partition
                return open_sstable(env, s, tmpdir_path, generation).then([&env, s] (shared_sstable sst) {
                    auto reader = make_lw_shared<flat_mutation_reader>(sstable_reader(sst, s, env.make_reader_permit())); // reader holds sst and s alive.
                    return read_mutation_from_flat_mutation_reader(*reader).then([reader, s] (mutation_opt m) {
                        BOOST_REQUIRE(m);
                        BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, data_value(sstring("jerry")))));
                        BOOST_REQUIRE(!m->partition().partition_tombstone());
                        auto rows = m->partition().clustered_rows();
                        BOOST_REQUIRE(rows.calculate_size() == 1);
                        auto &row = rows.begin()->row();
                        BOOST_REQUIRE(!row.deleted_at());
                        auto &cells = row.cells();
                        auto& cdef1 = *s->get_column_definition("age");
                        auto& cdef2 = *s->get_column_definition("height");
                        BOOST_REQUIRE(cells.cell_at(cdef1.id).as_atomic_cell(cdef1).value() == managed_bytes({0,0,0,40}));
                        BOOST_REQUIRE(cells.cell_at(cdef2.id).as_atomic_cell(cdef2).value() == managed_bytes({0,0,0,(int8_t)170}));
                        return read_mutation_from_flat_mutation_reader(*reader);
                    }).then([reader, s] (mutation_opt m) {
                        BOOST_REQUIRE(m);
                        BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, data_value(sstring("tom")))));
                        BOOST_REQUIRE(!m->partition().partition_tombstone());
                        auto rows = m->partition().clustered_rows();
                        BOOST_REQUIRE(rows.calculate_size() == 1);
                        auto &row = rows.begin()->row();
                        BOOST_REQUIRE(!row.deleted_at());
                        auto &cells = row.cells();
                        auto& cdef1 = *s->get_column_definition("age");
                        auto& cdef2 = *s->get_column_definition("height");
                        BOOST_REQUIRE(cells.cell_at(cdef1.id).as_atomic_cell(cdef1).value() == managed_bytes({0,0,0,20}));
                        BOOST_REQUIRE(cells.cell_at(cdef2.id).as_atomic_cell(cdef2).value() == managed_bytes({0,0,0,(int8_t)180}));
                        return read_mutation_from_flat_mutation_reader(*reader);
                    }).then([reader, s] (mutation_opt m) {
                        BOOST_REQUIRE(m);
                        BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, data_value(sstring("john")))));
                        BOOST_REQUIRE(!m->partition().partition_tombstone());
                        auto rows = m->partition().clustered_rows();
                        BOOST_REQUIRE(rows.calculate_size() == 1);
                        auto &row = rows.begin()->row();
                        BOOST_REQUIRE(!row.deleted_at());
                        auto &cells = row.cells();
                        auto& cdef1 = *s->get_column_definition("age");
                        auto& cdef2 = *s->get_column_definition("height");
                        BOOST_REQUIRE(cells.cell_at(cdef1.id).as_atomic_cell(cdef1).value() == managed_bytes({0,0,0,20}));
                        BOOST_REQUIRE(cells.find_cell(cdef2.id) == nullptr);
                        return read_mutation_from_flat_mutation_reader(*reader);
                    }).then([reader, s] (mutation_opt m) {
                        BOOST_REQUIRE(m);
                        BOOST_REQUIRE(m->key().equal(*s, partition_key::from_singular(*s, data_value(sstring("nadav")))));
                        BOOST_REQUIRE(m->partition().partition_tombstone());
                        auto rows = m->partition().clustered_rows();
                        BOOST_REQUIRE(rows.calculate_size() == 0);
                        return read_mutation_from_flat_mutation_reader(*reader);
                    }).then([reader] (mutation_opt m) {
                        BOOST_REQUIRE(!m);
                    }).finally([reader] {
                        return reader->close();
                    });
                });
            });
        });
    }).finally([cl_stats, tracker] { });
  });

    // verify that the compacted sstable look like
}

static std::vector<sstables::shared_sstable> get_candidates_for_leveled_strategy(replica::column_family& cf) {
    std::vector<sstables::shared_sstable> candidates;
    candidates.reserve(cf.sstables_count());
    for (auto sstables = cf.get_sstables(); auto& entry : *sstables) {
        candidates.push_back(entry);
    }
    return candidates;
}

// Return vector of sstables generated by compaction. Only relevant for leveled one.
static future<std::vector<unsigned long>> compact_sstables(test_env& env, sstring tmpdir_path, std::vector<unsigned long> generations_to_compact,
        unsigned long new_generation, bool create_sstables, uint64_t min_sstable_size, compaction_strategy_type strategy) {
    BOOST_REQUIRE(smp::count == 1);
    schema_builder builder(make_shared_schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", utf8_type}}, {}, utf8_type));
    builder.set_compressor_params(compression_parameters::no_compression());
    builder.set_min_compaction_threshold(4);
    auto s = builder.build(schema_builder::compact_storage::no);

    column_family_for_tests cf(env.manager(), s);

    auto generations = make_lw_shared<std::vector<unsigned long>>(std::move(generations_to_compact));
    auto sstables = make_lw_shared<std::vector<sstables::shared_sstable>>();
    auto created = make_lw_shared<std::vector<unsigned long>>();

    auto f = make_ready_future<>();

    return f.then([&env, generations, sstables, s, create_sstables, min_sstable_size, tmpdir_path] () mutable {
        if (!create_sstables) {
            return open_sstables(env, s, tmpdir_path, *generations).then([sstables] (auto opened_sstables) mutable {
                for (auto& sst : opened_sstables) {
                    sstables->push_back(sst);
                }
                return make_ready_future<>();
            });
        }
        return do_for_each(*generations, [&env, generations, sstables, s, min_sstable_size, tmpdir_path] (unsigned long generation) {
            auto mt = make_lw_shared<memtable>(s);

            const column_definition& r1_col = *s->get_column_definition("r1");

            sstring k = "key" + to_sstring(generation);
            auto key = partition_key::from_exploded(*s, {to_bytes(k)});
            auto c_key = clustering_key::from_exploded(*s, {to_bytes("abc")});

            mutation m(s, key);
            m.set_clustered_cell(c_key, r1_col, make_atomic_cell(utf8_type, bytes(min_sstable_size, 'a')));
            mt->apply(std::move(m));

            auto sst = env.make_sstable(s, tmpdir_path, generation, sstables::get_highest_sstable_version(), big);

            return write_memtable_to_sstable_for_test(*mt, sst).then([mt, sst, s, sstables] {
                return sst->load().then([sst, sstables] {
                    sstables->push_back(sst);
                    return make_ready_future<>();
                });
            });
        });
    }).then([&env, cf, sstables, new_generation, generations, strategy, created, min_sstable_size, s, tmpdir_path] () mutable {
        auto generation = make_lw_shared<unsigned long>(new_generation);
        auto new_sstable = [&env, generation, created, s, tmpdir_path] {
            auto gen = (*generation)++;
            created->push_back(gen);
            return env.make_sstable(s, tmpdir_path,
                gen, sstables::get_highest_sstable_version(), sstables::sstable::format_types::big);
        };
        // We must have opened at least all original candidates.
        BOOST_REQUIRE(generations->size() == sstables->size());

        if (strategy == compaction_strategy_type::size_tiered) {
            // Calling function that will return a list of sstables to compact based on size-tiered strategy.
            int min_threshold = cf->schema()->min_compaction_threshold();
            int max_threshold = cf->schema()->max_compaction_threshold();
            auto sstables_to_compact = sstables::size_tiered_compaction_strategy::most_interesting_bucket(*sstables, min_threshold, max_threshold);
            // We do expect that all candidates were selected for compaction (in this case).
            BOOST_REQUIRE(sstables_to_compact.size() == sstables->size());
            return compact_sstables(sstables::compaction_descriptor(std::move(sstables_to_compact), cf->get_sstable_set(),
                default_priority_class()), *cf, new_sstable).then([generation] (auto) {});
        } else if (strategy == compaction_strategy_type::leveled) {
            for (auto& sst : *sstables) {
                BOOST_REQUIRE(sst->get_sstable_level() == 0);
                BOOST_REQUIRE(sst->data_size() >= min_sstable_size);
                column_family_test(cf).add_sstable(sst);
            }
            auto candidates = get_candidates_for_leveled_strategy(*cf);
            sstables::size_tiered_compaction_strategy_options stcs_options;
            auto table_s = make_table_state_for_test(cf, env);
            leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, 1, stcs_options);
            std::vector<std::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
            std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
            auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
            BOOST_REQUIRE(candidate.sstables.size() == sstables->size());
            BOOST_REQUIRE(candidate.level == 1);
            BOOST_REQUIRE(candidate.max_sstable_bytes == 1024*1024);

            return compact_sstables(sstables::compaction_descriptor(std::move(candidate.sstables), cf->get_sstable_set(),
                default_priority_class(), candidate.level, 1024*1024), *cf, new_sstable).then([generation] (auto) {});
        } else {
            throw std::runtime_error("unexpected strategy");
        }
        return make_ready_future<>();
    }).then([cf, created] {
        return std::move(*created);
    }).finally([cf] () mutable {
        return cf.stop_and_keep_alive();
    });
}

static future<> compact_sstables(test_env& env, sstring tmpdir_path, std::vector<unsigned long> generations_to_compact, unsigned long new_generation, bool create_sstables = true) {
    uint64_t min_sstable_size = 50;
    return compact_sstables(env, tmpdir_path, std::move(generations_to_compact), new_generation, create_sstables, min_sstable_size,
                            compaction_strategy_type::size_tiered).then([new_generation] (auto ret) {
        // size tiered compaction will output at most one sstable, let's assert that.
        BOOST_REQUIRE(ret.size() == 1);
        BOOST_REQUIRE(ret[0] == new_generation);
        return make_ready_future<>();
    });
}

static future<> check_compacted_sstables(test_env& env, sstring tmpdir_path, unsigned long generation, std::vector<unsigned long> compacted_generations) {
    auto s = make_shared_schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", utf8_type}}, {}, utf8_type);

    auto generations = make_lw_shared<std::vector<unsigned long>>(std::move(compacted_generations));

    return open_sstable(env, s, tmpdir_path, generation).then([&env, s, generations] (shared_sstable sst) {
        auto reader = sstable_reader(sst, s, env.make_reader_permit()); // reader holds sst and s alive.
        auto keys = make_lw_shared<std::vector<partition_key>>();

        return with_closeable(std::move(reader), [generations, s, keys] (flat_mutation_reader& reader) {
            return do_for_each(*generations, [&reader, keys] (unsigned long generation) mutable {
                return read_mutation_from_flat_mutation_reader(reader).then([generation, keys] (mutation_opt m) {
                    BOOST_REQUIRE(m);
                    keys->push_back(m->key());
                });
            }).then([s, keys, generations] {
                // keys from compacted sstable aren't ordered lexographically,
                // thus we must read all keys into a vector, sort the vector
                // lexographically, then proceed with the comparison.
                std::sort(keys->begin(), keys->end(), partition_key::less_compare(*s));
                BOOST_REQUIRE(keys->size() == generations->size());
                auto i = 0;
                for (auto& k : *keys) {
                    sstring original_k = "key" + to_sstring((*generations)[i++]);
                    BOOST_REQUIRE(k.equal(*s, partition_key::from_singular(*s, data_value(original_k))));
                }
                return make_ready_future<>();
            });
        });
    });
}

SEASTAR_TEST_CASE(compact_02) {
    // NOTE: generations 18 to 38 are used here.

    // This tests size-tiered compaction strategy by creating 4 sstables of
    // similar size and compacting them to create a new tier.
    // The process above is repeated 4 times until you have 4 compacted
    // sstables of similar size. Then you compact these 4 compacted sstables,
    // and make sure that you have all partition keys.
    // By the way, automatic compaction isn't tested here, instead the
    // strategy algorithm that selects candidates for compaction.

    return test_setup::do_with_tmp_directory([] (test_env& env, sstring tmpdir_path) {
        // Compact 4 sstables into 1 using size-tiered strategy to select sstables.
        // E.g.: generations 18, 19, 20 and 21 will be compacted into generation 22.
        return compact_sstables(env, tmpdir_path, { 18, 19, 20, 21 }, 22).then([&env, tmpdir_path] {
            // Check that generation 22 contains all keys of generations 18, 19, 20 and 21.
            return check_compacted_sstables(env, tmpdir_path, 22, { 18, 19, 20, 21 });
        }).then([&env, tmpdir_path] {
            return compact_sstables(env, tmpdir_path, { 23, 24, 25, 26 }, 27).then([&env, tmpdir_path] {
                return check_compacted_sstables(env, tmpdir_path, 27, { 23, 24, 25, 26 });
            });
        }).then([&env, tmpdir_path] {
            return compact_sstables(env, tmpdir_path, { 28, 29, 30, 31 }, 32).then([&env, tmpdir_path] {
                return check_compacted_sstables(env, tmpdir_path, 32, { 28, 29, 30, 31 });
            });
        }).then([&env, tmpdir_path] {
            return compact_sstables(env, tmpdir_path, { 33, 34, 35, 36 }, 37).then([&env, tmpdir_path] {
                return check_compacted_sstables(env, tmpdir_path, 37, { 33, 34, 35, 36 });
            });
        }).then([&env, tmpdir_path] {
            // In this step, we compact 4 compacted sstables.
            return compact_sstables(env, tmpdir_path, { 22, 27, 32, 37 }, 38, false).then([&env, tmpdir_path] {
                // Check that the compacted sstable contains all keys.
                return check_compacted_sstables(env, tmpdir_path, 38,
                    { 18, 19, 20, 21, 23, 24, 25, 26, 28, 29, 30, 31, 33, 34, 35, 36 });
            });
        });
    });
}

// Leveled compaction strategy tests

static void add_sstable_for_leveled_test(test_env& env, lw_shared_ptr<replica::column_family> cf, int64_t gen, uint64_t fake_data_size,
                                         uint32_t sstable_level, sstring first_key, sstring last_key, int64_t max_timestamp = 0) {
    auto sst = env.make_sstable(cf->schema(), "", gen, la, big);
    sstables::test(sst).set_values_for_leveled_strategy(fake_data_size, sstable_level, max_timestamp, std::move(first_key), std::move(last_key));
    assert(sst->data_size() == fake_data_size);
    assert(sst->get_sstable_level() == sstable_level);
    assert(sst->get_stats_metadata().max_timestamp == max_timestamp);
    assert(sst->generation() == gen);
    column_family_test(cf).add_sstable(sst);
}

static shared_sstable add_sstable_for_overlapping_test(test_env& env, lw_shared_ptr<replica::column_family> cf, int64_t gen, sstring first_key, sstring last_key, stats_metadata stats = {}) {
    auto sst = env.make_sstable(cf->schema(), "", gen, la, big);
    sstables::test(sst).set_values(std::move(first_key), std::move(last_key), std::move(stats));
    column_family_test(cf).add_sstable(sst);
    return sst;
}
static shared_sstable sstable_for_overlapping_test(test_env& env, const schema_ptr& schema, int64_t gen, sstring first_key, sstring last_key, uint32_t level = 0) {
    auto sst = env.make_sstable(schema, "", gen, la, big);
    sstables::test(sst).set_values_for_leveled_strategy(0, level, 0, std::move(first_key), std::move(last_key));
    return sst;
}

// ranges: [a,b] and [c,d]
// returns true if token ranges overlap.
static bool key_range_overlaps(column_family_for_tests& cf, sstring a, sstring b, sstring c, sstring d) {
    const dht::i_partitioner& p = cf->schema()->get_partitioner();
    const dht::sharder& sharder = cf->schema()->get_sharder();
    auto range1 = create_token_range_from_keys(sharder, p, a, b);
    auto range2 = create_token_range_from_keys(sharder, p, c, d);
    return range1.overlaps(range2, dht::token_comparator());
}

static shared_sstable get_sstable(const lw_shared_ptr<replica::column_family>& cf, int64_t generation) {
    auto sstables = cf->get_sstables();
    auto entry = boost::range::find_if(*sstables, [generation] (shared_sstable sst) { return generation == sst->generation(); });
    assert(entry != sstables->end());
    assert((*entry)->generation() == generation);
    return *entry;
}

static bool sstable_overlaps(const lw_shared_ptr<replica::column_family>& cf, int64_t gen1, int64_t gen2) {
    auto candidate1 = get_sstable(cf, gen1);
    auto range1 = range<dht::token>::make(candidate1->get_first_decorated_key()._token, candidate1->get_last_decorated_key()._token);
    auto candidate2 = get_sstable(cf, gen2);
    auto range2 = range<dht::token>::make(candidate2->get_first_decorated_key()._token, candidate2->get_last_decorated_key()._token);
    return range1.overlaps(range2, dht::token_comparator());
}

SEASTAR_TEST_CASE(leveled_01) {
  BOOST_REQUIRE_EQUAL(smp::count, 1);
  return test_env::do_with([] (test_env& env) {
    column_family_for_tests cf(env.manager());

    auto key_and_token_pair = token_generation_for_current_shard(50);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;
    auto max_sstable_size_in_mb = 1;
    auto max_sstable_size = max_sstable_size_in_mb*1024*1024;

    // Creating two sstables which key range overlap.
    add_sstable_for_leveled_test(env, cf, /*gen*/1, max_sstable_size, /*level*/0, min_key, max_key);
    BOOST_REQUIRE(cf->get_sstables()->size() == 1);

    add_sstable_for_leveled_test(env, cf, /*gen*/2, max_sstable_size, /*level*/0, key_and_token_pair[1].first, max_key);
    BOOST_REQUIRE(cf->get_sstables()->size() == 2);

    BOOST_REQUIRE(key_range_overlaps(cf, min_key, max_key, key_and_token_pair[1].first, max_key) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 2) == true);

    auto candidates = get_candidates_for_leveled_strategy(*cf);
    sstables::size_tiered_compaction_strategy_options stcs_options;
    auto table_s = make_table_state_for_test(cf, env);
    leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, max_sstable_size_in_mb, stcs_options);
    BOOST_REQUIRE(manifest.get_level_size(0) == 2);
    std::vector<std::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.sstables.size() == 2);
    BOOST_REQUIRE(candidate.level == 1);

    std::set<unsigned long> gens = { 1, 2 };
    for (auto& sst : candidate.sstables) {
        BOOST_REQUIRE(gens.contains(sst->generation()));
        gens.erase(sst->generation());
        BOOST_REQUIRE(sst->get_sstable_level() == 0);
    }
    BOOST_REQUIRE(gens.empty());

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(leveled_02) {
  BOOST_REQUIRE_EQUAL(smp::count, 1);
  return test_env::do_with([] (test_env& env) {
    column_family_for_tests cf(env.manager());

    auto key_and_token_pair = token_generation_for_current_shard(50);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;
    auto max_sstable_size_in_mb = 1;
    auto max_sstable_size = max_sstable_size_in_mb*1024*1024;

    // Generation 1 will overlap only with generation 2.
    // Remember that for level0, leveled strategy prefer choosing older sstables as candidates.

    add_sstable_for_leveled_test(env, cf, /*gen*/1, max_sstable_size, /*level*/0, min_key, key_and_token_pair[10].first);
    BOOST_REQUIRE(cf->get_sstables()->size() == 1);

    add_sstable_for_leveled_test(env, cf, /*gen*/2, max_sstable_size, /*level*/0, min_key, key_and_token_pair[20].first);
    BOOST_REQUIRE(cf->get_sstables()->size() == 2);

    add_sstable_for_leveled_test(env, cf, /*gen*/3, max_sstable_size, /*level*/0, key_and_token_pair[30].first, max_key);
    BOOST_REQUIRE(cf->get_sstables()->size() == 3);

    BOOST_REQUIRE(key_range_overlaps(cf, min_key, key_and_token_pair[10].first, min_key, key_and_token_pair[20].first) == true);
    BOOST_REQUIRE(key_range_overlaps(cf, min_key, key_and_token_pair[20].first, key_and_token_pair[30].first, max_key) == false);
    BOOST_REQUIRE(key_range_overlaps(cf, min_key, key_and_token_pair[10].first, key_and_token_pair[30].first, max_key) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 2) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 1) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 3) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 3) == false);

    auto candidates = get_candidates_for_leveled_strategy(*cf);
    sstables::size_tiered_compaction_strategy_options stcs_options;
    auto table_s = make_table_state_for_test(cf, env);
    leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, max_sstable_size_in_mb, stcs_options);
    BOOST_REQUIRE(manifest.get_level_size(0) == 3);
    std::vector<std::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.sstables.size() == 3);
    BOOST_REQUIRE(candidate.level == 1);

    std::set<unsigned long> gens = { 1, 2, 3 };
    for (auto& sst : candidate.sstables) {
        BOOST_REQUIRE(gens.contains(sst->generation()));
        gens.erase(sst->generation());
        BOOST_REQUIRE(sst->get_sstable_level() == 0);
    }
    BOOST_REQUIRE(gens.empty());

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(leveled_03) {
  BOOST_REQUIRE_EQUAL(smp::count, 1);
  return test_env::do_with([] (test_env& env) {
    column_family_for_tests cf(env.manager());

    auto key_and_token_pair = token_generation_for_current_shard(50);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    // Creating two sstables of level 0 which overlap
    add_sstable_for_leveled_test(env, cf, /*gen*/1, /*data_size*/1024*1024, /*level*/0, min_key, key_and_token_pair[10].first);
    add_sstable_for_leveled_test(env, cf, /*gen*/2, /*data_size*/1024*1024, /*level*/0, min_key, key_and_token_pair[20].first);
    // Creating a sstable of level 1 which overlap with two sstables above.
    add_sstable_for_leveled_test(env, cf, /*gen*/3, /*data_size*/1024*1024, /*level*/1, min_key, key_and_token_pair[30].first);
    // Creating a sstable of level 1 which doesn't overlap with any sstable.
    add_sstable_for_leveled_test(env, cf, /*gen*/4, /*data_size*/1024*1024, /*level*/1, key_and_token_pair[40].first, max_key);

    BOOST_REQUIRE(cf->get_sstables()->size() == 4);

    BOOST_REQUIRE(key_range_overlaps(cf, min_key, key_and_token_pair[10].first, min_key, key_and_token_pair[20].first) == true);
    BOOST_REQUIRE(key_range_overlaps(cf, min_key, key_and_token_pair[10].first, min_key, key_and_token_pair[30].first) == true);
    BOOST_REQUIRE(key_range_overlaps(cf, min_key, key_and_token_pair[20].first, min_key, key_and_token_pair[30].first) == true);
    BOOST_REQUIRE(key_range_overlaps(cf, min_key, key_and_token_pair[10].first, key_and_token_pair[40].first, max_key) == false);
    BOOST_REQUIRE(key_range_overlaps(cf, min_key, key_and_token_pair[30].first, key_and_token_pair[40].first, max_key) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 2) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 3) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 3) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 4) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 4) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 3, 4) == false);

    auto max_sstable_size_in_mb = 1;
    auto candidates = get_candidates_for_leveled_strategy(*cf);
    sstables::size_tiered_compaction_strategy_options stcs_options;
    auto table_s = make_table_state_for_test(cf, env);
    leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, max_sstable_size_in_mb, stcs_options);
    BOOST_REQUIRE(manifest.get_level_size(0) == 2);
    BOOST_REQUIRE(manifest.get_level_size(1) == 2);
    std::vector<std::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.sstables.size() == 3);
    BOOST_REQUIRE(candidate.level == 1);

    std::set<std::pair<unsigned long, uint32_t>> gen_and_level = { {1,0}, {2,0}, {3,1} };
    for (auto& sst : candidate.sstables) {
        std::pair<unsigned long, uint32_t> pair(sst->generation(), sst->get_sstable_level());
        auto it = gen_and_level.find(pair);
        BOOST_REQUIRE(it != gen_and_level.end());
        BOOST_REQUIRE(sst->get_sstable_level() == it->second);
        gen_and_level.erase(pair);
    }
    BOOST_REQUIRE(gen_and_level.empty());

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(leveled_04) {
  BOOST_REQUIRE_EQUAL(smp::count, 1);
  return test_env::do_with([] (test_env& env) {
    column_family_for_tests cf(env.manager());

    auto key_and_token_pair = token_generation_for_current_shard(50);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    auto max_sstable_size_in_mb = 1;
    auto max_sstable_size_in_bytes = max_sstable_size_in_mb*1024*1024;

    // add 1 level-0 sstable to cf.
    add_sstable_for_leveled_test(env, cf, /*gen*/1, /*data_size*/max_sstable_size_in_bytes, /*level*/0, min_key, max_key);

    // create two big sstables in level1 to force leveled compaction on it.
    auto max_bytes_for_l1 = leveled_manifest::max_bytes_for_level(1, max_sstable_size_in_bytes);
    // NOTE: SSTables in level1 cannot overlap.
    add_sstable_for_leveled_test(env, cf, /*gen*/2, /*data_size*/max_bytes_for_l1, /*level*/1, min_key, key_and_token_pair[25].first);
    add_sstable_for_leveled_test(env, cf, /*gen*/3, /*data_size*/max_bytes_for_l1, /*level*/1, key_and_token_pair[26].first, max_key);

    // Create SSTable in level2 that overlaps with the ones in level1,
    // so compaction in level1 will select overlapping sstables in
    // level2.
    add_sstable_for_leveled_test(env, cf, /*gen*/4, /*data_size*/max_sstable_size_in_bytes, /*level*/2, min_key, max_key);

    BOOST_REQUIRE(cf->get_sstables()->size() == 4);

    BOOST_REQUIRE(key_range_overlaps(cf, min_key, max_key, min_key, max_key) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 2) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 1, 3) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 3) == false);
    BOOST_REQUIRE(sstable_overlaps(cf, 3, 4) == true);
    BOOST_REQUIRE(sstable_overlaps(cf, 2, 4) == true);

    auto candidates = get_candidates_for_leveled_strategy(*cf);
    sstables::size_tiered_compaction_strategy_options stcs_options;
    auto table_s = make_table_state_for_test(cf, env);
    leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, max_sstable_size_in_mb, stcs_options);
    BOOST_REQUIRE(manifest.get_level_size(0) == 1);
    BOOST_REQUIRE(manifest.get_level_size(1) == 2);
    BOOST_REQUIRE(manifest.get_level_size(2) == 1);

    // checks scores; used to determine the level of compaction to proceed with.
    auto level1_score = (double) manifest.get_total_bytes(manifest.get_level(1)) / (double) manifest.max_bytes_for_level(1);
    BOOST_REQUIRE(level1_score > 1.001);
    auto level2_score = (double) manifest.get_total_bytes(manifest.get_level(2)) / (double) manifest.max_bytes_for_level(2);
    BOOST_REQUIRE(level2_score < 1.001);

    std::vector<std::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.sstables.size() == 2);
    BOOST_REQUIRE(candidate.level == 2);

    std::set<unsigned long> levels = { 1, 2 };
    for (auto& sst : candidate.sstables) {
        BOOST_REQUIRE(levels.contains(sst->get_sstable_level()));
        levels.erase(sst->get_sstable_level());
    }
    BOOST_REQUIRE(levels.empty());

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(leveled_05) {
    // NOTE: Generations from 48 to 51 are used here.
    return test_setup::do_with_tmp_directory([] (test_env& env, sstring tmpdir_path) {

        // Check compaction code with leveled strategy. In this test, two sstables of level 0 will be created.
        return compact_sstables(env, tmpdir_path, { 48, 49 }, 50, true, 1024*1024, compaction_strategy_type::leveled).then([tmpdir_path] (auto generations) {
            BOOST_REQUIRE(generations.size() == 2);
            BOOST_REQUIRE(generations[0] == 50);
            BOOST_REQUIRE(generations[1] == 51);

            return seastar::async([&, generations = std::move(generations), tmpdir_path] {
                for (auto gen : generations) {
                    auto fname = sstable::filename(tmpdir_path, "ks", "cf", sstables::get_highest_sstable_version(), gen, big, component_type::Data);
                    BOOST_REQUIRE(file_size(fname).get0() >= 1024*1024);
                }
            });
        });
    });
}

SEASTAR_TEST_CASE(leveled_06) {
    // Test that we can compact a single L1 compaction into an empty L2.
  return test_env::do_with([] (test_env& env) {
    column_family_for_tests cf(env.manager());

    auto max_sstable_size_in_mb = 1;
    auto max_sstable_size_in_bytes = max_sstable_size_in_mb*1024*1024;

    auto max_bytes_for_l1 = leveled_manifest::max_bytes_for_level(1, max_sstable_size_in_bytes);
    // Create fake sstable that will be compacted into L2.
    add_sstable_for_leveled_test(env, cf, /*gen*/1, /*data_size*/max_bytes_for_l1*2, /*level*/1, "a", "a");
    BOOST_REQUIRE(cf->get_sstables()->size() == 1);

    auto candidates = get_candidates_for_leveled_strategy(*cf);
    sstables::size_tiered_compaction_strategy_options stcs_options;
    auto table_s = make_table_state_for_test(cf, env);
    leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, max_sstable_size_in_mb, stcs_options);
    BOOST_REQUIRE(manifest.get_level_size(0) == 0);
    BOOST_REQUIRE(manifest.get_level_size(1) == 1);
    BOOST_REQUIRE(manifest.get_level_size(2) == 0);

    std::vector<std::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.level == 2);
    BOOST_REQUIRE(candidate.sstables.size() == 1);
    auto& sst = (candidate.sstables)[0];
    BOOST_REQUIRE(sst->get_sstable_level() == 1);
    BOOST_REQUIRE(sst->generation() == 1);

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(leveled_07) {
  return test_env::do_with([] (test_env& env) {
    column_family_for_tests cf(env.manager());

    for (auto i = 0; i < leveled_manifest::MAX_COMPACTING_L0*2; i++) {
        add_sstable_for_leveled_test(env, cf, i, 1024*1024, /*level*/0, "a", "a", i /* max timestamp */);
    }
    auto candidates = get_candidates_for_leveled_strategy(*cf);
    sstables::size_tiered_compaction_strategy_options stcs_options;
    auto table_s = make_table_state_for_test(cf, env);
    leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, 1, stcs_options);
    std::vector<std::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    auto desc = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(desc.level == 1);
    BOOST_REQUIRE(desc.sstables.size() == leveled_manifest::MAX_COMPACTING_L0);
    // check that strategy returns the oldest sstables
    for (auto& sst : desc.sstables) {
        BOOST_REQUIRE(sst->get_stats_metadata().max_timestamp < leveled_manifest::MAX_COMPACTING_L0);
    }

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(leveled_invariant_fix) {
  return test_env::do_with([] (test_env& env) {
    column_family_for_tests cf(env.manager());

    auto sstables_no = cf.schema()->max_compaction_threshold();
    auto key_and_token_pair = token_generation_for_current_shard(sstables_no);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;
    auto sstable_max_size = 1024*1024;

    // add non overlapping with min token to be discarded by strategy
    add_sstable_for_leveled_test(env, cf, 0, sstable_max_size, /*level*/1, min_key, min_key);

    for (auto i = 1; i < sstables_no-1; i++) {
        add_sstable_for_leveled_test(env, cf, i, sstable_max_size, /*level*/1, key_and_token_pair[i].first, key_and_token_pair[i].first);
    }
    // add large token span sstable into level 1, which overlaps with all sstables added in loop above.
    add_sstable_for_leveled_test(env, cf, sstables_no, sstable_max_size, 1, key_and_token_pair[1].first, max_key);

    auto candidates = get_candidates_for_leveled_strategy(*cf);
    sstables::size_tiered_compaction_strategy_options stcs_options;
    auto table_s = make_table_state_for_test(cf, env);
    leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, 1, stcs_options);
    std::vector<std::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);

    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.level == 1);
    BOOST_REQUIRE(candidate.sstables.size() == size_t(sstables_no-1));
    BOOST_REQUIRE(boost::algorithm::all_of(candidate.sstables, [] (auto& sst) {
        return sst->generation() != 0;
    }));

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(leveled_stcs_on_L0) {
  return test_env::do_with([] (test_env& env) {
    schema_builder builder(make_shared_schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));
    builder.set_min_compaction_threshold(4);
    auto s = builder.build(schema_builder::compact_storage::no);

    column_family_for_tests cf(env.manager(), s);

    auto key_and_token_pair = token_generation_for_current_shard(1);
    auto sstable_max_size_in_mb = 1;
    auto l0_sstables_no = s->min_compaction_threshold();
    // we don't want level 0 to be worth promoting.
    auto l0_sstables_size = (sstable_max_size_in_mb*1024*1024)/(l0_sstables_no+1);

    add_sstable_for_leveled_test(env, cf, 0, sstable_max_size_in_mb*1024*1024, /*level*/1, key_and_token_pair[0].first, key_and_token_pair[0].first);
    for (auto gen = 0; gen < l0_sstables_no; gen++) {
        add_sstable_for_leveled_test(env, cf, gen+1, l0_sstables_size, /*level*/0, key_and_token_pair[0].first, key_and_token_pair[0].first);
    }
    auto candidates = get_candidates_for_leveled_strategy(*cf);
    BOOST_REQUIRE(candidates.size() == size_t(l0_sstables_no+1));
    BOOST_REQUIRE(cf->get_sstables()->size() == size_t(l0_sstables_no+1));

    std::vector<std::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    sstables::size_tiered_compaction_strategy_options stcs_options;

    auto table_s = make_table_state_for_test(cf, env);

    {
        leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, sstable_max_size_in_mb, stcs_options);
        BOOST_REQUIRE(!manifest.worth_promoting_L0_candidates(manifest.get_level(0)));
        auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
        BOOST_REQUIRE(candidate.level == 0);
        BOOST_REQUIRE(candidate.sstables.size() == size_t(l0_sstables_no));
        BOOST_REQUIRE(boost::algorithm::all_of(candidate.sstables, [] (auto& sst) {
            return sst->generation() != 0;
        }));
    }
    {
        candidates.resize(2);
        leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, sstable_max_size_in_mb, stcs_options);
        auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
        BOOST_REQUIRE(candidate.level == 0);
        BOOST_REQUIRE(candidate.sstables.empty());
    }

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(overlapping_starved_sstables_test) {
  return test_env::do_with([] (test_env& env) {
    column_family_for_tests cf(env.manager());

    auto key_and_token_pair = token_generation_for_current_shard(5);
    auto min_key = key_and_token_pair[0].first;
    auto max_sstable_size_in_mb = 1;
    auto max_sstable_size_in_bytes = max_sstable_size_in_mb*1024*1024;

    // we compact 2 sstables: 0->2 in L1 and 0->1 in L2, and rely on strategy
    // to bring a sstable from level 3 that theoretically wasn't compacted
    // for many rounds and won't introduce an overlap.
    auto max_bytes_for_l1 = leveled_manifest::max_bytes_for_level(1, max_sstable_size_in_bytes);
    add_sstable_for_leveled_test(env, cf, /*gen*/1, max_bytes_for_l1*1.1, /*level*/1, min_key, key_and_token_pair[2].first);
    add_sstable_for_leveled_test(env, cf, /*gen*/2, max_sstable_size_in_bytes, /*level*/2, min_key, key_and_token_pair[1].first);
    add_sstable_for_leveled_test(env, cf, /*gen*/3, max_sstable_size_in_bytes, /*level*/3, min_key, key_and_token_pair[1].first);

    std::vector<std::optional<dht::decorated_key>> last_compacted_keys(leveled_manifest::MAX_LEVELS);
    std::vector<int> compaction_counter(leveled_manifest::MAX_LEVELS);
    // make strategy think that level 3 wasn't compacted for many rounds
    compaction_counter[3] = leveled_manifest::NO_COMPACTION_LIMIT+1;

    auto candidates = get_candidates_for_leveled_strategy(*cf);
    sstables::size_tiered_compaction_strategy_options stcs_options;
    auto table_s = make_table_state_for_test(cf, env);
    leveled_manifest manifest = leveled_manifest::create(*table_s, candidates, max_sstable_size_in_mb, stcs_options);
    auto candidate = manifest.get_compaction_candidates(last_compacted_keys, compaction_counter);
    BOOST_REQUIRE(candidate.level == 2);
    BOOST_REQUIRE(candidate.sstables.size() == 3);

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(check_overlapping) {
  return test_env::do_with([] (test_env& env) {
    column_family_for_tests cf(env.manager());

    auto key_and_token_pair = token_generation_for_current_shard(4);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    auto sst1 = add_sstable_for_overlapping_test(env, cf, /*gen*/1, min_key, key_and_token_pair[1].first);
    auto sst2 = add_sstable_for_overlapping_test(env, cf, /*gen*/2, min_key, key_and_token_pair[2].first);
    auto sst3 = add_sstable_for_overlapping_test(env, cf, /*gen*/3, key_and_token_pair[3].first, max_key);
    auto sst4 = add_sstable_for_overlapping_test(env, cf, /*gen*/4, min_key, max_key);
    BOOST_REQUIRE(cf->get_sstables()->size() == 4);

    std::vector<shared_sstable> compacting = { sst1, sst2 };
    std::vector<shared_sstable> uncompacting = { sst3, sst4 };

    auto overlapping_sstables = leveled_manifest::overlapping(*cf.schema(), compacting, uncompacting);
    BOOST_REQUIRE(overlapping_sstables.size() == 1);
    BOOST_REQUIRE(overlapping_sstables.front()->generation() == 4);

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(tombstone_purge_test) {
    BOOST_REQUIRE(smp::count == 1);
    return test_env::do_with_async([] (test_env& env) {
        cell_locker_stats cl_stats;

        // In a column family with gc_grace_seconds set to 0, check that a tombstone
        // is purged after compaction.
        auto builder = schema_builder("tests", "tombstone_purge")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type);
        builder.set_gc_grace_seconds(0);
        auto s = builder.build();

        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
        };

        auto compact = [&, s] (std::vector<shared_sstable> all, std::vector<shared_sstable> to_compact) -> std::vector<shared_sstable> {
            column_family_for_tests cf(env.manager(), s);
            auto stop_cf = deferred_stop(cf);
            for (auto&& sst : all) {
                column_family_test(cf).add_sstable(sst);
            }
            return compact_sstables(sstables::compaction_descriptor(to_compact, cf->get_sstable_set(), default_priority_class()), *cf, sst_gen).get0().new_sstables;
        };

        auto next_timestamp = [] {
            static thread_local api::timestamp_type next = 1;
            return next++;
        };

        auto make_insert = [&] (partition_key key) {
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), next_timestamp());
            return m;
        };

        auto make_expiring = [&] (partition_key key, int ttl) {
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)),
                gc_clock::now().time_since_epoch().count(), gc_clock::duration(ttl));
            return m;
        };

        auto make_delete = [&] (partition_key key) {
            mutation m(s, key);
            tombstone tomb(next_timestamp(), gc_clock::now());
            m.partition().apply(tomb);
            return m;
        };

        auto assert_that_produces_dead_cell = [&] (auto& sst, partition_key& key) {
            auto reader = make_lw_shared<flat_mutation_reader>(sstable_reader(sst, s, env.make_reader_permit()));
            read_mutation_from_flat_mutation_reader(*reader).then([reader, s, &key] (mutation_opt m) {
                BOOST_REQUIRE(m);
                BOOST_REQUIRE(m->key().equal(*s, key));
                auto rows = m->partition().clustered_rows();
                BOOST_REQUIRE_EQUAL(rows.calculate_size(), 1);
                auto& row = rows.begin()->row();
                auto& cells = row.cells();
                BOOST_REQUIRE_EQUAL(cells.size(), 1);
                auto& cdef = *s->get_column_definition("value");
                BOOST_REQUIRE(!cells.cell_at(cdef.id).as_atomic_cell(cdef).is_live());
                return (*reader)();
            }).then([reader, s] (mutation_fragment_opt m) {
                BOOST_REQUIRE(!m);
            }).finally([reader] {
                return reader->close();
            }).get();
        };

        auto alpha = partition_key::from_exploded(*s, {to_bytes("alpha")});
        auto beta = partition_key::from_exploded(*s, {to_bytes("beta")});

        auto ttl = 10;

        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_insert(beta);
            auto mut3 = make_delete(alpha);

            std::vector<shared_sstable> sstables = {
                    make_sstable_containing(sst_gen, {mut1, mut2}),
                    make_sstable_containing(sst_gen, {mut3})
            };

            forward_jump_clocks(std::chrono::seconds(ttl));

            auto result = compact(sstables, sstables);
            BOOST_REQUIRE_EQUAL(1, result.size());

            assert_that(sstable_reader(result[0], s, env.make_reader_permit()))
                    .produces(mut2)
                    .produces_end_of_stream();
        }

        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_insert(alpha);
            auto mut3 = make_delete(alpha);

            auto sst1 = make_sstable_containing(sst_gen, {mut1});
            auto sst2 = make_sstable_containing(sst_gen, {mut2, mut3});

            forward_jump_clocks(std::chrono::seconds(ttl));

            auto result = compact({sst1, sst2}, {sst2});
            BOOST_REQUIRE_EQUAL(1, result.size());

            assert_that(sstable_reader(result[0], s, env.make_reader_permit()))
                    .produces(mut3)
                    .produces_end_of_stream();
        }

        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_delete(alpha);
            auto mut3 = make_insert(beta);
            auto mut4 = make_insert(alpha);

            auto sst1 = make_sstable_containing(sst_gen, {mut1, mut2, mut3});
            auto sst2 = make_sstable_containing(sst_gen, {mut4});

            forward_jump_clocks(std::chrono::seconds(ttl));

            auto result = compact({sst1, sst2}, {sst1});
            BOOST_REQUIRE_EQUAL(1, result.size());

            assert_that(sstable_reader(result[0], s, env.make_reader_permit()))
                    .produces(mut3)
                    .produces_end_of_stream();
        }

        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_delete(alpha);
            auto mut3 = make_insert(beta);
            auto mut4 = make_insert(beta);

            auto sst1 = make_sstable_containing(sst_gen, {mut1, mut2, mut3});
            auto sst2 = make_sstable_containing(sst_gen, {mut4});

            forward_jump_clocks(std::chrono::seconds(ttl));

            auto result = compact({sst1, sst2}, {sst1});
            BOOST_REQUIRE_EQUAL(1, result.size());

            assert_that(sstable_reader(result[0], s, env.make_reader_permit()))
                    .produces(mut3)
                    .produces_end_of_stream();
        }

        {
            // check that expired cell will not be purged if it will ressurect overwritten data.
            auto mut1 = make_insert(alpha);
            auto mut2 = make_expiring(alpha, ttl);

            auto sst1 = make_sstable_containing(sst_gen, {mut1});
            auto sst2 = make_sstable_containing(sst_gen, {mut2});

            forward_jump_clocks(std::chrono::seconds(ttl));

            auto result = compact({sst1, sst2}, {sst2});
            BOOST_REQUIRE_EQUAL(1, result.size());
            assert_that_produces_dead_cell(result[0], alpha);

            result = compact({sst1, sst2}, {sst1, sst2});
            BOOST_REQUIRE_EQUAL(0, result.size());
        }
        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_expiring(beta, ttl);

            auto sst1 = make_sstable_containing(sst_gen, {mut1});
            auto sst2 = make_sstable_containing(sst_gen, {mut2});

            forward_jump_clocks(std::chrono::seconds(ttl));

            auto result = compact({sst1, sst2}, {sst2});
            BOOST_REQUIRE_EQUAL(0, result.size());
        }
        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_expiring(alpha, ttl);
            auto mut3 = make_insert(beta);

            auto sst1 = make_sstable_containing(sst_gen, {mut1});
            auto sst2 = make_sstable_containing(sst_gen, {mut2, mut3});

            forward_jump_clocks(std::chrono::seconds(ttl));

            auto result = compact({sst1, sst2}, {sst1, sst2});
            BOOST_REQUIRE_EQUAL(1, result.size());
            assert_that(sstable_reader(result[0], s, env.make_reader_permit()))
                    .produces(mut3)
                    .produces_end_of_stream();
        }
    });
}

SEASTAR_TEST_CASE(sstable_rewrite) {
    BOOST_REQUIRE(smp::count == 1);
    return test_setup::do_with_tmp_directory([] (test_env& env, sstring tmpdir_path) {
        auto s = make_shared_schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", utf8_type}}, {}, utf8_type);

        auto mt = make_lw_shared<memtable>(s);

        const column_definition& r1_col = *s->get_column_definition("r1");

        auto key_for_this_shard = token_generation_for_current_shard(1);
        auto apply_key = [mt, s, &r1_col] (sstring key_to_write) {
            auto key = partition_key::from_exploded(*s, {to_bytes(key_to_write)});
            auto c_key = clustering_key::from_exploded(*s, {to_bytes("c1")});
            mutation m(s, key);
            m.set_clustered_cell(c_key, r1_col, make_atomic_cell(utf8_type, bytes("a")));
            mt->apply(std::move(m));
        };
        apply_key(key_for_this_shard[0].first);

        auto sst = env.make_sstable(s, tmpdir_path, 51, sstables::get_highest_sstable_version(), big);
        return write_memtable_to_sstable_for_test(*mt, sst).then([&env, s, sst, tmpdir_path] {
            return env.reusable_sst(s, tmpdir_path, 51);
        }).then([&env, s, key = key_for_this_shard[0].first, tmpdir_path] (auto sstp) mutable {
            auto new_tables = make_lw_shared<std::vector<sstables::shared_sstable>>();
            auto creator = [&env, new_tables, s, tmpdir_path] {
                auto sst = env.make_sstable(s, tmpdir_path, 52, sstables::get_highest_sstable_version(), big);
                new_tables->emplace_back(sst);
                return sst;
            };
            column_family_for_tests cf(env.manager(), s);
            std::vector<shared_sstable> sstables;
            sstables.push_back(std::move(sstp));

            return compact_sstables(sstables::compaction_descriptor(std::move(sstables), cf->get_sstable_set(), default_priority_class()), *cf, creator).then([&env, s, key, new_tables] (auto) {
                BOOST_REQUIRE(new_tables->size() == 1);
                auto newsst = (*new_tables)[0];
                BOOST_REQUIRE(newsst->generation() == 52);
                auto reader = make_lw_shared<flat_mutation_reader>(sstable_reader(newsst, s, env.make_reader_permit()));
                return (*reader)().then([s, reader, key] (mutation_fragment_opt m) {
                    BOOST_REQUIRE(m);
                    BOOST_REQUIRE(m->is_partition_start());
                    auto pkey = partition_key::from_exploded(*s, {to_bytes(key)});
                    BOOST_REQUIRE(m->as_partition_start().key().key().equal(*s, pkey));
                    return reader->next_partition();
                }).then([reader] {
                    return (*reader)();
                }).then([reader] (mutation_fragment_opt m) {
                    BOOST_REQUIRE(!m);
                }).finally([reader] {
                    return reader->close();
                });
            }).finally([cf] () mutable { return cf.stop_and_keep_alive(); });
        }).then([sst, mt, s] {});
    });
}


SEASTAR_TEST_CASE(test_sstable_max_local_deletion_time_2) {
    // Create sstable A with 5x column with TTL 100 and 1x column with TTL 1000
    // Create sstable B with tombstone for column in sstable A with TTL 1000.
    // Compact them and expect that maximum deletion time is that of column with TTL 100.
    return test_setup::do_with_tmp_directory([] (test_env& env, sstring tmpdir_path) {
        return seastar::async([&env, tmpdir_path] {
            for (auto version : writable_sstable_versions) {
                schema_builder builder(some_keyspace, some_column_family);
                builder.with_column("p1", utf8_type, column_kind::partition_key);
                builder.with_column("c1", utf8_type, column_kind::clustering_key);
                builder.with_column("r1", utf8_type);
                schema_ptr s = builder.build(schema_builder::compact_storage::no);
                column_family_for_tests cf(env.manager(), s);
                auto close_cf = deferred_stop(cf);
                auto mt = make_lw_shared<memtable>(s);
                auto now = gc_clock::now();
                int32_t last_expiry = 0;
                auto add_row = [&now, &mt, &s, &last_expiry](mutation &m, bytes column_name, uint32_t ttl) {
                    auto c_key = clustering_key::from_exploded(*s, {column_name});
                    last_expiry = (now + gc_clock::duration(ttl)).time_since_epoch().count();
                    m.set_clustered_cell(c_key, *s->get_column_definition("r1"),
                                         make_atomic_cell(utf8_type, bytes(""), ttl, last_expiry));
                    mt->apply(std::move(m));
                };
                auto get_usable_sst = [&env, s, tmpdir_path, version](memtable &mt, int64_t gen) -> future<sstable_ptr> {
                    auto sst = env.make_sstable(s, tmpdir_path, gen, version, big);
                    return write_memtable_to_sstable_for_test(mt, sst).then([&env, sst, gen, s, tmpdir_path, version] {
                        return env.reusable_sst(s, tmpdir_path, gen, version);
                    });
                };

                mutation m(s, partition_key::from_exploded(*s, {to_bytes("deletetest")}));
                for (auto i = 0; i < 5; i++) {
                    add_row(m, to_bytes("deletecolumn" + to_sstring(i)), 100);
                }
                add_row(m, to_bytes("todelete"), 1000);
                auto sst1 = get_usable_sst(*mt, 54).get0();
                BOOST_REQUIRE(last_expiry == sst1->get_stats_metadata().max_local_deletion_time);

                mt = make_lw_shared<memtable>(s);
                m = mutation(s, partition_key::from_exploded(*s, {to_bytes("deletetest")}));
                tombstone tomb(api::new_timestamp(), now);
                m.partition().apply_delete(*s, clustering_key::from_exploded(*s, {to_bytes("todelete")}), tomb);
                mt->apply(std::move(m));
                auto sst2 = get_usable_sst(*mt, 55).get0();
                BOOST_REQUIRE(now.time_since_epoch().count() == sst2->get_stats_metadata().max_local_deletion_time);

                auto creator = [&env, s, tmpdir_path, version, gen = make_lw_shared<unsigned>(56)] { return env.make_sstable(s, tmpdir_path, (*gen)++, version, big); };
                auto info = compact_sstables(sstables::compaction_descriptor({sst1, sst2}, cf->get_sstable_set(), default_priority_class()), *cf, creator).get0();
                BOOST_REQUIRE(info.new_sstables.size() == 1);
                BOOST_REQUIRE(((now + gc_clock::duration(100)).time_since_epoch().count()) ==
                              info.new_sstables.front()->get_stats_metadata().max_local_deletion_time);
            }
        });
    });
}

static stats_metadata build_stats(int64_t min_timestamp, int64_t max_timestamp, int32_t max_local_deletion_time) {
    stats_metadata stats = {};
    stats.min_timestamp = min_timestamp;
    stats.max_timestamp = max_timestamp;
    stats.max_local_deletion_time = max_local_deletion_time;
    return stats;
}

SEASTAR_TEST_CASE(get_fully_expired_sstables_test) {
  return test_env::do_with_async([] (test_env& env) {
    auto key_and_token_pair = token_generation_for_current_shard(4);
    auto min_key = key_and_token_pair[0].first;
    auto max_key = key_and_token_pair[key_and_token_pair.size()-1].first;

    auto t0 = gc_clock::from_time_t(1).time_since_epoch().count();
    auto t1 = gc_clock::from_time_t(10).time_since_epoch().count();
    auto t2 = gc_clock::from_time_t(15).time_since_epoch().count();
    auto t3 = gc_clock::from_time_t(20).time_since_epoch().count();
    auto t4 = gc_clock::from_time_t(30).time_since_epoch().count();

    {
        column_family_for_tests cf(env.manager());
        auto close_cf = deferred_stop(cf);

        auto sst1 = add_sstable_for_overlapping_test(env, cf, /*gen*/1, min_key, key_and_token_pair[1].first, build_stats(t0, t1, t1));
        auto sst2 = add_sstable_for_overlapping_test(env, cf, /*gen*/2, min_key, key_and_token_pair[2].first, build_stats(t0, t1, std::numeric_limits<int32_t>::max()));
        auto sst3 = add_sstable_for_overlapping_test(env, cf, /*gen*/3, min_key, max_key, build_stats(t3, t4, std::numeric_limits<int32_t>::max()));
        std::vector<sstables::shared_sstable> compacting = { sst1, sst2 };
        auto expired = get_fully_expired_sstables(cf->as_table_state(), compacting, /*gc before*/gc_clock::from_time_t(15) + cf->schema()->gc_grace_seconds());
        BOOST_REQUIRE(expired.size() == 0);
    }

    {
        column_family_for_tests cf(env.manager());
        auto close_cf = deferred_stop(cf);

        auto sst1 = add_sstable_for_overlapping_test(env, cf, /*gen*/1, min_key, key_and_token_pair[1].first, build_stats(t0, t1, t1));
        auto sst2 = add_sstable_for_overlapping_test(env, cf, /*gen*/2, min_key, key_and_token_pair[2].first, build_stats(t2, t3, std::numeric_limits<int32_t>::max()));
        auto sst3 = add_sstable_for_overlapping_test(env, cf, /*gen*/3, min_key, max_key, build_stats(t3, t4, std::numeric_limits<int32_t>::max()));
        std::vector<sstables::shared_sstable> compacting = { sst1, sst2 };
        auto expired = get_fully_expired_sstables(cf->as_table_state(), compacting, /*gc before*/gc_clock::from_time_t(25) + cf->schema()->gc_grace_seconds());
        BOOST_REQUIRE(expired.size() == 1);
        auto expired_sst = *expired.begin();
        BOOST_REQUIRE(expired_sst->generation() == 1);
    }
  });
}

SEASTAR_TEST_CASE(compaction_with_fully_expired_table) {
    return test_env::do_with_async([] (test_env& env) {
        auto builder = schema_builder("la", "cf")
            .with_column("pk", utf8_type, column_kind::partition_key)
            .with_column("ck1", utf8_type, column_kind::clustering_key)
            .with_column("r1", int32_type);

        builder.set_gc_grace_seconds(0);
        auto s = builder.build();

        auto tmp = tmpdir();
        auto key = partition_key::from_exploded(*s, {to_bytes("key1")});
        auto c_key = clustering_key_prefix::from_exploded(*s, {to_bytes("c1")});
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
        };

        auto mt = make_lw_shared<memtable>(s);
        mutation m(s, key);
        tombstone tomb(api::new_timestamp(), gc_clock::now() - std::chrono::seconds(3600));
        m.partition().apply_delete(*s, c_key, tomb);
        mt->apply(std::move(m));
        auto sst = sst_gen();
        write_memtable_to_sstable_for_test(*mt, sst).get();
        sst = env.reusable_sst(s, tmp.path().string(), 1).get0();

        column_family_for_tests cf(env.manager(), s);
        auto close_cf = deferred_stop(cf);

        auto ssts = std::vector<shared_sstable>{ sst };

        auto expired = get_fully_expired_sstables(cf->as_table_state(), ssts, gc_clock::now());
        BOOST_REQUIRE(expired.size() == 1);
        auto expired_sst = *expired.begin();
        BOOST_REQUIRE(expired_sst->generation() == 1);

        auto ret = compact_sstables(sstables::compaction_descriptor(ssts, cf->get_sstable_set(), default_priority_class()), *cf, sst_gen).get0();
        BOOST_REQUIRE(ret.new_sstables.empty());
        BOOST_REQUIRE(ret.end_size == 0);
    });
}

SEASTAR_TEST_CASE(basic_date_tiered_strategy_test) {
  return test_env::do_with([] (test_env& env) {
    schema_builder builder(make_shared_schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));
    builder.set_min_compaction_threshold(4);
    auto s = builder.build(schema_builder::compact_storage::no);
    column_family_for_tests cf(env.manager(), s);

    std::vector<sstables::shared_sstable> candidates;
    int min_threshold = cf->schema()->min_compaction_threshold();
    auto now = db_clock::now();
    auto past_hour = now - std::chrono::seconds(3600);
    int64_t timestamp_for_now = now.time_since_epoch().count() * 1000;
    int64_t timestamp_for_past_hour = past_hour.time_since_epoch().count() * 1000;

    for (auto i = 1; i <= min_threshold; i++) {
        auto sst = add_sstable_for_overlapping_test(env, cf, /*gen*/i, "a", "a",
            build_stats(timestamp_for_now, timestamp_for_now, std::numeric_limits<int32_t>::max()));
        candidates.push_back(sst);
    }
    // add sstable that belong to a different time tier.
    auto sst = add_sstable_for_overlapping_test(env, cf, /*gen*/min_threshold + 1, "a", "a",
        build_stats(timestamp_for_past_hour, timestamp_for_past_hour, std::numeric_limits<int32_t>::max()));
    candidates.push_back(sst);

    auto gc_before = gc_clock::now() - cf->schema()->gc_grace_seconds();
    std::map<sstring, sstring> options;
    date_tiered_manifest manifest(options);
    auto table_s = make_table_state_for_test(cf, env);
    auto sstables = manifest.get_next_sstables(*table_s, candidates, gc_before);
    BOOST_REQUIRE(sstables.size() == 4);
    for (auto& sst : sstables) {
        BOOST_REQUIRE(sst->generation() != (min_threshold + 1));
    }

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(date_tiered_strategy_test_2) {
  return test_env::do_with([] (test_env& env) {
    schema_builder builder(make_shared_schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type));
    builder.set_min_compaction_threshold(4);
    auto s = builder.build(schema_builder::compact_storage::no);
    column_family_for_tests cf(env.manager(), s);

    // deterministic timestamp for Fri, 01 Jan 2016 00:00:00 GMT.
    auto tp = db_clock::from_time_t(1451606400);
    int64_t timestamp = tp.time_since_epoch().count() * 1000; // in microseconds.

    std::vector<sstables::shared_sstable> candidates;
    int min_threshold = cf->schema()->min_compaction_threshold();

    // add sstables that belong to same time window until min threshold is satisfied.
    for (auto i = 1; i <= min_threshold; i++) {
        auto sst = add_sstable_for_overlapping_test(env, cf, /*gen*/i, "a", "a",
            build_stats(timestamp, timestamp, std::numeric_limits<int32_t>::max()));
        candidates.push_back(sst);
    }
    // belongs to the time window
    auto tp2 = tp + std::chrono::seconds(1800);
    timestamp = tp2.time_since_epoch().count() * 1000;
    auto sst = add_sstable_for_overlapping_test(env, cf, /*gen*/min_threshold + 1, "a", "a",
        build_stats(timestamp, timestamp, std::numeric_limits<int32_t>::max()));
    candidates.push_back(sst);

    // doesn't belong to the time window above
    auto tp3 = tp + std::chrono::seconds(4000);
    timestamp = tp3.time_since_epoch().count() * 1000;
    auto sst2 = add_sstable_for_overlapping_test(env, cf, /*gen*/min_threshold + 2, "a", "a",
        build_stats(timestamp, timestamp, std::numeric_limits<int32_t>::max()));
    candidates.push_back(sst2);

    std::map<sstring, sstring> options;
    // Use a 1-hour time window.
    options.emplace(sstring("base_time_seconds"), sstring("3600"));

    date_tiered_manifest manifest(options);
    auto gc_before = gc_clock::time_point(std::chrono::seconds(0)); // disable gc before.
    auto table_s = make_table_state_for_test(cf, env);
    auto sstables = manifest.get_next_sstables(*table_s, candidates, gc_before);
    std::unordered_set<int64_t> gens;
    for (auto sst : sstables) {
        gens.insert(sst->generation());
    }
    BOOST_REQUIRE(sstables.size() == size_t(min_threshold + 1));
    BOOST_REQUIRE(gens.contains(min_threshold + 1));
    BOOST_REQUIRE(!gens.contains(min_threshold + 2));

    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(time_window_strategy_time_window_tests) {
    using namespace std::chrono;

    api::timestamp_type tstamp1 = duration_cast<microseconds>(milliseconds(1451001601000L)).count(); // 2015-12-25 @ 00:00:01, in milliseconds
    api::timestamp_type tstamp2 = duration_cast<microseconds>(milliseconds(1451088001000L)).count(); // 2015-12-26 @ 00:00:01, in milliseconds
    api::timestamp_type low_hour = duration_cast<microseconds>(milliseconds(1451001600000L)).count(); // 2015-12-25 @ 00:00:00, in milliseconds


    // A 1 hour window should round down to the beginning of the hour
    BOOST_REQUIRE(time_window_compaction_strategy::get_window_lower_bound(duration_cast<seconds>(hours(1)), tstamp1) == low_hour);

    // A 1 minute window should round down to the beginning of the hour
    BOOST_REQUIRE(time_window_compaction_strategy::get_window_lower_bound(duration_cast<seconds>(minutes(1)), tstamp1) == low_hour);

    // A 1 day window should round down to the beginning of the hour
    BOOST_REQUIRE(time_window_compaction_strategy::get_window_lower_bound(duration_cast<seconds>(hours(24)), tstamp1) == low_hour);

    // The 2 day window of 2015-12-25 + 2015-12-26 should round down to the beginning of 2015-12-25
    BOOST_REQUIRE(time_window_compaction_strategy::get_window_lower_bound(duration_cast<seconds>(hours(24*2)), tstamp2) == low_hour);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(time_window_strategy_ts_resolution_check) {
  return test_env::do_with([] (test_env& env) {
    auto ts = 1451001601000L; // 2015-12-25 @ 00:00:01, in milliseconds
    auto ts_in_ms = std::chrono::milliseconds(ts);
    auto ts_in_us = std::chrono::duration_cast<std::chrono::microseconds>(ts_in_ms);

    auto s = schema_builder("tests", "time_window_strategy")
            .with_column("id", utf8_type, column_kind::partition_key)
            .with_column("value", int32_type).build();

    {
        std::map<sstring, sstring> opts = { { time_window_compaction_strategy_options::TIMESTAMP_RESOLUTION_KEY, "MILLISECONDS" }, };
        time_window_compaction_strategy_options options(opts);

        auto sst = env.make_sstable(s, "", 1, la, big);
        sstables::test(sst).set_values("key1", "key1", build_stats(ts_in_ms.count(), ts_in_ms.count(), std::numeric_limits<int32_t>::max()));

        auto ret = time_window_compaction_strategy::get_buckets({ sst }, options);
        auto expected = time_window_compaction_strategy::get_window_lower_bound(options.get_sstable_window_size(), ts_in_us.count());

        BOOST_REQUIRE(ret.second == expected);
    }

    {
        std::map<sstring, sstring> opts = { { time_window_compaction_strategy_options::TIMESTAMP_RESOLUTION_KEY, "MICROSECONDS" }, };
        time_window_compaction_strategy_options options(opts);

        auto sst = env.make_sstable(s, "", 1, la, big);
        sstables::test(sst).set_values("key1", "key1", build_stats(ts_in_us.count(), ts_in_us.count(), std::numeric_limits<int32_t>::max()));

        auto ret = time_window_compaction_strategy::get_buckets({ sst }, options);
        auto expected = time_window_compaction_strategy::get_window_lower_bound(options.get_sstable_window_size(), ts_in_us.count());

        BOOST_REQUIRE(ret.second == expected);
    }
    return make_ready_future<>();
  });
}

SEASTAR_TEST_CASE(time_window_strategy_correctness_test) {
    using namespace std::chrono;

    return test_env::do_with_async([] (test_env& env) {
        auto s = schema_builder("tests", "time_window_strategy")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type).build();

        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
        };

        auto make_insert = [&] (partition_key key, api::timestamp_type t) {
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), t);
            return m;
        };

        api::timestamp_type tstamp = api::timestamp_clock::now().time_since_epoch().count();
        api::timestamp_type tstamp2 = tstamp - duration_cast<microseconds>(seconds(2L * 3600L)).count();

        std::vector<shared_sstable> sstables;

        // create 5 sstables
        for (api::timestamp_type t = 0; t < 3; t++) {
            auto key = partition_key::from_exploded(*s, {to_bytes("key" + to_sstring(t))});
            auto mut = make_insert(std::move(key), t);
            sstables.push_back(make_sstable_containing(sst_gen, {std::move(mut)}));
        }
        // Decrement the timestamp to simulate a timestamp in the past hour
        for (api::timestamp_type t = 3; t < 5; t++) {
            // And add progressively more cells into each sstable
            auto key = partition_key::from_exploded(*s, {to_bytes("key" + to_sstring(t))});
            auto mut = make_insert(std::move(key), t);
            sstables.push_back(make_sstable_containing(sst_gen, {std::move(mut)}));
        }

        std::map<sstring, sstring> options;
        time_window_compaction_strategy twcs(options);
        std::map<api::timestamp_type, std::vector<shared_sstable>> buckets;
        column_family_for_tests cf(env.manager(), s);
        auto close_cf = deferred_stop(cf);
        auto table_s = make_table_state_for_test(cf, env);
        auto control = make_strategy_control_for_test(false);

        // We'll put 3 sstables into the newest bucket
        for (api::timestamp_type i = 0; i < 3; i++) {
            auto bound = time_window_compaction_strategy::get_window_lower_bound(duration_cast<seconds>(hours(1)), tstamp);
            buckets[bound].push_back(sstables[i]);
        }

        auto now = api::timestamp_clock::now().time_since_epoch().count();
        auto new_bucket = twcs.newest_bucket(*table_s, *control, buckets, 4, 32,
            time_window_compaction_strategy::get_window_lower_bound(duration_cast<seconds>(hours(1)), now));
        // incoming bucket should not be accepted when it has below the min threshold SSTables
        BOOST_REQUIRE(new_bucket.empty());

        now = api::timestamp_clock::now().time_since_epoch().count();
        new_bucket = twcs.newest_bucket(*table_s, *control, buckets, 2, 32,
            time_window_compaction_strategy::get_window_lower_bound(duration_cast<seconds>(hours(1)), now));
        // incoming bucket should be accepted when it is larger than the min threshold SSTables
        BOOST_REQUIRE(!new_bucket.empty());

        // And 2 into the second bucket (1 hour back)
        for (api::timestamp_type i = 3; i < 5; i++) {
            auto bound = time_window_compaction_strategy::get_window_lower_bound(duration_cast<seconds>(hours(1)), tstamp2);
            buckets[bound].push_back(sstables[i]);
        }

        // "an sstable with a single value should have equal min/max timestamps"
        for (auto& sst : sstables) {
            BOOST_REQUIRE(sst->get_stats_metadata().min_timestamp == sst->get_stats_metadata().max_timestamp);
        }

        // Test trim
        auto num_sstables = 40;
        for (int r = 5; r < num_sstables; r++) {
            auto key = partition_key::from_exploded(*s, {to_bytes("key" + to_sstring(r))});
            std::vector<mutation> mutations;
            for (int i = 0 ; i < r ; i++) {
                mutations.push_back(make_insert(key, tstamp + r));
            }
            sstables.push_back(make_sstable_containing(sst_gen, std::move(mutations)));
        }

        // Reset the buckets, overfill it now
        for (int i = 0 ; i < 40; i++) {
            auto bound = time_window_compaction_strategy::get_window_lower_bound(duration_cast<seconds>(hours(1)),
                sstables[i]->get_stats_metadata().max_timestamp);
            buckets[bound].push_back(sstables[i]);
        }

        now = api::timestamp_clock::now().time_since_epoch().count();
        new_bucket = twcs.newest_bucket(*table_s, *control, buckets, 4, 32,
            time_window_compaction_strategy::get_window_lower_bound(duration_cast<seconds>(hours(1)), now));
        // new bucket should be trimmed to max threshold of 32
        BOOST_REQUIRE(new_bucket.size() == size_t(32));
    });
}

// Check that TWCS will only perform size-tiered on the current window and also
// the past windows that were already previously compacted into a single SSTable.
SEASTAR_TEST_CASE(time_window_strategy_size_tiered_behavior_correctness) {
    using namespace std::chrono;

    return test_env::do_with_async([] (test_env& env) {
        auto s = schema_builder("tests", "time_window_strategy")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type).build();

        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
        };

        auto make_insert = [&] (partition_key key, api::timestamp_type t) {
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), t);
            return m;
        };

        std::map<sstring, sstring> options;
        time_window_compaction_strategy twcs(options);
        std::map<api::timestamp_type, std::vector<shared_sstable>> buckets; // windows
        int min_threshold = 4;
        int max_threshold = 32;
        auto window_size = duration_cast<seconds>(hours(1));

        auto add_new_sstable_to_bucket = [&] (api::timestamp_type ts, api::timestamp_type window_ts) {
            auto key = partition_key::from_exploded(*s, {to_bytes("key" + to_sstring(ts))});
            auto mut = make_insert(std::move(key), ts);
            auto sst = make_sstable_containing(sst_gen, {std::move(mut)});
            auto bound = time_window_compaction_strategy::get_window_lower_bound(window_size, window_ts);
            buckets[bound].push_back(std::move(sst));
        };

        column_family_for_tests cf(env.manager(), s);
        auto close_cf = deferred_stop(cf);
        auto major_compact_bucket = [&] (api::timestamp_type window_ts) {
            auto bound = time_window_compaction_strategy::get_window_lower_bound(window_size, window_ts);
            auto ret = compact_sstables(sstables::compaction_descriptor(std::move(buckets[bound]), cf->get_sstable_set(), default_priority_class()), *cf, sst_gen).get0();
            BOOST_REQUIRE(ret.new_sstables.size() == 1);
            buckets[bound] = std::move(ret.new_sstables);
        };

        api::timestamp_type current_window_ts = api::timestamp_clock::now().time_since_epoch().count();
        api::timestamp_type past_window_ts = current_window_ts - duration_cast<microseconds>(seconds(2L * 3600L)).count();

        // create 1 sstable into past time window and let the strategy know about it
        add_new_sstable_to_bucket(0, past_window_ts);

        auto now = time_window_compaction_strategy::get_window_lower_bound(window_size, past_window_ts);
        auto table_s = make_table_state_for_test(cf, env);
        auto control = make_strategy_control_for_test(false);

        // past window cannot be compacted because it has a single SSTable
        BOOST_REQUIRE(twcs.newest_bucket(*table_s, *control, buckets, min_threshold, max_threshold, now).size() == 0);

        // create min_threshold-1 sstables into current time window
        for (api::timestamp_type t = 0; t < min_threshold - 1; t++) {
            add_new_sstable_to_bucket(t, current_window_ts);
        }
        // add 1 sstable into past window.
        add_new_sstable_to_bucket(1, past_window_ts);

        now = time_window_compaction_strategy::get_window_lower_bound(window_size, current_window_ts);

        // past window can now be compacted into a single SSTable because it was the previous current (active) window.
        // current window cannot be compacted because it has less than min_threshold SSTables
        BOOST_REQUIRE(twcs.newest_bucket(*table_s, *control, buckets, min_threshold, max_threshold, now).size() == 2);

        major_compact_bucket(past_window_ts);

        // now past window cannot be compacted again, because it was already compacted into a single SSTable, now it switches to STCS mode.
        BOOST_REQUIRE(twcs.newest_bucket(*table_s, *control, buckets, min_threshold, max_threshold, now).size() == 0);

        // make past window contain more than min_threshold similar-sized SSTables, allowing it to be compacted again.
        for (api::timestamp_type t = 1; t < min_threshold; t++) {
            add_new_sstable_to_bucket(t, past_window_ts);
        }

        // now past window can be compacted again because it switched to STCS mode and has more than min_threshold SSTables.
        BOOST_REQUIRE(twcs.newest_bucket(*table_s, *control, buckets, min_threshold, max_threshold, now).size() == size_t(min_threshold));
    });
}

static void check_min_max_column_names(const sstable_ptr& sst, std::vector<bytes> min_components, std::vector<bytes> max_components) {
    const auto& st = sst->get_stats_metadata();
    BOOST_TEST_MESSAGE(fmt::format("min {}/{} max {}/{}", st.min_column_names.elements.size(), min_components.size(), st.max_column_names.elements.size(), max_components.size()));
    BOOST_REQUIRE(st.min_column_names.elements.size() == min_components.size());
    for (auto i = 0U; i < st.min_column_names.elements.size(); i++) {
        BOOST_REQUIRE(min_components[i] == st.min_column_names.elements[i].value);
    }
    BOOST_REQUIRE(st.max_column_names.elements.size() == max_components.size());
    for (auto i = 0U; i < st.max_column_names.elements.size(); i++) {
        BOOST_REQUIRE(max_components[i] == st.max_column_names.elements[i].value);
    }
}

SEASTAR_TEST_CASE(min_max_clustering_key_test_2) {
    return test_env::do_with_async([] (test_env& env) {
        for (const auto version : writable_sstable_versions) {
            auto s = schema_builder("ks", "cf")
                      .with_column("pk", utf8_type, column_kind::partition_key)
                      .with_column("ck1", utf8_type, column_kind::clustering_key)
                      .with_column("r1", int32_type)
                      .build();
            column_family_for_tests cf(env.manager(), s);
            auto close_cf = deferred_stop(cf);
            auto tmp = tmpdir();
            auto mt = make_lw_shared<memtable>(s);
            const column_definition &r1_col = *s->get_column_definition("r1");

            for (auto j = 0; j < 8; j++) {
                auto key = partition_key::from_exploded(*s, {to_bytes("key" + to_sstring(j))});
                mutation m(s, key);
                for (auto i = 100; i < 150; i++) {
                    auto c_key = clustering_key::from_exploded(*s, {to_bytes(to_sstring(j) + "ck" + to_sstring(i))});
                    m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type, int32_type->decompose(1)));
                }
                mt->apply(std::move(m));
            }
            auto sst = env.make_sstable(s, tmp.path().string(), 1, version, big);
            write_memtable_to_sstable_for_test(*mt, sst).get();
            sst = env.reusable_sst(s, tmp.path().string(), 1, version).get0();
            check_min_max_column_names(sst, {"0ck100"}, {"7ck149"});

            mt = make_lw_shared<memtable>(s);
            auto key = partition_key::from_exploded(*s, {to_bytes("key9")});
            mutation m(s, key);
            for (auto i = 101; i < 299; i++) {
                auto c_key = clustering_key::from_exploded(*s, {to_bytes(to_sstring(9) + "ck" + to_sstring(i))});
                m.set_clustered_cell(c_key, r1_col, make_atomic_cell(int32_type, int32_type->decompose(1)));
            }
            mt->apply(std::move(m));
            auto sst2 = env.make_sstable(s, tmp.path().string(), 2, version, big);
            write_memtable_to_sstable_for_test(*mt, sst2).get();
            sst2 = env.reusable_sst(s, tmp.path().string(), 2, version).get0();
            check_min_max_column_names(sst2, {"9ck101"}, {"9ck298"});

            auto creator = [&env, s, &tmp, version] { return env.make_sstable(s, tmp.path().string(), 3, version, big); };
            auto info = compact_sstables(sstables::compaction_descriptor({sst, sst2}, cf->get_sstable_set(), default_priority_class()), *cf, creator).get0();
            BOOST_REQUIRE(info.new_sstables.size() == 1);
            check_min_max_column_names(info.new_sstables.front(), {"0ck100"}, {"9ck298"});
        }
    });
}

SEASTAR_TEST_CASE(size_tiered_beyond_max_threshold_test) {
  return test_env::do_with([] (test_env& env) {
    column_family_for_tests cf(env.manager());
    auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::size_tiered, cf.schema()->compaction_strategy_options());

    std::vector<sstables::shared_sstable> candidates;
    int max_threshold = cf->schema()->max_compaction_threshold();
    candidates.reserve(max_threshold+1);
    for (auto i = 0; i < (max_threshold+1); i++) { // (max_threshold+1) sstables of similar size
        auto sst = env.make_sstable(cf.schema(), "", i, la, big);
        sstables::test(sst).set_data_file_size(1);
        candidates.push_back(std::move(sst));
    }
    auto table_s = make_table_state_for_test(cf, env);
    auto strategy_c = make_strategy_control_for_test(false);
    auto desc = cs.get_sstables_for_compaction(*table_s, *strategy_c, std::move(candidates));
    BOOST_REQUIRE(desc.sstables.size() == size_t(max_threshold));
    return cf.stop_and_keep_alive();
  });
}

SEASTAR_TEST_CASE(sstable_expired_data_ratio) {
    return test_env::do_with_async([] (test_env& env) {
        auto tmp = tmpdir();
        auto s = make_shared_schema({}, some_keyspace, some_column_family,
            {{"p1", utf8_type}}, {{"c1", utf8_type}}, {{"r1", utf8_type}}, {}, utf8_type);

        auto mt = make_lw_shared<memtable>(s);

        static constexpr float expired = 0.33;
        // we want number of expired keys to be ~ 1.5*sstables::TOMBSTONE_HISTOGRAM_BIN_SIZE so as to
        // test ability of histogram to return a good estimation after merging keys.
        static int total_keys = std::ceil(sstables::TOMBSTONE_HISTOGRAM_BIN_SIZE/expired)*1.5;

        auto insert_key = [&] (bytes k, uint32_t ttl, uint32_t expiration_time) {
            auto key = partition_key::from_exploded(*s, {k});
            mutation m(s, key);
            auto c_key = clustering_key::from_exploded(*s, {to_bytes("c1")});
            m.set_clustered_cell(c_key, *s->get_column_definition("r1"), make_atomic_cell(utf8_type, bytes("a"), ttl, expiration_time));
            mt->apply(std::move(m));
        };

        auto expired_keys = total_keys*expired;
        auto now = gc_clock::now();
        for (auto i = 0; i < expired_keys; i++) {
            // generate expiration time at different time points or only a few entries would be created in histogram
            auto expiration_time = (now - gc_clock::duration(DEFAULT_GC_GRACE_SECONDS*2+i)).time_since_epoch().count();
            insert_key(to_bytes("expired_key" + to_sstring(i)), 1, expiration_time);
        }
        auto remaining = total_keys-expired_keys;
        auto expiration_time = (now + gc_clock::duration(3600)).time_since_epoch().count();
        for (auto i = 0; i < remaining; i++) {
            insert_key(to_bytes("key" + to_sstring(i)), 3600, expiration_time);
        }
        auto sst = env.make_sstable(s, tmp.path().string(), 1, sstables::get_highest_sstable_version(), big);
        write_memtable_to_sstable_for_test(*mt, sst).get();
        sst = env.reusable_sst(s, tmp.path().string(), 1).get0();
        const auto& stats = sst->get_stats_metadata();
        BOOST_REQUIRE(stats.estimated_tombstone_drop_time.bin.size() == sstables::TOMBSTONE_HISTOGRAM_BIN_SIZE);
        auto gc_before = gc_clock::now() - s->gc_grace_seconds();
        auto uncompacted_size = sst->data_size();
        // Asserts that two keys are equal to within a positive delta
        BOOST_REQUIRE(std::fabs(sst->estimate_droppable_tombstone_ratio(gc_before) - expired) <= 0.1);
        sstable_run run;
        run.insert(sst);
        BOOST_REQUIRE(std::fabs(run.estimate_droppable_tombstone_ratio(gc_before) - expired) <= 0.1);

        column_family_for_tests cf(env.manager(), s);
        auto close_cf = deferred_stop(cf);
        auto creator = [&, gen = make_lw_shared<unsigned>(2)] {
            auto sst = env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
            return sst;
        };
        auto info = compact_sstables(sstables::compaction_descriptor({ sst }, cf->get_sstable_set(), default_priority_class()), *cf, creator).get0();
        BOOST_REQUIRE(info.new_sstables.size() == 1);
        BOOST_REQUIRE(info.new_sstables.front()->estimate_droppable_tombstone_ratio(gc_before) == 0.0f);
        BOOST_REQUIRE_CLOSE(info.new_sstables.front()->data_size(), uncompacted_size*(1-expired), 5);

        std::map<sstring, sstring> options;
        options.emplace("tombstone_threshold", "0.3f");

        auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::size_tiered, options);
        // that's needed because sstable with expired data should be old enough.
        sstables::test(sst).set_data_file_write_time(db_clock::time_point::min());
        auto table_s = make_table_state_for_test(cf, env);
        auto strategy_c = make_strategy_control_for_test(false);
        auto descriptor = cs.get_sstables_for_compaction(*table_s, *strategy_c, { sst });
        BOOST_REQUIRE(descriptor.sstables.size() == 1);
        BOOST_REQUIRE(descriptor.sstables.front() == sst);

        cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::leveled, options);
        sst->set_sstable_level(1);
        descriptor = cs.get_sstables_for_compaction(*table_s, *strategy_c, { sst });
        BOOST_REQUIRE(descriptor.sstables.size() == 1);
        BOOST_REQUIRE(descriptor.sstables.front() == sst);
        // make sure sstable picked for tombstone compaction removal won't be promoted or demoted.
        BOOST_REQUIRE(descriptor.sstables.front()->get_sstable_level() == 1U);

        // check tombstone compaction is disabled by default for DTCS
        cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::date_tiered, {});
        descriptor = cs.get_sstables_for_compaction(*table_s, *strategy_c, { sst });
        BOOST_REQUIRE(descriptor.sstables.size() == 0);
        cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::date_tiered, options);
        descriptor = cs.get_sstables_for_compaction(*table_s, *strategy_c, { sst });
        BOOST_REQUIRE(descriptor.sstables.size() == 1);
        BOOST_REQUIRE(descriptor.sstables.front() == sst);

        // sstable with droppable ratio of 0.3 won't be included due to threshold
        {
            std::map<sstring, sstring> options;
            options.emplace("tombstone_threshold", "0.5f");
            auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::size_tiered, options);
            auto descriptor = cs.get_sstables_for_compaction(*table_s, *strategy_c, { sst });
            BOOST_REQUIRE(descriptor.sstables.size() == 0);
        }
        // sstable which was recently created won't be included due to min interval
        {
            std::map<sstring, sstring> options;
            options.emplace("tombstone_compaction_interval", "3600");
            auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::size_tiered, options);
            sstables::test(sst).set_data_file_write_time(db_clock::now());
            auto descriptor = cs.get_sstables_for_compaction(*table_s, *strategy_c, { sst });
            BOOST_REQUIRE(descriptor.sstables.size() == 0);
        }
    });
}

SEASTAR_TEST_CASE(compaction_correctness_with_partitioned_sstable_set) {
    return test_env::do_with_async([] (test_env& env) {
        cell_locker_stats cl_stats;

        auto builder = schema_builder("tests", "tombstone_purge")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type);
        builder.set_gc_grace_seconds(0);
        builder.set_compaction_strategy(sstables::compaction_strategy_type::leveled);
        auto s = builder.build();

        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            auto sst = env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
            return sst;
        };

        auto compact = [&, s] (std::vector<shared_sstable> all) -> std::vector<shared_sstable> {
            // NEEDED for partitioned_sstable_set to actually have an effect
            std::for_each(all.begin(), all.end(), [] (auto& sst) { sst->set_sstable_level(1); });
            column_family_for_tests cf(env.manager(), s);
            auto close_cf = deferred_stop(cf);
            return compact_sstables(sstables::compaction_descriptor(std::move(all), cf->get_sstable_set(), default_priority_class(), 0, 0 /*std::numeric_limits<uint64_t>::max()*/),
                *cf, sst_gen).get0().new_sstables;
        };

        auto make_insert = [&] (auto p) {
            auto key = partition_key::from_exploded(*s, {to_bytes(p.first)});
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), 1 /* ts */);
            BOOST_REQUIRE(m.decorated_key().token() == p.second);
            return m;
        };

        auto tokens = token_generation_for_current_shard(4);
        auto mut1 = make_insert(tokens[0]);
        auto mut2 = make_insert(tokens[1]);
        auto mut3 = make_insert(tokens[2]);
        auto mut4 = make_insert(tokens[3]);

        {
            std::vector<shared_sstable> sstables = {
                    make_sstable_containing(sst_gen, {mut1, mut2}),
                    make_sstable_containing(sst_gen, {mut3, mut4})
            };

            auto result = compact(std::move(sstables));
            BOOST_REQUIRE_EQUAL(4, result.size());

            assert_that(sstable_reader(result[0], s, env.make_reader_permit()))
                    .produces(mut1)
                    .produces_end_of_stream();
            assert_that(sstable_reader(result[1], s, env.make_reader_permit()))
                    .produces(mut2)
                    .produces_end_of_stream();
            assert_that(sstable_reader(result[2], s, env.make_reader_permit()))
                    .produces(mut3)
                    .produces_end_of_stream();
            assert_that(sstable_reader(result[3], s, env.make_reader_permit()))
                    .produces(mut4)
                    .produces_end_of_stream();
        }

        {
            // with partitioned_sstable_set having an interval with exclusive lower boundary, example:
            // [mut1, mut2]
            // (mut2, mut3]
            std::vector<shared_sstable> sstables = {
                    make_sstable_containing(sst_gen, {mut1, mut2}),
                    make_sstable_containing(sst_gen, {mut2, mut3}),
                    make_sstable_containing(sst_gen, {mut3, mut4})
            };

            auto result = compact(std::move(sstables));
            BOOST_REQUIRE_EQUAL(4, result.size());

            assert_that(sstable_reader(result[0], s, env.make_reader_permit()))
                    .produces(mut1)
                    .produces_end_of_stream();
            assert_that(sstable_reader(result[1], s, env.make_reader_permit()))
                    .produces(mut2)
                    .produces_end_of_stream();
            assert_that(sstable_reader(result[2], s, env.make_reader_permit()))
                    .produces(mut3)
                    .produces_end_of_stream();
            assert_that(sstable_reader(result[3], s, env.make_reader_permit()))
                    .produces(mut4)
                    .produces_end_of_stream();
        }

        {
            // with gap between tables
            std::vector<shared_sstable> sstables = {
                    make_sstable_containing(sst_gen, {mut1, mut2}),
                    make_sstable_containing(sst_gen, {mut4, mut4})
            };

            auto result = compact(std::move(sstables));
            BOOST_REQUIRE_EQUAL(3, result.size());

            assert_that(sstable_reader(result[0], s, env.make_reader_permit()))
                    .produces(mut1)
                    .produces_end_of_stream();
            assert_that(sstable_reader(result[1], s, env.make_reader_permit()))
                    .produces(mut2)
                    .produces_end_of_stream();
            assert_that(sstable_reader(result[2], s, env.make_reader_permit()))
                    .produces(mut4)
                    .produces_end_of_stream();
        }
    });
}

SEASTAR_TEST_CASE(sstable_cleanup_correctness_test) {
    return do_with_cql_env([] (auto& e) {
        return test_env::do_with_async([&db = e.local_db()] (test_env& env) {
            cell_locker_stats cl_stats;

            auto ks_name = "ks";    // single_node_cql_env::ks_name
            auto s = schema_builder(ks_name, "correcness_test")
                    .with_column("id", utf8_type, column_kind::partition_key)
                    .with_column("value", int32_type).build();

            auto tmp = tmpdir();
            auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
                return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
            };

            auto make_insert = [&] (partition_key key) {
                mutation m(s, key);
                m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), api::timestamp_type(0));
                return m;
            };

            auto total_partitions = 10000U;
            auto local_keys = make_local_keys(total_partitions, s);
            std::vector<mutation> mutations;
            for (auto i = 0U; i < total_partitions; i++) {
                mutations.push_back(make_insert(partition_key::from_deeply_exploded(*s, { local_keys.at(i) })));
            }
            auto sst = make_sstable_containing(sst_gen, mutations);
            auto run_identifier = sst->run_identifier();

            auto cf = make_lw_shared<replica::column_family>(s, column_family_test_config(env.manager(), env.semaphore()), replica::column_family::no_commitlog(),
                db.get_compaction_manager(), cl_stats, db.row_cache_tracker());
            cf->mark_ready_for_writes();
            cf->start();

            dht::token_range_vector local_ranges = db.get_keyspace_local_ranges(ks_name);
            auto descriptor = sstables::compaction_descriptor({std::move(sst)}, cf->get_sstable_set(), default_priority_class(), compaction_descriptor::default_level,
                compaction_descriptor::default_max_sstable_bytes, run_identifier, compaction_type_options::make_cleanup(std::move(local_ranges)));
            auto ret = compact_sstables(std::move(descriptor), *cf, sst_gen).get0();

            BOOST_REQUIRE(ret.new_sstables.size() == 1);
            BOOST_REQUIRE(ret.new_sstables.front()->get_estimated_key_count() >= total_partitions);
            BOOST_REQUIRE((ret.new_sstables.front()->get_estimated_key_count() - total_partitions) <= uint64_t(s->min_index_interval()));
            BOOST_REQUIRE(ret.new_sstables.front()->run_identifier() == run_identifier);
        });
    });
}

std::vector<mutation_fragment> write_corrupt_sstable(test_env& env, sstable& sst, reader_permit permit,
        std::function<void(mutation_fragment&&, bool)> write_to_secondary) {
    auto schema = sst.get_schema();
    std::vector<mutation_fragment> corrupt_fragments;

    const auto ts = api::timestamp_type{1};

    auto local_keys = make_local_keys(3, schema);

    auto config = env.manager().configure_writer();
    config.validation_level = mutation_fragment_stream_validation_level::partition_region; // this test violates key order on purpose
    auto writer = sst.get_writer(*schema, local_keys.size(), config, encoding_stats{});

    auto make_static_row = [&, schema, ts] {
        auto r = row{};
        auto cdef = schema->static_column_at(0);
        auto ac = atomic_cell::make_live(*cdef.type, ts, cdef.type->decompose(data_value(1)));
        r.apply(cdef, atomic_cell_or_collection{std::move(ac)});
        return static_row(*schema, std::move(r));
    };

    auto make_clustering_row = [&, schema, ts] (unsigned i) {
        auto r = row{};
        auto cdef = schema->regular_column_at(0);
        auto ac = atomic_cell::make_live(*cdef.type, ts, cdef.type->decompose(data_value(1)));
        r.apply(cdef, atomic_cell_or_collection{std::move(ac)});
        return clustering_row(clustering_key::from_single_value(*schema, int32_type->decompose(data_value(int(i)))), {}, {}, std::move(r));
    };

    auto write_partition = [&, schema, ts] (int pk, bool is_corrupt) {
        auto pkey = partition_key::from_deeply_exploded(*schema, { local_keys.at(pk) });
        auto dkey = dht::decorate_key(*schema, pkey);

        testlog.trace("Writing partition {}", pkey.with_schema(*schema));

        write_to_secondary(mutation_fragment(*schema, permit, partition_start(dkey, {})), is_corrupt);
        corrupt_fragments.emplace_back(*schema, permit, partition_start(dkey, {}));
        writer.consume_new_partition(dkey);

        {
            auto sr = make_static_row();

            testlog.trace("Writing row {}", sr.position());

            write_to_secondary(mutation_fragment(*schema, permit, static_row(*schema, sr)), is_corrupt);
            corrupt_fragments.emplace_back(*schema, permit, static_row(*schema, sr));
            writer.consume(std::move(sr));
        }

        const unsigned rows_count = 10;
        for (unsigned i = 0; i < rows_count; ++i) {
            auto cr = make_clustering_row(i);

            testlog.trace("Writing row {}", cr.position());

            write_to_secondary(mutation_fragment(*schema, permit, clustering_row(*schema, cr)), is_corrupt);
            corrupt_fragments.emplace_back(*schema, permit, clustering_row(*schema, cr));
            writer.consume(clustering_row(*schema, cr));

            // write row twice
            if (i == (rows_count / 2)) {
                auto bad_cr = make_clustering_row(i - 2);
                testlog.trace("Writing out-of-order row {}", bad_cr.position());
                write_to_secondary(mutation_fragment(*schema, permit, clustering_row(*schema, cr)), true);
                corrupt_fragments.emplace_back(*schema, permit, clustering_row(*schema, bad_cr));
                writer.consume(std::move(bad_cr));
            }
        }

        testlog.trace("Writing partition_end");

        write_to_secondary(mutation_fragment(*schema, permit, partition_end{}), is_corrupt);
        corrupt_fragments.emplace_back(*schema, permit, partition_end{});
        writer.consume_end_of_partition();
    };

    write_partition(1, false);
    write_partition(0, true);
    write_partition(2, false);

    testlog.info("Writing done");
    writer.consume_end_of_stream();

    return corrupt_fragments;
}

SEASTAR_TEST_CASE(sstable_scrub_validate_mode_test) {
    cql_test_config test_cfg;

    auto& db_cfg = *test_cfg.db_config;

    // Disable cache to filter out its possible "corrections" to the corrupt sstable.
    db_cfg.enable_cache(false);
    db_cfg.enable_commitlog(false);

    return do_with_cql_env([this] (cql_test_env& cql_env) -> future<> {
        return test_env::do_with_async([this, &cql_env] (test_env& env) {
            cell_locker_stats cl_stats;

            auto& db = cql_env.local_db();
            auto& compaction_manager = db.get_compaction_manager();

            auto schema = schema_builder("ks", get_name())
                    .with_column("pk", utf8_type, column_kind::partition_key)
                    .with_column("ck", int32_type, column_kind::clustering_key)
                    .with_column("s", int32_type, column_kind::static_column)
                    .with_column("v", int32_type).build();
            auto permit = env.make_reader_permit();

            auto tmp = tmpdir();
            auto sst_gen = [&env, schema, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
                return env.make_sstable(schema, tmp.path().string(), (*gen)++);
            };

            auto scrubbed_mt = make_lw_shared<memtable>(schema);
            auto sst = sst_gen();

            testlog.info("Writing sstable {}", sst->get_filename());

            const auto corrupt_fragments = write_corrupt_sstable(env, *sst, permit, [&, mut = std::optional<mutation>()] (mutation_fragment&& mf, bool) mutable {
                if (mf.is_partition_start()) {
                    mut.emplace(schema, mf.as_partition_start().key());
                } else if (mf.is_end_of_partition()) {
                    scrubbed_mt->apply(std::move(*mut));
                    mut.reset();
                } else {
                    mut->apply(std::move(mf));
                }
            });

            sst->load().get();

            testlog.info("Loaded sstable {}", sst->get_filename());

            auto cfg = column_family_test_config(env.manager(), env.semaphore());
            cfg.datadir = tmp.path().string();
            auto table = make_lw_shared<replica::column_family>(schema, cfg, replica::column_family::no_commitlog(),
                db.get_compaction_manager(), cl_stats, db.row_cache_tracker());
            auto stop_table = defer([table] {
                table->stop().get();
            });
            table->mark_ready_for_writes();
            table->start();

            table->add_sstable_and_update_cache(sst).get();

            BOOST_REQUIRE(table->in_strategy_sstables().size() == 1);
            BOOST_REQUIRE(table->in_strategy_sstables().front() == sst);

            auto verify_fragments = [&] (sstables::shared_sstable sst, const std::vector<mutation_fragment>& mfs) {
                auto r = assert_that(sst->as_mutation_source().make_reader(schema, env.make_reader_permit()));
                for (const auto& mf : mfs) {
                   testlog.trace("Expecting {}", mutation_fragment::printer(*schema, mf));
                   r.produces(*schema, mf);
                }
                r.produces_end_of_stream();
            };

            testlog.info("Verifying written data...");

            // Make sure we wrote what we though we wrote.
            verify_fragments(sst, corrupt_fragments);

            testlog.info("Validate");

            // No way to really test validation besides observing the log messages.
            sstables::compaction_type_options::scrub opts = {
                .operation_mode = sstables::compaction_type_options::scrub::mode::validate,
            };
            compaction_manager.perform_sstable_scrub(table.get(), opts).get();

            BOOST_REQUIRE(sst->is_quarantined());
            BOOST_REQUIRE(table->in_strategy_sstables().empty());
            verify_fragments(sst, corrupt_fragments);
        });
    }, test_cfg);
}

SEASTAR_THREAD_TEST_CASE(scrub_validate_mode_validate_reader_test) {
    auto schema = schema_builder("ks", get_name())
            .with_column("pk", utf8_type, column_kind::partition_key)
            .with_column("ck", int32_type, column_kind::clustering_key)
            .with_column("s", int32_type, column_kind::static_column)
            .with_column("v", int32_type).build();
    tests::reader_concurrency_semaphore_wrapper semaphore;
    auto permit = semaphore.make_permit();

    std::deque<mutation_fragment> frags;

    const auto ts = api::timestamp_type{1};
    auto local_keys = make_local_keys(5, schema);

    auto make_partition_start = [&, schema] (unsigned pk) {
        auto pkey = partition_key::from_deeply_exploded(*schema, { local_keys.at(pk) });
        auto dkey = dht::decorate_key(*schema, pkey);
        return mutation_fragment(*schema, permit, partition_start(std::move(dkey), {}));
    };

    auto make_partition_end = [&, schema] {
        return mutation_fragment(*schema, permit, partition_end());
    };

    auto make_static_row = [&, schema, ts] {
        auto r = row{};
        auto cdef = schema->static_column_at(0);
        auto ac = atomic_cell::make_live(*cdef.type, ts, cdef.type->decompose(data_value(1)));
        r.apply(cdef, atomic_cell_or_collection{std::move(ac)});
        return mutation_fragment(*schema, permit, static_row(*schema, std::move(r)));
    };

    auto make_clustering_row = [&, schema, ts] (unsigned i) {
        auto r = row{};
        auto cdef = schema->regular_column_at(0);
        auto ac = atomic_cell::make_live(*cdef.type, ts, cdef.type->decompose(data_value(1)));
        r.apply(cdef, atomic_cell_or_collection{std::move(ac)});
        return mutation_fragment(*schema, permit,
                clustering_row(clustering_key::from_single_value(*schema, int32_type->decompose(data_value(int(i)))), {}, {}, std::move(r)));
    };

    auto info = make_lw_shared<compaction_data>();

    BOOST_TEST_MESSAGE("valid");
    {
        frags.emplace_back(make_partition_start(0));
        frags.emplace_back(make_static_row());
        frags.emplace_back(make_clustering_row(0));
        frags.emplace_back(make_clustering_row(1));
        frags.emplace_back(make_partition_end());
        frags.emplace_back(make_partition_start(2));
        frags.emplace_back(make_partition_end());

        const auto valid = scrub_validate_mode_validate_reader(make_flat_mutation_reader_from_fragments(schema, permit, std::move(frags)), *info).get();
        BOOST_REQUIRE(valid);
    }

    BOOST_TEST_MESSAGE("out-of-order clustering row");
    {
        frags.emplace_back(make_partition_start(0));
        frags.emplace_back(make_clustering_row(1));
        frags.emplace_back(make_clustering_row(0));
        frags.emplace_back(make_partition_end());

        const auto valid = scrub_validate_mode_validate_reader(make_flat_mutation_reader_from_fragments(schema, permit, std::move(frags)), *info).get();
        BOOST_REQUIRE(!valid);
    }

    BOOST_TEST_MESSAGE("out-of-order static row");
    {
        frags.emplace_back(make_partition_start(0));
        frags.emplace_back(make_clustering_row(0));
        frags.emplace_back(make_static_row());
        frags.emplace_back(make_partition_end());

        const auto valid = scrub_validate_mode_validate_reader(make_flat_mutation_reader_from_fragments(schema, permit, std::move(frags)), *info).get();
        BOOST_REQUIRE(!valid);
    }

    BOOST_TEST_MESSAGE("out-of-order partition start");
    {
        frags.emplace_back(make_partition_start(0));
        frags.emplace_back(make_clustering_row(1));
        frags.emplace_back(make_partition_start(2));
        frags.emplace_back(make_partition_end());

        const auto valid = scrub_validate_mode_validate_reader(make_flat_mutation_reader_from_fragments(schema, permit, std::move(frags)), *info).get();
        BOOST_REQUIRE(!valid);
    }

    BOOST_TEST_MESSAGE("out-of-order partition");
    {
        frags.emplace_back(make_partition_start(2));
        frags.emplace_back(make_clustering_row(0));
        frags.emplace_back(make_partition_end());
        frags.emplace_back(make_partition_start(0));
        frags.emplace_back(make_partition_end());

        const auto valid = scrub_validate_mode_validate_reader(make_flat_mutation_reader_from_fragments(schema, permit, std::move(frags)), *info).get();
        BOOST_REQUIRE(!valid);
    }

    BOOST_TEST_MESSAGE("missing end-of-partition at EOS");
    {
        frags.emplace_back(make_partition_start(0));
        frags.emplace_back(make_clustering_row(0));

        const auto valid = scrub_validate_mode_validate_reader(make_flat_mutation_reader_from_fragments(schema, permit, std::move(frags)), *info).get();
        BOOST_REQUIRE(!valid);
    }
}

SEASTAR_TEST_CASE(sstable_scrub_skip_mode_test) {
    cql_test_config test_cfg;

    auto& db_cfg = *test_cfg.db_config;

    // Disable cache to filter out its possible "corrections" to the corrupt sstable.
    db_cfg.enable_cache(false);
    db_cfg.enable_commitlog(false);

    return do_with_cql_env([this] (cql_test_env& cql_env) -> future<> {
        return test_env::do_with_async([this, &cql_env] (test_env& env) {
            cell_locker_stats cl_stats;

            auto& db = cql_env.local_db();
            auto& compaction_manager = db.get_compaction_manager();

            auto schema = schema_builder("ks", get_name())
                    .with_column("pk", utf8_type, column_kind::partition_key)
                    .with_column("ck", int32_type, column_kind::clustering_key)
                    .with_column("s", int32_type, column_kind::static_column)
                    .with_column("v", int32_type).build();
            auto permit = env.make_reader_permit();

            auto tmp = tmpdir();
            auto sst_gen = [&env, schema, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
                return env.make_sstable(schema, tmp.path().string(), (*gen)++);
            };

            std::vector<mutation_fragment> scrubbed_fragments;
            auto sst = sst_gen();

            const auto corrupt_fragments = write_corrupt_sstable(env, *sst, permit, [&] (mutation_fragment&& mf, bool is_corrupt) {
                if (!is_corrupt) {
                    scrubbed_fragments.emplace_back(std::move(mf));
                }
            });

            testlog.info("Writing sstable {}", sst->get_filename());

            sst->load().get();

            testlog.info("Loaded sstable {}", sst->get_filename());

            auto cfg = column_family_test_config(env.manager(), env.semaphore());
            cfg.datadir = tmp.path().string();
            auto table = make_lw_shared<replica::column_family>(schema, cfg, replica::column_family::no_commitlog(),
                db.get_compaction_manager(), cl_stats, db.row_cache_tracker());
            auto stop_table = defer([table] {
                table->stop().get();
            });
            table->mark_ready_for_writes();
            table->start();

            table->add_sstable_and_update_cache(sst).get();

            BOOST_REQUIRE(table->in_strategy_sstables().size() == 1);
            BOOST_REQUIRE(table->in_strategy_sstables().front() == sst);

            auto verify_fragments = [&] (sstables::shared_sstable sst, const std::vector<mutation_fragment>& mfs) {
                auto r = assert_that(sst->as_mutation_source().make_reader(schema, permit));
                for (const auto& mf : mfs) {
                   testlog.trace("Expecting {}", mutation_fragment::printer(*schema, mf));
                   r.produces(*schema, mf);
                }
                r.produces_end_of_stream();
            };

            testlog.info("Verifying written data...");

            // Make sure we wrote what we though we wrote.
            verify_fragments(sst, corrupt_fragments);

            testlog.info("Scrub in abort mode");

            // We expect the scrub with mode=srub::mode::abort to stop on the first invalid fragment.
            sstables::compaction_type_options::scrub opts = {};
            opts.operation_mode = sstables::compaction_type_options::scrub::mode::abort;
            compaction_manager.perform_sstable_scrub(table.get(), opts).get();

            BOOST_REQUIRE(table->in_strategy_sstables().size() == 1);
            verify_fragments(sst, corrupt_fragments);

            testlog.info("Scrub in skip mode");

            // We expect the scrub with mode=srub::mode::skip to get rid of all invalid data.
            opts.operation_mode = sstables::compaction_type_options::scrub::mode::skip;
            compaction_manager.perform_sstable_scrub(table.get(), opts).get();

            BOOST_REQUIRE(table->in_strategy_sstables().size() == 1);
            BOOST_REQUIRE(table->in_strategy_sstables().front() != sst);
            verify_fragments(table->in_strategy_sstables().front(), scrubbed_fragments);
        });
    }, test_cfg);
}

SEASTAR_TEST_CASE(sstable_scrub_segregate_mode_test) {
    cql_test_config test_cfg;

    auto& db_cfg = *test_cfg.db_config;

    // Disable cache to filter out its possible "corrections" to the corrupt sstable.
    db_cfg.enable_cache(false);
    db_cfg.enable_commitlog(false);

    return do_with_cql_env([this] (cql_test_env& cql_env) -> future<> {
        return test_env::do_with_async([this, &cql_env] (test_env& env) {
            cell_locker_stats cl_stats;

            auto& db = cql_env.local_db();
            auto& compaction_manager = db.get_compaction_manager();

            auto schema = schema_builder("ks", get_name())
                    .with_column("pk", utf8_type, column_kind::partition_key)
                    .with_column("ck", int32_type, column_kind::clustering_key)
                    .with_column("s", int32_type, column_kind::static_column)
                    .with_column("v", int32_type).build();
            auto permit = env.make_reader_permit();

            auto tmp = tmpdir();
            auto sst_gen = [&env, schema, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
                return env.make_sstable(schema, tmp.path().string(), (*gen)++);
            };

            auto scrubbed_mt = make_lw_shared<memtable>(schema);
            auto sst = sst_gen();

            testlog.info("Writing sstable {}", sst->get_filename());

            const auto corrupt_fragments = write_corrupt_sstable(env, *sst, permit, [&, mut = std::optional<mutation>()] (mutation_fragment&& mf, bool) mutable {
                if (mf.is_partition_start()) {
                    mut.emplace(schema, mf.as_partition_start().key());
                } else if (mf.is_end_of_partition()) {
                    scrubbed_mt->apply(std::move(*mut));
                    mut.reset();
                } else {
                    mut->apply(std::move(mf));
                }
            });

            sst->load().get();

            testlog.info("Loaded sstable {}", sst->get_filename());

            auto cfg = column_family_test_config(env.manager(), env.semaphore());
            cfg.datadir = tmp.path().string();
            auto table = make_lw_shared<replica::column_family>(schema, cfg, replica::column_family::no_commitlog(),
                db.get_compaction_manager(), cl_stats, db.row_cache_tracker());
            auto stop_table = defer([table] {
                table->stop().get();
            });
            table->mark_ready_for_writes();
            table->start();

            table->add_sstable_and_update_cache(sst).get();

            BOOST_REQUIRE(table->in_strategy_sstables().size() == 1);
            BOOST_REQUIRE(table->in_strategy_sstables().front() == sst);

            auto verify_fragments = [&] (sstables::shared_sstable sst, const std::vector<mutation_fragment>& mfs) {
                auto r = assert_that(sst->as_mutation_source().make_reader(schema, env.make_reader_permit()));
                for (const auto& mf : mfs) {
                   testlog.trace("Expecting {}", mutation_fragment::printer(*schema, mf));
                   r.produces(*schema, mf);
                }
                r.produces_end_of_stream();
            };

            testlog.info("Verifying written data...");

            // Make sure we wrote what we though we wrote.
            verify_fragments(sst, corrupt_fragments);

            testlog.info("Scrub in abort mode");

            // We expect the scrub with mode=srub::mode::abort to stop on the first invalid fragment.
            sstables::compaction_type_options::scrub opts = {};
            opts.operation_mode = sstables::compaction_type_options::scrub::mode::abort;
            compaction_manager.perform_sstable_scrub(table.get(), opts).get();

            BOOST_REQUIRE(table->in_strategy_sstables().size() == 1);
            verify_fragments(sst, corrupt_fragments);

            testlog.info("Scrub in segregate mode");

            // We expect the scrub with mode=srub::mode::segregate to fix all out-of-order data.
            opts.operation_mode = sstables::compaction_type_options::scrub::mode::segregate;
            compaction_manager.perform_sstable_scrub(table.get(), opts).get();

            testlog.info("Scrub resulted in {} sstables", table->in_strategy_sstables().size());
            BOOST_REQUIRE(table->in_strategy_sstables().size() > 1);
            {
                auto sst_reader = assert_that(table->as_mutation_source().make_reader(schema, env.make_reader_permit()));
                auto mt_reader = scrubbed_mt->as_data_source().make_reader(schema, env.make_reader_permit());
                auto mt_reader_close = deferred_close(mt_reader);
                while (auto mf_opt = mt_reader().get()) {
                   testlog.trace("Expecting {}", mutation_fragment::printer(*schema, *mf_opt));
                   sst_reader.produces(*schema, *mf_opt);
                }
                sst_reader.produces_end_of_stream();
            }
        });
    }, test_cfg);
}

SEASTAR_TEST_CASE(sstable_scrub_quarantine_mode_test) {
    cql_test_config test_cfg;

    auto& db_cfg = *test_cfg.db_config;

    // Disable cache to filter out its possible "corrections" to the corrupt sstable.
    db_cfg.enable_cache(false);
    db_cfg.enable_commitlog(false);

    constexpr std::array<sstables::compaction_type_options::scrub::quarantine_mode, 3> quarantine_modes = {
        sstables::compaction_type_options::scrub::quarantine_mode::include,
        sstables::compaction_type_options::scrub::quarantine_mode::exclude,
        sstables::compaction_type_options::scrub::quarantine_mode::only,
    };
    for (auto qmode : quarantine_modes) {
        co_await do_with_cql_env([this, qmode] (cql_test_env& cql_env) {
            return test_env::do_with_async([this, qmode, &cql_env] (test_env& env) {
                cell_locker_stats cl_stats;

                auto& db = cql_env.local_db();
                auto& compaction_manager = db.get_compaction_manager();

                auto schema = schema_builder("ks", get_name())
                        .with_column("pk", utf8_type, column_kind::partition_key)
                        .with_column("ck", int32_type, column_kind::clustering_key)
                        .with_column("s", int32_type, column_kind::static_column)
                        .with_column("v", int32_type).build();
                auto permit = env.make_reader_permit();

                auto tmp = tmpdir();
                auto sst_gen = [&env, schema, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
                    return env.make_sstable(schema, tmp.path().string(), (*gen)++);
                };

                auto scrubbed_mt = make_lw_shared<memtable>(schema);
                auto sst = sst_gen();

                testlog.info("Writing sstable {}", sst->get_filename());

                const auto corrupt_fragments = write_corrupt_sstable(env, *sst, permit, [&, mut = std::optional<mutation>()] (mutation_fragment&& mf, bool) mutable {
                    if (mf.is_partition_start()) {
                        mut.emplace(schema, mf.as_partition_start().key());
                    } else if (mf.is_end_of_partition()) {
                        scrubbed_mt->apply(std::move(*mut));
                        mut.reset();
                    } else {
                        mut->apply(std::move(mf));
                    }
                });

                sst->load().get();

                testlog.info("Loaded sstable {}", sst->get_filename());

                auto cfg = column_family_test_config(env.manager(), env.semaphore());
                cfg.datadir = tmp.path().string();
                auto table = make_lw_shared<replica::column_family>(schema, cfg, replica::column_family::no_commitlog(),
                    db.get_compaction_manager(), cl_stats, db.row_cache_tracker());
                auto stop_table = defer([table] {
                    table->stop().get();
                });
                table->mark_ready_for_writes();
                table->start();

                table->add_sstable_and_update_cache(sst).get();

                BOOST_REQUIRE(table->in_strategy_sstables().size() == 1);
                BOOST_REQUIRE(table->in_strategy_sstables().front() == sst);

                auto verify_fragments = [&] (sstables::shared_sstable sst, const std::vector<mutation_fragment>& mfs) {
                    auto r = assert_that(sst->as_mutation_source().make_reader(schema, env.make_reader_permit()));
                    for (const auto& mf : mfs) {
                    testlog.trace("Expecting {}", mutation_fragment::printer(*schema, mf));
                    r.produces(*schema, mf);
                    }
                    r.produces_end_of_stream();
                };

                testlog.info("Verifying written data...");

                // Make sure we wrote what we though we wrote.
                verify_fragments(sst, corrupt_fragments);

                testlog.info("Scrub in validate mode");

                // We expect the scrub with mode=scrub::mode::validate to quarantine the sstable.
                sstables::compaction_type_options::scrub opts = {};
                opts.operation_mode = sstables::compaction_type_options::scrub::mode::validate;
                compaction_manager.perform_sstable_scrub(table.get(), opts).get();

                BOOST_REQUIRE(table->in_strategy_sstables().empty());
                BOOST_REQUIRE(sst->is_quarantined());
                verify_fragments(sst, corrupt_fragments);

                testlog.info("Scrub in segregate mode with quarantine_mode {}", qmode);

                // We expect the scrub with mode=scrub::mode::segregate to fix all out-of-order data.
                opts.operation_mode = sstables::compaction_type_options::scrub::mode::segregate;
                opts.quarantine_operation_mode = qmode;
                compaction_manager.perform_sstable_scrub(table.get(), opts).get();

                switch (qmode) {
                case sstables::compaction_type_options::scrub::quarantine_mode::include:
                case sstables::compaction_type_options::scrub::quarantine_mode::only:
                    // The sstable should be found and scrubbed when scrub::quarantine_mode is scrub::quarantine_mode::{include,only}
                    testlog.info("Scrub resulted in {} sstables", table->in_strategy_sstables().size());
                    BOOST_REQUIRE(table->in_strategy_sstables().size() > 1);
                    {
                        auto sst_reader = assert_that(table->as_mutation_source().make_reader(schema, env.make_reader_permit()));
                        auto mt_reader = scrubbed_mt->as_data_source().make_reader(schema, env.make_reader_permit());
                        auto mt_reader_close = deferred_close(mt_reader);
                        while (auto mf_opt = mt_reader().get()) {
                            testlog.trace("Expecting {}", mutation_fragment::printer(*schema, *mf_opt));
                            sst_reader.produces(*schema, *mf_opt);
                        }
                        sst_reader.produces_end_of_stream();
                    }
                    break;
                case sstables::compaction_type_options::scrub::quarantine_mode::exclude:
                    // The sstable should not be found when scrub::quarantine_mode is scrub::quarantine_mode::exclude
                    BOOST_REQUIRE(table->in_strategy_sstables().empty());
                    BOOST_REQUIRE(sst->is_quarantined());
                    verify_fragments(sst, corrupt_fragments);
                    break;
                }
            });
        }, test_cfg);
    }
}

// Test the scrub_reader in segregate mode and segregate_by_partition together,
// as they are used in scrub compaction in segregate mode.
SEASTAR_THREAD_TEST_CASE(test_scrub_segregate_stack) {
    simple_schema ss;
    auto schema = ss.schema();
    tests::reader_concurrency_semaphore_wrapper semaphore;
    auto permit = semaphore.make_permit();

    struct expected_rows_type {
        using expected_clustering_rows_type = std::set<clustering_key, clustering_key::less_compare>;

        bool has_static_row = false;
        expected_clustering_rows_type clustering_rows;

        explicit expected_rows_type(const ::schema& s) : clustering_rows(s) { }
    };
    using expected_partitions_type = std::map<dht::decorated_key, expected_rows_type, dht::decorated_key::less_comparator>;
    expected_partitions_type expected_partitions{dht::decorated_key::less_comparator(schema)};

    std::deque<mutation_fragment> all_fragments;
    size_t double_partition_end = 0;
    size_t missing_partition_end = 0;

    for (uint32_t p = 0; p < 10; ++p) {
        auto dk = ss.make_pkey(tests::random::get_int<uint32_t>(0, 8));
        auto it = expected_partitions.find(dk);

        testlog.trace("Generating data for {} partition {}", it == expected_partitions.end() ? "new" : "existing", dk);

        if (it == expected_partitions.end()) {
            auto [inserted_it, _] = expected_partitions.emplace(dk, expected_rows_type(*schema));
            it = inserted_it;
        }

        all_fragments.emplace_back(*schema, permit, partition_start(dk, {}));

        auto& expected_rows = it->second;

        for (uint32_t r = 0; r < 10; ++r) {
            const auto is_clustering_row = tests::random::get_int<unsigned>(0, 8);
            if (is_clustering_row) {
                auto ck = ss.make_ckey(tests::random::get_int<uint32_t>(0, 8));
                testlog.trace("Generating clustering row {}", ck);

                all_fragments.emplace_back(*schema, permit, ss.make_row(permit, ck, "cv"));
                expected_rows.clustering_rows.insert(ck);
            } else {
                testlog.trace("Generating static row");

                all_fragments.emplace_back(*schema, permit, ss.make_static_row(permit, "sv"));
                expected_rows.has_static_row = true;
            }
        }

        const auto partition_end_roll = tests::random::get_int(0, 100);
        if (partition_end_roll < 80) {
            testlog.trace("Generating partition end");
            all_fragments.emplace_back(*schema, permit, partition_end());
        } else if (partition_end_roll < 90) {
            testlog.trace("Generating double partition end");
            ++double_partition_end;
            all_fragments.emplace_back(*schema, permit, partition_end());
            all_fragments.emplace_back(*schema, permit, partition_end());
        } else {
            testlog.trace("Not generating partition end");
            ++missing_partition_end;
        }
    }

    {
        size_t rows = 0;
        for (const auto& part : expected_partitions) {
            rows += part.second.clustering_rows.size();
        }
        testlog.info("Generated {} partitions (with {} double and {} missing partition ends), {} rows and {} fragments total", expected_partitions.size(), double_partition_end, missing_partition_end, rows, all_fragments.size());
    }

    auto copy_fragments = [&schema, &semaphore] (const std::deque<mutation_fragment>& frags) {
        auto permit = semaphore.make_permit();
        std::deque<mutation_fragment> copied_fragments;
        for (const auto& frag : frags) {
            copied_fragments.emplace_back(*schema, permit, frag);
        }
        return copied_fragments;
    };

    std::list<std::deque<mutation_fragment>> segregated_fragment_streams;

    mutation_writer::segregate_by_partition(
            make_scrubbing_reader(make_flat_mutation_reader_from_fragments(schema, permit, std::move(all_fragments)), sstables::compaction_type_options::scrub::mode::segregate),
            mutation_writer::segregate_config{default_priority_class(), 100000},
            [&schema, &segregated_fragment_streams] (flat_mutation_reader rd) {
        return async([&schema, &segregated_fragment_streams, rd = std::move(rd)] () mutable {
            auto close = deferred_close(rd);
            auto& fragments = segregated_fragment_streams.emplace_back();
            while (auto mf_opt = rd().get()) {
                fragments.emplace_back(*schema, rd.permit(), *mf_opt);
            }
        });
    }).get();

    testlog.info("Segregation resulted in {} fragment streams", segregated_fragment_streams.size());

    testlog.info("Checking position monotonicity of segregated streams");
    {
        size_t i = 0;
        for (const auto& segregated_fragment_stream : segregated_fragment_streams) {
            testlog.debug("Checking position monotonicity of segregated stream #{}", i++);
            assert_that(make_flat_mutation_reader_from_fragments(schema, permit, copy_fragments(segregated_fragment_stream)))
                    .has_monotonic_positions();
        }
    }

    testlog.info("Checking position monotonicity of re-combined stream");
    {
        std::vector<flat_mutation_reader_v2> readers;
        readers.reserve(segregated_fragment_streams.size());

        for (const auto& segregated_fragment_stream : segregated_fragment_streams) {
            readers.emplace_back(upgrade_to_v2(make_flat_mutation_reader_from_fragments(schema, permit, copy_fragments(segregated_fragment_stream))));
        }

        assert_that(make_combined_reader(schema, permit, std::move(readers))).has_monotonic_positions();
    }

    testlog.info("Checking content of re-combined stream");
    {
        std::vector<flat_mutation_reader_v2> readers;
        readers.reserve(segregated_fragment_streams.size());

        for (const auto& segregated_fragment_stream : segregated_fragment_streams) {
            readers.emplace_back(upgrade_to_v2(make_flat_mutation_reader_from_fragments(schema, permit, copy_fragments(segregated_fragment_stream))));
        }

        auto rd = assert_that(make_combined_reader(schema, permit, std::move(readers)));
        for (const auto& [pkey, content] : expected_partitions) {
            testlog.debug("Checking content of partition {}", pkey);
            rd.produces_partition_start(pkey);
            if (content.has_static_row) {
                rd.produces_static_row();
            }
            for (const auto& ckey : content.clustering_rows) {
                rd.produces_row_with_key(ckey);
            }
            rd.produces_partition_end();
        }
        rd.produces_end_of_stream();
    }
}

SEASTAR_THREAD_TEST_CASE(sstable_scrub_reader_test) {
    auto schema = schema_builder("ks", get_name())
            .with_column("pk", utf8_type, column_kind::partition_key)
            .with_column("ck", int32_type, column_kind::clustering_key)
            .with_column("s", int32_type, column_kind::static_column)
            .with_column("v", int32_type).build();
    tests::reader_concurrency_semaphore_wrapper semaphore;
    auto permit = semaphore.make_permit();

    std::deque<mutation_fragment> corrupt_fragments;
    std::deque<mutation_fragment> scrubbed_fragments;

    const auto ts = api::timestamp_type{1};
    auto local_keys = make_local_keys(5, schema);

    auto make_partition_start = [&, schema] (unsigned pk) {
        auto pkey = partition_key::from_deeply_exploded(*schema, { local_keys.at(pk) });
        auto dkey = dht::decorate_key(*schema, pkey);
        return mutation_fragment(*schema, permit, partition_start(std::move(dkey), {}));
    };

    auto make_static_row = [&, schema, ts] {
        auto r = row{};
        auto cdef = schema->static_column_at(0);
        auto ac = atomic_cell::make_live(*cdef.type, ts, cdef.type->decompose(data_value(1)));
        r.apply(cdef, atomic_cell_or_collection{std::move(ac)});
        return mutation_fragment(*schema, permit, static_row(*schema, std::move(r)));
    };

    auto make_clustering_row = [&, schema, ts] (unsigned i) {
        auto r = row{};
        auto cdef = schema->regular_column_at(0);
        auto ac = atomic_cell::make_live(*cdef.type, ts, cdef.type->decompose(data_value(1)));
        r.apply(cdef, atomic_cell_or_collection{std::move(ac)});
        return mutation_fragment(*schema, permit,
                clustering_row(clustering_key::from_single_value(*schema, int32_type->decompose(data_value(int(i)))), {}, {}, std::move(r)));
    };

    auto add_fragment = [&, schema] (mutation_fragment mf, bool add_to_scrubbed = true) {
        corrupt_fragments.emplace_back(mutation_fragment(*schema, permit, mf));
        if (add_to_scrubbed) {
            scrubbed_fragments.emplace_back(std::move(mf));
        }
    };

    // Partition 0
    add_fragment(make_partition_start(0));
    add_fragment(make_static_row());
    add_fragment(make_clustering_row(0));
    add_fragment(make_clustering_row(2));
    add_fragment(make_clustering_row(1), false); // out-of-order clustering key
    scrubbed_fragments.emplace_back(*schema, permit, partition_end{}); // missing partition-end

    // Partition 2
    add_fragment(make_partition_start(2));
    add_fragment(make_static_row());
    add_fragment(make_clustering_row(0));
    add_fragment(make_clustering_row(1));
    add_fragment(make_static_row(), false); // out-of-order static row
    add_fragment(mutation_fragment(*schema, permit, partition_end{}));

    // Partition 1 - out-of-order
    add_fragment(make_partition_start(1), false);
    add_fragment(make_static_row(), false);
    add_fragment(make_clustering_row(0), false);
    add_fragment(make_clustering_row(1), false);
    add_fragment(make_clustering_row(2), false);
    add_fragment(make_clustering_row(3), false);
    add_fragment(mutation_fragment(*schema, permit, partition_end{}), false);

    // Partition 3
    add_fragment(make_partition_start(3));
    add_fragment(make_static_row());
    add_fragment(make_clustering_row(0));
    add_fragment(make_clustering_row(1));
    add_fragment(make_clustering_row(2));
    add_fragment(make_clustering_row(3));
    scrubbed_fragments.emplace_back(*schema, permit, partition_end{}); // missing partition-end - at EOS

    auto r = assert_that(make_scrubbing_reader(make_flat_mutation_reader_from_fragments(schema, permit, std::move(corrupt_fragments)),
                compaction_type_options::scrub::mode::skip));
    for (const auto& mf : scrubbed_fragments) {
       testlog.info("Expecting {}", mutation_fragment::printer(*schema, mf));
       r.produces(*schema, mf);
    }
    r.produces_end_of_stream();
}

SEASTAR_TEST_CASE(sstable_run_based_compaction_test) {
    return test_env::do_with_async([] (test_env& env) {
        cell_locker_stats cl_stats;

        auto builder = schema_builder("tests", "sstable_run_based_compaction_test")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type);
        auto s = builder.build();

        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            auto sst = env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
            return sst;
        };

        auto tracker = make_lw_shared<cache_tracker>();
        column_family_for_tests cf(env.manager(), s);
        auto close_cf = deferred_stop(cf);
        cf->mark_ready_for_writes();
        cf->start();
        cf->set_compaction_strategy(sstables::compaction_strategy_type::size_tiered);
        auto compact = [&, s] (std::vector<shared_sstable> all, auto replacer) -> std::vector<shared_sstable> {
            return compact_sstables(sstables::compaction_descriptor(std::move(all), cf->get_sstable_set(), default_priority_class(), 1, 0), *cf, sst_gen, replacer).get0().new_sstables;
        };
        auto make_insert = [&] (auto p) {
            auto key = partition_key::from_exploded(*s, {to_bytes(p.first)});
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), 1 /* ts */);
            BOOST_REQUIRE(m.decorated_key().token() == p.second);
            return m;
        };

        auto tokens = token_generation_for_current_shard(16);
        std::unordered_set<shared_sstable> sstables;
        std::vector<utils::observer<sstable&>> observers;
        sstables::sstable_run_based_compaction_strategy_for_tests cs;

        auto do_replace = [&] (const std::vector<shared_sstable>& old_sstables, const std::vector<shared_sstable>& new_sstables) {
            for (auto& old_sst : old_sstables) {
                BOOST_REQUIRE(sstables.contains(old_sst));
                sstables.erase(old_sst);
            }
            for (auto& new_sst : new_sstables) {
                BOOST_REQUIRE(!sstables.contains(new_sst));
                sstables.insert(new_sst);
            }
            column_family_test(cf).rebuild_sstable_list(new_sstables, old_sstables);
            cf->get_compaction_manager().propagate_replacement(&*cf, old_sstables, new_sstables);
        };

        auto do_incremental_replace = [&] (auto old_sstables, auto new_sstables, auto& expected_sst, auto& closed_sstables_tracker) {
            // that's because each sstable will contain only 1 mutation.
            BOOST_REQUIRE(old_sstables.size() == 1);
            BOOST_REQUIRE(new_sstables.size() == 1);
            // check that sstable replacement follows token order
            BOOST_REQUIRE(*expected_sst == old_sstables.front()->generation());
            expected_sst++;
            // check that previously released sstables were already closed
            if (old_sstables.front()->generation() % 4 == 0) {
                // Due to performance reasons, sstables are not released immediately, but in batches.
                // At the time of writing, mutation_reader_merger releases it's sstable references
                // in batches of 4. That's why we only perform this check every 4th sstable. 
                BOOST_REQUIRE(*closed_sstables_tracker == old_sstables.front()->generation());
            }

            do_replace(old_sstables, new_sstables);

            observers.push_back(old_sstables.front()->add_on_closed_handler([&] (sstable& sst) {
                testlog.info("Closing sstable of generation {}", sst.generation());
                closed_sstables_tracker++;
            }));

            testlog.info("Removing sstable of generation {}, refcnt: {}", old_sstables.front()->generation(), old_sstables.front().use_count());
        };

        auto do_compaction = [&] (size_t expected_input, size_t expected_output) -> std::vector<shared_sstable> {
            auto input_ssts = std::vector<shared_sstable>(sstables.begin(), sstables.end());
            auto table_s = make_table_state_for_test(cf, env);
            auto strategy_c = make_strategy_control_for_test(false);
            auto desc = cs.get_sstables_for_compaction(*table_s, *strategy_c, std::move(input_ssts));

            // nothing to compact, move on.
            if (desc.sstables.empty()) {
                return {};
            }
            std::unordered_set<utils::UUID> run_ids;
            bool incremental_enabled = std::any_of(desc.sstables.begin(), desc.sstables.end(), [&run_ids] (shared_sstable& sst) {
                return !run_ids.insert(sst->run_identifier()).second;
            });

            BOOST_REQUIRE(desc.sstables.size() == expected_input);
            auto sstable_run = boost::copy_range<std::set<int64_t>>(desc.sstables
                | boost::adaptors::transformed([] (auto& sst) { return sst->generation(); }));
            auto expected_sst = sstable_run.begin();
            auto closed_sstables_tracker = sstable_run.begin();
            auto replacer = [&] (sstables::compaction_completion_desc desc) {
                auto old_sstables = std::move(desc.old_sstables);
                auto new_sstables = std::move(desc.new_sstables);
                BOOST_REQUIRE(expected_sst != sstable_run.end());
                if (incremental_enabled) {
                    do_incremental_replace(std::move(old_sstables), std::move(new_sstables), expected_sst, closed_sstables_tracker);
                } else {
                    do_replace(std::move(old_sstables), std::move(new_sstables));
                    expected_sst = sstable_run.end();
                }
            };

            auto result = compact(std::move(desc.sstables), replacer);
            BOOST_REQUIRE_EQUAL(expected_output, result.size());
            BOOST_REQUIRE(expected_sst == sstable_run.end());
            return result;
        };

        // Generate 4 sstable runs composed of 4 fragments each after 4 compactions.
        // All fragments non-overlapping.
        for (auto i = 0U; i < tokens.size(); i++) {
            auto sst = make_sstable_containing(sst_gen, { make_insert(tokens[i]) });
            sst->set_sstable_level(1);
            BOOST_REQUIRE(sst->get_sstable_level() == 1);
            column_family_test(cf).add_sstable(sst);
            sstables.insert(std::move(sst));
            do_compaction(4, 4);
        }
        BOOST_REQUIRE(sstables.size() == 16);

        // Generate 1 sstable run from 4 sstables runs of similar size
        auto result = do_compaction(16, 16);
        BOOST_REQUIRE(result.size() == 16);
        for (auto i = 0U; i < tokens.size(); i++) {
            assert_that(sstable_reader(result[i], s, env.make_reader_permit()))
                .produces(make_insert(tokens[i]))
                .produces_end_of_stream();
        }
    });
}

SEASTAR_TEST_CASE(compaction_strategy_aware_major_compaction_test) {
    return test_env::do_with_async([] (test_env& env) {
        cell_locker_stats cl_stats;

        auto s = schema_builder("tests", "compaction_strategy_aware_major_compaction_test")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type).build();

        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
        };
        auto make_insert = [&] (partition_key key) {
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), api::timestamp_type(0));
            return m;
        };

        auto alpha = partition_key::from_exploded(*s, {to_bytes("alpha")});
        auto sst = make_sstable_containing(sst_gen, {make_insert(alpha)});
        sst->set_sstable_level(2);
        auto sst2 = make_sstable_containing(sst_gen, {make_insert(alpha)});
        sst2->set_sstable_level(3);
        auto candidates = std::vector<sstables::shared_sstable>({ sst, sst2 });

        column_family_for_tests cf(env.manager());
        auto close_cf = deferred_stop(cf);
        auto table_s = make_table_state_for_test(cf, env);

        {
            auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::leveled, cf.schema()->compaction_strategy_options());
            auto descriptor = cs.get_major_compaction_job(*table_s, candidates);
            BOOST_REQUIRE(descriptor.sstables.size() == candidates.size());
            BOOST_REQUIRE(uint32_t(descriptor.level) == sst2->get_sstable_level());
        }

        {
            auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::size_tiered, cf.schema()->compaction_strategy_options());
            auto descriptor = cs.get_major_compaction_job(*table_s, candidates);
            BOOST_REQUIRE(descriptor.sstables.size() == candidates.size());
            BOOST_REQUIRE(descriptor.level == 0);
        }
    });
}

SEASTAR_TEST_CASE(backlog_tracker_correctness_after_changing_compaction_strategy) {
    return test_env::do_with_async([] (test_env& env) {
        cell_locker_stats cl_stats;

        auto builder = schema_builder("tests", "backlog_tracker_correctness_after_changing_compaction_strategy")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type);
        auto s = builder.build();

        auto tmp = make_lw_shared<tmpdir>();
        auto sst_gen = [&env, s, tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            auto sst = env.make_sstable(s, tmp->path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
            return sst;
        };

        column_family_for_tests cf(env.manager(), s);
        auto close_cf = deferred_stop(cf);
        cf->set_compaction_strategy(sstables::compaction_strategy_type::leveled);

        {
            auto tokens = token_generation_for_current_shard(4);
            auto make_insert = [&] (auto p) {
                auto key = partition_key::from_exploded(*s, {to_bytes(p.first)});
                mutation m(s, key);
                m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), 1 /* ts */);
                BOOST_REQUIRE(m.decorated_key().token() == p.second);
                return m;
            };
            auto mut1 = make_insert(tokens[0]);
            auto mut2 = make_insert(tokens[1]);
            auto mut3 = make_insert(tokens[2]);
            auto mut4 = make_insert(tokens[3]);
            std::vector<shared_sstable> ssts = {
                    make_sstable_containing(sst_gen, {mut1, mut2}),
                    make_sstable_containing(sst_gen, {mut3, mut4})
            };

            for (auto& sst : ssts) {
                cf->get_compaction_strategy().get_backlog_tracker().add_sstable(sst);
            }

            // Start compaction, then stop tracking compaction, switch to TWCS, wait for compaction to finish and check for backlog.
            // That's done to assert backlog will work for compaction that is finished and was stopped tracking.

            auto fut = compact_sstables(sstables::compaction_descriptor(ssts, cf->get_sstable_set(), default_priority_class()), *cf, sst_gen);

            // set_compaction_strategy() itself is responsible for transferring charges from old to new backlog tracker.
            cf->set_compaction_strategy(sstables::compaction_strategy_type::time_window);
            for (auto& sst : ssts) {
                cf->get_compaction_strategy().get_backlog_tracker().add_sstable(sst);
            }

            auto ret = fut.get0();
            BOOST_REQUIRE(ret.new_sstables.size() == 1);
        }
        // triggers code that iterates through registered compactions.
        cf._data->cm.backlog();
        cf->get_compaction_strategy().get_backlog_tracker().backlog();
    });
}

SEASTAR_TEST_CASE(partial_sstable_run_filtered_out_test) {
    BOOST_REQUIRE(smp::count == 1);
    return test_env::do_with_async([] (test_env& env) {
        auto s = schema_builder("tests", "partial_sstable_run_filtered_out_test")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type).build();

        auto tmp = tmpdir();

        auto cm = make_lw_shared<compaction_manager>();
        cm->enable();

        replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
        cfg.datadir = tmp.path().string();
        cfg.enable_commitlog = false;
        cfg.enable_incremental_backups = false;
        auto cl_stats = make_lw_shared<cell_locker_stats>();
        auto tracker = make_lw_shared<cache_tracker>();
        auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), *cm, *cl_stats, *tracker);
        cf->start();
        cf->mark_ready_for_writes();

        utils::UUID partial_sstable_run_identifier = utils::make_random_uuid();
        mutation mut(s, partition_key::from_exploded(*s, {to_bytes("alpha")}));
        mut.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), 0);

        sstable_writer_config sst_cfg = env.manager().configure_writer();
        sst_cfg.run_identifier = partial_sstable_run_identifier;
        auto partial_sstable_run_sst = make_sstable_easy(env, tmp.path(), make_flat_mutation_reader_from_mutations(s, env.make_reader_permit(), { std::move(mut) }), sst_cfg);

        column_family_test(cf).add_sstable(partial_sstable_run_sst);
        column_family_test::update_sstables_known_generation(*cf, partial_sstable_run_sst->generation());

        auto generation_exists = [&cf] (int64_t generation) {
            auto sstables = cf->get_sstables();
            auto entry = boost::range::find_if(*sstables, [generation] (shared_sstable sst) { return generation == sst->generation(); });
            return entry != sstables->end();
        };

        BOOST_REQUIRE(generation_exists(partial_sstable_run_sst->generation()));

        // register partial sstable run
        auto cm_test = compaction_manager_test(*cm);
        auto& cdata = cm_test.register_compaction(partial_sstable_run_identifier);
        auto deregister_compaction = defer([&] () noexcept {
            cm_test.deregister_compaction(cdata);
        });

        cf->compact_all_sstables().get();

        deregister_compaction.cancel();
        cm_test.deregister_compaction(cdata);

        // make sure partial sstable run has none of its fragments compacted.
        BOOST_REQUIRE(generation_exists(partial_sstable_run_sst->generation()));

        cm->stop().get();
    });
}

// Make sure that a custom tombstone-gced-only writer will be feeded with gc'able tombstone
// from the regular compaction's input sstable.
SEASTAR_TEST_CASE(purged_tombstone_consumer_sstable_test) {
    BOOST_REQUIRE(smp::count == 1);
    return test_env::do_with_async([] (test_env& env) {
        cell_locker_stats cl_stats;

        auto builder = schema_builder("tests", "purged_tombstone_consumer_sstable_test")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type);
        builder.set_gc_grace_seconds(0);
        auto s = builder.build();

        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
        };

        class compacting_sstable_writer_test {
            shared_sstable& _sst;
            sstable_writer _writer;
        public:
            explicit compacting_sstable_writer_test(const schema_ptr& s, shared_sstable& sst, sstables_manager& manager)
                : _sst(sst),
                  _writer(sst->get_writer(*s, 1, manager.configure_writer("test"),
                        encoding_stats{}, service::get_local_compaction_priority())) {}

            void consume_new_partition(const dht::decorated_key& dk) { _writer.consume_new_partition(dk); }
            void consume(tombstone t) { _writer.consume(t); }
            stop_iteration consume(static_row&& sr, tombstone, bool) { return _writer.consume(std::move(sr)); }
            stop_iteration consume(clustering_row&& cr, row_tombstone tomb, bool) { return _writer.consume(std::move(cr)); }
            stop_iteration consume(range_tombstone&& rt) { return _writer.consume(std::move(rt)); }

            stop_iteration consume_end_of_partition() { return _writer.consume_end_of_partition(); }
            void consume_end_of_stream() { _writer.consume_end_of_stream(); _sst->open_data().get0(); }
        };

        std::optional<gc_clock::time_point> gc_before;
        auto max_purgeable_ts = api::max_timestamp;
        auto is_tombstone_purgeable = [&gc_before, max_purgeable_ts](const tombstone& t) {
            bool can_gc = t.deletion_time < *gc_before;
            return t && can_gc && t.timestamp < max_purgeable_ts;
        };

        auto compact = [&] (std::vector<shared_sstable> all) -> std::pair<shared_sstable, shared_sstable> {
            auto max_purgeable_func = [max_purgeable_ts] (const dht::decorated_key& dk) {
                return max_purgeable_ts;
            };

            auto non_purged = sst_gen();
            auto purged_only = sst_gen();

            auto cr = compacting_sstable_writer_test(s, non_purged, env.manager());
            auto purged_cr = compacting_sstable_writer_test(s, purged_only, env.manager());

            auto gc_now = gc_clock::now();
            gc_before = gc_now - s->gc_grace_seconds();
            auto gc_grace_seconds = s->gc_grace_seconds();

            auto cfc = make_stable_flattened_mutations_consumer<compact_for_compaction<compacting_sstable_writer_test, compacting_sstable_writer_test>>(
                *s, gc_now, max_purgeable_func, std::move(cr), std::move(purged_cr));

            auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::size_tiered, s->compaction_strategy_options());
            auto compacting = make_lw_shared<sstables::sstable_set>(cs.make_sstable_set(s));
            for (auto&& sst : all) {
                compacting->insert(std::move(sst));
            }
            auto reader = downgrade_to_v1(compacting->make_range_sstable_reader(s,
                env.make_reader_permit(),
                query::full_partition_range,
                s->full_slice(),
                service::get_local_compaction_priority(),
                nullptr,
                ::streamed_mutation::forwarding::no,
                ::mutation_reader::forwarding::no));

            auto r = std::move(reader);
            auto close_r = deferred_close(r);
            r.consume_in_thread(std::move(cfc));

            return {std::move(non_purged), std::move(purged_only)};
        };

        auto next_timestamp = [] {
            static thread_local api::timestamp_type next = 1;
            return next++;
        };

        auto make_insert = [&] (partition_key key) {
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), next_timestamp());
            return m;
        };

        auto make_delete = [&] (partition_key key) -> std::pair<mutation, tombstone> {
            mutation m(s, key);
            tombstone tomb(next_timestamp(), gc_clock::now());
            m.partition().apply(tomb);
            return {m, tomb};
        };

        auto alpha = partition_key::from_exploded(*s, {to_bytes("alpha")});
        auto beta = partition_key::from_exploded(*s, {to_bytes("beta")});

        auto ttl = 5;

        auto assert_that_produces_purged_tombstone = [&] (auto& sst, partition_key& key, tombstone tomb) {
            auto reader = make_lw_shared<flat_mutation_reader>(sstable_reader(sst, s, env.make_reader_permit()));
            read_mutation_from_flat_mutation_reader(*reader).then([reader, s, &key, is_tombstone_purgeable, &tomb] (mutation_opt m) {
                BOOST_REQUIRE(m);
                BOOST_REQUIRE(m->key().equal(*s, key));
                auto rows = m->partition().clustered_rows();
                BOOST_REQUIRE_EQUAL(rows.calculate_size(), 0);
                BOOST_REQUIRE(is_tombstone_purgeable(m->partition().partition_tombstone()));
                BOOST_REQUIRE(m->partition().partition_tombstone() == tomb);
                return (*reader)();
            }).then([reader, s] (mutation_fragment_opt m) {
                BOOST_REQUIRE(!m);
            }).finally([reader] {
                return reader->close();
            }).get();
        };

        // gc'ed tombstone for alpha will go to gc-only consumer, whereas live data goes to regular consumer.
        {
            auto mut1 = make_insert(alpha);
            auto mut2 = make_insert(beta);
            auto [mut3, mut3_tombstone] = make_delete(alpha);

            std::vector<shared_sstable> sstables = {
                make_sstable_containing(sst_gen, {mut1, mut2}),
                make_sstable_containing(sst_gen, {mut3})
            };

            forward_jump_clocks(std::chrono::seconds(ttl));

            auto [non_purged, purged_only] = compact(std::move(sstables));

            assert_that(sstable_reader(non_purged, s, env.make_reader_permit()))
                    .produces(mut2)
                    .produces_end_of_stream();

            assert_that_produces_purged_tombstone(purged_only, alpha, mut3_tombstone);
        }
    });
}

/*  Make sure data is not ressurrected.
    sstable 1 with key A and key B and key C
    sstable 2 with expired (GC'able) tombstone for key A

    use max_sstable_size = 1;

    so key A and expired tombstone for key A are compacted away.
    key B is written into a new sstable, and sstable 2 is removed.

    Need to stop compaction at this point!!!

    Result: sstable 1 is alive in the table, whereas sstable 2 is gone.

    if key A can be read from table, data was ressurrected.
 */
SEASTAR_TEST_CASE(incremental_compaction_data_resurrection_test) {
    return test_env::do_with_async([] (test_env& env) {
        cell_locker_stats cl_stats;

        // In a column family with gc_grace_seconds set to 0, check that a tombstone
        // is purged after compaction.
        auto builder = schema_builder("tests", "incremental_compaction_data_resurrection_test")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type);
        builder.set_gc_grace_seconds(0);
        auto s = builder.build();

        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
        };

        auto next_timestamp = [] {
            static thread_local api::timestamp_type next = 1;
            return next++;
        };

        auto make_insert = [&] (partition_key key) {
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), next_timestamp());
            return m;
        };

        auto deletion_time = gc_clock::now();
        auto make_delete = [&] (partition_key key) {
            mutation m(s, key);
            tombstone tomb(next_timestamp(), deletion_time);
            m.partition().apply(tomb);
            return m;
        };

        auto tokens = token_generation_for_current_shard(3);
        auto alpha = partition_key::from_exploded(*s, {to_bytes(tokens[0].first)});
        auto beta = partition_key::from_exploded(*s, {to_bytes(tokens[1].first)});
        auto gamma = partition_key::from_exploded(*s, {to_bytes(tokens[2].first)});

        auto ttl = 5;

        auto mut1 = make_insert(alpha);
        auto mut2 = make_insert(beta);
        auto mut3 = make_insert(gamma);
        auto mut1_deletion = make_delete(alpha);

        auto non_expired_sst = make_sstable_containing(sst_gen, {mut1, mut2, mut3});
        auto expired_sst = make_sstable_containing(sst_gen, {mut1_deletion});
        // make ssts belong to same run for compaction to enable incremental approach
        utils::UUID run_id = utils::make_random_uuid();
        sstables::test(non_expired_sst).set_run_identifier(run_id);
        sstables::test(expired_sst).set_run_identifier(run_id);

        std::vector<shared_sstable> sstables = {
                non_expired_sst,
                expired_sst,
        };

        // make mut1_deletion gc'able.
        forward_jump_clocks(std::chrono::seconds(ttl));

        auto cm = make_lw_shared<compaction_manager>();
        replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
        cfg.datadir = tmp.path().string();
        cfg.enable_disk_writes = false;
        cfg.enable_commitlog = false;
        cfg.enable_cache = true;
        cfg.enable_incremental_backups = false;
        auto tracker = make_lw_shared<cache_tracker>();
        auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), *cm, cl_stats, *tracker);
        auto stop_cf = deferred_stop(*cf);
        cf->mark_ready_for_writes();
        cf->start();
        cf->set_compaction_strategy(sstables::compaction_strategy_type::null);

        auto is_partition_dead = [&s, &cf, &env] (partition_key& pkey) {
            replica::column_family::const_mutation_partition_ptr mp = cf->find_partition_slow(s, env.make_reader_permit(), pkey).get0();
            return mp && bool(mp->partition_tombstone());
        };

        cf->add_sstable_and_update_cache(non_expired_sst).get();
        BOOST_REQUIRE(!is_partition_dead(alpha));
        cf->add_sstable_and_update_cache(expired_sst).get();
        BOOST_REQUIRE(is_partition_dead(alpha));

        auto replacer = [&] (sstables::compaction_completion_desc desc) {
            auto old_sstables = std::move(desc.old_sstables);
            auto new_sstables = std::move(desc.new_sstables);
            // expired_sst is exhausted, and new sstable is written with mut 2.
            BOOST_REQUIRE_EQUAL(old_sstables.size(), 1);
            BOOST_REQUIRE(old_sstables.front() == expired_sst);
            BOOST_REQUIRE_EQUAL(new_sstables.size(), 2);
            for (auto& new_sstable : new_sstables) {
                if (new_sstable->get_max_local_deletion_time() == deletion_time) { // Skipping GC SSTable.
                    continue;
                }
                assert_that(sstable_reader(new_sstable, s, env.make_reader_permit()))
                    .produces(mut2)
                    .produces_end_of_stream();
            }
            column_family_test(cf).rebuild_sstable_list(new_sstables, old_sstables);
            // force compaction failure after sstable containing expired tombstone is removed from set.
            throw std::runtime_error("forcing compaction failure on early replacement");
        };

        bool swallowed = false;
        try {
            // The goal is to have one sstable generated for each mutation to trigger the issue.
            auto max_sstable_size = 0;
            auto result = compact_sstables(sstables::compaction_descriptor(sstables, cf->get_sstable_set(), default_priority_class(), 0, max_sstable_size), *cf, sst_gen, replacer).get0().new_sstables;
            BOOST_REQUIRE_EQUAL(2, result.size());
        } catch (...) {
            // swallow exception
            swallowed = true;
        }
        BOOST_REQUIRE(swallowed);
        // check there's no data resurrection
        BOOST_REQUIRE(is_partition_dead(alpha));
    });
}

SEASTAR_TEST_CASE(twcs_major_compaction_test) {
    // Tests that two mutations that were written a month apart are compacted
    // to two different SSTables, whereas two mutations that were written 1ms apart
    // are compacted to the same SSTable.
    return test_env::do_with_async([] (test_env& env) {
        cell_locker_stats cl_stats;

        // In a column family with gc_grace_seconds set to 0, check that a tombstone
        // is purged after compaction.
        auto builder = schema_builder("tests", "twcs_major")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("cl", int32_type, column_kind::clustering_key)
                .with_column("value", int32_type);
        auto s = builder.build();

        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
        };

        auto next_timestamp = [] (auto step) {
            using namespace std::chrono;
            return (api::timestamp_clock::now().time_since_epoch() - duration_cast<microseconds>(step)).count();
        };

        auto make_insert = [&] (api::timestamp_clock::duration step) {
            static thread_local int32_t value = 1;

            auto key_and_token_pair = token_generation_for_current_shard(1);
            auto key_str = key_and_token_pair[0].first;
            auto key = partition_key::from_exploded(*s, {to_bytes(key_str)});

            mutation m(s, key);
            auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(value++)});
            m.set_clustered_cell(c_key, bytes("value"), data_value(int32_t(value)), next_timestamp(step));
            return m;
        };


        // Two mutations, one of them 30 days ago. Should be split when
        // compacting
        auto mut1 = make_insert(0ms);
        auto mut2 = make_insert(720h);

        // Two mutations, close together. Should end up in the same SSTable
        auto mut3 = make_insert(0ms);
        auto mut4 = make_insert(1ms);

        auto cm = make_lw_shared<compaction_manager>();
        replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
        cfg.datadir = tmp.path().string();
        cfg.enable_disk_writes = true;
        cfg.enable_commitlog = false;
        cfg.enable_cache = false;
        cfg.enable_incremental_backups = false;
        auto tracker = make_lw_shared<cache_tracker>();
        auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), *cm, cl_stats, *tracker);
        cf->mark_ready_for_writes();
        cf->start();
        cf->set_compaction_strategy(sstables::compaction_strategy_type::time_window);

        auto original_together = make_sstable_containing(sst_gen, {mut3, mut4});

        auto ret = compact_sstables(sstables::compaction_descriptor({original_together}, cf->get_sstable_set(), default_priority_class()), *cf, sst_gen, replacer_fn_no_op()).get0();
        BOOST_REQUIRE(ret.new_sstables.size() == 1);

        auto original_apart = make_sstable_containing(sst_gen, {mut1, mut2});
        ret = compact_sstables(sstables::compaction_descriptor({original_apart}, cf->get_sstable_set(), default_priority_class()), *cf, sst_gen, replacer_fn_no_op()).get0();
        BOOST_REQUIRE(ret.new_sstables.size() == 2);
    });
}

SEASTAR_TEST_CASE(autocompaction_control_test) {
    return test_env::do_with_async([] (test_env& env) {
        cell_locker_stats cl_stats;
        cache_tracker tracker;

        compaction_manager cm;
        auto stop_compaction_manager = deferred_stop(cm);
        cm.enable();

        auto s = schema_builder(some_keyspace, some_column_family)
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("value", int32_type)
                .build();

        auto tmp = tmpdir();
        replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
        cfg.datadir = tmp.path().string();
        cfg.enable_commitlog = false;
        cfg.enable_disk_writes = true;

        auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), cm, cl_stats, tracker);
        cf->set_compaction_strategy(sstables::compaction_strategy_type::size_tiered);
        cf->mark_ready_for_writes();

        // no compactions done yet
        auto& ss = cm.get_stats();
        BOOST_REQUIRE(ss.pending_tasks == 0 && ss.active_tasks == 0 && ss.completed_tasks == 0);
        // auto compaction is enabled by default
        BOOST_REQUIRE(!cf->is_auto_compaction_disabled_by_user());
        // disable auto compaction by user
        cf->disable_auto_compaction().get();
        // check it is disabled
        BOOST_REQUIRE(cf->is_auto_compaction_disabled_by_user());

        // generate a few sstables
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
        };
        auto make_insert = [&] (partition_key key) {
            mutation m(s, key);
            m.set_clustered_cell(clustering_key::make_empty(), bytes("value"), data_value(int32_t(1)), 1 /* ts */);
            return m;
        };
        auto min_threshold = cf->schema()->min_compaction_threshold();
        auto tokens = token_generation_for_current_shard(1);
        for (auto i = 0; i < 2 * min_threshold; ++i) {
            auto key = partition_key::from_exploded(*s, {to_bytes(tokens[0].first)});
            auto mut = make_insert(key);
            auto sst = make_sstable_containing(sst_gen, {mut});
            cf->add_sstable_and_update_cache(sst).wait();
        }

        // check compaction manager does not receive background compaction submissions
        cf->start();
        auto stop_cf = deferred_stop(*cf);
        cf->trigger_compaction();
        cf->get_compaction_manager().submit(cf.get());
        BOOST_REQUIRE(ss.pending_tasks == 0 && ss.active_tasks == 0 && ss.completed_tasks == 0);
        // enable auto compaction
        cf->enable_auto_compaction();
        // check enabled
        BOOST_REQUIRE(!cf->is_auto_compaction_disabled_by_user());
        // trigger background compaction
        cf->trigger_compaction();
        // wait until compaction finished
        do_until([&ss] { return ss.pending_tasks == 0 && ss.active_tasks == 0; }, [] {
            return sleep(std::chrono::milliseconds(100));
        }).wait();
        // test compaction successfully finished
        BOOST_REQUIRE(ss.errors == 0);
        BOOST_REQUIRE(ss.completed_tasks == 1);
    });
}

//
// Test that https://github.com/scylladb/scylla/issues/6472 is gone
//
SEASTAR_TEST_CASE(test_bug_6472) {
    return test_setup::do_with_tmp_directory([] (test_env& env, sstring tmpdir_path) {
        auto builder = schema_builder("tests", "test_bug_6472")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("cl", int32_type, column_kind::clustering_key)
                .with_column("value", int32_type);
        builder.set_compaction_strategy(sstables::compaction_strategy_type::time_window);
        std::map<sstring, sstring> opts = {
            { time_window_compaction_strategy_options::COMPACTION_WINDOW_UNIT_KEY, "HOURS" },
            { time_window_compaction_strategy_options::COMPACTION_WINDOW_SIZE_KEY, "1" },
        };
        builder.set_compaction_strategy_options(std::move(opts));
        builder.set_gc_grace_seconds(0);
        auto s = builder.build();

        auto sst_gen = [&env, s, tmpdir_path, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmpdir_path, (*gen)++, sstables::get_highest_sstable_version(), big);
        };

        auto next_timestamp = [] (auto step) {
            using namespace std::chrono;
            return (gc_clock::now().time_since_epoch() - duration_cast<microseconds>(step)).count();
        };

        auto tokens = token_generation_for_shard(1, this_shard_id(), test_db_config.murmur3_partitioner_ignore_msb_bits(), smp::count);

        auto make_expiring_cell = [&] (std::chrono::hours step) {
            static thread_local int32_t value = 1;

            auto key_str = tokens[0].first;
            auto key = partition_key::from_exploded(*s, {to_bytes(key_str)});

            mutation m(s, key);
            auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(value++)});
            m.set_clustered_cell(c_key, bytes("value"), data_value(int32_t(value)), next_timestamp(step), gc_clock::duration(step + 5s));
            return m;
        };

        auto cm = make_lw_shared<compaction_manager>();
        replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
        cfg.datadir = tmpdir_path;
        cfg.enable_disk_writes = true;
        cfg.enable_commitlog = false;
        cfg.enable_cache = false;
        cfg.enable_incremental_backups = false;
        auto tracker = make_lw_shared<cache_tracker>();
        cell_locker_stats cl_stats;
        auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), *cm, cl_stats, *tracker);
        cf->mark_ready_for_writes();
        cf->start();

        // Make 100 expiring cells which belong to different time windows
        std::vector<mutation> muts;
        muts.reserve(101);
        for (auto i = 1; i < 101; i++) {
            muts.push_back(make_expiring_cell(std::chrono::hours(i)));
        }
        muts.push_back(make_expiring_cell(std::chrono::hours(110)));

        //
        // Reproduce issue 6472 by making an input set which causes both interposer and GC writer to be enabled
        //
        std::vector<shared_sstable> sstables_spanning_many_windows = {
            make_sstable_containing(sst_gen, muts),
            make_sstable_containing(sst_gen, muts),
        };
        utils::UUID run_id = utils::make_random_uuid();
        for (auto& sst : sstables_spanning_many_windows) {
            sstables::test(sst).set_run_identifier(run_id);
        }

        // Make sure everything we wanted expired is expired by now.
        forward_jump_clocks(std::chrono::hours(101));

        auto ret = compact_sstables(sstables::compaction_descriptor(sstables_spanning_many_windows,
            cf->get_sstable_set(),default_priority_class()), *cf, sst_gen, replacer_fn_no_op()).get0();
        BOOST_REQUIRE(ret.new_sstables.size() == 1);
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(sstable_needs_cleanup_test) {
  return test_env::do_with([] (test_env& env) {
    auto s = make_shared_schema({}, some_keyspace, some_column_family,
        {{"p1", utf8_type}}, {}, {}, {}, utf8_type);

    auto tokens = token_generation_for_current_shard(10);

    auto sst_gen = [&env, s, gen = make_lw_shared<unsigned>(1)] (sstring first, sstring last) mutable {
        return sstable_for_overlapping_test(env, s, (*gen)++, first, last);
    };
    auto token = [&] (size_t index) -> dht::token {
        return tokens[index].second;
    };
    auto key_from_token = [&] (size_t index) -> sstring {
        return tokens[index].first;
    };
    auto token_range = [&] (size_t first, size_t last) -> dht::token_range {
        return dht::token_range::make(token(first), token(last));
    };

    {
        auto local_ranges = { token_range(0, 9) };
        auto sst = sst_gen(key_from_token(0), key_from_token(9));
        BOOST_REQUIRE(!needs_cleanup(sst, local_ranges, s));
    }

    {
        auto local_ranges = { token_range(0, 1), token_range(3, 4), token_range(5, 6) };

        auto sst = sst_gen(key_from_token(0), key_from_token(1));
        BOOST_REQUIRE(!needs_cleanup(sst, local_ranges, s));

        auto sst2 = sst_gen(key_from_token(2), key_from_token(2));
        BOOST_REQUIRE(needs_cleanup(sst2, local_ranges, s));

        auto sst3 = sst_gen(key_from_token(0), key_from_token(6));
        BOOST_REQUIRE(needs_cleanup(sst3, local_ranges, s));

        auto sst5 = sst_gen(key_from_token(7), key_from_token(7));
        BOOST_REQUIRE(needs_cleanup(sst5, local_ranges, s));
    }

    return make_ready_future<>();
  });
}

SEASTAR_TEST_CASE(test_twcs_partition_estimate) {
    return test_setup::do_with_tmp_directory([] (test_env& env, sstring tmpdir_path) {
        auto builder = schema_builder("tests", "test_bug_6472")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("cl", int32_type, column_kind::clustering_key)
                .with_column("value", int32_type);
        builder.set_compaction_strategy(sstables::compaction_strategy_type::time_window);
        std::map<sstring, sstring> opts = {
            { time_window_compaction_strategy_options::COMPACTION_WINDOW_UNIT_KEY, "HOURS" },
            { time_window_compaction_strategy_options::COMPACTION_WINDOW_SIZE_KEY, "1" },
        };
        builder.set_compaction_strategy_options(std::move(opts));
        builder.set_gc_grace_seconds(0);
        auto s = builder.build();

        const auto rows_per_partition = 200;

        auto sst_gen = [&env, s, tmpdir_path, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmpdir_path, (*gen)++, sstables::get_highest_sstable_version(), big);
        };

        auto next_timestamp = [] (int sstable_idx, int ck_idx) {
            using namespace std::chrono;
            auto window = hours(sstable_idx * rows_per_partition + ck_idx);
            return (gc_clock::now().time_since_epoch() - duration_cast<microseconds>(window)).count();
        };

        auto tokens = token_generation_for_shard(4, this_shard_id(), test_db_config.murmur3_partitioner_ignore_msb_bits(), smp::count);

        auto make_sstable = [&] (int sstable_idx) {
            static thread_local int32_t value = 1;

            auto key_str = tokens[sstable_idx].first;
            auto key = partition_key::from_exploded(*s, {to_bytes(key_str)});

            mutation m(s, key);
            for (auto ck = 0; ck < rows_per_partition; ++ck) {
                auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(value++)});
                m.set_clustered_cell(c_key, bytes("value"), data_value(int32_t(value)), next_timestamp(sstable_idx, ck));
            }
            return make_sstable_containing(sst_gen, {m});
        };

        auto cm = make_lw_shared<compaction_manager>();
        replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
        cfg.datadir = tmpdir_path;
        cfg.enable_disk_writes = true;
        cfg.enable_commitlog = false;
        cfg.enable_cache = false;
        cfg.enable_incremental_backups = false;
        auto tracker = make_lw_shared<cache_tracker>();
        cell_locker_stats cl_stats;
        auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), *cm, cl_stats, *tracker);
        cf->mark_ready_for_writes();
        cf->start();

        std::vector<shared_sstable> sstables_spanning_many_windows = {
            make_sstable(0),
            make_sstable(1),
            make_sstable(2),
            make_sstable(3),
        };

        auto ret = compact_sstables(sstables::compaction_descriptor(sstables_spanning_many_windows,
                    cf->get_sstable_set(), default_priority_class()), *cf, sst_gen, replacer_fn_no_op()).get0();
        // The real test here is that we don't assert() in
        // sstables::prepare_summary() with the compact_sstables() call above,
        // this is only here as a sanity check.
        BOOST_REQUIRE_EQUAL(ret.new_sstables.size(), std::min(sstables_spanning_many_windows.size() * rows_per_partition,
                    sstables::time_window_compaction_strategy::max_data_segregation_window_count));
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(stcs_reshape_test) {
    return test_env::do_with_async([] (test_env& env) {
        simple_schema ss;
        auto s = ss.schema();
        std::vector<shared_sstable> sstables;
        sstables.reserve(s->max_compaction_threshold());
        auto key_and_token_pair = token_generation_for_current_shard(s->max_compaction_threshold() + 2);
        for (auto gen = 1; gen <= s->max_compaction_threshold(); gen++) {
            auto sst = env.make_sstable(s, "", gen);
            sstables::test(sst).set_data_file_size(1);
            sstables::test(sst).set_values(key_and_token_pair[gen - 1].first, key_and_token_pair[gen + 1].first, stats_metadata{});
            sstables.push_back(std::move(sst));
        }

        auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::size_tiered,
                                                    s->compaction_strategy_options());

        BOOST_REQUIRE(cs.get_reshaping_job(sstables, s, default_priority_class(), reshape_mode::strict).sstables.size());
        BOOST_REQUIRE(cs.get_reshaping_job(sstables, s, default_priority_class(), reshape_mode::relaxed).sstables.size());
    });
}

SEASTAR_TEST_CASE(lcs_reshape_test) {
    return test_env::do_with_async([] (test_env& env) {
        simple_schema ss;
        auto s = ss.schema();
        auto keys = token_generation_for_current_shard(256);
        auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::leveled,
                                                     s->compaction_strategy_options());

        // non overlapping
        {
            std::vector <shared_sstable> sstables;
            for (auto i = 0; i < 256; i++) {
                auto sst = env.make_sstable(s, "", i + 1);
                auto key = keys[i].first;
                sstables::test(sst).set_values_for_leveled_strategy(1 /* size */, 0 /* level */, 0 /* max ts */, key, key);
                sstables.push_back(std::move(sst));
            }

            BOOST_REQUIRE(cs.get_reshaping_job(sstables, s, default_priority_class(), reshape_mode::strict).sstables.size() == 256);
        }
        // all overlapping
        {
            std::vector <shared_sstable> sstables;
            for (auto i = 0; i < 256; i++) {
                auto sst = env.make_sstable(s, "", i + 1);
                auto key = keys[0].first;
                sstables::test(sst).set_values_for_leveled_strategy(1 /* size */, 0 /* level */, 0 /* max ts */, key, key);
                sstables.push_back(std::move(sst));
            }

            BOOST_REQUIRE(cs.get_reshaping_job(sstables, s, default_priority_class(), reshape_mode::strict).sstables.size() == uint64_t(s->max_compaction_threshold()));
        }
        // single sstable
        {
            auto sst = env.make_sstable(s, "", 1);
            auto key = keys[0].first;
            sstables::test(sst).set_values_for_leveled_strategy(1 /* size */, 0 /* level */, 0 /* max ts */, key, key);

            BOOST_REQUIRE(cs.get_reshaping_job({ sst }, s, default_priority_class(), reshape_mode::strict).sstables.size() == 0);
        }
    });
}

SEASTAR_TEST_CASE(test_twcs_interposer_on_memtable_flush) {
    return test_env::do_with_async([] (test_env& env) {
      auto test_interposer_on_flush = [&] (bool split_during_flush) {
        auto builder = schema_builder("tests", "test_twcs_interposer_on_flush")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("cl", int32_type, column_kind::clustering_key)
                .with_column("value", int32_type);
        builder.set_compaction_strategy(sstables::compaction_strategy_type::time_window);
        std::map<sstring, sstring> opts = {
            { time_window_compaction_strategy_options::COMPACTION_WINDOW_UNIT_KEY, "HOURS" },
            { time_window_compaction_strategy_options::COMPACTION_WINDOW_SIZE_KEY, "1" },
        };
        builder.set_compaction_strategy_options(std::move(opts));
        auto s = builder.build();

        auto next_timestamp = [] (auto step) {
            using namespace std::chrono;
            return (gc_clock::now().time_since_epoch() - duration_cast<microseconds>(step)).count();
        };
        auto tokens = token_generation_for_shard(1, this_shard_id(), test_db_config.murmur3_partitioner_ignore_msb_bits(), smp::count);

        auto make_row = [&] (std::chrono::hours step) {
            static thread_local int32_t value = 1;
            auto key_str = tokens[0].first;
            auto key = partition_key::from_exploded(*s, {to_bytes(key_str)});

            mutation m(s, key);
            auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(value++)});
            m.set_clustered_cell(c_key, bytes("value"), data_value(int32_t(value)), next_timestamp(step));
            return m;
        };

        auto tmp = tmpdir();
        auto cm = make_lw_shared<compaction_manager>();
        replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
        cfg.datadir = tmp.path().string();
        cfg.enable_disk_writes = true;
        cfg.enable_cache = false;
        auto tracker = make_lw_shared<cache_tracker>();
        cell_locker_stats cl_stats;
        auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), *cm, cl_stats, *tracker);
        cf->mark_ready_for_writes();
        cf->start();

        size_t target_windows_span = (split_during_flush) ? 10 : 1;
        constexpr size_t rows_per_window = 10;

        auto mt = make_lw_shared<memtable>(s);
        for (unsigned i = 1; i <= target_windows_span; i++) {
            for (unsigned j = 0; j < rows_per_window; j++) {
                mt->apply(make_row(std::chrono::hours(i)));
            }
        }

        auto ret = column_family_test(cf).try_flush_memtable_to_sstable(mt).get0();
        BOOST_REQUIRE(ret == stop_iteration::yes);

        auto expected_ssts = (split_during_flush) ? target_windows_span : 1;
        testlog.info("split_during_flush={}, actual={}, expected={}", split_during_flush, cf->get_sstables()->size(), expected_ssts);
        BOOST_REQUIRE(cf->get_sstables()->size() == expected_ssts);
      };

      test_interposer_on_flush(true);
      test_interposer_on_flush(false);
  });
}

SEASTAR_TEST_CASE(test_twcs_compaction_across_buckets) {
    return test_env::do_with_async([] (test_env& env) {
        auto builder = schema_builder("tests", "test_twcs_compaction_across_buckets")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("cl", int32_type, column_kind::clustering_key)
                .with_column("value", int32_type);
        builder.set_compaction_strategy(sstables::compaction_strategy_type::time_window);
        std::map<sstring, sstring> opts = {
                { time_window_compaction_strategy_options::COMPACTION_WINDOW_UNIT_KEY, "HOURS" },
                { time_window_compaction_strategy_options::COMPACTION_WINDOW_SIZE_KEY, "1" },
        };
        builder.set_compaction_strategy_options(std::move(opts));
        auto s = builder.build();

        auto next_timestamp = [] (std::chrono::hours step = std::chrono::hours(0)) {
            return (gc_clock::now().time_since_epoch() - std::chrono::duration_cast<std::chrono::microseconds>(step)).count();
        };
        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)] () mutable {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::get_highest_sstable_version(), big);
        };
        auto tokens = token_generation_for_shard(1, this_shard_id(), test_db_config.murmur3_partitioner_ignore_msb_bits(), smp::count);
        auto pkey = partition_key::from_exploded(*s, {to_bytes(tokens[0].first)});

        auto make_row = [&] (std::chrono::hours step) {
            static thread_local int32_t value = 1;
            mutation m(s, pkey);
            auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(value++)});
            m.set_clustered_cell(c_key, bytes("value"), data_value(int32_t(value)), next_timestamp(step));
            return m;
        };

        column_family_for_tests cf(env.manager(), s);
        auto close_cf = deferred_stop(cf);

        constexpr unsigned windows = 10;

        std::vector<shared_sstable> sstables_spanning_many_windows;
        sstables_spanning_many_windows.reserve(windows + 1);

        for (unsigned w = 0; w < windows; w++) {
            sstables_spanning_many_windows.push_back(make_sstable_containing(sst_gen, {make_row(std::chrono::hours((w + 1) * 2))}));
        }
        auto deletion_mut = [&] () {
            mutation m(s, pkey);
            tombstone tomb(next_timestamp(), gc_clock::now());
            m.partition().apply(tomb);
            return m;
        }();
        sstables_spanning_many_windows.push_back(make_sstable_containing(sst_gen, {deletion_mut}));

        auto ret = compact_sstables(sstables::compaction_descriptor(std::move(sstables_spanning_many_windows),
            std::nullopt, default_priority_class()), *cf, sst_gen, replacer_fn_no_op()).get0();

        BOOST_REQUIRE(ret.new_sstables.size() == 1);
        assert_that(sstable_reader(ret.new_sstables[0], s, env.make_reader_permit()))
            .produces(deletion_mut)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_offstrategy_sstable_compaction) {
    return test_env::do_with_async([tmpdirs = std::vector<decltype(tmpdir())>()] (test_env& env) mutable {
        for (const auto version : writable_sstable_versions) {
            tmpdirs.push_back(tmpdir());
            auto& tmp = tmpdirs.back();
            simple_schema ss;
            auto s = ss.schema();

            auto pk = ss.make_pkey(make_local_key(s));
            auto mut = mutation(s, pk);
            ss.add_row(mut, ss.make_ckey(0), "val");

            auto cm = make_lw_shared<compaction_manager>();
            replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
            cfg.datadir = tmp.path().string();
            cfg.enable_disk_writes = true;
            cfg.enable_cache = false;
            auto tracker = make_lw_shared<cache_tracker>();
            cell_locker_stats cl_stats;
            auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), *cm, cl_stats, *tracker);
            auto sst_gen = [&env, s, cf, path = tmp.path().string(), version] () mutable {
                return env.make_sstable(s, path, column_family_test::calculate_generation_for_new_table(*cf), version, big);
            };

            cf->mark_ready_for_writes();
            cf->start();

            for (auto i = 0; i < cf->schema()->max_compaction_threshold(); i++) {
                auto sst = make_sstable_containing(sst_gen, {mut});
                cf->add_sstable_and_update_cache(std::move(sst), sstables::offstrategy::yes).get();
            }
            auto info = make_lw_shared<sstables::compaction_data>();
            cf->run_offstrategy_compaction(*info).get();

            // Make sure we release reference to all sstables, allowing them to be deleted before dir is destroyed
            cf->stop().get();
        }
    });
}

SEASTAR_TEST_CASE(twcs_reshape_with_disjoint_set_test) {
    static constexpr unsigned disjoint_sstable_count = 256;

    return test_env::do_with_async([] (test_env& env) {
        auto builder = schema_builder("tests", "twcs_reshape_test")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("cl", ::timestamp_type, column_kind::clustering_key)
                .with_column("value", int32_type);
        builder.set_compaction_strategy(sstables::compaction_strategy_type::time_window);
        std::map <sstring, sstring> opts = {
                {time_window_compaction_strategy_options::COMPACTION_WINDOW_UNIT_KEY, "HOURS"},
                {time_window_compaction_strategy_options::COMPACTION_WINDOW_SIZE_KEY, "8"},
        };
        builder.set_compaction_strategy_options(std::move(opts));
        auto s = builder.build();
        auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::time_window, s->compaction_strategy_options());

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(1, 3600*24);

        using namespace std::chrono;

        // Make it easier to reproduce timing-based issues by running this test multiple times.
        auto offset_duration = duration_cast<microseconds>(minutes(distrib(gen)));

        auto now = gc_clock::now().time_since_epoch() + offset_duration;
        // The twcs is configured with 8-hours time window. If the starting time
        // is not aligned with that then some buckets may get less than this
        // number of sstables in and potentially hit the minimal threshold of
        // 4 sstables. Align the starting time not to make this happen.
        auto now_in_minutes = duration_cast<minutes>(now);
        constexpr auto window_size_in_minutes = 8 * 60;
        forward_jump_clocks(minutes(window_size_in_minutes - now_in_minutes.count() % window_size_in_minutes));
        now = gc_clock::now().time_since_epoch() + offset_duration;
        assert(std::chrono::duration_cast<minutes>(now).count() % window_size_in_minutes == 0);

        auto next_timestamp = [now](auto step) {
            return (now + duration_cast<seconds>(step)).count();
        };

        auto tokens = token_generation_for_shard(disjoint_sstable_count, this_shard_id(), test_db_config.murmur3_partitioner_ignore_msb_bits(), smp::count);

        auto make_row = [&](unsigned token_idx, std::chrono::hours step) {
            static thread_local int32_t value = 1;
            auto key_str = tokens[token_idx].first;
            auto key = partition_key::from_exploded(*s, {to_bytes(key_str)});

            mutation m(s, key);
            auto next_ts = next_timestamp(step);
            auto c_key = clustering_key::from_exploded(*s, {::timestamp_type->decompose(next_ts)});
            m.set_clustered_cell(c_key, bytes("value"), data_value(int32_t(value++)), next_ts);
            return m;
        };

        auto tmp = tmpdir();

        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)]() {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::sstable::version_types::md, big);
        };

        {
            // create set of 256 disjoint ssts that belong to the same time window and expect that twcs reshape allows them all to be compacted at once

            std::vector<sstables::shared_sstable> sstables;
            sstables.reserve(disjoint_sstable_count);
            for (unsigned i = 0; i < disjoint_sstable_count; i++) {
                auto sst = make_sstable_containing(sst_gen, {make_row(i, std::chrono::hours(1))});
                sstables.push_back(std::move(sst));
            }

            BOOST_REQUIRE_EQUAL(cs.get_reshaping_job(sstables, s, default_priority_class(), reshape_mode::strict).sstables.size(), disjoint_sstable_count);
        }

        {
            // create set of 256 disjoint ssts that belong to different windows and expect that twcs reshape allows them all to be compacted at once

            std::vector<sstables::shared_sstable> sstables;
            sstables.reserve(disjoint_sstable_count);
            for (auto i = 0; i < disjoint_sstable_count; i++) {
                auto sst = make_sstable_containing(sst_gen, {make_row(i, std::chrono::hours(i))});
                sstables.push_back(std::move(sst));
            }

            BOOST_REQUIRE_EQUAL(cs.get_reshaping_job(sstables, s, default_priority_class(), reshape_mode::strict).sstables.size(), disjoint_sstable_count);
        }

        {
            // create set of 256 disjoint ssts that belong to different windows with none over the threshold and expect that twcs reshape selects none of them

            std::vector<sstables::shared_sstable> sstables;
            sstables.reserve(disjoint_sstable_count);
            for (auto i = 0; i < disjoint_sstable_count; i++) {
                auto sst = make_sstable_containing(sst_gen, {make_row(i, std::chrono::hours(24*i + 1)), make_row(i, std::chrono::hours(24*i + 2))});
                sstables.push_back(std::move(sst));
            }

            BOOST_REQUIRE_EQUAL(cs.get_reshaping_job(sstables, s, default_priority_class(), reshape_mode::strict).sstables.size(), 0);
        }

        {
            // create set of 256 overlapping ssts that belong to the same time window and expect that twcs reshape allows only 32 to be compacted at once

            std::vector<sstables::shared_sstable> sstables;
            sstables.reserve(disjoint_sstable_count);
            for (unsigned i = 0; i < disjoint_sstable_count; i++) {
                auto sst = make_sstable_containing(sst_gen, {make_row(0, std::chrono::hours(1))});
                sstables.push_back(std::move(sst));
            }

            BOOST_REQUIRE_EQUAL(cs.get_reshaping_job(sstables, s, default_priority_class(), reshape_mode::strict).sstables.size(), uint64_t(s->max_compaction_threshold()));
        }
    });
}


SEASTAR_TEST_CASE(stcs_reshape_overlapping_test) {
    static constexpr unsigned disjoint_sstable_count = 256;

    return test_env::do_with_async([] (test_env& env) {
        auto builder = schema_builder("tests", "stcs_reshape_test")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("cl", ::timestamp_type, column_kind::clustering_key)
                .with_column("value", int32_type);
        builder.set_compaction_strategy(sstables::compaction_strategy_type::size_tiered);
        auto s = builder.build();
        std::map<sstring, sstring> opts;
        auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::size_tiered, std::move(opts));

        auto tokens = token_generation_for_shard(disjoint_sstable_count, this_shard_id(), test_db_config.murmur3_partitioner_ignore_msb_bits(), smp::count);

        auto make_row = [&](unsigned token_idx) {
            auto key_str = tokens[token_idx].first;
            auto key = partition_key::from_exploded(*s, {to_bytes(key_str)});

            mutation m(s, key);
            auto value = 1;
            auto next_ts = 1;
            auto c_key = clustering_key::from_exploded(*s, {::timestamp_type->decompose(next_ts)});
            m.set_clustered_cell(c_key, bytes("value"), data_value(int32_t(value)), next_ts);
            return m;
        };

        auto tmp = tmpdir();

        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)]() {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::sstable::version_types::md, big);
        };

        {
            // create set of 256 disjoint ssts and expect that stcs reshape allows them all to be compacted at once

            std::vector<sstables::shared_sstable> sstables;
            sstables.reserve(disjoint_sstable_count);
            for (unsigned i = 0; i < disjoint_sstable_count; i++) {
                auto sst = make_sstable_containing(sst_gen, {make_row(i)});
                sstables.push_back(std::move(sst));
            }

            BOOST_REQUIRE(cs.get_reshaping_job(sstables, s, default_priority_class(), reshape_mode::strict).sstables.size() == disjoint_sstable_count);
        }

        {
            // create set of 256 overlapping ssts and expect that stcs reshape allows only 32 to be compacted at once

            std::vector<sstables::shared_sstable> sstables;
            sstables.reserve(disjoint_sstable_count);
            for (unsigned i = 0; i < disjoint_sstable_count; i++) {
                auto sst = make_sstable_containing(sst_gen, {make_row(0)});
                sstables.push_back(std::move(sst));
            }

            BOOST_REQUIRE(cs.get_reshaping_job(sstables, s, default_priority_class(), reshape_mode::strict).sstables.size() == uint64_t(s->max_compaction_threshold()));
        }
    });
}

// Regression test for #8432
SEASTAR_TEST_CASE(test_twcs_single_key_reader_filtering) {
    return test_env::do_with_async([] (test_env& env) {
        auto builder = schema_builder("tests", "twcs_single_key_reader_filtering")
                .with_column("pk", int32_type, column_kind::partition_key)
                .with_column("ck", int32_type, column_kind::clustering_key)
                .with_column("v", int32_type);
        builder.set_compaction_strategy(sstables::compaction_strategy_type::time_window);
        auto s = builder.build();

        auto tmp = tmpdir();
        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)]() {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::sstable::version_types::md, big);
        };

        auto make_row = [&] (int32_t pk, int32_t ck) {
            mutation m(s, partition_key::from_single_value(*s, int32_type->decompose(pk)));
            m.set_clustered_cell(clustering_key::from_single_value(*s, int32_type->decompose(ck)), to_bytes("v"), int32_t(0), api::new_timestamp());
            return m;
        };

        auto sst1 = make_sstable_containing(sst_gen, {make_row(0, 0)});
        auto sst2 = make_sstable_containing(sst_gen, {make_row(0, 1)});
        auto dkey = sst1->get_first_decorated_key();

        auto cm = make_lw_shared<compaction_manager>();
        replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
        replica::cf_stats cf_stats{0};
        cfg.cf_stats = &cf_stats;
        cfg.datadir = tmp.path().string();
        auto tracker = make_lw_shared<cache_tracker>();
        cell_locker_stats cl_stats;
        replica::column_family cf(s, cfg, replica::column_family::no_commitlog(), *cm, cl_stats, *tracker);
        cf.mark_ready_for_writes();
        cf.start();

        auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::time_window, {});

        auto set = cs.make_sstable_set(s);
        set.insert(std::move(sst1));
        set.insert(std::move(sst2));

        reader_permit permit = env.make_reader_permit();
        utils::estimated_histogram eh;
        auto pr = dht::partition_range::make_singular(dkey);

        auto slice = partition_slice_builder(*s)
                    .with_range(query::clustering_range {
                        query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(0)) },
                        query::clustering_range::bound { clustering_key_prefix::from_single_value(*s, int32_type->decompose(1)) },
                    }).build();

        auto reader = set.create_single_key_sstable_reader(
                &cf, s, permit, eh, pr, slice, default_priority_class(),
                tracing::trace_state_ptr(), ::streamed_mutation::forwarding::no,
                ::mutation_reader::forwarding::no);
        auto close_reader = deferred_close(reader);

        auto checked_by_ck = cf_stats.sstables_checked_by_clustering_filter;
        auto surviving_after_ck = cf_stats.surviving_sstables_after_clustering_filter;

        // consume all fragments
        while (reader().get());

        // At least sst2 should be checked by the CK filter during fragment consumption and should pass.
        // With the bug in #8432, sst2 wouldn't even be checked by the CK filter since it would pass right after checking the PK filter.
        BOOST_REQUIRE_GE(cf_stats.sstables_checked_by_clustering_filter - checked_by_ck, 1);
        BOOST_REQUIRE_EQUAL(
                cf_stats.surviving_sstables_after_clustering_filter - surviving_after_ck,
                cf_stats.sstables_checked_by_clustering_filter - checked_by_ck);
    });
}

SEASTAR_TEST_CASE(max_ongoing_compaction_test) {
    return test_env::do_with_async([] (test_env& env) {
        BOOST_REQUIRE(smp::count == 1);

        auto make_schema = [] (auto idx) {
            auto builder = schema_builder("tests", std::to_string(idx))
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("cl", int32_type, column_kind::clustering_key)
                .with_column("value", int32_type);
            builder.set_compaction_strategy(sstables::compaction_strategy_type::time_window);
            std::map <sstring, sstring> opts = {
                {time_window_compaction_strategy_options::COMPACTION_WINDOW_UNIT_KEY,                  "HOURS"},
                {time_window_compaction_strategy_options::COMPACTION_WINDOW_SIZE_KEY,                  "1"},
                {time_window_compaction_strategy_options::EXPIRED_SSTABLE_CHECK_FREQUENCY_SECONDS_KEY, "0"},
            };
            builder.set_compaction_strategy_options(std::move(opts));
            builder.set_gc_grace_seconds(0);
            return builder.build();
        };

        auto cm = make_lw_shared<compaction_manager>();
        cm->enable();
        auto stop_cm = defer([&cm] {
            cm->stop().get();
        });

        auto tmp = tmpdir();
        auto cl_stats = make_lw_shared<cell_locker_stats>();
        auto tracker = make_lw_shared<cache_tracker>();
        auto tokens = token_generation_for_shard(1, this_shard_id(), test_db_config.murmur3_partitioner_ignore_msb_bits(), smp::count);

        auto next_timestamp = [] (auto step) {
            using namespace std::chrono;
            return (gc_clock::now().time_since_epoch() - duration_cast<microseconds>(step)).count();
        };
        auto make_expiring_cell = [&] (schema_ptr s, std::chrono::hours step) {
            static thread_local int32_t value = 1;

            auto key_str = tokens[0].first;
            auto key = partition_key::from_exploded(*s, {to_bytes(key_str)});

            mutation m(s, key);
            auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(value++)});
            m.set_clustered_cell(c_key, bytes("value"), data_value(int32_t(value)), next_timestamp(step), gc_clock::duration(step + 5s));
            return m;
        };

        auto make_table_with_single_fully_expired_sstable = [&] (auto idx) {
            auto s = make_schema(idx);
            replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
            cfg.datadir = tmp.path().string() + "/" + std::to_string(idx);
            touch_directory(cfg.datadir).get();
            cfg.enable_commitlog = false;
            cfg.enable_incremental_backups = false;

            auto sst_gen = [&env, s, dir = cfg.datadir, gen = make_lw_shared<unsigned>(1)] () mutable {
                return env.make_sstable(s, dir, (*gen)++, sstables::sstable::version_types::md, big);
            };

            auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), *cm, *cl_stats, *tracker);
            cf->start();
            cf->mark_ready_for_writes();

            auto muts = { make_expiring_cell(s, std::chrono::hours(1)) };
            auto sst = make_sstable_containing(sst_gen, muts);
            column_family_test(cf).add_sstable(sst);
            return cf;
        };

        std::vector<lw_shared_ptr<replica::column_family>> tables;
        auto stop_tables = defer([&tables] {
            for (auto& t : tables) {
                t->stop().get();
            }
        });
        for (auto i = 0; i < 100; i++) {
            tables.push_back(make_table_with_single_fully_expired_sstable(i));
        }

        // Make sure everything is expired
        forward_jump_clocks(std::chrono::hours(100));

        for (auto& t : tables) {
            BOOST_REQUIRE(t->sstables_count() == 1);
            t->trigger_compaction();
        }

        BOOST_REQUIRE(cm->get_stats().pending_tasks >= 1 || cm->get_stats().active_tasks >= 1);

        size_t max_ongoing_compaction = 0;

        // wait for submitted jobs to finish.
        auto end = [cm, &tables] {
            return cm->get_stats().pending_tasks == 0 && cm->get_stats().active_tasks == 0
                && boost::algorithm::all_of(tables, [] (auto& t) { return t->sstables_count() == 0; });
        };
        while (!end()) {
            if (!cm->get_stats().pending_tasks && !cm->get_stats().active_tasks) {
                for (auto& t : tables) {
                    if (t->sstables_count()) {
                        t->trigger_compaction();
                    }
                }
            }
            max_ongoing_compaction = std::max(cm->get_stats().active_tasks, max_ongoing_compaction);
            later().get();
        }
        BOOST_REQUIRE(cm->get_stats().errors == 0);
        BOOST_REQUIRE(max_ongoing_compaction == 1);
    });
}

SEASTAR_TEST_CASE(compound_sstable_set_incremental_selector_test) {
    return test_env::do_with([] (test_env& env) {
        auto s = make_shared_schema({}, some_keyspace, some_column_family,
                                    {{"p1", utf8_type}}, {}, {}, {}, utf8_type);
        auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::leveled, s->compaction_strategy_options());
        auto key_and_token_pair = token_generation_for_current_shard(8);
        auto decorated_keys = boost::copy_range<std::vector<dht::decorated_key>>(
                key_and_token_pair | boost::adaptors::transformed([&s] (const std::pair<sstring, dht::token>& key_and_token) {
                    auto value = bytes(reinterpret_cast<const signed char*>(key_and_token.first.data()), key_and_token.first.size());
                    auto pk = sstables::key::from_bytes(value).to_partition_key(*s);
                    return dht::decorate_key(*s, std::move(pk));
                }));

        auto check = [] (sstable_set::incremental_selector& selector, const dht::decorated_key& key, std::unordered_set<int64_t> expected_gens) {
            auto sstables = selector.select(key).sstables;
            BOOST_REQUIRE_EQUAL(sstables.size(), expected_gens.size());
            for (auto& sst : sstables) {
                BOOST_REQUIRE(expected_gens.contains(sst->generation()));
            }
        };

        {
            auto set1 = make_lw_shared<sstable_set>(cs.make_sstable_set(s));
            auto set2 = make_lw_shared<sstable_set>(cs.make_sstable_set(s));
            set1->insert(sstable_for_overlapping_test(env, s, 1, key_and_token_pair[0].first, key_and_token_pair[1].first, 1));
            set2->insert(sstable_for_overlapping_test(env, s, 2, key_and_token_pair[0].first, key_and_token_pair[1].first, 1));
            set1->insert(sstable_for_overlapping_test(env, s, 3, key_and_token_pair[3].first, key_and_token_pair[4].first, 1));
            set2->insert(sstable_for_overlapping_test(env, s, 4, key_and_token_pair[4].first, key_and_token_pair[4].first, 1));
            set1->insert(sstable_for_overlapping_test(env, s, 5, key_and_token_pair[4].first, key_and_token_pair[5].first, 1));

            sstable_set compound = sstables::make_compound_sstable_set(s, { set1, set2 });
            sstable_set::incremental_selector sel = compound.make_incremental_selector();
            check(sel, decorated_keys[0], {1, 2});
            check(sel, decorated_keys[1], {1, 2});
            check(sel, decorated_keys[2], {});
            check(sel, decorated_keys[3], {3});
            check(sel, decorated_keys[4], {3, 4, 5});
            check(sel, decorated_keys[5], {5});
            check(sel, decorated_keys[6], {});
            check(sel, decorated_keys[7], {});
        }

        {
            auto set1 = make_lw_shared<sstable_set>(cs.make_sstable_set(s));
            auto set2 = make_lw_shared<sstable_set>(cs.make_sstable_set(s));
            set1->insert(sstable_for_overlapping_test(env, s, 0, key_and_token_pair[0].first, key_and_token_pair[1].first, 0));
            set2->insert(sstable_for_overlapping_test(env, s, 1, key_and_token_pair[0].first, key_and_token_pair[1].first, 1));
            set1->insert(sstable_for_overlapping_test(env, s, 2, key_and_token_pair[0].first, key_and_token_pair[1].first, 1));
            set2->insert(sstable_for_overlapping_test(env, s, 3, key_and_token_pair[3].first, key_and_token_pair[4].first, 1));
            set1->insert(sstable_for_overlapping_test(env, s, 4, key_and_token_pair[4].first, key_and_token_pair[4].first, 1));
            set2->insert(sstable_for_overlapping_test(env, s, 5, key_and_token_pair[4].first, key_and_token_pair[5].first, 1));

            sstable_set compound = sstables::make_compound_sstable_set(s, { set1, set2 });
            sstable_set::incremental_selector sel = compound.make_incremental_selector();
            check(sel, decorated_keys[0], {0, 1, 2});
            check(sel, decorated_keys[1], {0, 1, 2});
            check(sel, decorated_keys[2], {0});
            check(sel, decorated_keys[3], {0, 3});
            check(sel, decorated_keys[4], {0, 3, 4, 5});
            check(sel, decorated_keys[5], {0, 5});
            check(sel, decorated_keys[6], {0});
            check(sel, decorated_keys[7], {0});
        }

        {
            // reproduces use-after-free failure in incremental reader selector with compound set where the next position
            // returned by a set can be used after freed as selector position in another set, producing incorrect results.

            enum class strategy_param : bool {
                ICS = false,
                LCS = true,
            };

            auto incremental_selection_test = [&] (strategy_param param) {
                auto set1 = make_lw_shared<sstable_set>(sstables::make_partitioned_sstable_set(s, make_lw_shared<sstable_list>(), false));
                auto set2 = make_lw_shared<sstable_set>(sstables::make_partitioned_sstable_set(s, make_lw_shared<sstable_list>(), bool(param)));
                set1->insert(sstable_for_overlapping_test(env, s, 0, key_and_token_pair[1].first, key_and_token_pair[1].first, 1));
                set2->insert(sstable_for_overlapping_test(env, s, 1, key_and_token_pair[0].first, key_and_token_pair[2].first, 1));
                set2->insert(sstable_for_overlapping_test(env, s, 2, key_and_token_pair[3].first, key_and_token_pair[3].first, 1));
                set2->insert(sstable_for_overlapping_test(env, s, 3, key_and_token_pair[4].first, key_and_token_pair[4].first, 1));

                sstable_set compound = sstables::make_compound_sstable_set(s, { set1, set2 });
                sstable_set::incremental_selector sel = compound.make_incremental_selector();

                dht::ring_position_view pos = dht::ring_position_view::min();
                std::unordered_set<sstables::shared_sstable> sstables;
                do {
                    auto ret = sel.select(pos);
                    pos = ret.next_position;
                    sstables.insert(ret.sstables.begin(), ret.sstables.end());
                } while (!pos.is_max());

                BOOST_REQUIRE(sstables.size() == 4);
            };

            incremental_selection_test(strategy_param::ICS);
            incremental_selection_test(strategy_param::LCS);
        }

        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(twcs_single_key_reader_through_compound_set_test) {
    return test_env::do_with_async([] (test_env& env) {
        auto builder = schema_builder("tests", "single_key_reader_through_compound_set_test")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("cl", ::timestamp_type, column_kind::clustering_key)
                .with_column("value", int32_type);
        builder.set_compaction_strategy(sstables::compaction_strategy_type::time_window);
        std::map <sstring, sstring> opts = {
                {time_window_compaction_strategy_options::COMPACTION_WINDOW_UNIT_KEY, "HOURS"},
                {time_window_compaction_strategy_options::COMPACTION_WINDOW_SIZE_KEY, "1"},
        };
        builder.set_compaction_strategy_options(std::move(opts));
        auto s = builder.build();
        auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::time_window, std::move(opts));

        auto next_timestamp = [](auto step) {
            using namespace std::chrono;
            return (gc_clock::now().time_since_epoch() + duration_cast<microseconds>(step)).count();
        };
        auto tokens = token_generation_for_shard(1, this_shard_id(), test_db_config.murmur3_partitioner_ignore_msb_bits(), smp::count);

        auto make_row = [&](std::chrono::hours step) {
            static thread_local int32_t value = 1;
            auto key_str = tokens[0].first;
            auto key = partition_key::from_exploded(*s, {to_bytes(key_str)});

            mutation m(s, key);
            auto next_ts = next_timestamp(step);
            auto c_key = clustering_key::from_exploded(*s, {::timestamp_type->decompose(next_ts)});
            m.set_clustered_cell(c_key, bytes("value"), data_value(int32_t(value++)), next_ts);
            return m;
        };

        auto tmp = tmpdir();
        auto cm = make_lw_shared<compaction_manager>();
        replica::column_family::config cfg = column_family_test_config(env.manager(), env.semaphore());
        replica::cf_stats cf_stats{0};
        cfg.cf_stats = &cf_stats;
        cfg.datadir = tmp.path().string();
        cfg.enable_disk_writes = true;
        cfg.enable_cache = false;
        auto tracker = make_lw_shared<cache_tracker>();
        cell_locker_stats cl_stats;
        auto cf = make_lw_shared<replica::column_family>(s, cfg, replica::column_family::no_commitlog(), *cm, cl_stats, *tracker);
        cf->mark_ready_for_writes();
        cf->start();

        auto set1 = make_lw_shared<sstable_set>(cs.make_sstable_set(s));
        auto set2 = make_lw_shared<sstable_set>(cs.make_sstable_set(s));

        auto sst_gen = [&env, s, &tmp, gen = make_lw_shared<unsigned>(1)]() {
            return env.make_sstable(s, tmp.path().string(), (*gen)++, sstables::sstable::version_types::md, big);
        };

        // sstables with same key but belonging to different windows
        auto sst1 = make_sstable_containing(sst_gen, {make_row(std::chrono::hours(1))});
        auto sst2 = make_sstable_containing(sst_gen, {make_row(std::chrono::hours(5))});
        BOOST_REQUIRE(sst1->get_first_decorated_key().token() == sst2->get_last_decorated_key().token());
        auto dkey = sst1->get_first_decorated_key();

        set1->insert(std::move(sst1));
        set2->insert(std::move(sst2));
        sstable_set compound = sstables::make_compound_sstable_set(s, {set1, set2});

        reader_permit permit = env.make_reader_permit();
        utils::estimated_histogram eh;
        auto pr = dht::partition_range::make_singular(dkey);

        auto reader = compound.create_single_key_sstable_reader(&*cf, s, permit, eh, pr, s->full_slice(), default_priority_class(),
                                                                tracing::trace_state_ptr(), ::streamed_mutation::forwarding::no,
                                                                ::mutation_reader::forwarding::no);
        auto close_reader = deferred_close(reader);
        auto mfopt = read_mutation_from_flat_mutation_reader(reader).get0();
        BOOST_REQUIRE(mfopt);
        mfopt = read_mutation_from_flat_mutation_reader(reader).get0();
        BOOST_REQUIRE(!mfopt);
        BOOST_REQUIRE(cf_stats.clustering_filter_count > 0);
    });
}

SEASTAR_TEST_CASE(test_major_does_not_miss_data_in_memtable) {
    return test_env::do_with_async([] (test_env& env) {
        auto builder = schema_builder("tests", "test_major_does_not_miss_data_in_memtable")
                .with_column("id", utf8_type, column_kind::partition_key)
                .with_column("cl", int32_type, column_kind::clustering_key)
                .with_column("value", int32_type);
        auto s = builder.build();

        auto tmp = tmpdir();
        auto tokens = token_generation_for_shard(1, this_shard_id(), test_db_config.murmur3_partitioner_ignore_msb_bits(), smp::count);
        auto pkey = partition_key::from_exploded(*s, {to_bytes(tokens[0].first)});

        column_family_for_tests cf(env.manager(), s, tmp.path().string());
        auto close_cf = deferred_stop(cf);
        auto sst_gen = [&env, &cf, s, &tmp] () mutable {
            return env.make_sstable(s, tmp.path().string(), column_family_test::calculate_generation_for_new_table(*cf),
                sstables::get_highest_sstable_version(), big);
        };

        auto row_mut = [&] () {
            static thread_local int32_t value = 1;
            mutation m(s, pkey);
            auto c_key = clustering_key::from_exploded(*s, {int32_type->decompose(value++)});
            m.set_clustered_cell(c_key, bytes("value"), data_value(int32_t(value)), gc_clock::now().time_since_epoch().count());
            return m;
        }();
        auto sst = make_sstable_containing(sst_gen, {std::move(row_mut)});
        cf->add_sstable_and_update_cache(sst).get();
        BOOST_REQUIRE(cf->get_sstables()->size() == 1);

        auto deletion_mut = [&] () {
            mutation m(s, pkey);
            tombstone tomb(gc_clock::now().time_since_epoch().count(), gc_clock::now());
            m.partition().apply(tomb);
            return m;
        }();
        cf->apply(deletion_mut);

        cf->compact_all_sstables().get();
        BOOST_REQUIRE(cf->get_sstables()->size() == 1);
        auto new_sst = *(cf->get_sstables()->begin());
        BOOST_REQUIRE(new_sst->generation() != sst->generation());
        assert_that(sstable_reader(new_sst, s, env.make_reader_permit()))
                .produces(deletion_mut)
                .produces_end_of_stream();
    });
}
