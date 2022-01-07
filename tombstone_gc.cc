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

#include <chrono>
#include <boost/icl/interval.hpp>
#include <boost/icl/interval_map.hpp>
#include "schema.hh"
#include "dht/i_partitioner.hh"
#include "gc_clock.hh"
#include "tombstone_gc.hh"
#include "locator/token_metadata.hh"
#include "exceptions/exceptions.hh"
#include "locator/abstract_replication_strategy.hh"
#include "replica/database.hh"
#include "gms/feature_service.hh"

extern logging::logger dblog;

class repair_history_map {
public:
    boost::icl::interval_map<dht::token, gc_clock::time_point, boost::icl::partial_absorber, std::less, boost::icl::inplace_max> map;
};

thread_local std::unordered_map<utils::UUID, seastar::lw_shared_ptr<repair_history_map>> repair_history_maps;

static seastar::lw_shared_ptr<repair_history_map> get_or_create_repair_history_map_for_table(const utils::UUID& id) {
    auto it = repair_history_maps.find(id);
    if (it != repair_history_maps.end()) {
        return it->second;
    } else {
        repair_history_maps[id] = seastar::make_lw_shared<repair_history_map>();
        return repair_history_maps[id];
    }
}

seastar::lw_shared_ptr<repair_history_map> get_repair_history_map_for_table(const utils::UUID& id) {
    auto it = repair_history_maps.find(id);
    if (it != repair_history_maps.end()) {
        return it->second;
    } else {
        return {};
    }
}

void drop_repair_history_map_for_table(const utils::UUID& id) {
    repair_history_maps.erase(id);
}

// This is useful for a sstable to query a gc_before for a range. The range is
// defined by the first and last key in the sstable.
//
// The min_gc_before and max_gc_before returned are the min and max gc_before for all the keys in the range.
//
// The knows_entire_range is set to true:
// 1) if the tombstone_gc_mode is not repair, since we have the same value for all the keys in the ranges.
// 2) if the tombstone_gc_mode is repair, and the range is a sub range of a range in the repair history map.
get_gc_before_for_range_result get_gc_before_for_range(schema_ptr s, const dht::token_range& range, const gc_clock::time_point& query_time) {
    bool knows_entire_range = true;
    const auto& options = s->tombstone_gc_options();
    switch (options.mode()) {
    case tombstone_gc_mode::timeout: {
        dblog.trace("Get gc_before for ks={}, table={}, range={}, mode=timeout", s->ks_name(), s->cf_name(), range);
        auto gc_before = saturating_subtract(query_time, s->gc_grace_seconds());
        return {gc_before, gc_before, knows_entire_range};
    }
    case tombstone_gc_mode::disabled: {
        dblog.trace("Get gc_before for ks={}, table={}, range={}, mode=disabled", s->ks_name(), s->cf_name(), range);
        return {gc_clock::time_point::min(), gc_clock::time_point::min(), knows_entire_range};
    }
    case tombstone_gc_mode::immediate: {
        dblog.trace("Get gc_before for ks={}, table={}, range={}, mode=immediate", s->ks_name(), s->cf_name(), range);
        return {gc_clock::time_point::max(), gc_clock::time_point::max(), knows_entire_range};
    }
    case tombstone_gc_mode::repair: {
        const std::chrono::seconds& propagation_delay = options.propagation_delay_in_seconds();
        auto min_gc_before = gc_clock::time_point::min();
        auto max_gc_before = gc_clock::time_point::min();
        auto min_repair_timestamp = gc_clock::time_point::min();
        auto max_repair_timestamp = gc_clock::time_point::min();
        int hits = 0;
        knows_entire_range = false;
        auto m = get_repair_history_map_for_table(s->id());
        if (m) {
            auto interval = locator::token_metadata::range_to_interval(range);
            auto min = gc_clock::time_point::max();
            auto max = gc_clock::time_point::min();
            bool contains_all = false;
            for (auto& x : boost::make_iterator_range(m->map.equal_range(interval))) {
                auto r = locator::token_metadata::interval_to_range(x.first);
                min = std::min(x.second, min);
                max = std::max(x.second, max);
                if (++hits == 1 && r.contains(range, dht::tri_compare)) {
                    contains_all = true;
                }
            }
            if (hits == 0) {
                min_repair_timestamp = gc_clock::time_point::min();
                max_repair_timestamp = gc_clock::time_point::min();
            } else {
                knows_entire_range = hits == 1 && contains_all;
                min_repair_timestamp = min;
                max_repair_timestamp = max;
            }
            min_gc_before = saturating_subtract(min_repair_timestamp, propagation_delay);
            max_gc_before = saturating_subtract(max_repair_timestamp, propagation_delay);
        };
        dblog.trace("Get gc_before for ks={}, table={}, range={}, mode=repair, min_repair_timestamp={}, max_repair_timestamp={}, propagation_delay={}, min_gc_before={}, max_gc_before={}, hits={}, knows_entire_range={}",
                s->ks_name(), s->cf_name(), range, min_repair_timestamp, max_repair_timestamp, propagation_delay.count(), min_gc_before, max_gc_before, hits, knows_entire_range);
        return {min_gc_before, max_gc_before, knows_entire_range};
    }
    }
}

gc_clock::time_point get_gc_before_for_key(schema_ptr s, const dht::decorated_key& dk, const gc_clock::time_point& query_time) {
    // if mode = timeout    // default option, if user does not specify tombstone_gc options
    // if mode = disabled   // never gc tombstone
    // if mode = immediate  // can gc tombstone immediately
    // if mode = repair     // gc after repair
    const auto& options = s->tombstone_gc_options();
    switch (options.mode()) {
    case tombstone_gc_mode::timeout:
        dblog.trace("Get gc_before for ks={}, table={}, dk={}, mode=timeout", s->ks_name(), s->cf_name(), dk);
        return saturating_subtract(query_time, s->gc_grace_seconds());
    case tombstone_gc_mode::disabled:
        dblog.trace("Get gc_before for ks={}, table={}, dk={}, mode=disabled", s->ks_name(), s->cf_name(), dk);
        return gc_clock::time_point::min();
    case tombstone_gc_mode::immediate:
        dblog.trace("Get gc_before for ks={}, table={}, dk={}, mode=immediate", s->ks_name(), s->cf_name(), dk);
        return gc_clock::time_point::max();
    case tombstone_gc_mode::repair:
        const std::chrono::seconds& propagation_delay = options.propagation_delay_in_seconds();
        auto gc_before = gc_clock::time_point::min();
        auto repair_timestamp = gc_clock::time_point::min();
        auto m = get_repair_history_map_for_table(s->id());
        if (m) {
            const auto it = m->map.find(dk.token());
            if (it == m->map.end()) {
                gc_before = gc_clock::time_point::min();
            } else {
                repair_timestamp = it->second;
                gc_before = saturating_subtract(repair_timestamp, propagation_delay);
            }
        }
        dblog.trace("Get gc_before for ks={}, table={}, dk={}, mode=repair, repair_timestamp={}, propagation_delay={}, gc_before={}",
                s->ks_name(), s->cf_name(), dk, repair_timestamp, propagation_delay.count(), gc_before);
        return gc_before;
    }
}

void update_repair_time(schema_ptr s, const dht::token_range& range, gc_clock::time_point repair_time) {
    auto m = get_or_create_repair_history_map_for_table(s->id());
    m->map += std::make_pair(locator::token_metadata::range_to_interval(range), repair_time);
}

static bool needs_repair_before_gc(const replica::database& db, sstring ks_name) {
    // If a table uses local replication strategy or rf one, there is no
    // need to run repair even if tombstone_gc mode = repair.
    auto& ks = db.find_keyspace(ks_name);
    auto& rs = ks.get_replication_strategy();
    auto erm = ks.get_effective_replication_map();
    bool needs_repair = rs.get_type() != locator::replication_strategy_type::local
            && erm->get_replication_factor() != 1;
    return needs_repair;
}

void validate_tombstone_gc_options(const tombstone_gc_options* options, const replica::database& db, sstring ks_name) {
    if (!options) {
        return;
    }
    if (!db.features().cluster_supports_tombstone_gc_options()) {
        throw exceptions::configuration_exception("tombstone_gc option not supported by the cluster");
    }

    if (options->mode() == tombstone_gc_mode::repair && !needs_repair_before_gc(db, ks_name)) {
        throw exceptions::configuration_exception("tombstone_gc option with mode = repair not supported for table with RF one or local replication strategy");
    }
}
