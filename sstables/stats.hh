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

#include <cstdint>

namespace sstables {

class sstables_stats {
    static thread_local struct stats {
        uint64_t partition_writes = 0;
        uint64_t static_row_writes = 0;
        uint64_t row_writes = 0;
        uint64_t tombstone_writes = 0;
        uint64_t range_tombstone_writes = 0;
        uint64_t range_tombstone_reads = 0;
        uint64_t row_tombstone_reads = 0;
        uint64_t cell_writes = 0;
        uint64_t cell_tombstone_writes = 0;
        uint64_t single_partition_reads = 0;
        uint64_t range_partition_reads = 0;
        uint64_t partition_reads = 0;
        uint64_t partition_seeks = 0;
        uint64_t row_reads = 0;
        uint64_t capped_local_deletion_time = 0;
        uint64_t capped_tombstone_deletion_time = 0;
        uint64_t open_for_reading = 0;
        uint64_t closed_for_reading = 0;
        uint64_t open_for_writing = 0;
        uint64_t closed_for_writing = 0;
        uint64_t deleted = 0;
    } _shard_stats;

    stats& _stats = _shard_stats;

public:
    static const stats& get_shard_stats() noexcept {
        return _shard_stats;
    }

    inline void on_partition_write() noexcept {
        ++_stats.partition_writes;
    }

    inline void on_static_row_write() noexcept {
        ++_stats.static_row_writes;
    }

    inline void on_row_write() noexcept {
        ++_stats.row_writes;
    }

    inline void on_tombstone_write() noexcept {
        ++_stats.tombstone_writes;
    }

    inline void on_range_tombstone_write() noexcept {
        ++_stats.range_tombstone_writes;
    }

    inline void on_range_tombstone_read() noexcept {
        ++_stats.range_tombstone_reads;
    }

    inline void on_row_tombstone_read() noexcept {
        ++_stats.row_tombstone_reads;
    }

    inline void on_cell_write() noexcept {
        ++_stats.cell_writes;
    }

    inline void on_cell_tombstone_write() noexcept {
        ++_stats.cell_tombstone_writes;
    }

    inline void on_single_partition_read() noexcept {
        ++_stats.single_partition_reads;
    }

    inline void on_range_partition_read() noexcept {
        ++_stats.range_partition_reads;
    }

    inline void on_partition_read() noexcept {
        ++_stats.partition_reads;
    }

    inline void on_partition_seek() noexcept {
        ++_stats.partition_seeks;
    }

    inline void on_row_read() noexcept {
        ++_stats.row_reads;
    }

    inline void on_capped_local_deletion_time() noexcept {
        ++_stats.capped_local_deletion_time;
    }

    inline void on_capped_tombstone_deletion_time() noexcept {
        ++_stats.capped_tombstone_deletion_time;
    }

    inline void on_open_for_reading() noexcept {
        ++_stats.open_for_reading;
    }
    inline void on_close_for_reading() noexcept {
        ++_stats.closed_for_reading;
    }

    inline void on_open_for_writing() noexcept {
        ++_stats.open_for_writing;
    }
    inline void on_close_for_writing() noexcept {
        ++_stats.closed_for_writing;
    }

    inline void on_delete() noexcept {
        ++_stats.deleted;
    }
};

}
