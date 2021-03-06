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

#include "cql3/statements/raw/cf_statement.hh"
#include "cql3/statements/raw/modification_statement.hh"
#include "service/client_state.hh"

namespace cql3 {

namespace statements {

namespace raw {

class modification_statement;

class batch_statement : public raw::cf_statement {
public:
    enum class type {
        LOGGED, UNLOGGED, COUNTER
    };
private:
    type _type;
    std::unique_ptr<attributes::raw> _attrs;
    std::vector<std::unique_ptr<raw::modification_statement>> _parsed_statements;
public:
    batch_statement(
        type type_,
        std::unique_ptr<attributes::raw> attrs,
        std::vector<std::unique_ptr<raw::modification_statement>> parsed_statements)
            : cf_statement(std::nullopt)
            , _type(type_)
            , _attrs(std::move(attrs))
            , _parsed_statements(std::move(parsed_statements)) {
    }

    virtual void prepare_keyspace(const service::client_state& state) override {
        for (auto&& s : _parsed_statements) {
            s->prepare_keyspace(state);
        }
    }

    virtual std::unique_ptr<prepared_statement> prepare(data_dictionary::database db, cql_stats& stats) override;
};

}

}
}
