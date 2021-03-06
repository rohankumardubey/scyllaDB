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

#pragma once

#include <seastar/core/seastar.hh>
#include <seastar/core/sstring.hh>
#include <utility>
#include <optional>
#include "dht/token.hh"
#include "seastarx.hh"

namespace dht {

class token;

}

namespace db {

using system_keyspace_view_name = std::pair<sstring, sstring>;

struct system_keyspace_view_build_progress {
    system_keyspace_view_name view;
    dht::token first_token;
    std::optional<dht::token> next_token;
    shard_id cpu_id;
};

}
