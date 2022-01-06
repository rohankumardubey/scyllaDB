/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Modified by ScyllaDB
 * Copyright (C) 2015-present ScyllaDB
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

#pragma once

#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
#include "schema_fwd.hh"
#include "utils/UUID.hh"
#include "gms/inet_address.hh"
#include "query-result-set.hh"
#include "db_clock.hh"
#include "db/commitlog/replay_position.hh"
#include "mutation_query.hh"
#include "system_keyspace_view_types.hh"
#include <map>
#include <seastar/core/distributed.hh>
#include "cdc/generation_id.hh"

namespace service {

class storage_proxy;
class storage_service;

namespace paxos {
    class paxos_state;
    class proposal;
} // namespace service::paxos

}

namespace netw {
    class messaging_service;
}

namespace cql3 {
    class query_processor;
}

namespace gms {
    class feature;
    class feature_service;
}

namespace locator {
    class endpoint_dc_rack;
} // namespace locator

namespace gms {
    class gossiper;
}

bool is_system_keyspace(std::string_view ks_name);

namespace db {

sstring system_keyspace_name();

class config;

using system_keyspace_view_name = std::pair<sstring, sstring>;
class system_keyspace_view_build_progress;

struct truncation_record;
typedef std::vector<db::replay_position> replay_positions;

class system_keyspace {
    static schema_ptr raft_config();
    static schema_ptr local();
    static schema_ptr peers();
    static schema_ptr peer_events();
    static schema_ptr range_xfers();
    static schema_ptr compactions_in_progress();
    static schema_ptr compaction_history();
    static schema_ptr sstable_activity();
    static schema_ptr large_partitions();
    static schema_ptr large_rows();
    static schema_ptr large_cells();
    static schema_ptr scylla_local();
    static schema_ptr clients();
    static future<> setup_version(sharded<netw::messaging_service>& ms, const db::config& cfg);
    static future<> check_health(const sstring& cluster_name);
    static future<> force_blocking_flush(sstring cfname);
    static future<> build_dc_rack_info();
    static future<> build_bootstrap_info();
    static future<> cache_truncation_record(distributed<replica::database>& db);
    template <typename Value>
    static future<> update_cached_values(gms::inet_address ep, sstring column_name, Value value);
public:
    static schema_ptr size_estimates();
public:
    static constexpr auto NAME = "system";
    static constexpr auto HINTS = "hints";
    static constexpr auto BATCHLOG = "batchlog";
    static constexpr auto PAXOS = "paxos";
    static constexpr auto BUILT_INDEXES = "IndexInfo";
    static constexpr auto LOCAL = "local";
    static constexpr auto TRUNCATED = "truncated";
    static constexpr auto PEERS = "peers";
    static constexpr auto PEER_EVENTS = "peer_events";
    static constexpr auto RANGE_XFERS = "range_xfers";
    static constexpr auto COMPACTIONS_IN_PROGRESS = "compactions_in_progress";
    static constexpr auto COMPACTION_HISTORY = "compaction_history";
    static constexpr auto SSTABLE_ACTIVITY = "sstable_activity";
    static constexpr auto SIZE_ESTIMATES = "size_estimates";
    static constexpr auto LARGE_PARTITIONS = "large_partitions";
    static constexpr auto LARGE_ROWS = "large_rows";
    static constexpr auto LARGE_CELLS = "large_cells";
    static constexpr auto SCYLLA_LOCAL = "scylla_local";
    static constexpr auto RAFT = "raft";
    static constexpr auto RAFT_SNAPSHOTS = "raft_snapshots";
    static constexpr auto RAFT_CONFIG = "raft_config";
    static constexpr auto REPAIR_HISTORY = "repair_history";
    static const char *const CLIENTS;

    struct v3 {
        static constexpr auto BATCHES = "batches";
        static constexpr auto PAXOS = "paxos";
        static constexpr auto BUILT_INDEXES = "IndexInfo";
        static constexpr auto LOCAL = "local";
        static constexpr auto PEERS = "peers";
        static constexpr auto PEER_EVENTS = "peer_events";
        static constexpr auto RANGE_XFERS = "range_xfers";
        static constexpr auto COMPACTION_HISTORY = "compaction_history";
        static constexpr auto SSTABLE_ACTIVITY = "sstable_activity";
        static constexpr auto SIZE_ESTIMATES = "size_estimates";
        static constexpr auto AVAILABLE_RANGES = "available_ranges";
        static constexpr auto VIEWS_BUILDS_IN_PROGRESS = "views_builds_in_progress";
        static constexpr auto BUILT_VIEWS = "built_views";
        static constexpr auto SCYLLA_VIEWS_BUILDS_IN_PROGRESS = "scylla_views_builds_in_progress";
        static constexpr auto CDC_LOCAL = "cdc_local";
        static schema_ptr batches();
        static schema_ptr built_indexes();
        static schema_ptr local();
        static schema_ptr truncated();
        static schema_ptr peers();
        static schema_ptr peer_events();
        static schema_ptr range_xfers();
        static schema_ptr compaction_history();
        static schema_ptr sstable_activity();
        static schema_ptr size_estimates();
        static schema_ptr large_partitions();
        static schema_ptr scylla_local();
        static schema_ptr available_ranges();
        static schema_ptr views_builds_in_progress();
        static schema_ptr built_views();
        static schema_ptr scylla_views_builds_in_progress();
        static schema_ptr cdc_local();
    };

    struct legacy {
        static constexpr auto HINTS = "hints";
        static constexpr auto BATCHLOG = "batchlog";
        static constexpr auto KEYSPACES = "schema_keyspaces";
        static constexpr auto COLUMNFAMILIES = "schema_columnfamilies";
        static constexpr auto COLUMNS = "schema_columns";
        static constexpr auto TRIGGERS = "schema_triggers";
        static constexpr auto USERTYPES = "schema_usertypes";
        static constexpr auto FUNCTIONS = "schema_functions";
        static constexpr auto AGGREGATES = "schema_aggregates";

        static schema_ptr keyspaces();
        static schema_ptr column_families();
        static schema_ptr columns();
        static schema_ptr triggers();
        static schema_ptr usertypes();
        static schema_ptr functions();
        static schema_ptr aggregates();
        static schema_ptr hints();
        static schema_ptr batchlog();
    };

    static constexpr const char* extra_durable_tables[] = { PAXOS, SCYLLA_LOCAL };

    static bool is_extra_durable(const sstring& name);

    // Partition estimates for a given range of tokens.
    struct range_estimates {
        schema_ptr schema;
        bytes range_start_token;
        bytes range_end_token;
        int64_t partitions_count;
        int64_t mean_partition_size;
    };

    using view_name = system_keyspace_view_name;
    using view_build_progress = system_keyspace_view_build_progress;

    static schema_ptr hints();
    static schema_ptr batchlog();
    static schema_ptr paxos();
    static schema_ptr built_indexes(); // TODO (from Cassandra): make private
    static schema_ptr raft();
    static schema_ptr raft_snapshots();
    static schema_ptr repair_history();

    static table_schema_version generate_schema_version(utils::UUID table_id, uint16_t offset = 0);

    // Only for testing.
    static void minimal_setup(distributed<cql3::query_processor>& qp);

    static future<> init_local_cache();
    static future<> deinit_local_cache();
    static future<> setup(distributed<replica::database>& db,
                distributed<cql3::query_processor>& qp,
                sharded<netw::messaging_service>& ms);
    static future<> update_schema_version(utils::UUID version);

    /*
    * Save tokens used by this node in the LOCAL table.
    */
    static future<> update_tokens(const std::unordered_set<dht::token>& tokens);

    /**
     * Record tokens being used by another node in the PEERS table.
     */
    static future<> update_tokens(gms::inet_address ep, const std::unordered_set<dht::token>& tokens);

    static future<> update_preferred_ip(gms::inet_address ep, gms::inet_address preferred_ip);
    static future<std::unordered_map<gms::inet_address, gms::inet_address>> get_preferred_ips();

    template <typename Value>
    static future<> update_peer_info(gms::inet_address ep, sstring column_name, Value value);

    static future<> remove_endpoint(gms::inet_address ep);

    static future<> set_scylla_local_param(const sstring& key, const sstring& value);
    static future<std::optional<sstring>> get_scylla_local_param(const sstring& key);

    static std::vector<schema_ptr> all_tables(const db::config& cfg);
    static future<> make(distributed<replica::database>& db, distributed<service::storage_service>& ss, sharded<gms::gossiper>& g, db::config& cfg);

    /// overloads

    future<foreign_ptr<lw_shared_ptr<reconcilable_result>>>
    static query_mutations(distributed<service::storage_proxy>& proxy,
                    const sstring& ks_name,
                    const sstring& cf_name);

    // Returns all data from given system table.
    // Intended to be used by code which is not performance critical.
    static future<lw_shared_ptr<query::result_set>> query(distributed<service::storage_proxy>& proxy,
                    const sstring& ks_name,
                    const sstring& cf_name);

    // Returns a slice of given system table.
    // Intended to be used by code which is not performance critical.
    static future<lw_shared_ptr<query::result_set>> query(
        distributed<service::storage_proxy>& proxy,
        const sstring& ks_name,
        const sstring& cf_name,
        const dht::decorated_key& key,
        query::clustering_range row_ranges = query::clustering_range::make_open_ended_both_sides());


    /**
     * Return a map of IP addresses containing a map of dc and rack info
     */
    static std::unordered_map<gms::inet_address, locator::endpoint_dc_rack>
    load_dc_rack_info();

    enum class bootstrap_state {
        NEEDS_BOOTSTRAP,
        COMPLETED,
        IN_PROGRESS,
        DECOMMISSIONED
    };

    struct compaction_history_entry {
        utils::UUID id;
        sstring ks;
        sstring cf;
        int64_t compacted_at = 0;
        int64_t bytes_in = 0;
        int64_t bytes_out = 0;
        // Key: number of rows merged
        // Value: counter
        std::unordered_map<int32_t, int64_t> rows_merged;
    };

    static future<> update_compaction_history(utils::UUID uuid, sstring ksname, sstring cfname, int64_t compacted_at, int64_t bytes_in, int64_t bytes_out,
                                       std::unordered_map<int32_t, int64_t> rows_merged);
    using compaction_history_consumer = noncopyable_function<future<>(const compaction_history_entry&)>;
    static future<> get_compaction_history(compaction_history_consumer&& f);

    typedef std::vector<db::replay_position> replay_positions;

    static future<> save_truncation_record(utils::UUID, db_clock::time_point truncated_at, db::replay_position);
    static future<> save_truncation_record(const replica::column_family&, db_clock::time_point truncated_at, db::replay_position);
    static future<replay_positions> get_truncated_position(utils::UUID);
    static future<db::replay_position> get_truncated_position(utils::UUID, uint32_t shard);
    static future<db_clock::time_point> get_truncated_at(utils::UUID);
    static future<truncation_record> get_truncation_record(utils::UUID cf_id);

    /**
     * Return a map of stored tokens to IP addresses
     *
     */
    static future<std::unordered_map<gms::inet_address, std::unordered_set<dht::token>>> load_tokens();

    /**
     * Return a map of store host_ids to IP addresses
     *
     */
    static future<std::unordered_map<gms::inet_address, utils::UUID>> load_host_ids();

    /*
     * Read this node's tokens stored in the LOCAL table.
     * Used to initialize a restarting node.
     */
    static future<std::unordered_set<dht::token>> get_saved_tokens();

    /*
     * Gets this node's non-empty set of tokens.
     * TODO: maybe get this data from token_metadata instance?
     */
    static future<std::unordered_set<dht::token>> get_local_tokens();

    static future<std::unordered_map<gms::inet_address, sstring>> load_peer_features();

    static future<int> increment_and_get_generation();
    static bool bootstrap_complete();
    static bool bootstrap_in_progress();
    static bootstrap_state get_bootstrap_state();
    static bool was_decommissioned();
    static future<> set_bootstrap_state(bootstrap_state state);

    /**
     * Read the host ID from the system keyspace, creating (and storing) one if
     * none exists.
     */
    static future<utils::UUID> load_local_host_id();
    /**
     * Read the host ID from the system keyspace, generating an exception if it
     * doesn't exist.
     */
    static utils::UUID get_local_host_id();

    /**
     * Sets the local host ID explicitly.  Should only be called outside of SystemTable when replacing a node.
     */
    static future<utils::UUID> set_local_host_id(utils::UUID host_id);

    static api::timestamp_type schema_creation_timestamp();

    /**
     * Builds a mutation for SIZE_ESTIMATES_CF containing the specified estimates.
     */
    static mutation make_size_estimates_mutation(const sstring& ks, std::vector<range_estimates> estimates);

    static future<> register_view_for_building(sstring ks_name, sstring view_name, const dht::token& token);
    static future<> update_view_build_progress(sstring ks_name, sstring view_name, const dht::token& token);
    static future<> remove_view_build_progress(sstring ks_name, sstring view_name);
    static future<> remove_view_build_progress_across_all_shards(sstring ks_name, sstring view_name);
    static future<> mark_view_as_built(sstring ks_name, sstring view_name);
    static future<> remove_built_view(sstring ks_name, sstring view_name);
    static future<std::vector<view_name>> load_built_views();
    static future<std::vector<view_build_progress>> load_view_build_progress();

    // Paxos related functions
    static future<service::paxos::paxos_state> load_paxos_state(partition_key_view key, schema_ptr s, gc_clock::time_point now,
            db::timeout_clock::time_point timeout);
    static future<> save_paxos_promise(const schema& s, const partition_key& key, const utils::UUID& ballot, db::timeout_clock::time_point timeout);
    static future<> save_paxos_proposal(const schema& s, const service::paxos::proposal& proposal, db::timeout_clock::time_point timeout);
    static future<> save_paxos_decision(const schema& s, const service::paxos::proposal& decision, db::timeout_clock::time_point timeout);
    static future<> delete_paxos_decision(const schema& s, const partition_key& key, const utils::UUID& ballot, db::timeout_clock::time_point timeout);

    // CDC related functions

    /*
    * Save the CDC generation ID announced by this node in persistent storage.
    */
    static future<> update_cdc_generation_id(cdc::generation_id);

    /*
    * Read the CDC generation ID announced by this node from persistent storage.
    * Used to initialize a restarting node.
    */
    static future<std::optional<cdc::generation_id>> get_cdc_generation_id();

    static future<bool> cdc_is_rewritten();
    static future<> cdc_set_rewritten(std::optional<cdc::generation_id_v1>);

    static future<> enable_features_on_startup(sharded<gms::feature_service>& feat);

    // Load Raft Group 0 id from scylla.local
    static future<utils::UUID> get_raft_group0_id();

    // Load this server id from scylla.local
    static future<utils::UUID> get_raft_server_id();

    // Persist Raft Group 0 id. Should be a TIMEUUID.
    static future<> set_raft_group0_id(utils::UUID id);

    // Called once at fresh server startup to make sure every server
    // has a Raft ID
    static future<> set_raft_server_id(utils::UUID id);

    // Save advertised gossip feature set to system.local
    static future<> save_local_supported_features(const std::set<std::string_view>& feats);
}; // class system_keyspace

future<> system_keyspace_make(distributed<replica::database>& db, distributed<service::storage_service>& ss, sharded<gms::gossiper>& g);
extern const char *const system_keyspace_CLIENTS;

} // namespace db
