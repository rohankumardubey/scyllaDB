
/*
 * Copyright (C) 2020-present ScyllaDB
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

#include "raft_address_map.hh"

namespace gms {
class gossiper;
}

namespace service {

// Scylla-specific implementation of raft failure detector module.
//
// Currently uses gossiper as underlying implementation to test for `is_alive(gms::inet_address)`.
// Gets the mapping from server id to gms::inet_address from RPC module.
class raft_gossip_failure_detector : public raft::failure_detector {
    gms::gossiper& _gossip;
    raft_address_map<>& _address_map;

public:
    raft_gossip_failure_detector(gms::gossiper& gs, raft_address_map<>& address_map);

    bool is_alive(raft::server_id server) override;
};

} // end of namespace service
