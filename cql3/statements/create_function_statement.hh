/*
 * Copyright (C) 2019-present ScyllaDB
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

#include "cql3/statements/function_statement.hh"
#include "cql3/cql3_type.hh"

namespace cql3 {

class query_processor;

namespace functions {
    class user_function;
}

namespace statements {

class create_function_statement final : public create_function_statement_base {
    virtual std::unique_ptr<prepared_statement> prepare(data_dictionary::database db, cql_stats& stats) override;
    future<std::pair<::shared_ptr<cql_transport::event::schema_change>, std::vector<mutation>>> prepare_schema_mutations(query_processor& qp) const override;

    virtual shared_ptr<functions::function> create(query_processor& qp, functions::function* old) const override;
    sstring _language;
    sstring _body;
    std::vector<shared_ptr<column_identifier>> _arg_names;
    shared_ptr<cql3_type::raw> _return_type;
    bool _called_on_null_input;

public:
    create_function_statement(functions::function_name name, sstring language, sstring body,
            std::vector<shared_ptr<column_identifier>> arg_names, std::vector<shared_ptr<cql3_type::raw>> arg_types,
            shared_ptr<cql3_type::raw> return_type, bool called_on_null_input, bool or_replace, bool if_not_exists);
};
}
}
