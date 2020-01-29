/*
 * Copyright (C) 2020 ScyllaDB
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

#include <algorithm>
#include <limits>
#include <ostream>
#include <utility>

#include "dht/token.hh"
#include "dht/i_partitioner.hh"

namespace dht {

inline int64_t long_token(const token& t) {
    if (t.is_minimum() || t.is_maximum()) {
        return std::numeric_limits<long>::min();
    }

    return t._data;
}

static const token min_token{ token::kind::before_all_keys, 0 };
static const token max_token{ token::kind::after_all_keys, 0 };

const token&
minimum_token() {
    return min_token;
}

const token&
maximum_token() {
    return max_token;
}

int tri_compare(const token& t1, const token& t2) {
    if (t1._kind < t2._kind) {
            return -1;
    } else if (t1._kind > t2._kind) {
            return 1;
    } else if (t1._kind == token_kind::key) {
        auto l1 = long_token(t1);
        auto l2 = long_token(t2);
        if (l1 == l2) {
            return 0;
        } else {
            return l1 < l2 ? -1 : 1;
        }
    }
    return 0;
}

std::ostream& operator<<(std::ostream& out, const token& t) {
    if (t._kind == token::kind::after_all_keys) {
        out << "maximum token";
    } else if (t._kind == token::kind::before_all_keys) {
        out << "minimum token";
    } else {
        out << t.to_sstring();
    }
    return out;
}

sstring token::to_sstring() const {
    return seastar::to_sstring<sstring>(long_token(*this));
}

// Assuming that x>=y, return the positive difference x-y.
// The return type is an unsigned type, as the difference may overflow
// a signed type (e.g., consider very positive x and very negative y).
template <typename T>
static std::make_unsigned_t<T> positive_subtract(T x, T y) {
        return std::make_unsigned_t<T>(x) - std::make_unsigned_t<T>(y);
}

token token::midpoint(const token& t1, const token& t2) {
    auto l1 = long_token(t1);
    auto l2 = long_token(t2);
    int64_t mid;
    if (l1 <= l2) {
        // To find the midpoint, we cannot use the trivial formula (l1+l2)/2
        // because the addition can overflow the integer. To avoid this
        // overflow, we first notice that the above formula is equivalent to
        // l1 + (l2-l1)/2. Now, "l2-l1" can still overflow a signed integer
        // (e.g., think of a very positive l2 and very negative l1), but
        // because l1 <= l2 in this branch, we note that l2-l1 is positive
        // and fits an *unsigned* int's range. So,
        mid = l1 + positive_subtract(l2, l1)/2;
    } else {
        // When l2 < l1, we need to switch l1 and and l2 in the above
        // formula, because now l1 - l2 is positive.
        // Additionally, we consider this case is a "wrap around", so we need
        // to behave as if l2 + 2^64 was meant instead of l2, i.e., add 2^63
        // to the average.
        mid = l2 + positive_subtract(l1, l2)/2 + 0x8000'0000'0000'0000;
    }
    return token{kind::key, mid};
}

token token::get_random_token() {
    return {kind::key, dht::get_random_number<int64_t>()};
}

token token::from_sstring(const sstring& t) {
    auto lp = boost::lexical_cast<long>(t);
    if (lp == std::numeric_limits<long>::min()) {
        return minimum_token();
    } else {
        return token(kind::key, uint64_t(lp));
    }
}

token token::from_bytes(bytes_view bytes) {
    if (bytes.size() != sizeof(int64_t)) {
        throw runtime_exception(format("Invalid token. Should have size {:d}, has size {:d}\n", sizeof(int64_t), bytes.size()));
    }

    int64_t v;
    std::copy_n(bytes.begin(), sizeof(v), reinterpret_cast<int8_t *>(&v));
    auto tok = net::ntoh(v);
    if (tok == std::numeric_limits<int64_t>::min()) {
        return minimum_token();
    } else {
        return dht::token(dht::token::kind::key, tok);
    }
}

static float ratio_helper(int64_t a, int64_t b) {

    uint64_t val = (a > b)? static_cast<uint64_t>(a) - static_cast<uint64_t>(b) : (static_cast<uint64_t>(a) - static_cast<uint64_t>(b) - 1);
    return val/(float)std::numeric_limits<uint64_t>::max();
}

std::map<token, float>
token::describe_ownership(const std::vector<token>& sorted_tokens) {
    std::map<token, float> ownerships;
    auto i = sorted_tokens.begin();

    // 0-case
    if (i == sorted_tokens.end()) {
        throw runtime_exception("No nodes present in the cluster. Has this node finished starting up?");
    }
    // 1-case
    if (sorted_tokens.size() == 1) {
        ownerships[sorted_tokens[0]] = 1.0;
    // n-case
    } else {
        const token& start = sorted_tokens[0];

        int64_t ti = long_token(start);  // The first token and its value
        int64_t start_long = ti;
        int64_t tim1 = ti; // The last token and its value (after loop)
        for (i++; i != sorted_tokens.end(); i++) {
            ti = long_token(*i); // The next token and its value
            ownerships[*i]= ratio_helper(ti, tim1);  // save (T(i) -> %age)
            tim1 = ti;
        }

        // The start token's range extends backward to the last token, which is why both were saved above.
        ownerships[start] = ratio_helper(start_long, ti);
    }

    return ownerships;
}

} // namespace dht
