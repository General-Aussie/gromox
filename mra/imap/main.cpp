// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <libHX/misc.h>
#include <libHX/option.h>
#include <gromox/atomic.hpp>
#include <gromox/fileio.h>
#include <gromox/paths.h>
#include <gromox/scope.hpp>
#include <gromox/config_file.hpp>
#include "listener.h" 
#include "resource.h" 
#include "imap_parser.h" 
#include "blocks_allocator.h" 
#include <gromox/threads_pool.hpp>
#include "console_cmd_handler.h"
#include <gromox/console_server.hpp>
#include <gromox/contexts_pool.hpp>
#include "service.h" 
#include "system_services.h"
#include <gromox/util.hpp>
#include <gromox/lib_buffer.hpp>
#include <cstdio>
#include <unistd.h>
#include <csignal>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

using namespace gromox;

gromox::atomic_bool g_notify_stop;
std::shared_ptr<CONFIG_FILE> g_config_file;
static char *opt_config_file;
static gromox::atomic_bool g_hup_signalled;

static struct HXoption g_options_table[] = {
	{nullptr, 'c', HXTYPE_STRING, &opt_config_file, nullptr, nullptr, 0, "Config file to read", "FILE"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

static constexpr const char *g_dfl_svc_plugins[] = {
	"libgxs_event_proxy.so",
	"libgxs_event_stub.so",
	"libgxs_logthru.so",
	"libgxs_midb_agent.so",
	"libgxs_ldap_adaptor.so",
	"libgxs_mysql_adaptor.so",
	"libgxs_authmgr.so",
	"libgxs_user_filter.so",
	NULL,
};

static void term_handler(int signo);

int main(int argc, const char **argv) try
{ 
	int retcode = EXIT_FAILURE;
	struct rlimit rl;
	char temp_buff[256];

	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (HX_getopt(g_options_table, &argc, &argv,
	    HXOPT_USAGEONERR | HXOPT_KEEP_ARGV) != HXOPT_ERR_SUCCESS)
		return EXIT_FAILURE;
	startup_banner("gromox-imap");
	struct sigaction sact{};
	sigemptyset(&sact.sa_mask);
	sact.sa_handler = [](int) {};
	sigaction(SIGALRM, &sact, nullptr);
	sact.sa_handler = [](int) { g_hup_signalled = true; };
	sigaction(SIGHUP, &sact, nullptr);
	sact.sa_handler = SIG_IGN;
	sact.sa_flags   = SA_RESTART;
	sigaction(SIGPIPE, &sact, nullptr);
	sact.sa_handler = term_handler;
	sact.sa_flags   = SA_RESETHAND;
	sigaction(SIGINT, &sact, nullptr);
	sigaction(SIGTERM, &sact, nullptr);
	g_config_file = config_file_prg(opt_config_file, "imap.cfg");
	if (opt_config_file != nullptr && g_config_file == nullptr)
		printf("[resource]: config_file_init %s: %s\n", opt_config_file, strerror(errno));
	if (g_config_file == nullptr)
		return EXIT_FAILURE;

	static constexpr cfg_directive cfg_default_values[] = {
		{"block_interval_auths", "1min", CFG_TIME, "1s"},
		{"config_file_path", PKGSYSCONFDIR "/imap:" PKGSYSCONFDIR},
		{"console_server_ip", "::1"},
		{"console_server_port", "4455"},
		{"context_average_mem", "128K", CFG_SIZE, "128K"},
		{"context_average_mitem", "512", CFG_SIZE, "128"},
		{"context_max_mem", "2M", CFG_SIZE},
		{"context_num", "400", CFG_SIZE},
		{"data_file_path", PKGDATADIR "/imap:" PKGDATADIR},
		{"default_lang", "en"},
		{"imap_auth_times", "10", CFG_SIZE, "1"},
		{"imap_autologout_time", "30min", CFG_TIME, "1s"},
		{"imap_conn_timeout", "3min", CFG_TIME, "1s"},
		{"imap_force_starttls", "false", CFG_BOOL},
		{"imap_support_starttls", "false", CFG_BOOL},
		{"listen_port", "143"},
		{"listen_ssl_port", "0"},
		{"running_identity", "gromox"},
		{"service_plugin_ignore_errors", "false", CFG_BOOL},
		{"service_plugin_path", PKGLIBDIR},
		{"state_path", PKGSTATEDIR},
		{"thread_charge_num", "20", CFG_SIZE, "4"},
		{"thread_init_num", "5", CFG_SIZE},
		{},
	};
	config_file_apply(*g_config_file, cfg_default_values);

	int listen_port = 0, listen_ssl_port = 0;
	g_config_file->get_int("LISTEN_PORT", &listen_port);
	g_config_file->get_int("LISTEN_SSL_PORT", &listen_ssl_port);

	auto str_val = g_config_file->get_value("host_id");
	if (str_val == NULL) {
		memset(temp_buff, 0, arsizeof(temp_buff));
		gethostname(temp_buff, arsizeof(temp_buff));
		resource_set_string("HOST_ID", temp_buff);
		str_val = temp_buff;
		printf("[system]: warning! cannot find host ID, OS host name will be "
			"used as host ID\n");
	}
	printf("[system]: host ID is %s\n", str_val);
	
	str_val = resource_get_string("DEFAULT_DOMAIN");
	if (str_val == NULL) {
		memset(temp_buff, 0, arsizeof(temp_buff));
		getdomainname(temp_buff, arsizeof(temp_buff));
		resource_set_string("DEFAULT_DOMAIN", temp_buff);
		str_val = temp_buff;
		printf("[system]: warning! cannot find default domain, OS domain name "
			"will be used as default domain\n");
	}
	printf("[system]: default domain is %s\n", str_val);
	
	unsigned int context_num = g_config_file->get_ll("context_num");
	unsigned int thread_charge_num = g_config_file->get_ll("thread_charge_num");
	if (thread_charge_num % 4 != 0) {
		thread_charge_num = thread_charge_num / 4 * 4;
		resource_set_integer("THREAD_CHARGE_NUM", thread_charge_num);
	}
	printf("[system]: one thread is in charge of %d contexts\n",
		thread_charge_num);
	
	unsigned int thread_init_num = g_config_file->get_ll("thread_init_num");
	if (thread_init_num * thread_charge_num > context_num) {
		thread_init_num = context_num / thread_charge_num;
		if (0 == thread_init_num) {
			thread_init_num = 1;
			context_num = thread_charge_num;
			resource_set_integer("CONTEXT_NUM", context_num);
			printf("[system]: rectify contexts number %d\n", context_num);
		}
		resource_set_integer("THREAD_INIT_NUM", thread_init_num);
	}
	printf("[system]: threads pool initial threads number is %d\n",
		thread_init_num);

	unsigned int context_aver_mem = g_config_file->get_ll("context_average_mem") / (64 * 1024);
	bytetoa(context_aver_mem*64*1024, temp_buff);
	printf("[imap]: context average memory is %s\n", temp_buff);
 
	unsigned int context_max_mem = g_config_file->get_ll("context_max_mem") / (64 * 1024);
	if (context_max_mem < context_aver_mem) {
		context_max_mem = context_aver_mem;
		bytetoa(context_max_mem*64*1024, temp_buff);
		resource_set_string("CONTEXT_MAX_MEM", temp_buff);
	} 
	context_max_mem *= 64*1024;
	bytetoa(context_max_mem, temp_buff);
	printf("[imap]: context maximum memory is %s\n", temp_buff);
 
	unsigned int context_aver_mitem = g_config_file->get_ll("context_average_mitem");
	printf("[imap]: context average mitem number is %d\n", context_aver_mitem);
	
	long imap_conn_timeout = g_config_file->get_ll("imap_conn_timeout");
	itvltoa(imap_conn_timeout, temp_buff);
	printf("[imap]: imap socket read write time out is %s\n", temp_buff);
 
	int autologout_time = g_config_file->get_ll("imap_autologout_time");
	itvltoa(autologout_time, temp_buff);
	printf("[imap]: imap session autologout time is %s\n", temp_buff);
 
	int imap_auth_times = g_config_file->get_ll("imap_auth_times");
	printf("[imap]: maximum authentification failure times is %d\n", 
			imap_auth_times);

	int block_interval_auth = g_config_file->get_ll("block_interval_auths");
	itvltoa(block_interval_auth, temp_buff);
	printf("[imap]: block client %s when authentification failure times "
			"is exceeded\n", temp_buff);

	auto imap_support_stls = parse_bool(g_config_file->get_value("imap_support_starttls"));
	auto certificate_path = g_config_file->get_value("imap_certificate_path");
	auto cb_passwd = g_config_file->get_value("imap_certificate_passwd");
	auto private_key_path = g_config_file->get_value("imap_private_key_path");
	if (imap_support_stls) {
		if (NULL == certificate_path || NULL == private_key_path) {
			imap_support_stls = false;
			printf("[imap]: turn off TLS support because certificate or "
				"private key path is empty\n");
		} else {
			printf("[imap]: imap support TLS mode\n");
		}
	} else {
		printf("[imap]: imap doesn't support TLS mode\n");
	}
	
	auto imap_force_stls = parse_bool(g_config_file->get_value("imap_force_starttls"));
	if (imap_support_stls && imap_force_stls)
		printf("[imap]: imap MUST running in TLS mode\n");
	if (!imap_support_stls && listen_ssl_port > 0)
		listen_ssl_port = 0;
	if (listen_ssl_port > 0) {
		printf("[system]: system SSL listening port %d\n", listen_ssl_port);
	}

	const char *str_value = resource_get_string("SERVICE_PLUGIN_LIST");
	char **service_plugin_list = nullptr;
	auto cl_0 = make_scope_exit([&]() { HX_zvecfree(service_plugin_list); });
	if (str_value != NULL) {
		service_plugin_list = read_file_by_line(str_value);
		if (service_plugin_list == NULL) {
			printf("read_file_by_line %s: %s\n", str_value, strerror(errno));
			return EXIT_FAILURE;
		}
	}

	auto console_server_ip = g_config_file->get_value("console_server_ip");
	int console_server_port = 0;
	g_config_file->get_int("console_server_port", &console_server_port);
	printf("[console_server]: console server is address [%s]:%d\n",
	       *console_server_ip == '\0' ? "*" : console_server_ip, console_server_port);

	if (resource_run() != 0) {
		printf("[system]: Failed to load resource\n");
		return EXIT_FAILURE;
	}
	auto cleanup_2 = make_scope_exit(resource_stop);
	listener_init(listen_port, listen_ssl_port);
	auto cleanup_3 = make_scope_exit(listener_free);
																			
	if (0 != listener_run()) {
		printf("[system]: fail to start listener\n");
		return EXIT_FAILURE;
	}
	auto cleanup_4 = make_scope_exit(listener_stop);

	if (0 != getrlimit(RLIMIT_NOFILE, &rl)) {
		printf("[system]: fail to get file limitation\n");
		return EXIT_FAILURE;
	}
	if (rl.rlim_cur < 2*context_num + 128 ||
		rl.rlim_max < 2*context_num + 128) {
		rl.rlim_cur = 2*context_num + 128;
		rl.rlim_max = 2*context_num + 128;
		if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
			printf("[system]: fail to set file limitation\n");
		else
			printf("[system]: set file limitation to %zu\n", static_cast<size_t>(rl.rlim_cur));
	}
	service_init({g_config_file->get_value("service_plugin_path"),
		g_config_file->get_value("config_file_path"),
		g_config_file->get_value("data_file_path"),
		g_config_file->get_value("state_path"),
		service_plugin_list != NULL ? service_plugin_list : g_dfl_svc_plugins,
		parse_bool(g_config_file->get_value("service_plugin_ignore_errors")),
		context_num});
	printf("--------------------------- service plugins begin"
		   "---------------------------\n");
	if (service_run_early() != 0) {
		printf("[system]: failed to run PLUGIN_EARLY_INIT\n");
		return EXIT_FAILURE;
	}
	auto ret = switch_user_exec(*g_config_file, argv);
	if (ret < 0)
		return EXIT_FAILURE;
	if (0 != service_run()) { 
		printf("---------------------------- service plugins end"
		   "----------------------------\n");
		printf("[system]: failed to run service\n");
		return EXIT_FAILURE;
	} else {
		printf("---------------------------- service plugins end"
		   "----------------------------\n");
	}
	auto cleanup_6 = make_scope_exit(service_stop);
	
	if (0 != system_services_run()) { 
		printf("[system]: failed to run system service\n");
		return EXIT_FAILURE;
	}
	auto cleanup_8 = make_scope_exit(system_services_stop);

	blocks_allocator_init(context_num * context_aver_mem);     
 
	if (0 != blocks_allocator_run()) { 
		printf("[system]: can not run blocks allocator\n"); 
		return EXIT_FAILURE;
	}
	auto cleanup_9 = make_scope_exit(blocks_allocator_free);
	auto cleanup_10 = make_scope_exit(blocks_allocator_stop);

	imap_parser_init(context_num, context_aver_mitem, context_max_mem,
		imap_conn_timeout, autologout_time, imap_auth_times,
		block_interval_auth, imap_support_stls ? TRUE : false,
		imap_force_stls ? TRUE : false,
		certificate_path, cb_passwd, private_key_path);  
 
	if (0 != imap_parser_run()) { 
		printf("[system]: failed to run imap parser\n");
		return EXIT_FAILURE;
	}
	auto cleanup_11 = make_scope_exit(imap_parser_free);
	auto cleanup_12 = make_scope_exit(imap_parser_stop);
	
	contexts_pool_init(imap_parser_get_contexts_list(),  
		context_num,
		imap_parser_get_context_socket,
		imap_parser_get_context_timestamp,
		thread_charge_num, imap_conn_timeout); 
 
	if (0 != contexts_pool_run()) { 
		printf("[system]: failed to run contexts pool\n");
		return EXIT_FAILURE;
	}
	auto cleanup_13 = make_scope_exit(contexts_pool_free);
	auto cleanup_14 = make_scope_exit(contexts_pool_stop);

	console_server_init(console_server_ip, console_server_port);
	console_server_register_command("imap", cmd_handler_imap_control);
	console_server_register_command("system", cmd_handler_system_control);
	console_server_register_command("help", cmd_handler_help);
	console_server_register_command(nullptr, cmd_handler_service_plugins);

	if (0 != console_server_run()) {
		printf("[system]: failed to run console server\n");
		return EXIT_FAILURE;
	}
	auto cleanup_15 = make_scope_exit(console_server_free);
	auto cleanup_16 = make_scope_exit(console_server_stop);
	
	threads_pool_init(thread_init_num, reinterpret_cast<int (*)(SCHEDULE_CONTEXT *)>(imap_parser_process));
	threads_pool_register_event_proc(imap_parser_threads_event_proc);
	if (0 != threads_pool_run()) {
		printf("[system]: failed to run threads pool\n");
		return EXIT_FAILURE;
	}
	auto cleanup_17 = make_scope_exit(threads_pool_free);
	auto cleanup_18 = make_scope_exit(threads_pool_stop);

	/* accept the connection */
	if (listener_trigger_accept() != 0) {
		printf("[system]: fail trigger accept\n");
		return EXIT_FAILURE;
	}
	
	retcode = EXIT_SUCCESS;
	printf("[system]: IMAP DAEMON is now running\n");
	while (!g_notify_stop) {
		sleep(3);
		if (g_hup_signalled.exchange(false))
			service_reload_all();
	}
	listener_stop_accept();
	return retcode;
} catch (const cfg_error &) {
	return EXIT_FAILURE;
}

static void term_handler(int signo)
{
	console_server_notify_main_stop();
}


 
