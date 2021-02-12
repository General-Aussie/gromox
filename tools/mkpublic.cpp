// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <string>
#include <vector>
#include <libHX/defs.h>
#include <libHX/option.h>
#include <libHX/string.h>
#include <gromox/database.h>
#include <gromox/defs.h>
#include <gromox/paths.h>
#include <gromox/config_file.hpp>
#include <gromox/ext_buffer.hpp>
#include <gromox/mapi_types.hpp>
#include <gromox/list_file.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/proptags.hpp>
#include <gromox/guid.hpp>
#include <gromox/pcl.hpp>
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
#define LLU(x) static_cast<unsigned long long>(x)

using namespace gromox;

static uint32_t g_last_art;
static uint64_t g_last_cn = CHANGE_NUMBER_BEGIN;
static uint64_t g_last_eid = ALLOCATED_EID_RANGE;
static char *opt_config_file, *opt_datadir;

static const struct HXoption g_options_table[] = {
	{nullptr, 'c', HXTYPE_STRING, &opt_config_file, nullptr, nullptr, 0, "Config file to read", "FILE"},
	{nullptr, 'd', HXTYPE_STRING, &opt_datadir, nullptr, nullptr, 0, "Data directory", "DIR"},
	HXOPT_TABLEEND,
};


static BOOL create_generic_folder(sqlite3 *psqlite,
	uint64_t folder_id, uint64_t parent_id, int domain_id,
	const char *pdisplayname, const char *pcontainer_class)
{
	PCL *ppcl;
	BINARY *pbin;
	SIZED_XID xid;
	time_t cur_time;
	uint64_t nt_time;
	uint64_t cur_eid;
	uint64_t max_eid;
	uint32_t art_num;
	EXT_PUSH ext_push;
	uint64_t change_num;
	sqlite3_stmt* pstmt;
	char sql_string[256];
	uint8_t tmp_buff[24];
	
	cur_eid = g_last_eid + 1;
	g_last_eid += ALLOCATED_EID_RANGE;
	max_eid = g_last_eid;
	sprintf(sql_string, "INSERT INTO allocated_eids"
	        " VALUES (%llu, %llu, %lld, 1)", LLU(cur_eid),
	        LLU(max_eid), static_cast<long long>(time(nullptr)));
	if (SQLITE_OK != sqlite3_exec(psqlite,
		sql_string, NULL, NULL, NULL)) {
		return FALSE;
	}
	g_last_cn ++;
	change_num = g_last_cn;
	sprintf(sql_string, "INSERT INTO folders "
				"(folder_id, parent_id, change_number, "
				"cur_eid, max_eid) VALUES (?, ?, ?, ?, ?)");
	if (!gx_sql_prep(psqlite, sql_string, &pstmt))
		return FALSE;
	sqlite3_bind_int64(pstmt, 1, folder_id);
	if (0 == parent_id) {
		sqlite3_bind_null(pstmt, 2);
	} else {
		sqlite3_bind_int64(pstmt, 2, parent_id);
	}
	sqlite3_bind_int64(pstmt, 3, change_num);
	sqlite3_bind_int64(pstmt, 4, cur_eid);
	sqlite3_bind_int64(pstmt, 5, max_eid);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_finalize(pstmt);
	g_last_art ++;
	art_num = g_last_art;
	sprintf(sql_string, "INSERT INTO "
	          "folder_properties VALUES (%llu, ?, ?)", LLU(folder_id));
	if (!gx_sql_prep(psqlite, sql_string, &pstmt))
		return FALSE;
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_DELETEDCOUNTTOTAL);
	sqlite3_bind_int64(pstmt, 2, 0);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_DELETEDFOLDERTOTAL);
	sqlite3_bind_int64(pstmt, 2, 0);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_HIERARCHYCHANGENUMBER);
	sqlite3_bind_int64(pstmt, 2, 0);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_INTERNETARTICLENUMBER);
	sqlite3_bind_int64(pstmt, 2, art_num);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_ARTICLENUMBERNEXT);
	sqlite3_bind_int64(pstmt, 2, 1);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_DISPLAYNAME);
	sqlite3_bind_text(pstmt, 2, pdisplayname, -1, SQLITE_STATIC);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_COMMENT);
	sqlite3_bind_text(pstmt, 2, "", -1, SQLITE_STATIC);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	if (NULL != pcontainer_class) {
		sqlite3_bind_int64(pstmt, 1, PROP_TAG_CONTAINERCLASS);
		sqlite3_bind_text(pstmt, 2, pcontainer_class, -1, SQLITE_STATIC);
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			sqlite3_finalize(pstmt);
			return FALSE;
		}
		sqlite3_reset(pstmt);
	}
	time(&cur_time);
	nt_time = rop_util_unix_to_nttime(cur_time);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_CREATIONTIME);
	sqlite3_bind_int64(pstmt, 2, nt_time);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_LASTMODIFICATIONTIME);
	sqlite3_bind_int64(pstmt, 2, nt_time);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_LOCALCOMMITTIMEMAX);
	sqlite3_bind_int64(pstmt, 2, nt_time);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_HIERREV);
	sqlite3_bind_int64(pstmt, 2, nt_time);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	
	xid.size = 22;
	xid.xid.guid = rop_util_make_domain_guid(domain_id);
	rop_util_value_to_gc(change_num, xid.xid.local_id);
	ext_buffer_push_init(&ext_push, tmp_buff, sizeof(tmp_buff), 0);
	ext_buffer_push_xid(&ext_push, 22, &xid.xid);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_CHANGEKEY);
	sqlite3_bind_blob(pstmt, 2, ext_push.data,
			ext_push.offset, SQLITE_STATIC);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	sqlite3_reset(pstmt);
	
	ppcl = pcl_init();
	if (NULL == ppcl) {
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	if (FALSE == pcl_append(ppcl, &xid)) {
		pcl_free(ppcl);
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	pbin = pcl_serialize(ppcl);
	if (NULL == pbin) {
		pcl_free(ppcl);
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	pcl_free(ppcl);
	
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_PREDECESSORCHANGELIST);
	sqlite3_bind_blob(pstmt, 2, pbin->pb, pbin->cb, SQLITE_STATIC);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		rop_util_free_binary(pbin);
		sqlite3_finalize(pstmt);
		return FALSE;
	}
	rop_util_free_binary(pbin);
	sqlite3_finalize(pstmt);
	
	return TRUE;
}

int main(int argc, const char **argv)
{
	int i;
	int fd;
	int domain_id;
	char *err_msg;
	MYSQL *pmysql;
	char dir[256];
	GUID tmp_guid;
	int mysql_port;
	int store_ratio;
	uint16_t propid;
	char *str_value;
	MYSQL_ROW myrow;
	uint64_t nt_time;
	sqlite3 *psqlite;
	char db_name[256];
	uint64_t max_size;
	MYSQL_RES *pmyres;
	char *mysql_passwd;
	char tmp_buff[256];
	char temp_path[256];
	sqlite3_stmt* pstmt;
	char mysql_host[256];
	char mysql_user[256];
	struct stat node_stat;
	char mysql_string[1024];
	
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (HX_getopt(g_options_table, &argc, &argv, HXOPT_USAGEONERR) != HXOPT_ERR_SUCCESS)
		return EXIT_FAILURE;
	if (2 != argc) {
		printf("usage: %s <domainname>\n", argv[0]);
		return 1;
	}
	auto pconfig = config_file_prg(opt_config_file, "sa.cfg");
	if (opt_config_file != nullptr && pconfig == nullptr) {
		printf("config_file_init %s: %s\n", opt_config_file, strerror(errno));
		return 2;
	}
	str_value = config_file_get_value(pconfig, "PUBLIC_STORE_RATIO");
	if (NULL == str_value) {
		store_ratio = 10;
	} else {
		store_ratio = atoi(str_value);
		if (store_ratio <= 0 || store_ratio >= 1000) {
			store_ratio = 10;
		}
	}
	str_value = config_file_get_value(pconfig, "MYSQL_HOST");
	if (NULL == str_value) {
		strcpy(mysql_host, "localhost");
	} else {
		HX_strlcpy(mysql_host, str_value, GX_ARRAY_SIZE(mysql_host));
	}
	
	str_value = config_file_get_value(pconfig, "MYSQL_PORT");
	if (NULL == str_value) {
		mysql_port = 3306;
	} else {
		mysql_port = atoi(str_value);
		if (mysql_port <= 0) {
			mysql_port = 3306;
		}
	}

	str_value = config_file_get_value(pconfig, "MYSQL_USERNAME");
	if (NULL == str_value) {
		mysql_user[0] = '\0';
	} else {
		strcpy(mysql_user, str_value);
	}

	mysql_passwd = config_file_get_value(pconfig, "MYSQL_PASSWORD");

	str_value = config_file_get_value(pconfig, "MYSQL_DBNAME");
	if (NULL == str_value) {
		strcpy(db_name, "email");
	} else {
		HX_strlcpy(db_name, str_value, GX_ARRAY_SIZE(db_name));
	}
	const char *datadir = opt_datadir != nullptr ? opt_datadir :
	                      config_file_get_value(pconfig, "data_file_path");
	if (datadir == nullptr)
		datadir = PKGDATADIR;
	
	if (NULL == (pmysql = mysql_init(NULL))) {
		printf("Failed to init mysql object\n");
		return 3;
	}

	if (NULL == mysql_real_connect(pmysql, mysql_host, mysql_user,
		mysql_passwd, db_name, mysql_port, NULL, 0)) {
		mysql_close(pmysql);
		printf("Failed to connect to the database\n");
		return 3;
	}
	
	sprintf(mysql_string, "SELECT max_size, homedir, domain_type, "
		"domain_status, id FROM domains WHERE domainname='%s'", argv[1]);
	
	if (0 != mysql_query(pmysql, mysql_string) ||
		NULL == (pmyres = mysql_store_result(pmysql))) {
		printf("fail to query database\n");
		mysql_close(pmysql);
		return 3;
	}
		
	if (1 != mysql_num_rows(pmyres)) {
		printf("cannot find information from database "
				"for username %s\n", argv[1]);
		mysql_free_result(pmyres);
		mysql_close(pmysql);
		return 3;
	}

	myrow = mysql_fetch_row(pmyres);
	
	if (0 != atoi(myrow[2])) {
		printf("domain type is not normal\n");
		mysql_free_result(pmyres);
		mysql_close(pmysql);
		return 4;
	}
	
	if (0 != atoi(myrow[3])) {
		printf("warning: domain status is not alive!\n");
	}
	
	max_size = atoll(myrow[0])*1024/store_ratio;
	if (max_size > 0x7FFFFFFF) {
		max_size = 0x7FFFFFFF;
	}
	HX_strlcpy(dir, myrow[1], GX_ARRAY_SIZE(dir));
	domain_id = atoi(myrow[4]);
	mysql_free_result(pmyres);
	mysql_close(pmysql);
	
	snprintf(temp_path, 256, "%s/exmdb", dir);
	if (0 != stat(temp_path, &node_stat)) {
		mkdir(temp_path, 0777);
	}
	
	snprintf(temp_path, 256, "%s/exmdb/exchange.sqlite3", dir);
	if (0 == stat(temp_path, &node_stat)) {
		printf("can not create store database,"
			" %s already exits\n", temp_path);
		return 6;
	}
	
	char common_tpl[256], public_tpl[256];
	snprintf(common_tpl, GX_ARRAY_SIZE(common_tpl), "%s/sqlite3_common.txt", datadir);
	snprintf(public_tpl, GX_ARRAY_SIZE(public_tpl), "%s/sqlite3_public.txt", datadir);
	fd = open(common_tpl, O_RDONLY);
	if (fd < 0) {
		printf("Failed to open \"%s\": %s\n", common_tpl, strerror(errno));
		return 7;
	}
	size_t str_size = fstat(fd, &node_stat) ? node_stat.st_size : 0;
	auto sql_string = static_cast<char *>(malloc(str_size));
	if (read(fd, sql_string, str_size) != str_size) {
		printf("read %s: %s\n", common_tpl, strerror(errno));
		close(fd);
		free(sql_string);
		return 7;
	}
	close(fd);
	fd = open(public_tpl, O_RDONLY);
	if (fd < 0) {
		printf("Failed to open \"%s\": %s\n", public_tpl, strerror(errno));
		return 7;
	}
	size_t str_size1 = fstat(fd, &node_stat) ? node_stat.st_size : 0;
	auto sql_string1 = static_cast<char *>(realloc(sql_string, str_size + str_size1 + 1));
	if (sql_string1 == nullptr) {
		printf("%s\n", strerror(errno));
		close(fd);
		return 7;
	}
	sql_string = sql_string1;
	if (read(fd, sql_string + str_size, str_size1) != str_size1) {
		printf("read %s: %s\n", public_tpl, strerror(errno));
		close(fd);
		free(sql_string);
		return 7;
	}
	sql_string[str_size + str_size1] = '\0';
	if (SQLITE_OK != sqlite3_initialize()) {
		printf("Failed to initialize sqlite engine\n");
		free(sql_string);
		return 9;
	}
	if (SQLITE_OK != sqlite3_open_v2(temp_path, &psqlite,
		SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, NULL)) {
		printf("fail to create store database\n");
		free(sql_string);
		sqlite3_shutdown();
		return 9;
	}
	chmod(temp_path, 0666);
	/* begin the transaction */
	sqlite3_exec(psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	
	if (SQLITE_OK != sqlite3_exec(psqlite,
		sql_string, NULL, NULL, &err_msg)) {
		printf("fail to execute table creation sql, error: %s\n", err_msg);
		free(sql_string);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	free(sql_string);
	
	char proppath[256];
	snprintf(proppath, GX_ARRAY_SIZE(proppath), "%s/propnames.txt", datadir);
	std::vector<std::string> namedprop_list;
	auto ret = list_file_read_fixedstrings(proppath, namedprop_list);
	if (ret == -ENOENT) {
	} else if (ret < 0) {
		printf("Failed to read \"%s\": %s\n", proppath, strerror(-ret));
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 7;
	}
	const char *csql_string = "INSERT INTO named_properties VALUES (?, ?)";
	if (!gx_sql_prep(psqlite, csql_string, &pstmt)) {
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	
	i = 0;
	for (const auto &name : namedprop_list) {
		propid = 0x8001 + i;
		sqlite3_bind_int64(pstmt, 1, propid);
		sqlite3_bind_text(pstmt, 2, name.c_str(), -1, SQLITE_STATIC);
		if (sqlite3_step(pstmt) != SQLITE_DONE) {
			printf("fail to step sql inserting\n");
			sqlite3_finalize(pstmt);
			sqlite3_close(psqlite);
			sqlite3_shutdown();
			return 9;
		}
		sqlite3_reset(pstmt);
	}
	sqlite3_finalize(pstmt);
	
	csql_string = "INSERT INTO store_properties VALUES (?, ?)";
	if (!gx_sql_prep(psqlite, csql_string, &pstmt)) {
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	csql_string = "INSERT INTO store_properties VALUES (?, ?)";
	if (!gx_sql_prep(psqlite, csql_string, &pstmt)) {
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	
	nt_time = rop_util_unix_to_nttime(time(NULL));
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_CREATIONTIME);
	sqlite3_bind_int64(pstmt, 2, nt_time);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_PROHIBITRECEIVEQUOTA);
	sqlite3_bind_int64(pstmt, 2, max_size);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_PROHIBITSENDQUOTA);
	sqlite3_bind_int64(pstmt, 2, max_size);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_STORAGEQUOTALIMIT);
	sqlite3_bind_int64(pstmt, 2, max_size);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_MESSAGESIZEEXTENDED);
	sqlite3_bind_int64(pstmt, 2, 0);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_ASSOCMESSAGESIZEEXTENDED);
	sqlite3_bind_int64(pstmt, 2, 0);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, PROP_TAG_NORMALMESSAGESIZEEXTENDED);
	sqlite3_bind_int64(pstmt, 2, 0);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_finalize(pstmt);
	if (FALSE == create_generic_folder(psqlite, PUBLIC_FID_ROOT,
		0, domain_id, "Root Container", NULL)) {
		printf("fail to create \"root\" folder\n");
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 10;
	}
	if (FALSE == create_generic_folder(psqlite, PUBLIC_FID_IPMSUBTREE,
		PUBLIC_FID_ROOT, domain_id, "IPM_SUBTREE", NULL)) {
		printf("fail to create \"ipmsubtree\" folder\n");
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 10;
	}
	if (FALSE == create_generic_folder(psqlite, PUBLIC_FID_NONIPMSUBTREE,
		PUBLIC_FID_ROOT, domain_id, "NON_IPM_SUBTREE", NULL)) {
		printf("fail to create \"ipmsubtree\" folder\n");
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 10;
	}
	if (FALSE == create_generic_folder(psqlite, PUBLIC_FID_EFORMSREGISTRY,
		PUBLIC_FID_NONIPMSUBTREE, domain_id, "EFORMS REGISTRY", NULL)) {
		printf("fail to create \"ipmsubtree\" folder\n");
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 10;
	}
	
	csql_string = "INSERT INTO configurations VALUES (?, ?)";
	if (!gx_sql_prep(psqlite, csql_string, &pstmt)) {
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	tmp_guid = guid_random_new();
	guid_to_string(&tmp_guid, tmp_buff, sizeof(tmp_buff));
	sqlite3_bind_int64(pstmt, 1, CONFIG_ID_MAILBOX_GUID);
	sqlite3_bind_text(pstmt, 2, tmp_buff, -1, SQLITE_STATIC);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, CONFIG_ID_CURRENT_EID);
	sqlite3_bind_int64(pstmt, 2, 0x100);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, CONFIG_ID_MAXIMUM_EID);
	sqlite3_bind_int64(pstmt, 2, ALLOCATED_EID_RANGE);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, CONFIG_ID_LAST_CHANGE_NUMBER);
	sqlite3_bind_int64(pstmt, 2, g_last_cn);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, CONFIG_ID_LAST_CID);
	sqlite3_bind_int64(pstmt, 2, 0);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, CONFIG_ID_LAST_ARTICLE_NUMBER);
	sqlite3_bind_int64(pstmt, 2, g_last_art);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, CONFIG_ID_SEARCH_STATE);
	sqlite3_bind_int64(pstmt, 2, 0);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, CONFIG_ID_DEFAULT_PERMISSION);
	sqlite3_bind_int64(pstmt, 2, PERMISSION_READANY|
		PERMISSION_CREATE|PERMISSION_FOLDERVISIBLE|
		PERMISSION_EDITOWNED|PERMISSION_DELETEOWNED);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, CONFIG_ID_ANONYMOUS_PERMISSION);
	sqlite3_bind_int64(pstmt, 2, 0);
	if (sqlite3_step(pstmt) != SQLITE_DONE) {
		printf("fail to step sql inserting\n");
		sqlite3_finalize(pstmt);
		sqlite3_close(psqlite);
		sqlite3_shutdown();
		return 9;
	}
	sqlite3_finalize(pstmt);
	
	/* commit the transaction */
	sqlite3_exec(psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	sqlite3_close(psqlite);
	sqlite3_shutdown();
	exit(0);
}
