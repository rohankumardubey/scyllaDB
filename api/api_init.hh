/*
 * Copyright 2016 ScylaDB
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

#include <seastar/http/httpd.hh>
#include <seastar/core/future.hh>

#include "replica/database_fwd.hh"
#include "seastarx.hh"

namespace service {

class load_meter;
class storage_proxy;
class storage_service;

} // namespace service

class sstables_loader;

namespace streaming {
class stream_manager;
}

namespace locator {

class token_metadata;
class shared_token_metadata;

} // namespace locator

namespace cql_transport { class controller; }
class thrift_controller;
namespace db {
class snapshot_ctl;
class config;
namespace view {
class view_builder;
}
}
namespace netw { class messaging_service; }
class repair_service;
namespace cdc { class generation_service; }

namespace gms {

class gossiper;

}

namespace api {

struct http_context {
    sstring api_dir;
    sstring api_doc;
    httpd::http_server_control http_server;
    distributed<replica::database>& db;
    distributed<service::storage_proxy>& sp;
    service::load_meter& lmeter;
    const sharded<locator::shared_token_metadata>& shared_token_metadata;

    http_context(distributed<replica::database>& _db,
            distributed<service::storage_proxy>& _sp,
            service::load_meter& _lm, const sharded<locator::shared_token_metadata>& _stm)
            : db(_db), sp(_sp), lmeter(_lm), shared_token_metadata(_stm) {
    }

    const locator::token_metadata& get_token_metadata();
};

future<> set_server_init(http_context& ctx);
future<> set_server_config(http_context& ctx, const db::config& cfg);
future<> set_server_snitch(http_context& ctx);
future<> set_server_storage_service(http_context& ctx, sharded<service::storage_service>& ss, sharded<gms::gossiper>& g, sharded<cdc::generation_service>& cdc_gs);
future<> set_server_sstables_loader(http_context& ctx, sharded<sstables_loader>& sst_loader);
future<> unset_server_sstables_loader(http_context& ctx);
future<> set_server_view_builder(http_context& ctx, sharded<db::view::view_builder>& vb);
future<> unset_server_view_builder(http_context& ctx);
future<> set_server_repair(http_context& ctx, sharded<repair_service>& repair);
future<> unset_server_repair(http_context& ctx);
future<> set_transport_controller(http_context& ctx, cql_transport::controller& ctl);
future<> unset_transport_controller(http_context& ctx);
future<> set_rpc_controller(http_context& ctx, thrift_controller& ctl);
future<> unset_rpc_controller(http_context& ctx);
future<> set_server_snapshot(http_context& ctx, sharded<db::snapshot_ctl>& snap_ctl);
future<> unset_server_snapshot(http_context& ctx);
future<> set_server_gossip(http_context& ctx, sharded<gms::gossiper>& g);
future<> set_server_load_sstable(http_context& ctx);
future<> set_server_messaging_service(http_context& ctx, sharded<netw::messaging_service>& ms);
future<> unset_server_messaging_service(http_context& ctx);
future<> set_server_storage_proxy(http_context& ctx, sharded<service::storage_service>& ss);
future<> set_server_stream_manager(http_context& ctx, sharded<streaming::stream_manager>& sm);
future<> unset_server_stream_manager(http_context& ctx);
future<> set_hinted_handoff(http_context& ctx, sharded<gms::gossiper>& g);
future<> unset_hinted_handoff(http_context& ctx);
future<> set_server_gossip_settle(http_context& ctx, sharded<gms::gossiper>& g);
future<> set_server_cache(http_context& ctx);
future<> set_server_compaction_manager(http_context& ctx);
future<> set_server_done(http_context& ctx);

}
