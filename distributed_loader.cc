/*
 * Copyright (C) 2018-present ScyllaDB
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

#include <seastar/util/closeable.hh>
#include "distributed_loader.hh"
#include "replica/database.hh"
#include "db/config.hh"
#include "db/system_keyspace.hh"
#include "db/system_distributed_keyspace.hh"
#include "db/schema_tables.hh"
#include "lister.hh"
#include "compaction/compaction.hh"
#include "compaction/compaction_manager.hh"
#include "sstables/sstables.hh"
#include "sstables/sstables_manager.hh"
#include "sstables/sstable_directory.hh"
#include "service/priority_manager.hh"
#include "auth/common.hh"
#include "tracing/trace_keyspace_helper.hh"
#include "db/view/view_update_checks.hh"
#include <unordered_map>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/min_element.hpp>
#include "db/view/view_update_generator.hh"
#include "utils/directories.hh"

extern logging::logger dblog;

static const std::unordered_set<std::string_view> system_keyspaces = {
                db::system_keyspace::NAME, db::schema_tables::NAME
};

bool is_system_keyspace(std::string_view name) {
    return system_keyspaces.contains(name);
}

static const std::unordered_set<std::string_view> internal_keyspaces = {
        db::system_distributed_keyspace::NAME,
        db::system_distributed_keyspace::NAME_EVERYWHERE,
        db::system_keyspace::NAME,
        db::schema_tables::NAME,
        auth::meta::AUTH_KS,
        tracing::trace_keyspace_helper::KEYSPACE_NAME
};

bool is_internal_keyspace(std::string_view name) {
    return internal_keyspaces.contains(name);
}

static io_error_handler error_handler_for_upload_dir() {
    return [] (std::exception_ptr eptr) {
        // do nothing about sstable exception and caller will just rethrow it.
    };
}

io_error_handler error_handler_gen_for_upload_dir(disk_error_signal_type& dummy) {
    return error_handler_for_upload_dir();
}

// global_column_family_ptr provides a way to easily retrieve local instance of a given column family.
class global_column_family_ptr {
    distributed<replica::database>& _db;
    utils::UUID _id;
private:
    replica::column_family& get() const { return _db.local().find_column_family(_id); }
public:
    global_column_family_ptr(distributed<replica::database>& db, sstring ks_name, sstring cf_name)
        : _db(db)
        , _id(_db.local().find_column_family(ks_name, cf_name).schema()->id()) {
    }

    replica::column_family* operator->() const {
        return &get();
    }
    replica::column_family& operator*() const {
        return get();
    }
};

future<>
distributed_loader::process_sstable_dir(sharded<sstables::sstable_directory>& dir, bool sort_sstables_according_to_owner) {
    return dir.invoke_on(0, [] (const sstables::sstable_directory& d) {
        return utils::directories::verify_owner_and_mode(d.sstable_dir());
    }).then([&dir, sort_sstables_according_to_owner] {
      return dir.invoke_on_all([&dir, sort_sstables_according_to_owner] (sstables::sstable_directory& d) {
        // Supposed to be called with the node either down or on behalf of maintenance tasks
        // like nodetool refresh
        return d.process_sstable_dir(service::get_local_streaming_priority(), sort_sstables_according_to_owner).then([&dir, &d] {
            return d.move_foreign_sstables(dir);
        });
      });
    }).then([&dir] {
        return dir.invoke_on_all([&dir] (sstables::sstable_directory& d) {
            return d.commit_directory_changes();
        });
    });
}

future<>
distributed_loader::lock_table(sharded<sstables::sstable_directory>& dir, sharded<replica::database>& db, sstring ks_name, sstring cf_name) {
    return dir.invoke_on_all([&db, ks_name, cf_name] (sstables::sstable_directory& d) {
        auto& table = db.local().find_column_family(ks_name, cf_name);
        d.store_phaser(table.write_in_progress());
        return make_ready_future<>();
    });
}

// Helper structure for resharding.
//
// Describes the sstables (represented by their foreign_sstable_open_info) that are shared and
// need to be resharded. Each shard will keep one such descriptor, that contains the list of
// SSTables assigned to it, and their total size. The total size is used to make sure we are
// fairly balancing SSTables among shards.
struct reshard_shard_descriptor {
    sstables::sstable_directory::sstable_info_vector info_vec;
    uint64_t uncompressed_data_size = 0;

    bool total_size_smaller(const reshard_shard_descriptor& rhs) const {
        return uncompressed_data_size < rhs.uncompressed_data_size;
    }

    uint64_t size() const {
        return uncompressed_data_size;
    }
};

// Collects shared SSTables from all shards and returns a vector containing them all.
// This function assumes that the list of SSTables can be fairly big so it is careful to
// manipulate it in a do_for_each loop (which yields) instead of using standard accumulators.
future<sstables::sstable_directory::sstable_info_vector>
collect_all_shared_sstables(sharded<sstables::sstable_directory>& dir) {
    return do_with(sstables::sstable_directory::sstable_info_vector(), [&dir] (sstables::sstable_directory::sstable_info_vector& info_vec) {
        // We want to make sure that each distributed object reshards about the same amount of data.
        // Each sharded object has its own shared SSTables. We can use a clever algorithm in which they
        // all distributely figure out which SSTables to exchange, but we'll keep it simple and move all
        // their foreign_sstable_open_info to a coordinator (the shard who called this function). We can
        // move in bulk and that's efficient. That shard can then distribute the work among all the
        // others who will reshard.
        auto coordinator = this_shard_id();
        // We will first move all of the foreign open info to temporary storage so that we can sort
        // them. We want to distribute bigger sstables first.
        return dir.invoke_on_all([&info_vec, coordinator] (sstables::sstable_directory& d) {
            return smp::submit_to(coordinator, [&info_vec, info = d.retrieve_shared_sstables()] () mutable {
                // We want do_for_each here instead of a loop to avoid stalls. Resharding can be
                // called during node operations too. For example, if it is called to load new
                // SSTables into the system.
                return do_for_each(info, [&info_vec] (sstables::foreign_sstable_open_info& info) {
                    info_vec.push_back(std::move(info));
                });
            });
        }).then([&info_vec] () mutable {
            return make_ready_future<sstables::sstable_directory::sstable_info_vector>(std::move(info_vec));
        });
    });
}

// Given a vector of shared sstables to be resharded, distribute it among all shards.
// The vector is first sorted to make sure that we are moving the biggest SSTables first.
//
// Returns a reshard_shard_descriptor per shard indicating the work that each shard has to do.
future<std::vector<reshard_shard_descriptor>>
distribute_reshard_jobs(sstables::sstable_directory::sstable_info_vector source) {
    return do_with(std::move(source), std::vector<reshard_shard_descriptor>(smp::count),
            [] (sstables::sstable_directory::sstable_info_vector& source, std::vector<reshard_shard_descriptor>& destinations) mutable {
        std::sort(source.begin(), source.end(), [] (const sstables::foreign_sstable_open_info& a, const sstables::foreign_sstable_open_info& b) {
            // Sort on descending SSTable sizes.
            return a.uncompressed_data_size > b.uncompressed_data_size;
        });
        return do_for_each(source, [&destinations] (sstables::foreign_sstable_open_info& info) mutable {
            auto shard_it = boost::min_element(destinations, std::mem_fn(&reshard_shard_descriptor::total_size_smaller));
            shard_it->uncompressed_data_size += info.uncompressed_data_size;
            shard_it->info_vec.push_back(std::move(info));
        }).then([&destinations] () mutable {
            return make_ready_future<std::vector<reshard_shard_descriptor>>(std::move(destinations));
        });
    });
}

future<> run_resharding_jobs(sharded<sstables::sstable_directory>& dir, std::vector<reshard_shard_descriptor> reshard_jobs,
                             sharded<replica::database>& db, sstring ks_name, sstring table_name, sstables::compaction_sstable_creator_fn creator) {

    uint64_t total_size = boost::accumulate(reshard_jobs | boost::adaptors::transformed(std::mem_fn(&reshard_shard_descriptor::size)), uint64_t(0));
    if (total_size == 0) {
        return make_ready_future<>();
    }

    return do_with(std::move(reshard_jobs), [&dir, &db, ks_name, table_name, creator = std::move(creator), total_size] (std::vector<reshard_shard_descriptor>& reshard_jobs) {
        auto start = std::chrono::steady_clock::now();
        dblog.info("{}", fmt::format("Resharding {} for {}.{}", sstables::pretty_printed_data_size(total_size), ks_name, table_name));

        return dir.invoke_on_all([&dir, &db, &reshard_jobs, ks_name, table_name, creator] (sstables::sstable_directory& d) mutable {
            auto& table = db.local().find_column_family(ks_name, table_name);
            auto info_vec = std::move(reshard_jobs[this_shard_id()].info_vec);
            auto& cm = table.get_compaction_manager();
            auto max_threshold = table.schema()->max_compaction_threshold();
            auto& iop = service::get_local_streaming_priority();
            return d.reshard(std::move(info_vec), cm, table, max_threshold, creator, iop).then([&d, &dir] {
                return d.move_foreign_sstables(dir);
            });
        }).then([start, total_size, ks_name, table_name] {
            auto duration = std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - start);
            dblog.info("{}", fmt::format("Resharded {} for {}.{} in {:.2f} seconds, {}", sstables::pretty_printed_data_size(total_size), ks_name, table_name, duration.count(), sstables::pretty_printed_throughput(total_size, duration)));
            return make_ready_future<>();
        });
    });
}

// Global resharding function. Done in two parts:
//  - The first part spreads the foreign_sstable_open_info across shards so that all of them are
//    resharding about the same amount of data
//  - The second part calls each shard's distributed object to reshard the SSTables they were
//    assigned.
future<>
distributed_loader::reshard(sharded<sstables::sstable_directory>& dir, sharded<replica::database>& db, sstring ks_name, sstring table_name, sstables::compaction_sstable_creator_fn creator) {
    return collect_all_shared_sstables(dir).then([] (sstables::sstable_directory::sstable_info_vector all_jobs) mutable {
        return distribute_reshard_jobs(std::move(all_jobs));
    }).then([&dir, &db, ks_name, table_name, creator = std::move(creator)] (std::vector<reshard_shard_descriptor> destinations) mutable {
        return run_resharding_jobs(dir, std::move(destinations), db, ks_name, table_name, std::move(creator));
    });
}

future<int64_t>
highest_generation_seen(sharded<sstables::sstable_directory>& directory) {
    return directory.map_reduce0(std::mem_fn(&sstables::sstable_directory::highest_generation_seen), int64_t(0), [] (int64_t a, int64_t b) {
        return std::max(a, b);
    });
}

future<sstables::sstable::version_types>
highest_version_seen(sharded<sstables::sstable_directory>& dir, sstables::sstable_version_types system_version) {
    using version = sstables::sstable_version_types;
    return dir.map_reduce0(std::mem_fn(&sstables::sstable_directory::highest_version_seen), system_version, [] (version a, version b) {
        return std::max(a, b);
    });
}

future<>
distributed_loader::reshape(sharded<sstables::sstable_directory>& dir, sharded<replica::database>& db, sstables::reshape_mode mode,
        sstring ks_name, sstring table_name, sstables::compaction_sstable_creator_fn creator) {

    auto start = std::chrono::steady_clock::now();
    return dir.map_reduce0([&dir, &db, ks_name = std::move(ks_name), table_name = std::move(table_name), creator = std::move(creator), mode] (sstables::sstable_directory& d) {
        auto& table = db.local().find_column_family(ks_name, table_name);
        auto& cm = table.get_compaction_manager();
        auto& iop = service::get_local_streaming_priority();
        return d.reshape(cm, table, creator, iop, mode);
    }, uint64_t(0), std::plus<uint64_t>()).then([start] (uint64_t total_size) {
        if (total_size > 0) {
            auto duration = std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - start);
            dblog.info("{}", fmt::format("Reshaped {} in {:.2f} seconds, {}", sstables::pretty_printed_data_size(total_size), duration.count(), sstables::pretty_printed_throughput(total_size, duration)));
        }
        return make_ready_future<>();
    });
}

// Loads SSTables into the main directory (or staging) and returns how many were loaded
future<size_t>
distributed_loader::make_sstables_available(sstables::sstable_directory& dir, sharded<replica::database>& db,
        sharded<db::view::view_update_generator>& view_update_generator, fs::path datadir, sstring ks, sstring cf) {

    auto& table = db.local().find_column_family(ks, cf);

    return do_with(std::vector<sstables::shared_sstable>(),
            [&table, &dir, &view_update_generator, datadir = std::move(datadir)] (std::vector<sstables::shared_sstable>& new_sstables) {
        return dir.do_for_each_sstable([&table, datadir = std::move(datadir), &new_sstables] (sstables::shared_sstable sst) {
            auto gen = table.calculate_generation_for_new_table();
            dblog.trace("Loading {} into {}, new generation {}", sst->get_filename(), datadir.native(), gen);
            return sst->move_to_new_dir(datadir.native(), gen,  true).then([&table, &new_sstables, sst] {
                // When loading an imported sst, set level to 0 because it may overlap with existing ssts on higher levels.
                sst->set_sstable_level(0);
                new_sstables.push_back(std::move(sst));
                return make_ready_future<>();
            });
        }).then([&table, &new_sstables] {
            // nothing loaded
            if (new_sstables.empty()) {
                return make_ready_future<>();
            }

            return do_for_each(new_sstables, [&table] (sstables::shared_sstable& sst) {
                return table.add_sstable_and_update_cache(sst).handle_exception([&sst] (std::exception_ptr ep) {
                    dblog.error("Failed to load {}: {}. Aborting.", sst->toc_filename(), ep);
                    abort();
                });
            });
        }).then([&view_update_generator, &table, &new_sstables] {
            return parallel_for_each(new_sstables, [&view_update_generator, &table] (sstables::shared_sstable& sst) {
                if (sst->requires_view_building()) {
                    return view_update_generator.local().register_staging_sstable(sst, table.shared_from_this());
                }
                return make_ready_future<>();
            });
        }).then_wrapped([&new_sstables] (future<> f) {
            if (!f.failed()) {
                return make_ready_future<size_t>(new_sstables.size());
            } else {
                return make_exception_future<size_t>(f.get_exception());
            }
        });
    });
}

future<>
distributed_loader::process_upload_dir(distributed<replica::database>& db, distributed<db::system_distributed_keyspace>& sys_dist_ks,
        distributed<db::view::view_update_generator>& view_update_generator, sstring ks, sstring cf) {
    seastar::thread_attributes attr;
    attr.sched_group = db.local().get_streaming_scheduling_group();

    return seastar::async(std::move(attr), [&db, &view_update_generator, &sys_dist_ks, ks = std::move(ks), cf = std::move(cf)] {
        global_column_family_ptr global_table(db, ks, cf);

        sharded<sstables::sstable_directory> directory;
        auto upload = fs::path(global_table->dir()) / sstables::upload_dir;
        directory.start(upload, db.local().get_config().initial_sstable_loading_concurrency(), std::ref(db.local().get_sharded_sst_dir_semaphore()),
            sstables::sstable_directory::need_mutate_level::yes,
            sstables::sstable_directory::lack_of_toc_fatal::no,
            sstables::sstable_directory::enable_dangerous_direct_import_of_cassandra_counters(db.local().get_config().enable_dangerous_direct_import_of_cassandra_counters()),
            sstables::sstable_directory::allow_loading_materialized_view::no,
            [&global_table] (fs::path dir, int64_t gen, sstables::sstable_version_types v, sstables::sstable_format_types f) {
                return global_table->make_sstable(dir.native(), gen, v, f, &error_handler_gen_for_upload_dir);

        }).get();

        auto stop = deferred_stop(directory);

        lock_table(directory, db, ks, cf).get();
        process_sstable_dir(directory).get();

        auto generation = highest_generation_seen(directory).get0();
        auto shard_generation_base = generation / smp::count + 1;

        // We still want to do our best to keep the generation numbers shard-friendly.
        // Each destination shard will manage its own generation counter.
        std::vector<std::atomic<int64_t>> shard_gen(smp::count);
        for (shard_id s = 0; s < smp::count; ++s) {
            shard_gen[s].store(shard_generation_base * smp::count + s, std::memory_order_relaxed);
        }

        reshard(directory, db, ks, cf, [&global_table, upload, &shard_gen] (shard_id shard) mutable {
            // we need generation calculated by instance of cf at requested shard
            auto gen = shard_gen[shard].fetch_add(smp::count, std::memory_order_relaxed);

            return global_table->make_sstable(upload.native(), gen,
                    global_table->get_sstables_manager().get_highest_supported_format(),
                    sstables::sstable::format_types::big, &error_handler_gen_for_upload_dir);
        }).get();

        reshape(directory, db, sstables::reshape_mode::strict, ks, cf, [global_table, upload, &shard_gen] (shard_id shard) {
            auto gen = shard_gen[shard].fetch_add(smp::count, std::memory_order_relaxed);
            return global_table->make_sstable(upload.native(), gen,
                  global_table->get_sstables_manager().get_highest_supported_format(),
                  sstables::sstable::format_types::big,
                  &error_handler_gen_for_upload_dir);
        }).get();

        const bool use_view_update_path = db::view::check_needs_view_update_path(sys_dist_ks.local(), *global_table, streaming::stream_reason::repair).get0();

        auto datadir = upload.parent_path();
        if (use_view_update_path) {
            // Move to staging directory to avoid clashes with future uploads. Unique generation number ensures no collisions.
           datadir /= sstables::staging_dir;
        }

        size_t loaded = directory.map_reduce0([&db, ks, cf, datadir, &view_update_generator] (sstables::sstable_directory& dir) {
            return make_sstables_available(dir, db, view_update_generator, datadir, ks, cf);
        }, size_t(0), std::plus<size_t>()).get0();

        dblog.info("Loaded {} SSTables into {}", loaded, datadir.native());
    });
}

future<std::tuple<utils::UUID, std::vector<std::vector<sstables::shared_sstable>>>>
distributed_loader::get_sstables_from_upload_dir(distributed<replica::database>& db, sstring ks, sstring cf) {
    return seastar::async([&db, ks = std::move(ks), cf = std::move(cf)] {
        global_column_family_ptr global_table(db, ks, cf);
        sharded<sstables::sstable_directory> directory;
        auto table_id = global_table->schema()->id();
        auto upload = fs::path(global_table->dir()) / sstables::upload_dir;

        directory.start(upload, db.local().get_config().initial_sstable_loading_concurrency(), std::ref(db.local().get_sharded_sst_dir_semaphore()),
            sstables::sstable_directory::need_mutate_level::yes,
            sstables::sstable_directory::lack_of_toc_fatal::no,
            sstables::sstable_directory::enable_dangerous_direct_import_of_cassandra_counters(db.local().get_config().enable_dangerous_direct_import_of_cassandra_counters()),
            sstables::sstable_directory::allow_loading_materialized_view::no,
            [&global_table] (fs::path dir, int64_t gen, sstables::sstable_version_types v, sstables::sstable_format_types f) {
                return global_table->make_sstable(dir.native(), gen, v, f, &error_handler_gen_for_upload_dir);

        }).get();

        auto stop = deferred_stop(directory);

        std::vector<std::vector<sstables::shared_sstable>> sstables_on_shards(smp::count);
        lock_table(directory, db, ks, cf).get();
        bool sort_sstables_according_to_owner = false;
        process_sstable_dir(directory, sort_sstables_according_to_owner).get();
        directory.invoke_on_all([&sstables_on_shards] (sstables::sstable_directory& d) mutable {
            sstables_on_shards[this_shard_id()] = d.get_unsorted_sstables();
        }).get();

        return std::make_tuple(table_id, std::move(sstables_on_shards));
    });
}

future<> distributed_loader::cleanup_column_family_temp_sst_dirs(sstring sstdir) {
    return do_with(std::vector<future<>>(), [sstdir = std::move(sstdir)] (std::vector<future<>>& futures) {
        return lister::scan_dir(sstdir, { directory_entry_type::directory }, [&futures] (fs::path sstdir, directory_entry de) {
            // push futures that remove files/directories into an array of futures,
            // so that the supplied callback will not block scan_dir() from
            // reading the next entry in the directory.
            fs::path dirpath = sstdir / de.name;
            if (sstables::sstable::is_temp_dir(dirpath)) {
                dblog.info("Found temporary sstable directory: {}, removing", dirpath);
                futures.push_back(io_check([dirpath = std::move(dirpath)] () { return lister::rmdir(dirpath); }));
            }
            return make_ready_future<>();
        }).then([&futures] {
            return when_all_succeed(futures.begin(), futures.end()).discard_result();
        });
    });
}

future<> distributed_loader::handle_sstables_pending_delete(sstring pending_delete_dir) {
    return do_with(std::vector<future<>>(), [dir = std::move(pending_delete_dir)] (std::vector<future<>>& futures) {
        return lister::scan_dir(dir, { directory_entry_type::regular }, [&futures] (fs::path dir, directory_entry de) {
            // push nested futures that remove files/directories into an array of futures,
            // so that the supplied callback will not block scan_dir() from
            // reading the next entry in the directory.
            fs::path file_path = dir / de.name;
            if (file_path.extension() == ".tmp") {
                dblog.info("Found temporary pending_delete log file: {}, deleting", file_path);
                futures.push_back(remove_file(file_path.string()));
            } else if (file_path.extension() == ".log") {
                dblog.info("Found pending_delete log file: {}, replaying", file_path);
                auto f = sstables::replay_pending_delete_log(file_path.string()).then([file_path = std::move(file_path)] {
                    dblog.debug("Replayed {}, removing", file_path);
                    return remove_file(file_path.string());
                });
                futures.push_back(std::move(f));
            } else {
                dblog.debug("Found unknown file in pending_delete directory: {}, ignoring", file_path);
            }
            return make_ready_future<>();
        }).then([&futures] {
            return when_all_succeed(futures.begin(), futures.end()).discard_result();
        });
    });
}

future<> distributed_loader::populate_column_family(distributed<replica::database>& db, sstring sstdir, sstring ks, sstring cf, bool must_exist) {
    return async([&db, sstdir = std::move(sstdir), ks = std::move(ks), cf = std::move(cf), must_exist] {
        assert(this_shard_id() == 0);

        if (!file_exists(sstdir).get0()) {
            if (must_exist) {
                throw std::runtime_error(format("Populating {}/{} failed: {} does not exist", ks, cf, sstdir));
            }
            return;
        }

        // First pass, cleanup temporary sstable directories and sstables pending delete.
        cleanup_column_family_temp_sst_dirs(sstdir).get();
        auto pending_delete_dir = sstdir + "/" + sstables::sstable::pending_delete_dir_basename();
        auto exists = file_exists(pending_delete_dir).get0();
        if (exists) {
            handle_sstables_pending_delete(pending_delete_dir).get();
        }

        global_column_family_ptr global_table(db, ks, cf);

        sharded<sstables::sstable_directory> directory;
        directory.start(fs::path(sstdir), db.local().get_config().initial_sstable_loading_concurrency(), std::ref(db.local().get_sharded_sst_dir_semaphore()),
            sstables::sstable_directory::need_mutate_level::no,
            sstables::sstable_directory::lack_of_toc_fatal::yes,
            sstables::sstable_directory::enable_dangerous_direct_import_of_cassandra_counters(db.local().get_config().enable_dangerous_direct_import_of_cassandra_counters()),
            sstables::sstable_directory::allow_loading_materialized_view::yes,
            [&global_table] (fs::path dir, int64_t gen, sstables::sstable_version_types v, sstables::sstable_format_types f) {
                return global_table->make_sstable(dir.native(), gen, v, f);
        }).get();

        auto stop = deferred_stop(directory);

        lock_table(directory, db, ks, cf).get();
        process_sstable_dir(directory).get();

        // If we are resharding system tables before we can read them, we will not
        // know which is the highest format we support: this information is itself stored
        // in the system tables. In that case we'll rely on what we find on disk: we'll
        // at least not downgrade any files. If we already know that we support a higher
        // format than the one we see then we use that.
        auto sys_format = global_table->get_sstables_manager().get_highest_supported_format();
        auto sst_version = highest_version_seen(directory, sys_format).get0();
        auto generation = highest_generation_seen(directory).get0();

        db.invoke_on_all([&global_table, generation] (replica::database& db) {
            global_table->update_sstables_known_generation(generation);
            return global_table->disable_auto_compaction();
        }).get();

        reshard(directory, db, ks, cf, [&global_table, sstdir, sst_version] (shard_id shard) mutable {
            auto gen = smp::submit_to(shard, [&global_table] () {
                return global_table->calculate_generation_for_new_table();
            }).get0();

            return global_table->make_sstable(sstdir, gen, sst_version, sstables::sstable::format_types::big);
        }).get();

        // The node is offline at this point so we are very lenient with what we consider
        // offstrategy.
        reshape(directory, db, sstables::reshape_mode::relaxed, ks, cf, [global_table, sstdir, sst_version] (shard_id shard) {
            auto gen = global_table->calculate_generation_for_new_table();
            return global_table->make_sstable(sstdir, gen, sst_version, sstables::sstable::format_types::big);
        }).get();

        directory.invoke_on_all([global_table] (sstables::sstable_directory& dir) {
            return dir.do_for_each_sstable([&global_table] (sstables::shared_sstable sst) {
                return global_table->add_sstable_and_update_cache(sst);
            });
        }).get();
    });
}

future<> distributed_loader::populate_keyspace(distributed<replica::database>& db, sstring datadir, sstring ks_name) {
    auto ksdir = datadir + "/" + ks_name;
    auto& keyspaces = db.local().get_keyspaces();
    auto i = keyspaces.find(ks_name);
    if (i == keyspaces.end()) {
        dblog.warn("Skipping undefined keyspace: {}", ks_name);
        return make_ready_future<>();
    } else {
        dblog.info("Populating Keyspace {}", ks_name);
        auto& ks = i->second;
        auto& column_families = db.local().get_column_families();

        return parallel_for_each(ks.metadata()->cf_meta_data() | boost::adaptors::map_values,
            [ks_name, ksdir, &ks, &column_families, &db] (schema_ptr s) {
                utils::UUID uuid = s->id();
                lw_shared_ptr<replica::column_family> cf = column_families[uuid];
                sstring cfname = cf->schema()->cf_name();
                auto sstdir = ks.column_family_directory(ksdir, cfname, uuid);
                dblog.info("Keyspace {}: Reading CF {} id={} version={}", ks_name, cfname, uuid, s->version());
                return ks.make_directory_for_column_family(cfname, uuid).then([&db, sstdir, uuid, ks_name, cfname] {
                    return distributed_loader::populate_column_family(db, sstdir + "/" + sstables::staging_dir, ks_name, cfname);
                }).then([&db, sstdir, ks_name, cfname] {
                    return distributed_loader::populate_column_family(db, sstdir + "/" + sstables::quarantine_dir, ks_name, cfname, false /* must_exist */);
                }).then([&db, sstdir, uuid, ks_name, cfname] {
                    return distributed_loader::populate_column_family(db, sstdir, ks_name, cfname);
                }).handle_exception([ks_name, cfname, sstdir](std::exception_ptr eptr) {
                    std::string msg =
                        format("Exception while populating keyspace '{}' with column family '{}' from file '{}': {}",
                               ks_name, cfname, sstdir, eptr);
                    dblog.error("Exception while populating keyspace '{}' with column family '{}' from file '{}': {}",
                                ks_name, cfname, sstdir, eptr);
                    try {
                        std::rethrow_exception(eptr);
                    } catch (sstables::compaction_stopped_exception& e) {
                        // swallow compaction stopped exception, to allow clean shutdown.
                    } catch (...) {
                        throw std::runtime_error(msg.c_str());
                    }
                });
            });
    }
}

future<> distributed_loader::init_system_keyspace(distributed<replica::database>& db, distributed<service::storage_service>& ss, sharded<gms::gossiper>& g, db::config& cfg) {
    return seastar::async([&db, &ss, &cfg, &g] {
        db.invoke_on_all([&db, &ss, &cfg, &g] (replica::database&) {
            return db::system_keyspace::make(db, ss, g, cfg);
        }).get();

        const auto& cfg = db.local().get_config();
        for (auto& data_dir : cfg.data_file_directories()) {
            for (auto ksname : system_keyspaces) {
                distributed_loader::populate_keyspace(db, data_dir, sstring(ksname)).get();
            }
        }

        db.invoke_on_all([] (replica::database& db) {
            for (auto ksname : system_keyspaces) {
                auto& ks = db.find_keyspace(ksname);
                for (auto& pair : ks.metadata()->cf_meta_data()) {
                    auto cfm = pair.second;
                    auto& cf = db.find_column_family(cfm);
                    cf.mark_ready_for_writes();
                }
                // for system keyspaces, we only do this post all population, and
                // only as a consistency measure.
                // change this if it is ever needed to sync system keyspace
                // population
                ks.mark_as_populated();
            }
            return make_ready_future<>();
        }).get();
    });
}

future<> distributed_loader::ensure_system_table_directories(distributed<replica::database>& db) {
    return parallel_for_each(system_keyspaces, [&db](std::string_view ksname) {
        auto& ks = db.local().find_keyspace(ksname);
        return parallel_for_each(ks.metadata()->cf_meta_data(), [&ks] (auto& pair) {
            auto cfm = pair.second;
            return ks.make_directory_for_column_family(cfm->cf_name(), cfm->id());
        });
    });
}

future<> distributed_loader::init_non_system_keyspaces(distributed<replica::database>& db,
        distributed<service::storage_proxy>& proxy) {
    return seastar::async([&db, &proxy] {
        db.invoke_on_all([&proxy] (replica::database& db) {
            return db.parse_system_tables(proxy);
        }).get();

        const auto& cfg = db.local().get_config();
        using ks_dirs = std::unordered_multimap<sstring, sstring>;

        ks_dirs dirs;

        parallel_for_each(cfg.data_file_directories(), [&db, &dirs] (sstring directory) {
            // we want to collect the directories first, so we can get a full set of potential dirs
            return lister::scan_dir(directory, { directory_entry_type::directory }, [&dirs] (fs::path datadir, directory_entry de) {
                if (!is_system_keyspace(de.name)) {
                    dirs.emplace(de.name, datadir.native());
                }
                return make_ready_future<>();
            });
        }).get();

        db.invoke_on_all([&dirs] (replica::database& db) {
            for (auto& [name, ks] : db.get_keyspaces()) {
                // mark all user keyspaces that are _not_ on disk as already
                // populated.
                if (!dirs.contains(ks.metadata()->name())) {
                    ks.mark_as_populated();
                }
            }
        }).get();

        std::vector<future<>> futures;

        // treat "dirs" as immutable to avoid modifying it while still in 
        // a range-iteration. Also to simplify the "finally"
        for (auto i = dirs.begin(); i != dirs.end();) {
            auto& ks_name = i->first;
            auto e = dirs.equal_range(ks_name).second;
            auto j = i++;
            // might have more than one dir for a keyspace iff data_file_directories is > 1 and
            // somehow someone placed sstables in more than one of them for a given ks. (import?) 
            futures.emplace_back(parallel_for_each(j, e, [&](const std::pair<sstring, sstring>& p) {
                auto& datadir = p.second;
                return distributed_loader::populate_keyspace(db, datadir, ks_name);
            }).finally([&] {
                return db.invoke_on_all([ks_name] (replica::database& db) {
                    // can be false if running test environment
                    // or ks_name was just a borked directory not representing
                    // a keyspace in schema tables.
                    if (db.has_keyspace(ks_name)) {
                        db.find_keyspace(ks_name).mark_as_populated();
                    }
                    return make_ready_future<>();
                });
            }));
        }

        when_all_succeed(futures.begin(), futures.end()).discard_result().get();

        db.invoke_on_all([] (replica::database& db) {
            return parallel_for_each(db.get_non_system_column_families(), [] (lw_shared_ptr<replica::table> table) {
                // Make sure this is called even if the table is empty
                table->mark_ready_for_writes();
                return make_ready_future<>();
            });
        }).get();
    });
}

