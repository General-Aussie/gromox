// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <memory>
#include <optional>
#include <string>
#include <libHX/io.h>
#include <libHX/option.h>
#include <libHX/string.h>
#include <gromox/database.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/paths.h>
#include <gromox/scope.hpp>
#include <gromox/config_file.hpp>
#include <ctime>
#include <cstdio>
#include <fcntl.h>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mysql.h>
#include "exch/mysql_adaptor/mysql_adaptor.h"
#include "mkshared.hpp"
using namespace std::string_literals;
using namespace gromox;

enum {
	CONFIG_ID_USERNAME = 1, /* obsolete */
};

static unsigned int opt_force;
static char *opt_config_file, *opt_datadir;
static constexpr HXoption g_options_table[] = {
	{nullptr, 'c', HXTYPE_STRING, &opt_config_file, nullptr, nullptr, 0, "Config file to read", "FILE"},
	{nullptr, 'd', HXTYPE_STRING, &opt_datadir, nullptr, nullptr, 0, "Data directory", "DIR"},
	{nullptr, 'f', HXTYPE_NONE, &opt_force, nullptr, nullptr, 0, "Allow overwriting exchange.sqlite3"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

int main(int argc, const char **argv) try
{
	MYSQL *pmysql;
	MYSQL_ROW myrow;
	sqlite3 *psqlite;
	MYSQL_RES *pmyres;
	
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (HX_getopt(g_options_table, &argc, &argv, HXOPT_USAGEONERR) != HXOPT_ERR_SUCCESS)
		return EXIT_FAILURE;
	if (2 != argc) {
		printf("usage: %s <username>\n", argv[0]);
		return EXIT_FAILURE;
	}
	auto pconfig = config_file_prg(opt_config_file, "mysql_adaptor.cfg");
	if (opt_config_file != nullptr && pconfig == nullptr)
		printf("config_file_init %s: %s\n", opt_config_file, strerror(errno));
	if (pconfig == nullptr)
		return EXIT_FAILURE;
	static constexpr cfg_directive cfg_default_values[] = {
		{"mysql_host", "localhost"},
		{"mysql_port", "3306"},
		{"mysql_username", "root"},
		{"mysql_dbname", "email"},
		CFG_TABLE_END,
	};
	config_file_apply(*pconfig, cfg_default_values);
	std::string mysql_host = znul(pconfig->get_value("mysql_host"));
	uint16_t mysql_port = pconfig->get_ll("mysql_port");
	std::string mysql_user = znul(pconfig->get_value("mysql_username"));
	std::optional<std::string> mysql_pass;
	if (auto s = pconfig->get_value("mysql_password"))
		mysql_pass.emplace(s);
	std::string db_name = znul(pconfig->get_value("mysql_dbname"));

	pconfig = config_file_prg(opt_config_file, "sa.cfg");
	if (opt_config_file != nullptr && pconfig == nullptr)
		printf("config_file_init %s: %s\n", opt_config_file, strerror(errno));
	if (pconfig == nullptr)
		return EXIT_FAILURE;
	const char *datadir = opt_datadir != nullptr ? opt_datadir :
	                      pconfig->get_value("data_file_path");
	if (datadir == nullptr)
		datadir = PKGDATADIR;
	
	if (NULL == (pmysql = mysql_init(NULL))) {
		printf("Failed to init mysql object\n");
		return EXIT_FAILURE;
	}

	if (mysql_real_connect(pmysql, mysql_host.c_str(), mysql_user.c_str(),
	    mysql_pass.has_value() ? mysql_pass->c_str() : nullptr,
	    db_name.c_str(), mysql_port, nullptr, 0) == nullptr) {
		mysql_close(pmysql);
		printf("Failed to connect to the database %s@%s/%s\n",
		       mysql_user.c_str(), mysql_host.c_str(), db_name.c_str());
		return EXIT_FAILURE;
	}
	if (mysql_set_character_set(pmysql, "utf8mb4") != 0) {
		fprintf(stderr, "\"utf8mb4\" not available: %s", mysql_error(pmysql));
		mysql_close(pmysql);
		return EXIT_FAILURE;
	}
	
	auto qstr =
		"SELECT up.propval_str AS dtypx, u.address_status, u.maildir "
		"FROM users AS u "
		"LEFT JOIN user_properties AS up ON u.id=up.user_id AND up.proptag=956628995 " /* PR_DISPLAY_TYPE_EX */
		"WHERE username='"s + argv[1] + "'";
	if (mysql_query(pmysql, qstr.c_str()) != 0 ||
		NULL == (pmyres = mysql_store_result(pmysql))) {
		printf("fail to query database\n");
		mysql_close(pmysql);
		return EXIT_FAILURE;
	}
		
	if (1 != mysql_num_rows(pmyres)) {
		printf("cannot find information from database "
				"for username %s\n", argv[1]);
		mysql_free_result(pmyres);
		mysql_close(pmysql);
		return EXIT_FAILURE;
	}

	myrow = mysql_fetch_row(pmyres);
	auto dtypx = DT_MAILUSER;
	if (myrow[0] != nullptr)
		dtypx = static_cast<enum display_type>(strtoul(myrow[0], nullptr, 0));
	if (dtypx != DT_MAILUSER && dtypx != DT_ROOM && dtypx != DT_EQUIPMENT) {
		printf("Refusing to create a private store for mailing lists, groups and aliases. "
		       "(PR_DISPLAY_TYPE=%xh)\n", dtypx);
		mysql_free_result(pmyres);
		mysql_close(pmysql);
		return EXIT_FAILURE;
	}

	auto address_status = strtoul(myrow[1], nullptr, 0);
	if (address_status != AF_USER_NORMAL && address_status != AF_USER_SHAREDMBOX)
		printf("Warning: Address status is not \"alive\"(0) but %lu\n", address_status);
	std::string dir = znul(myrow[2]);
	mysql_free_result(pmyres);
	mysql_close(pmysql);
	
	auto temp_path = dir + "/exmdb"s;
	if (mkdir(temp_path.c_str(), 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "E-1337: mkdir %s: %s\n", temp_path.c_str(), strerror(errno));
		return EXIT_FAILURE;
	}
	adjust_rights(temp_path.c_str());
	temp_path += "/midb.sqlite3";
	/*
	 * sqlite3_open does not expose O_EXCL, so let's create the file under
	 * EXCL semantics ahead of time.
	 */
	unsigned int tfdflags = O_RDWR | O_CREAT | O_EXCL;
	if (opt_force) {
		tfdflags = ~O_EXCL;
		tfdflags |= O_TRUNC;
	}
	auto tfd = open(temp_path.c_str(), tfdflags, 0660);
	if (tfd >= 0) {
		adjust_rights(tfd);
		close(tfd);
	} else if (errno == EEXIST) {
		printf("mkmidb: %s already exists\n", temp_path.c_str());
		printf("mkmidb: Use the -f option to force overwrite.\n");
		return EXIT_FAILURE;
	}

	auto filp = fopen_sd("sqlite3_midb.txt", datadir);
	if (filp == nullptr) {
		fprintf(stderr, "fopen_sd sqlite3_midb.txt: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	std::unique_ptr<char[], stdlib_delete> slurp_data(HX_slurp_fd(fileno(filp.get()), nullptr));
	if (SQLITE_OK != sqlite3_initialize()) {
		printf("Failed to initialize sqlite engine\n");
		return EXIT_FAILURE;
	}
	auto cl_0 = make_scope_exit([]() { sqlite3_shutdown(); });
	if (sqlite3_open_v2(temp_path.c_str(), &psqlite,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
		printf("fail to create store database\n");
		return EXIT_FAILURE;
	}
	auto cl_1 = make_scope_exit([&]() { sqlite3_close(psqlite); });
	auto sql_transact = gx_sql_begin_trans(psqlite);
	if (gx_sql_exec(psqlite, slurp_data.get()) != SQLITE_OK)
		return EXIT_FAILURE;
	slurp_data.reset();
	sql_transact.commit();
	return EXIT_SUCCESS;
} catch (const cfg_error &) {
	return EXIT_FAILURE;
}
