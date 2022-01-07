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

#include <fmt/core.h>
#include <ostream>
#include <seastar/core/shared_ptr.hh>
#include <variant>
#include <concepts>
#include <numeric>

#include "bytes.hh"
#include "cql3/statements/bound.hh"
#include "cql3/column_identifier.hh"
#include "cql3/assignment_testable.hh"
#include "cql3/cql3_type.hh"
#include "cql3/functions/function_name.hh"
#include "data_dictionary/data_dictionary.hh"
#include "gc_clock.hh"
#include "range.hh"
#include "seastarx.hh"
#include "utils/overloaded_functor.hh"
#include "utils/variant_element.hh"
#include "cql3/values.hh"
#include "replica/database_fwd.hh"

class row;

namespace secondary_index {
class index;
class secondary_index_manager;
} // namespace secondary_index

namespace query {
    class result_row_view;
} // namespace query

namespace cql3 {
struct prepare_context;

class column_identifier_raw;
class query_options;

namespace selection {
    class selection;
} // namespace selection

namespace functions {

class function;

}

namespace expr {

struct allow_local_index_tag {};
using allow_local_index = bool_class<allow_local_index_tag>;

struct binary_operator;
struct conjunction;
struct column_value;
struct token;
struct unresolved_identifier;
struct column_mutation_attribute;
struct function_call;
struct cast;
struct field_selection;
struct null;
struct bind_variable;
struct untyped_constant;
struct constant;
struct tuple_constructor;
struct collection_constructor;
struct usertype_constructor;

template <typename T>
concept ExpressionElement
        = std::same_as<T, conjunction>
        || std::same_as<T, binary_operator>
        || std::same_as<T, column_value>
        || std::same_as<T, token>
        || std::same_as<T, unresolved_identifier>
        || std::same_as<T, column_mutation_attribute>
        || std::same_as<T, function_call>
        || std::same_as<T, cast>
        || std::same_as<T, field_selection>
        || std::same_as<T, null>
        || std::same_as<T, bind_variable>
        || std::same_as<T, untyped_constant>
        || std::same_as<T, constant>
        || std::same_as<T, tuple_constructor>
        || std::same_as<T, collection_constructor>
        || std::same_as<T, usertype_constructor>
        ;

template <typename Func>
concept invocable_on_expression
        = std::invocable<Func, conjunction>
        && std::invocable<Func, binary_operator>
        && std::invocable<Func, column_value>
        && std::invocable<Func, token>
        && std::invocable<Func, unresolved_identifier>
        && std::invocable<Func, column_mutation_attribute>
        && std::invocable<Func, function_call>
        && std::invocable<Func, cast>
        && std::invocable<Func, field_selection>
        && std::invocable<Func, null>
        && std::invocable<Func, bind_variable>
        && std::invocable<Func, untyped_constant>
        && std::invocable<Func, constant>
        && std::invocable<Func, tuple_constructor>
        && std::invocable<Func, collection_constructor>
        && std::invocable<Func, usertype_constructor>
        ;

template <typename Func>
concept invocable_on_expression_ref
        = std::invocable<Func, conjunction&>
        && std::invocable<Func, binary_operator&>
        && std::invocable<Func, column_value&>
        && std::invocable<Func, token&>
        && std::invocable<Func, unresolved_identifier&>
        && std::invocable<Func, column_mutation_attribute&>
        && std::invocable<Func, function_call&>
        && std::invocable<Func, cast&>
        && std::invocable<Func, field_selection&>
        && std::invocable<Func, null&>
        && std::invocable<Func, bind_variable&>
        && std::invocable<Func, untyped_constant&>
        && std::invocable<Func, constant&>
        && std::invocable<Func, tuple_constructor&>
        && std::invocable<Func, collection_constructor&>
        && std::invocable<Func, usertype_constructor&>
        ;

/// A CQL expression -- union of all possible expression types.  bool means a Boolean constant.
class expression final {
    // 'impl' holds a variant of all expression types, but since 
    // variants of incomplete types are not allowed, we forward declare it
    // here and fully define it later.
    struct impl;                 
    std::unique_ptr<impl> _v;
public:
    expression(); // FIXME: remove
    expression(ExpressionElement auto e);

    expression(const expression&);
    expression(expression&&) noexcept = default;
    expression& operator=(const expression&);
    expression& operator=(expression&&) noexcept = default;

    friend auto visit(invocable_on_expression auto&& visitor, const expression& e);
    friend auto visit(invocable_on_expression_ref auto&& visitor, expression& e);

    template <ExpressionElement E>
    friend bool is(const expression& e);

    template <ExpressionElement E>
    friend const E& as(const expression& e);

    template <ExpressionElement E>
    friend const E* as_if(const expression* e);
};

// An expression that doesn't contain subexpressions
template <typename E>
concept LeafExpression
        = std::same_as<bool, E>
        || std::same_as<token, E> 
        || std::same_as<unresolved_identifier, E> 
        || std::same_as<null, E> 
        || std::same_as<bind_variable, E> 
        || std::same_as<untyped_constant, E> 
        || std::same_as<constant, E>
        ;

/// A column, optionally subscripted by a value (eg, c1 or c2['abc']).
struct column_value {
    const column_definition* col;
    std::optional<expression> sub; ///< If present, this LHS is col[sub], otherwise just col.
    /// For easy creation of vector<column_value> from vector<column_definition*>.
    column_value(const column_definition* col) : col(col) {}
    /// The compiler doesn't auto-generate this due to the other constructor's existence.
    column_value(const column_definition* col, expr::expression sub) : col(col), sub(std::move(sub)) {}
};

/// Represents token function on LHS of an operator relation.  No need to list column definitions
/// here -- token takes exactly the partition key as its argument.
struct token {};

enum class oper_t { EQ, NEQ, LT, LTE, GTE, GT, IN, CONTAINS, CONTAINS_KEY, IS_NOT, LIKE };

/// Describes the nature of clustering-key comparisons.  Useful for implementing SCYLLA_CLUSTERING_BOUND.
enum class comparison_order : char {
    cql, ///< CQL order. (a,b)>(1,1) is equivalent to a>1 OR (a=1 AND b>1).
    clustering, ///< Table's clustering order. (a,b)>(1,1) means any row past (1,1) in storage.
};

/// Operator restriction: LHS op RHS.
struct binary_operator {
    expression lhs;
    oper_t op;
    expression rhs;
    comparison_order order;

    binary_operator(expression lhs, oper_t op, expression rhs, comparison_order order = comparison_order::cql);
};

/// A conjunction of restrictions.
struct conjunction {
    std::vector<expression> children;
};

// Gets resolved eventually into a column_value.
struct unresolved_identifier {
    ::shared_ptr<column_identifier_raw> ident;

    ~unresolved_identifier();
};

// An attribute attached to a column mutation: writetime or ttl
struct column_mutation_attribute {
    enum class attribute_kind { writetime, ttl };

    attribute_kind kind;
    // note: only unresolved_identifier is legal here now. One day, when prepare()
    // on expressions yields expressions, column_value will also be legal here.
    expression column;
};

struct function_call {
    std::variant<functions::function_name, shared_ptr<functions::function>> func;
    std::vector<expression> args;

    // 0-based index of the function call within a CQL statement.
    // Used to populate the cache of execution results while passing to
    // another shard (handling `bounce_to_shard` messages) in LWT statements.
    //
    // The id is set only for the function calls that are a part of LWT
    // statement restrictions for the partition key. Otherwise, the id is not
    // set and the call is not considered when using or populating the cache.
    //
    // For example in a query like:
    // INSERT INTO t (pk) VALUES (uuid()) IF NOT EXISTS
    // The query should be executed on a shard that has the pk partition,
    // but it changes with each uuid() call.
    // uuid() call result is cached and sent to the proper shard.
    //
    // Cache id is kept in shared_ptr because of how prepare_context works.
    // During fill_prepare_context all function cache ids are collected
    // inside prepare_context.
    // Later when some condition occurs we might decide to clear
    // cache ids of all function calls found in prepare_context.
    // However by this time these function calls could have been
    // copied multiple times. Prepare_context keeps a shared_ptr
    // to function_call ids, and then clearing the shared id
    // clears it in all possible copies.
    // This logic was introduced back when everything was shared_ptr<term>,
    // now a better solution might exist.
    //
    // This field can be nullptr, it means that there is no cache id set.
    ::shared_ptr<std::optional<uint8_t>> lwt_cache_id;
};

struct cast {
    expression arg;
    std::variant<cql3_type, shared_ptr<cql3_type::raw>> type;
};

struct field_selection {
    expression structure;
    shared_ptr<column_identifier_raw> field;
};

struct null {
};

struct bind_variable {
    enum class shape_type { scalar, scalar_in, tuple, tuple_in };
    // FIXME: infer shape from expression rather than from grammar
    shape_type shape;
    int32_t bind_index;

    // Describes where this bound value will be assigned.
    // Contains value type and other useful information.
    ::lw_shared_ptr<column_specification> receiver;
};

// A constant which does not yet have a date type. It is partially typed
// (we know if it's floating or int) but not sized.
struct untyped_constant {
    enum type_class { integer, floating_point, string, boolean, duration, uuid, hex };
    type_class partial_type;
    sstring raw_text;
};

// Represents a constant value with known value and type
// For null and unset the type can sometimes be set to empty_type
struct constant {
    // A value serialized using the internal (latest) cql_serialization_format
    cql3::raw_value value;

    // Never nullptr, for NULL and UNSET might be empty_type
    data_type type;

    constant(cql3::raw_value value, data_type type);
    static constant make_null(data_type val_type = empty_type);
    static constant make_unset_value(data_type val_type = empty_type);
    static constant make_bool(bool bool_val);

    bool is_null() const;
    bool is_unset_value() const;
    bool is_null_or_unset() const;
    bool has_empty_value_bytes() const;

    cql3::raw_value_view view() const;
};

// Denotes construction of a tuple from its elements, e.g.  ('a', ?, some_column) in CQL.
struct tuple_constructor {
    std::vector<expression> elements;

    // Might be nullptr before prepare.
    // After prepare always holds a valid type, although it might be reversed_type(tuple_type).
    data_type type;
};

// Constructs a collection of same-typed elements
struct collection_constructor {
    enum class style_type { list, set, map };
    style_type style;
    std::vector<expression> elements;

    // Might be nullptr before prepare.
    // After prepare always holds a valid type, although it might be reversed_type(collection_type).
    data_type type;
};

// Constructs an object of a user-defined type
struct usertype_constructor {
    using elements_map_type = std::unordered_map<column_identifier, expression>;
    elements_map_type elements;

    // Might be nullptr before prepare.
    // After prepare always holds a valid type, although it might be reversed_type(user_type).
    data_type type;
};

// now that all expression types are fully defined, we can define expression::impl
struct expression::impl final {
    using variant_type = std::variant<
            conjunction, binary_operator, column_value, token, unresolved_identifier,
            column_mutation_attribute, function_call, cast, field_selection, null,
            bind_variable, untyped_constant, constant, tuple_constructor, collection_constructor,
            usertype_constructor>;
    variant_type v;
    impl(variant_type v) : v(std::move(v)) {}
};

expression::expression(ExpressionElement auto e)
        : _v(std::make_unique<impl>(std::move(e))) {
}

inline expression::expression()
        : expression(conjunction{}) {
}

auto visit(invocable_on_expression auto&& visitor, const expression& e) {
    return std::visit(visitor, e._v->v);
}

auto visit(invocable_on_expression_ref auto&& visitor, expression& e) {
    return std::visit(visitor, e._v->v);
}

template <ExpressionElement E>
bool is(const expression& e) {
    return std::holds_alternative<E>(e._v->v);
}

template <ExpressionElement E>
const E& as(const expression& e) {
    return std::get<E>(e._v->v);
}

template <ExpressionElement E>
const E* as_if(const expression* e) {
    return std::get_if<E>(&e->_v->v);
}

/// Creates a conjunction of a and b.  If either a or b is itself a conjunction, its children are inserted
/// directly into the resulting conjunction's children, flattening the expression tree.
extern expression make_conjunction(expression a, expression b);

extern std::ostream& operator<<(std::ostream&, oper_t);

/// True iff restr is satisfied with respect to the row provided from a partition slice.
extern bool is_satisfied_by(
        const expression& restr,
        const std::vector<bytes>& partition_key, const std::vector<bytes>& clustering_key,
        const query::result_row_view& static_row, const query::result_row_view* row,
        const selection::selection&, const query_options&);

/// A set of discrete values.
using value_list = std::vector<managed_bytes>; // Sorted and deduped using value comparator.

/// General set of values.  Empty set and single-element sets are always value_list.  nonwrapping_range is
/// never singular and never has start > end.  Universal set is a nonwrapping_range with both bounds null.
using value_set = std::variant<value_list, nonwrapping_range<managed_bytes>>;

/// A set of all column values that would satisfy an expression.  If column is null, a set of all token values
/// that satisfy.
///
/// An expression restricts possible values of a column or token:
/// - `A>5` restricts A from below
/// - `A>5 AND A>6 AND B<10 AND A=12 AND B>0` restricts A to 12 and B to between 0 and 10
/// - `A IN (1, 3, 5)` restricts A to 1, 3, or 5
/// - `A IN (1, 3, 5) AND A>3` restricts A to just 5
/// - `A=1 AND A<=0` restricts A to an empty list; no value is able to satisfy the expression
/// - `A>=NULL` also restricts A to an empty list; all comparisons to NULL are false
/// - an expression without A "restricts" A to unbounded range
extern value_set possible_lhs_values(const column_definition*, const expression&, const query_options&);

/// Turns value_set into a range, unless it's a multi-valued list (in which case this throws).
extern nonwrapping_range<managed_bytes> to_range(const value_set&);

/// A range of all X such that X op val.
nonwrapping_range<clustering_key_prefix> to_range(oper_t op, const clustering_key_prefix& val);

/// True iff the index can support the entire expression.
extern bool is_supported_by(const expression&, const secondary_index::index&);

/// True iff any of the indices from the manager can support the entire expression.  If allow_local, use all
/// indices; otherwise, use only global indices.
extern bool has_supporting_index(
        const expression&, const secondary_index::secondary_index_manager&, allow_local_index allow_local);

extern sstring to_string(const expression&);

extern std::ostream& operator<<(std::ostream&, const column_value&);

extern std::ostream& operator<<(std::ostream&, const expression&);

extern bool recurse_until(const expression& e, const noncopyable_function<bool (const expression&)>& predicate_fun);

// Looks into the expression and finds the given expression variant
// for which the predicate function returns true.
// If nothing is found returns nullptr.
// For example:
// find_in_expression<binary_operator>(e, [](const binary_operator&) {return true;})
// Will return the first binary operator found in the expression
template<ExpressionElement ExprElem, class Fn>
requires std::invocable<Fn, const ExprElem&>
      && std::same_as<std::invoke_result_t<Fn, const ExprElem&>, bool>
const ExprElem* find_in_expression(const expression& e, Fn predicate_fun) {
    const ExprElem* ret = nullptr;
    recurse_until(e, [&] (const expression& e) {
        if (auto expr_elem = as_if<ExprElem>(&e)) {
            if (predicate_fun(*expr_elem)) {
                ret = expr_elem;
                return true;
            }
        }
        return false;
    });
    return ret;
}

/// If there is a binary_operator atom b for which f(b) is true, returns it.  Otherwise returns null.
template<class Fn>
requires std::invocable<Fn, const binary_operator&>
      && std::same_as<std::invoke_result_t<Fn, const binary_operator&>, bool>
const binary_operator* find_binop(const expression& e, Fn predicate_fun) {
    return find_in_expression<binary_operator>(e, predicate_fun);
}

/// Counts binary_operator atoms b for which f(b) is true.
size_t count_if(const expression& e, const noncopyable_function<bool (const binary_operator&)>& f);

inline const binary_operator* find(const expression& e, oper_t op) {
    return find_binop(e, [&] (const binary_operator& o) { return o.op == op; });
}

inline bool needs_filtering(oper_t op) {
    return (op == oper_t::CONTAINS) || (op == oper_t::CONTAINS_KEY) || (op == oper_t::LIKE) ||
           (op == oper_t::IS_NOT) || (op == oper_t::NEQ) ;
}

inline auto find_needs_filtering(const expression& e) {
    return find_binop(e, [] (const binary_operator& bo) { return needs_filtering(bo.op); });
}

inline bool is_slice(oper_t op) {
    return (op == oper_t::LT) || (op == oper_t::LTE) || (op == oper_t::GT) || (op == oper_t::GTE);
}

inline bool has_slice(const expression& e) {
    return find_binop(e, [] (const binary_operator& bo) { return is_slice(bo.op); });
}

inline bool is_compare(oper_t op) {
    switch (op) {
    case oper_t::EQ:
    case oper_t::LT:
    case oper_t::LTE:
    case oper_t::GT:
    case oper_t::GTE:
    case oper_t::NEQ:
        return true;
    default:
        return false;
    }
}

inline bool is_multi_column(const binary_operator& op) {
    return expr::is<tuple_constructor>(op.lhs);
}

inline bool has_token(const expression& e) {
    return find_binop(e, [] (const binary_operator& o) { return expr::is<token>(o.lhs); });
}

inline bool has_slice_or_needs_filtering(const expression& e) {
    return find_binop(e, [] (const binary_operator& o) { return is_slice(o.op) || needs_filtering(o.op); });
}

inline bool is_clustering_order(const binary_operator& op) {
    return op.order == comparison_order::clustering;
}

inline auto find_clustering_order(const expression& e) {
    return find_binop(e, is_clustering_order);
}

/// True iff binary_operator involves a collection.
extern bool is_on_collection(const binary_operator&);

// Checks whether the given column occurs in the expression.
// Uses column_defintion::operator== for comparison, columns with the same name but different schema will not be equal.
bool contains_column(const column_definition& column, const expression& e);

// Checks whether this expression contains a nonpure function.
// The expression must be prepared, so that function names are converted to function pointers.
bool contains_nonpure_function(const expression&);

// Checks whether the given column has an EQ restriction in the expression.
// EQ restriction is `col = ...` or `(col, col2) = ...`
// IN restriction is NOT an EQ restriction, this function will not look for IN restrictions.
// Uses column_defintion::operator== for comparison, columns with the same name but different schema will not be equal.
bool has_eq_restriction_on_column(const column_definition& column, const expression& e);

/// Replaces every column_definition in an expression with this one.  Throws if any LHS is not a single
/// column_value.
extern expression replace_column_def(const expression&, const column_definition*);

// Replaces all occurences of token(p1, p2) on the left hand side with the given colum.
// For example this changes token(p1, p2) < token(1, 2) to my_column_name < token(1, 2).
extern expression replace_token(const expression&, const column_definition*);

// Recursively copies e and returns it. Calls replace_candidate() on all nodes. If it returns nullopt,
// continue with the copying. If it returns an expression, that expression replaces the current node.
extern expression search_and_replace(const expression& e,
        const noncopyable_function<std::optional<expression> (const expression& candidate)>& replace_candidate);

extern expression prepare_expression(const expression& expr, data_dictionary::database db, const sstring& keyspace, lw_shared_ptr<column_specification> receiver);
extern expression prepare_expression_multi_column(const expression& expr, data_dictionary::database db, const sstring& keyspace, const std::vector<lw_shared_ptr<column_specification>>& receivers);


/**
 * @return whether this object can be assigned to the provided receiver. We distinguish
 * between 3 values: 
 *   - EXACT_MATCH if this object is exactly of the type expected by the receiver
 *   - WEAKLY_ASSIGNABLE if this object is not exactly the expected type but is assignable nonetheless
 *   - NOT_ASSIGNABLE if it's not assignable
 * Most caller should just call the is_assignable() method on the result, though functions have a use for
 * testing "strong" equality to decide the most precise overload to pick when multiple could match.
 */
extern assignment_testable::test_result test_assignment(const expression& expr, data_dictionary::database db, const sstring& keyspace, const column_specification& receiver);

// Test all elements of exprs for assignment. If all are exact match, return exact match. If any is not assignable,
// return not assignable. Otherwise, return weakly assignable.
extern assignment_testable::test_result test_assignment_all(const std::vector<expression>& exprs, data_dictionary::database db, const sstring& keyspace, const column_specification& receiver);

extern shared_ptr<assignment_testable> as_assignment_testable(expression e);

inline oper_t pick_operator(statements::bound b, bool inclusive) {
    return is_start(b) ?
            (inclusive ? oper_t::GTE : oper_t::GT) :
            (inclusive ? oper_t::LTE : oper_t::LT);
}

// Extracts all binary operators which have the given column on their left hand side.
// Extracts only single-column restrictions.
// Does not include multi-column restrictions.
// Does not include token() restrictions.
// Does not include boolean constant restrictions.
// For example "WHERE c = 1 AND (a, c) = (2, 1) AND token(p) < 2 AND FALSE" will return {"c = 1"}.
std::vector<expression> extract_single_column_restrictions_for_column(const expression&, const column_definition&);

std::optional<bool> get_bool_value(const constant&);

// Similar to evaluate(), but ignores any NULL values in the final list value.
// In an IN restriction nulls can be ignored, because nothing equals NULL.
constant evaluate_IN_list(const expression&, const query_options&);

// Takes a prepared expression and calculates its value.
// Evaluates bound values, calls functions and returns just the bytes and type.
constant evaluate(const expression& e, const query_options&);
constant evaluate(const bind_variable&, const query_options&);
constant evaluate(const tuple_constructor&, const query_options&);
constant evaluate(const collection_constructor&, const query_options&);
constant evaluate(const usertype_constructor&, const query_options&);
constant evaluate(const function_call&, const query_options&);

utils::chunked_vector<managed_bytes> get_list_elements(const constant&);
utils::chunked_vector<managed_bytes> get_set_elements(const constant&);
std::vector<managed_bytes_opt> get_tuple_elements(const constant&);
std::vector<managed_bytes_opt> get_user_type_elements(const constant&);
std::vector<std::pair<managed_bytes, managed_bytes>> get_map_elements(const constant&);

// Gets the elements of a constant which can be a list, set, tuple or user type
std::vector<managed_bytes_opt> get_elements(const constant&);

// Get elements of list<tuple<>> as vector<vector<managed_bytes_opt>
// It is useful with IN restrictions like (a, b) IN [(1, 2), (3, 4)].
utils::chunked_vector<std::vector<managed_bytes_opt>> get_list_of_tuples_elements(const constant&);

// Retrieves information needed in prepare_context.
// Collects the column specification for the bind variables in this expression.
// Sets lwt_cache_id field in function_calls.
void fill_prepare_context(expression&, cql3::prepare_context&);

// Checks whether there is a bind_variable inside this expression
// It's important to note, that even when there are no bind markers,
// there can be other things that prevent immediate evaluation of an expression.
// For example an expression can contain calls to nonpure functions.
bool contains_bind_marker(const expression& e);
} // namespace expr

} // namespace cql3

/// Required for fmt::join() to work on expression.
template <>
struct fmt::formatter<cql3::expr::expression> {
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.end();
    }

    template <typename FormatContext>
    auto format(const cql3::expr::expression& expr, FormatContext& ctx) {
        std::ostringstream os;
        os << expr;
        return format_to(ctx.out(), "{}", os.str());
    }
};

/// Required for fmt::join() to work on column_value.
template <>
struct fmt::formatter<cql3::expr::column_value> {
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.end();
    }

    template <typename FormatContext>
    auto format(const cql3::expr::column_value& col, FormatContext& ctx) {
        std::ostringstream os;
        os << col;
        return format_to(ctx.out(), "{}", os.str());
    }
};
