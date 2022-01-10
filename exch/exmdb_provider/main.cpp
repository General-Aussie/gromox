// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <memory>
#include <string>
#include <utility>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/exmdb_rpc.hpp>
#include <gromox/paths.h>
#include "bounce_producer.h"
#include <gromox/svc_common.h>
#include "exmdb_listener.h"
#include "exmdb_client.h"
#include "exmdb_server.h"
#include "exmdb_parser.h"
#include "common_util.h"
#include <gromox/config_file.hpp>
#include "db_engine.h"
#include <gromox/util.hpp>
#include <cstring>
#include <cstdlib>
#include <cstdio>

using namespace std::string_literals;
using namespace gromox;

DECLARE_SVC_API();

static std::shared_ptr<CONFIG_FILE> g_config_during_init;

static constexpr cfg_directive cfg_default_values[] = {
	{"cache_interval", "2h", CFG_TIME, "1s"},
	{"dbg_synthesize_content", "0"},
	{"exrpc_debug", "0"},
	{"enable_dam", "1", CFG_BOOL},
	{"listen_ip", "::1"},
	{"listen_port", "5000"},
	{"max_ext_rule_number", "20", CFG_SIZE, "1", "100"},
	{"max_router_connections", "4095M", CFG_SIZE},
	{"max_rpc_stub_threads", "4095M", CFG_SIZE},
	{"max_rule_number", "1000", CFG_SIZE, "1", "2000"},
	{"max_store_message_count", "200000", CFG_SIZE},
	{"mbox_contention_warning", "5", CFG_SIZE},
	{"mbox_contention_reject", "5", CFG_SIZE},
	{"notify_stub_threads_num", "4", CFG_SIZE, "0"},
	{"populating_threads_num", "50", CFG_SIZE, "1", "50"},
	{"rpc_proxy_connection_num", "10", CFG_SIZE, "0"},
	{"separator_for_bounce", ";"},
	{"sqlite_mmap_size", "0", CFG_SIZE},
	{"sqlite_synchronous", "false", CFG_BOOL},
	{"sqlite_wal_mode", "false", CFG_BOOL},
	{"table_size", "5000", CFG_SIZE, "100"},
	{"x500_org_name", "Gromox default"},
	CFG_TABLE_END,
};

unsigned int g_dbg_synth_content;
unsigned int g_mbox_contention_warning, g_mbox_contention_reject;

static bool exmdb_provider_reload(std::shared_ptr<CONFIG_FILE> pconfig)
{
	if (pconfig == nullptr) {
		pconfig = config_file_initd("exmdb_provider.cfg", get_config_path());
		if (pconfig != nullptr)
			config_file_apply(*pconfig, cfg_default_values);
	}
	if (pconfig == nullptr) {
		printf("[exmdb_provider]: config_file_initd exmdb_provider.cfg: %s\n",
		       strerror(errno));
		return false;
	}
	try {
		g_exrpc_debug = pconfig->get_ll("exrpc_debug");
		g_dbg_synth_content = pconfig->get_ll("dbg_synthesize_content");
		g_enable_dam = parse_bool(pconfig->get_value("enable_dam"));
		g_mbox_contention_warning = pconfig->get_ll("mbox_contention_warning");
		g_mbox_contention_reject = pconfig->get_ll("mbox_contention_reject");
	} catch (const cfg_error &) {
		return false;
	}
	return true;
}

static BOOL svc_exmdb_provider(int reason, void **ppdata) try
{
	char temp_buff[64];

	switch(reason) {
	case PLUGIN_RELOAD:
		exmdb_provider_reload(nullptr);
		return TRUE;
	case PLUGIN_EARLY_INIT: {
		LINK_SVC_API(ppdata);
		exmdb_rpc_alloc = common_util_alloc;
		exmdb_rpc_free = [](void *) {};
		exmdb_rpc_exec = exmdb_client_do_rpc;
		std::string cfg_path = get_plugin_name();
		auto pos = cfg_path.find_last_of('.');
		if (pos != cfg_path.npos)
			cfg_path.erase(pos);
		cfg_path += ".cfg";
		auto pconfig = g_config_during_init = config_file_initd(cfg_path.c_str(), get_config_path());
		if (NULL == pconfig) {
			printf("[exmdb_provider]: config_file_initd %s: %s\n",
			       cfg_path.c_str(), strerror(errno));
			return FALSE;
		}

		config_file_apply(*pconfig, cfg_default_values);
		auto listen_ip = pconfig->get_value("listen_ip");
		uint16_t listen_port = pconfig->get_ll("listen_port");
		printf("[exmdb_provider]: listen address is [%s]:%hu\n",
		       *listen_ip == '\0' ? "*" : listen_ip, listen_port);

		exmdb_listener_init(listen_ip, listen_port);
		if (exmdb_listener_run(get_config_path()) != 0) {
			printf("[exmdb_provider]: failed to run exmdb listener\n");
			return FALSE;
		}
		return TRUE;
	}
	case PLUGIN_INIT: {
		auto pconfig = std::move(g_config_during_init);
		auto separator = pconfig->get_value("separator_for_bounce");
		auto org_name = pconfig->get_value("x500_org_name");
		printf("[exmdb_provider]: x500 org name is \"%s\"\n", org_name);
		
		int connection_num = pconfig->get_ll("rpc_proxy_connection_num");
		printf("[exmdb_provider]: exmdb rpc proxy "
			"connection number is %d\n", connection_num);
			
		int threads_num = pconfig->get_ll("notify_stub_threads_num");
		printf("[exmdb_provider]: exmdb notify stub "
			"threads number is %d\n", threads_num);
		
		size_t max_threads = pconfig->get_ll("max_rpc_stub_threads");
		size_t max_routers = pconfig->get_ll("max_router_connections");
		int table_size = pconfig->get_ll("table_size");
		printf("[exmdb_provider]: db hash table size is %d\n", table_size);
		
		int cache_interval = pconfig->get_ll("cache_interval");
		itvltoa(cache_interval, temp_buff);
		printf("[exmdb_provider]: cache interval is %s\n", temp_buff);
		
		int max_msg_count = pconfig->get_ll("max_store_message_count");
		printf("[exmdb_provider]: maximum message "
			"count per store is %d\n", max_msg_count);
		
		int max_rule = pconfig->get_ll("max_rule_number");
		printf("[exmdb_provider]: maximum rule "
			"number per folder is %d\n", max_rule);
		
		int max_ext_rule = pconfig->get_ll("max_ext_rule_number");
		printf("[exmdb_provider]: maximum ext rule "
			"number per folder is %d\n", max_ext_rule);
		
		auto b_async = parse_bool(pconfig->get_value("sqlite_synchronous"));
		printf("[exmdb_provider]: sqlite synchronous PRAGMA is %s\n", b_async ? "ON" : "OFF");
		
		auto b_wal = parse_bool(pconfig->get_value("sqlite_wal_mode"));
		printf("[exmdb_provider]: sqlite journal mode is %s\n", b_wal ? "WAL" : "DELETE");
		
		uint64_t mmap_size = pconfig->get_ll("sqlite_mmap_size");
		if (0 == mmap_size) {
			printf("[exmdb_provider]: sqlite mmap_size is disabled\n");
		} else {
			bytetoa(mmap_size, temp_buff);
			printf("[exmdb_provider]: sqlite mmap_size is %s\n", temp_buff);
		}
		
		int populating_num = pconfig->get_ll("populating_threads_num");
		printf("[exmdb_provider]: populating threads"
				" number is %d\n", populating_num);
		if (!exmdb_provider_reload(pconfig))
			return false;
		
		common_util_init(org_name, max_msg_count, max_rule, max_ext_rule);
		bounce_producer_init(separator);
		db_engine_init(table_size, cache_interval,
			b_async ? TRUE : false, b_wal ? TRUE : false, mmap_size, populating_num);
		exmdb_server_init();
		uint16_t listen_port = pconfig->get_ll("listen_port");
		if (0 == listen_port) {
			exmdb_parser_init(0, 0);
		} else {
			exmdb_parser_init(max_threads, max_routers);
		}
		exmdb_client_init(connection_num, threads_num);
		
		if (bounce_producer_run(get_data_path()) != 0) {
			printf("[exmdb_provider]: failed to run bounce producer\n");
			exmdb_server_free();
			db_engine_free();
			common_util_free();
			return FALSE;
		}
		if (0 != db_engine_run()) {
			printf("[exmdb_provider]: failed to run db engine\n");
			exmdb_server_free();
			db_engine_free();
			common_util_free();
			return FALSE;
		}
		if (0 != exmdb_server_run()) {
			printf("[exmdb_provider]: failed to run exmdb server\n");
			db_engine_stop();
			exmdb_server_free();
			db_engine_free();
			common_util_free();
			return FALSE;
		}
		if (exmdb_parser_run(get_config_path()) != 0) {
			printf("[exmdb_provider]: failed to run exmdb parser\n");
			exmdb_server_stop();
			db_engine_stop();
			exmdb_server_free();
			db_engine_free();
			common_util_free();
			return FALSE;
		}
		if (0 != exmdb_listener_trigger_accept()) {
			printf("[exmdb_provider]: fail to trigger exmdb listener\n");
			exmdb_listener_stop();
			exmdb_parser_stop();
			exmdb_server_stop();
			db_engine_stop();
			exmdb_server_free();
			db_engine_free();
			common_util_free();
			return FALSE;
		}
		if (exmdb_client_run(get_config_path()) != 0) {
			printf("[exmdb_provider]: failed to run exmdb client\n");
			exmdb_listener_stop();
			exmdb_parser_stop();
			exmdb_server_stop();
			db_engine_stop();
			exmdb_server_free();
			db_engine_free();
			common_util_free();
			return FALSE;
		}

#define EXMIDL(n, p) register_service("exmdb_client_" #n, exmdb_client::n);
#define IDLOUT
#include <gromox/exmdb_idef.hpp>
#undef EXMIDL
#undef IDLOUT
		register_service("exmdb_client_register_proc", exmdb_server_register_proc);
		register_service("pass_service", common_util_pass_service);
		return TRUE;
	}
	case PLUGIN_FREE:
		exmdb_client_stop();
		exmdb_listener_stop();
		exmdb_parser_stop();
		exmdb_server_stop();
		db_engine_stop();
		exmdb_server_free();
		db_engine_free();
		common_util_free();
		return TRUE;
	}
	return TRUE;
} catch (const cfg_error &) {
	return false;
}

SVC_ENTRY(svc_exmdb_provider);
