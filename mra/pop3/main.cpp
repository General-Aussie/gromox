// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <libHX/option.h>
#include <gromox/fileio.h>
#include <gromox/paths.h>
#include <gromox/scope.hpp>
#include <gromox/config_file.hpp>
#include "listener.h" 
#include "resource.h" 
#include "pop3_parser.h" 
#include "blocks_allocator.h" 
#include <gromox/threads_pool.hpp>
#include "console_cmd_handler.h"
#include <gromox/console_server.hpp>
#include <gromox/contexts_pool.hpp>
#include "service.h" 
#include "system_services.h"
#include <gromox/util.hpp>
#include <gromox/lib_buffer.hpp>
#include <pwd.h>
#include <cstdio>
#include <unistd.h>
#include <csignal>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

using namespace gromox;

std::atomic<bool> g_notify_stop{false};
std::shared_ptr<CONFIG_FILE> g_config_file;
static char *opt_config_file;
static std::atomic<bool> g_hup_signalled{false};

static struct HXoption g_options_table[] = {
	{nullptr, 'c', HXTYPE_STRING, &opt_config_file, nullptr, nullptr, 0, "Config file to read", "FILE"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

static const char *const g_dfl_svc_plugins[] = {
	"libgxs_event_proxy.so",
	"libgxs_logthru.so",
	"libgxs_midb_agent.so",
	"libgxs_ldap_adaptor.so",
	"libgxs_mysql_adaptor.so",
	"libgxs_authmgr.so",
	"libgxs_user_filter.so",
	NULL,
};

static void term_handler(int signo);

int main(int argc, const char **argv)
{ 
	int retcode = EXIT_FAILURE;
	struct rlimit rl;
	struct passwd *puser_pass;
	char temp_buff[256];

	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (HX_getopt(g_options_table, &argc, &argv, HXOPT_USAGEONERR) != HXOPT_ERR_SUCCESS)
		return EXIT_FAILURE;
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
	g_config_file = config_file_prg(opt_config_file, "pop3.cfg");
	if (opt_config_file != nullptr && g_config_file == nullptr)
		printf("[resource]: config_file_init %s: %s\n", opt_config_file, strerror(errno));
	if (g_config_file == nullptr)
		return EXIT_FAILURE;

	static constexpr cfg_directive cfg_default_values[] = {
		{"block_interval_auths", "1min", CFG_TIME, "1s"},
		{"cdn_cache_path", "/cdn"},
		{"config_file_path", PKGSYSCONFDIR "/pop3:" PKGSYSCONFDIR},
		{"console_server_ip", "::1"},
		{"console_server_port", "7788"},
		{"context_average_mem", "512K", CFG_SIZE, "128K"},
		{"context_average_units", "5000", CFG_SIZE, "256"},
		{"context_max_mem", "2M", CFG_SIZE},
		{"context_num", "400", CFG_SIZE},
		{"data_file_path", PKGDATADIR "/pop3:" PKGDATADIR},
		{"listen_port", "110"},
		{"listen_ssl_port", "0"},
		{"pop3_auth_times", "10", CFG_SIZE, "1"},
		{"pop3_conn_timeout", "3min", CFG_TIME, "1s"},
		{"pop3_force_stls", "false", CFG_BOOL},
		{"pop3_support_stls", "false", CFG_BOOL},
		{"running_identity", "gromox"},
		{"service_plugin_ignore_errors", "false", CFG_BOOL},
		{"service_plugin_path", PKGLIBDIR},
		{"state_path", PKGSTATEDIR},
		{"thread_charge_num", "20", CFG_SIZE, "4"},
		{"thread_init_num", "5", CFG_SIZE},
		{},
	};
	config_file_apply(*g_config_file, cfg_default_values);

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
	printf("[pop3]: context average memory is %s\n", temp_buff);
 
	size_t context_max_mem = g_config_file->get_ll("context_max_mem") / (64 * 1024);
	if (context_max_mem < context_aver_mem) {
		context_max_mem = context_aver_mem;
		bytetoa(context_max_mem*64*1024, temp_buff);
		resource_set_string("CONTEXT_MAX_MEM", temp_buff);
	} 
	context_max_mem *= 64*1024;
	bytetoa(context_max_mem, temp_buff);
	printf("[pop3]: context maximum memory is %s\n", temp_buff);

	unsigned int context_aver_units = g_config_file->get_ll("context_average_units");
	printf("[pop3]: context average units number is %d\n", context_aver_units);
	
	int pop3_conn_timeout = g_config_file->get_ll("pop3_conn_timeout");
	itvltoa(pop3_conn_timeout, temp_buff);
	printf("[pop3]: pop3 socket read write time out is %s\n", temp_buff);
 
	int pop3_auth_times = g_config_file->get_ll("pop3_auth_times");
	printf("[pop3]: maximum authentification failure times is %d\n", 
			pop3_auth_times);

	int block_interval_auth = g_config_file->get_ll("block_interval_auths");
	itvltoa(block_interval_auth, temp_buff);
	printf("[pop3]: block client %s when authentification failure times "
			"is exceeded\n", temp_buff);

	auto pop3_support_stls = parse_bool(g_config_file->get_value("pop3_support_stls"));
	auto certificate_path = g_config_file->get_value("pop3_certificate_path");
	auto cb_passwd = g_config_file->get_value("pop3_certificate_passwd");
	auto private_key_path = g_config_file->get_value("pop3_private_key_path");
	if (pop3_support_stls) {
		if (NULL == certificate_path || NULL == private_key_path) {
			pop3_support_stls = false;
			printf("[pop3]: turn off TLS support because certificate or "
				"private key path is empty\n");
		} else {
			printf("[pop3]: pop3 support TLS mode\n");
		}
	} else {
		printf("[pop3]: pop3 doesn't support TLS mode\n");
	}
	
	auto pop3_force_stls = parse_bool(g_config_file->get_value("pop3_force_stls"));
	if (pop3_support_stls && pop3_force_stls)
		printf("[pop3]: pop3 MUST running in TLS mode\n");
	int listen_ssl_port = 0;
	g_config_file->get_int("listen_ssl_port", &listen_ssl_port);
	if (!pop3_support_stls && listen_ssl_port > 0)
		listen_ssl_port = 0;
	if (listen_ssl_port > 0) {
		printf("[system]: system SSL listening port %d\n", listen_ssl_port);
	}

	const char *str_value = resource_get_string("SERVICE_PLUGIN_LIST");
	const char *const *service_plugin_list = NULL;
	if (str_value != NULL) {
		service_plugin_list = const_cast<const char *const *>(read_file_by_line(str_value));
		if (service_plugin_list == NULL) {
			printf("read_file_by_line %s: %s\n", str_value, strerror(errno));
			return EXIT_FAILURE;
		}
	}

	auto console_server_ip = g_config_file->get_value("console_server_ip");
	int console_server_port = 0;
	g_config_file->get_int("console_server_port", &console_server_port);
	printf("[console_server]: console server address is [%s]:%d\n",
	       *console_server_ip == '\0' ? "*" : console_server_ip, console_server_port);

	if (resource_run() != 0) {
		printf("[system]: Failed to load resource\n");
		return EXIT_FAILURE;
	}
	auto cleanup_2 = make_scope_exit(resource_stop);
	int listen_port = 110;
	g_config_file->get_int("listen_port", &listen_port);
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
		if (0 != setrlimit(RLIMIT_NOFILE, &rl)) {
			printf("[system]: fail to set file limitation\n");
		}
		printf("[system]: set file limitation to %d\n", 2*context_num + 128);
	}
	auto user_name = g_config_file->get_value("running_identity");
	if (*user_name != '\0') {
		puser_pass = getpwnam(user_name);
		if (NULL == puser_pass) {
			printf("[system]: no such user \"%s\"\n", user_name);
			return EXIT_FAILURE;
		}
		
		if (0 != setgid(puser_pass->pw_gid)) {
			printf("[system]: can not run group of \"%s\"\n", user_name);
			return EXIT_FAILURE;
		}
		if (0 != setuid(puser_pass->pw_uid)) {
			printf("[system]: can not run as \"%s\"\n", user_name);
			return EXIT_FAILURE;
		}
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
	auto cleanup_11 = make_scope_exit(blocks_allocator_free);
	auto cleanup_12 = make_scope_exit(blocks_allocator_stop);

	pop3_parser_init(context_num, context_max_mem, pop3_conn_timeout,
		pop3_auth_times, block_interval_auth, pop3_support_stls,
		pop3_force_stls, certificate_path, cb_passwd,
		private_key_path, g_config_file->get_value("cdn_cache_path"));
 
	if (0 != pop3_parser_run()) { 
		printf("[system]: failed to run pop3 parser\n");
		return EXIT_FAILURE;
	}
	auto cleanup_13 = make_scope_exit(pop3_parser_free);
	auto cleanup_14 = make_scope_exit(pop3_parser_stop);
	
	contexts_pool_init(pop3_parser_get_contexts_list(), context_num,
		pop3_parser_get_context_socket,
		pop3_parser_get_context_timestamp,
		thread_charge_num, pop3_conn_timeout); 
 
	if (0 != contexts_pool_run()) { 
		printf("[system]: failed to run contexts pool\n");
		return EXIT_FAILURE;
	}
	auto cleanup_15 = make_scope_exit(contexts_pool_free);
	auto cleanup_16 = make_scope_exit(contexts_pool_stop);

	console_server_init(console_server_ip, console_server_port);
	console_server_register_command("pop3", cmd_handler_pop3_control);
	console_server_register_command("system", cmd_handler_system_control);
	console_server_register_command("help", cmd_handler_help);
	console_server_register_command(nullptr, cmd_handler_service_plugins);

	if (0 != console_server_run()) {
		printf("[system]: failed to run console server\n");
		return EXIT_FAILURE;
	}
	auto cleanup_17 = make_scope_exit(console_server_free);
	auto cleanup_18 = make_scope_exit(console_server_stop);
	
	threads_pool_init(thread_init_num, reinterpret_cast<int (*)(SCHEDULE_CONTEXT *)>(pop3_parser_process));
	threads_pool_register_event_proc(pop3_parser_threads_event_proc);
	if (0 != threads_pool_run()) {
		printf("[system]: failed to run threads pool\n");
		return EXIT_FAILURE;
	}
	auto cleanup_19 = make_scope_exit(threads_pool_free);
	auto cleanup_20 = make_scope_exit(threads_pool_stop);

	/* accept the connection */
	if (listener_trigger_accept() != 0) {
		printf("[system]: fail trigger accept\n");
		return EXIT_FAILURE;
	}
	
	retcode = EXIT_SUCCESS;
	printf("[system]: POP3 DAEMON is now running\n");
	while (!g_notify_stop) {
		sleep(3);
		if (g_hup_signalled.exchange(false))
			service_reload_all();
	}
	listener_stop_accept();
	return retcode;
} 

static void term_handler(int signo)
{
	console_server_notify_main_stop();
}


 
