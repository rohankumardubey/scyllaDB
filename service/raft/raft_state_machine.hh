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
#pragma once

#include "gms/inet_address.hh"
#include "raft/raft.hh"

namespace service {

// Scylla specific extention for raft state machine
// Snapshot transfer is delegated to a state machine implementation
class raft_state_machine : public raft::state_machine {
public:
    virtual future<> transfer_snapshot(gms::inet_address from, raft::snapshot_id snp) = 0;
};

} // end of namespace service
