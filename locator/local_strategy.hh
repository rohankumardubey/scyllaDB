/*
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

#include "abstract_replication_strategy.hh"

#include <optional>
#include <set>

// forward declaration since replica/database.hh includes this file
class keyspace;

namespace locator {

using inet_address = gms::inet_address;
using token = dht::token;

class local_strategy : public abstract_replication_strategy {
public:
    local_strategy(snitch_ptr& snitch, const replication_strategy_config_options& config_options);
    virtual ~local_strategy() {};
    virtual size_t get_replication_factor(const token_metadata&) const override;

    virtual future<inet_address_vector_replica_set> calculate_natural_endpoints(const token& search_token, const token_metadata& tm) const override;

    virtual void validate_options() const override;

    virtual std::optional<std::set<sstring>> recognized_options(const topology&) const override;

    virtual bool allow_remove_node_being_replaced_from_natural_endpoints() const override {
        return false;
    }

    /**
     * We need to override this because the default implementation depends
     * on token calculations but LocalStrategy may be used before tokens are set up.
     */
    inet_address_vector_replica_set get_natural_endpoints(const token&, const effective_replication_map&) const override;
};

}
