/*
 * Copyright (C) 2017-present ScyllaDB
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

#include "test/lib/sstable_utils.hh"

#include "replica/database.hh"
#include "memtable-sstable.hh"
#include "dht/i_partitioner.hh"
#include "dht/murmur3_partitioner.hh"
#include <boost/range/irange.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/range/algorithm/sort.hpp>
#include "sstables/version.hh"
#include "test/lib/flat_mutation_reader_assertions.hh"
#include "test/lib/reader_concurrency_semaphore.hh"
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/coroutine.hh>

using namespace sstables;
using namespace std::chrono_literals;

std::vector<sstring> do_make_keys(unsigned n, const schema_ptr& s, size_t min_key_size, std::optional<shard_id> shard) {
    std::vector<std::pair<sstring, dht::decorated_key>> p;
    p.reserve(n);

    auto key_id = 0U;
    auto generated = 0U;
    while (generated < n) {
        auto raw_key = sstring(std::max(min_key_size, sizeof(key_id)), int8_t(0));
        std::copy_n(reinterpret_cast<int8_t*>(&key_id), sizeof(key_id), raw_key.begin());
        auto dk = dht::decorate_key(*s, partition_key::from_single_value(*s, to_bytes(raw_key)));
        key_id++;
        if (shard) {
            if (*shard != shard_of(*s, dk.token())) {
                continue;
            }
        }
        generated++;
        p.emplace_back(std::move(raw_key), std::move(dk));
    }
    boost::sort(p, [&] (auto& p1, auto& p2) {
        return p1.second.less_compare(*s, p2.second);
    });
    return boost::copy_range<std::vector<sstring>>(p | boost::adaptors::map_keys);
}

std::vector<sstring> do_make_keys(unsigned n, const schema_ptr& s, size_t min_key_size, local_shard_only lso) {
    return do_make_keys(n, s, min_key_size, lso ? std::optional(this_shard_id()) : std::nullopt);
}

sstables::shared_sstable make_sstable_containing(std::function<sstables::shared_sstable()> sst_factory, std::vector<mutation> muts) {
    tests::reader_concurrency_semaphore_wrapper semaphore;

    auto sst = sst_factory();
    schema_ptr s = muts[0].schema();
    auto mt = make_lw_shared<memtable>(s);

    std::size_t i{0};
    for (auto&& m : muts) {
        mt->apply(m);
        ++i;

        // Give the reactor some time to breathe
        if(i == 10) {
            seastar::thread::yield();
            i = 0;
        }
    }
    write_memtable_to_sstable_for_test(*mt, sst).get();
    sst->open_data().get();

    std::set<mutation, mutation_decorated_key_less_comparator> merged;
    for (auto&& m : muts) {
        auto result = merged.insert(m);
        if (!result.second) {
            auto old = *result.first;
            merged.erase(result.first);
            merged.insert(old + m);
        }
    }

    // validate the sstable
    auto rd = assert_that(sst->as_mutation_source().make_reader(s, semaphore.make_permit()));
    for (auto&& m : merged) {
        rd.produces(m);
    }
    rd.produces_end_of_stream();

    return sst;
}

shared_sstable make_sstable(sstables::test_env& env, schema_ptr s, sstring dir, std::vector<mutation> mutations,
        sstable_writer_config cfg, sstables::sstable::version_types version, gc_clock::time_point query_time) {
    auto mt = make_lw_shared<memtable>(s);
    fs::path dir_path(dir);

    for (auto&& m : mutations) {
        mt->apply(m);
    }

    return make_sstable_easy(env, dir_path, mt, cfg, 1, version, mutations.size(), query_time);
}

shared_sstable make_sstable_easy(test_env& env, const fs::path& path, flat_mutation_reader rd, sstable_writer_config cfg,
        int64_t generation, const sstables::sstable::version_types version, int expected_partition) {
    auto s = rd.schema();
    auto sst = env.make_sstable(s, path.string(), generation, version, sstable_format_types::big);
    sst->write_components(std::move(rd), expected_partition, s, cfg, encoding_stats{}).get();
    sst->load().get();
    return sst;
}

shared_sstable make_sstable_easy(test_env& env, const fs::path& path, lw_shared_ptr<memtable> mt, sstable_writer_config cfg,
        unsigned long gen, const sstable::version_types v, int estimated_partitions, gc_clock::time_point query_time) {
    schema_ptr s = mt->schema();
    auto sst = env.make_sstable(s, path.string(), gen, v, sstable_format_types::big, default_sstable_buffer_size, query_time);
    auto mr = mt->make_flat_reader(s, env.make_reader_permit());
    sst->write_components(std::move(mr), estimated_partitions, s, cfg, mt->get_encoding_stats()).get();
    sst->load().get();
    return sst;
}

std::vector<std::pair<sstring, dht::token>>
token_generation_for_shard(unsigned tokens_to_generate, unsigned shard,
        unsigned ignore_msb, unsigned smp_count) {
    unsigned tokens = 0;
    unsigned key_id = 0;
    std::vector<std::pair<sstring, dht::token>> key_and_token_pair;

    key_and_token_pair.reserve(tokens_to_generate);
    dht::murmur3_partitioner partitioner;
    dht::sharder sharder(smp_count, ignore_msb);

    while (tokens < tokens_to_generate) {
        sstring key = to_sstring(key_id++);
        dht::token token = create_token_from_key(partitioner, key);
        if (shard != sharder.shard_of(token)) {
            continue;
        }
        tokens++;
        key_and_token_pair.emplace_back(key, token);
    }
    assert(key_and_token_pair.size() == tokens_to_generate);

    std::sort(key_and_token_pair.begin(),key_and_token_pair.end(), [] (auto& i, auto& j) {
        return i.second < j.second;
    });

    return key_and_token_pair;
}

future<compaction_result> compact_sstables(sstables::compaction_descriptor descriptor, replica::column_family& cf, std::function<shared_sstable()> creator, compaction_sstable_replacer_fn replacer) {
    descriptor.creator = [creator = std::move(creator)] (shard_id dummy) mutable {
        return creator();
    };
    descriptor.replacer = std::move(replacer);
    auto& cm = cf.get_compaction_manager();
    auto& cdata = compaction_manager_test(cm).register_compaction(descriptor.run_identifier, &cf);
    return sstables::compact_sstables(std::move(descriptor), cdata, cf.as_table_state()).then([&cdata, &cm] (sstables::compaction_result res) {
        return res;
    }).finally([&cm, &cdata] {
        compaction_manager_test(cm).deregister_compaction(cdata);
    });
}

std::vector<std::pair<sstring, dht::token>> token_generation_for_current_shard(unsigned tokens_to_generate) {
    return token_generation_for_shard(tokens_to_generate, this_shard_id());
}

static sstring toc_filename(const sstring& dir, schema_ptr schema, unsigned int generation, sstable_version_types v) {
    return sstable::filename(dir, schema->ks_name(), schema->cf_name(), v, generation,
                             sstable_format_types::big, component_type::TOC);
}

future<shared_sstable> test_env::reusable_sst(schema_ptr schema, sstring dir, unsigned long generation) {
    for (auto v : boost::adaptors::reverse(all_sstable_versions)) {
        if (co_await file_exists(toc_filename(dir, schema, generation, v))) {
            co_return co_await reusable_sst(schema, dir, generation, v);
        }
    }
    throw sst_not_found(dir, generation);
}

sstables::compaction_data& compaction_manager_test::register_compaction(utils::UUID output_run_id, replica::column_family* cf) {
    auto task = make_lw_shared<compaction_manager::task>(cf, sstables::compaction_type::Compaction, _cm._compaction_state[cf]);
    testlog.debug("compaction_manager_test: register_compaction: task {} cf={}", fmt::ptr(task.get()), fmt::ptr(cf));
    task->compaction_running = true;
    task->compaction_data = compaction_manager::create_compaction_data();
    task->output_run_identifier = std::move(output_run_id);
    _cm._tasks.push_back(task);
    return task->compaction_data;
}

void compaction_manager_test::deregister_compaction(const sstables::compaction_data& c) {
    auto it = boost::find_if(_cm._tasks, [&c] (auto& task) { return task->compaction_data.compaction_uuid == c.compaction_uuid; });
    if (it != _cm._tasks.end()) {
        auto task = *it;
        testlog.debug("compaction_manager_test: deregister_compaction uuid={}: task {} table={}", c.compaction_uuid, fmt::ptr(task.get()), fmt::ptr(task->compacting_table));
        _cm._tasks.erase(it);
    } else {
        testlog.debug("compaction_manager_test: deregister_compaction uuid={}: task not found", c.compaction_uuid);
    }
}
