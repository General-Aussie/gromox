// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020 grammm GmbH
// This file is part of Gromox.
#include <gromox/database.h>
#include <gromox/mapidefs.h>
#include "exmdb_server.h"
#include "common_util.h"
#include <gromox/list_file.hpp>
#include "db_engine.h"
#include <gromox/rop_util.hpp>
#include <gromox/guid.hpp>
#include <gromox/scope.hpp>
#include <gromox/util.hpp>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#define MAXIMUM_ALLOCATION_NUMBER				1000000

#define ALLOCATION_INTERVAL						24*60*60

using namespace gromox;

namespace {
struct dlgitem {
	/* This is used by list_file_*, don't switch to UADDR_SIZE */
	char user[324];
};
}

BOOL exmdb_server_ping_store(const char *dir)
{
	auto pdb = db_engine_get_db(dir);
	if (NULL == pdb) {
		return FALSE;
	}
	return TRUE;
}

BOOL exmdb_server_get_all_named_propids(
	const char *dir, PROPID_ARRAY *ppropids)
{
	int total_count;
	char sql_string[256];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	sprintf(sql_string, "SELECT "
			"count(*) FROM named_properties");
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return FALSE;
	total_count = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	if (0 == total_count) {
		ppropids->count = 0;
		ppropids->ppropid = NULL;
		return TRUE;
	}
	ppropids->ppropid = cu_alloc<uint16_t>(total_count);
	if (NULL == ppropids->ppropid) {
		return FALSE;
	}
	sprintf(sql_string, "SELECT"
		" propid FROM named_properties");
	pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	ppropids->count = 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		ppropids->ppropid[ppropids->count] =
				sqlite3_column_int64(pstmt, 0);
		ppropids->count ++;
	}
	return TRUE;
}

BOOL exmdb_server_get_named_propids(const char *dir,
	BOOL b_create, const PROPNAME_ARRAY *ppropnames,
	PROPID_ARRAY *ppropids)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == common_util_get_named_propids(
		pdb->psqlite, b_create, ppropnames, ppropids)) {
		/* rollback the transaction */
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	/* commit the transaction */
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	return TRUE;
}

BOOL exmdb_server_get_named_propnames(const char *dir,
	const PROPID_ARRAY *ppropids, PROPNAME_ARRAY *ppropnames)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == common_util_get_named_propnames(
		pdb->psqlite, ppropids, ppropnames)) {
		return FALSE;
	}
	return TRUE;
}

/* public only */
BOOL exmdb_server_get_mapping_guid(const char *dir,
	uint16_t replid, BOOL *pb_found, GUID *pguid)
{
	if (TRUE == exmdb_server_check_private()) {
		return FALSE;
	}
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == common_util_get_mapping_guid(
		pdb->psqlite, replid, pb_found, pguid)) {
		return FALSE;
	}
	*pb_found = TRUE;
	return TRUE;
}

/* public only */
BOOL exmdb_server_get_mapping_replid(const char *dir,
	GUID guid, BOOL *pb_found, uint16_t *preplid)
{
	char guid_string[64];
	char sql_string[128];
	
	if (TRUE == exmdb_server_check_private()) {
		return FALSE;
	}
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	guid_to_string(&guid, guid_string, 64);
	sprintf(sql_string, "SELECT replid FROM "
		"replca_mapping WHERE replguid='%s'", guid_string);
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		*pb_found = FALSE;
		return TRUE;
	}
	*preplid = sqlite3_column_int64(pstmt, 0);
	*pb_found = TRUE;
	return TRUE;
}

BOOL exmdb_server_get_store_all_proptags(
	const char *dir, PROPTAG_ARRAY *pproptags)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == common_util_get_proptags(
		STORE_PROPERTIES_TABLE, 0,
		pdb->psqlite, pproptags)) {
		return FALSE;
	}
	return TRUE;
}

BOOL exmdb_server_get_store_properties(const char *dir,
	uint32_t cpid, const PROPTAG_ARRAY *pproptags,
	TPROPVAL_ARRAY *ppropvals)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == common_util_get_properties(
		STORE_PROPERTIES_TABLE, 0, cpid, pdb->psqlite,
		pproptags, ppropvals)) {
		return FALSE;
	}
	return TRUE;
}

BOOL exmdb_server_set_store_properties(const char *dir,
	uint32_t cpid, const TPROPVAL_ARRAY *ppropvals,
	PROBLEM_ARRAY *pproblems)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == common_util_set_properties(
		STORE_PROPERTIES_TABLE, 0, cpid, pdb->psqlite,
		ppropvals, pproblems)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	return TRUE;
}

BOOL exmdb_server_remove_store_properties(
	const char *dir, const PROPTAG_ARRAY *pproptags)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == common_util_remove_properties(
		STORE_PROPERTIES_TABLE, 0, pdb->psqlite, pproptags)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	return TRUE;
}

/* private only */
BOOL exmdb_server_check_mailbox_permission(const char *dir,
	const char *username, uint32_t *ppermission)
{
	char temp_path[256];
	char sql_string[128];
	
	if (FALSE == exmdb_server_check_private()) {
		return FALSE;
	}
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	*ppermission = rightsNone;
	sprintf(sql_string, "SELECT permission "
				"FROM permissions WHERE username=?");
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	sqlite3_bind_text(pstmt, 1, username, -1, SQLITE_STATIC);
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		*ppermission |= sqlite3_column_int64(pstmt, 0);
	}
	pstmt.finalize();
	sprintf(sql_string, "SELECT "
		"username, permission FROM permissions");
	pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		if (common_util_check_mlist_include(reinterpret_cast<const char *>(sqlite3_column_text(pstmt, 0)), username))
			*ppermission |= sqlite3_column_int64(pstmt, 1);
	}
	pstmt.finalize();
	pdb.reset();
	sprintf(temp_path, "%s/config/delegates.txt", dir);
	auto pfile = list_file_initd(temp_path, nullptr, "%s:324");
	if (NULL != pfile) {
		auto item_num = pfile->get_size();
		auto pitem = static_cast<dlgitem *>(pfile->get_list());
		for (decltype(item_num) i = 0; i < item_num; ++i) {
			if (strcasecmp(pitem[i].user, username) == 0 ||
			    common_util_check_mlist_include(pitem[i].user, username)) {
				*ppermission |= frightsGromoxSendAs;
				break;
			}
		}
	}
	return TRUE;
}

BOOL exmdb_server_allocate_cn(const char *dir, uint64_t *pcn)
{
	uint64_t change_num;
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == common_util_allocate_cn(pdb->psqlite, &change_num)) {
		return FALSE;
	}
	*pcn = rop_util_make_eid_ex(1, change_num);
	return TRUE;
}

/* if *pbegin_eid is 0, means too many
	allocation requests within an interval */
BOOL exmdb_server_allocate_ids(const char *dir,
	uint32_t count, uint64_t *pbegin_eid)
{
	uint64_t tmp_eid;
	uint64_t range_end;
	uint64_t range_begin;
	char sql_string[128];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	sprintf(sql_string, "SELECT range_begin, "
				"range_end, is_system FROM allocated_eids"
				" WHERE allocate_time>=%lu",
				time(NULL) - ALLOCATION_INTERVAL);
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	range_begin = 0;
	range_end = 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		if (1 == sqlite3_column_int64(pstmt, 2)) {
			continue;
		}
		tmp_eid = sqlite3_column_int64(pstmt, 0);
		if (0 == range_begin) {
			range_begin = tmp_eid;
		} else {
			if (tmp_eid < range_begin) {
				range_begin = tmp_eid;
			}
		}
		tmp_eid = sqlite3_column_int64(pstmt, 1);
		if (0 == range_end) {
			range_end = tmp_eid;
		} else {
			if (tmp_eid > range_end) {
				range_end = tmp_eid;
			}
		}
	}
	pstmt.finalize();
	if (range_end - range_begin + count > MAXIMUM_ALLOCATION_NUMBER) {
		*pbegin_eid = 0;
		return TRUE;
	}
	sprintf(sql_string, "SELECT "
		"max(range_end) FROM allocated_eids");
	pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return FALSE;
	tmp_eid = sqlite3_column_int64(pstmt, 0) + 1;
	pstmt.finalize();
	sprintf(sql_string, "INSERT INTO allocated_eids "
	          "VALUES (%llu, %llu, %lld, 0)",
	          static_cast<unsigned long long>(tmp_eid),
	          static_cast<unsigned long long>(tmp_eid + count),
	          static_cast<long long>(time(nullptr)));
	if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
		sql_string, NULL, NULL, NULL)) {
		return FALSE;
	}
	*pbegin_eid = rop_util_make_eid_ex(1, tmp_eid);
	return TRUE;
}

BOOL exmdb_server_subscribe_notification(const char *dir,
	uint16_t notificaton_type, BOOL b_whole, uint64_t folder_id,
	uint64_t message_id, uint32_t *psub_id)
{
	uint16_t replid;
	NSUB_NODE *pnsub;
	const char *remote_id;
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	pnode = double_list_get_tail(&pdb->nsub_list);
	uint32_t last_id = pnode == nullptr ? 0 :
	                   static_cast<NSUB_NODE *>(pnode->pdata)->sub_id;
	pnsub = me_alloc<NSUB_NODE>();
	if (NULL == pnsub) {
		return FALSE;
	}
	pnsub->node.pdata = pnsub;
	pnsub->sub_id = last_id + 1;
	remote_id = exmdb_server_get_remote_id();
	if (NULL == remote_id) {
		pnsub->remote_id = NULL;
	} else {
		pnsub->remote_id = strdup(remote_id);
		if (NULL == pnsub->remote_id) {
			free(pnsub);
			return FALSE;
		}
	}
	pnsub->notificaton_type = notificaton_type;
	pnsub->b_whole = b_whole;
	if (0 == folder_id) {
		pnsub->folder_id = 0;
	} else {
		if (TRUE == exmdb_server_check_private()) {
			pnsub->folder_id = rop_util_get_gc_value(folder_id);
		} else {
			replid = rop_util_get_replid(folder_id);
			if (1 == replid) {
				pnsub->folder_id = rop_util_get_gc_value(folder_id);
			} else {
				pnsub->folder_id = replid;
				pnsub->folder_id <<= 48;
				pnsub->folder_id |= rop_util_get_gc_value(folder_id);
			}
		}
	}
	pnsub->message_id = message_id == 0 ? 0 :
	                    rop_util_get_gc_value(message_id);
	double_list_append_as_tail(&pdb->nsub_list, &pnsub->node);
	*psub_id = last_id + 1;
	return TRUE;
}

BOOL exmdb_server_unsubscribe_notification(
	const char *dir, uint32_t sub_id)
{
	NSUB_NODE *pnsub;
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->nsub_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->nsub_list, pnode)) {
		pnsub = (NSUB_NODE*)pnode->pdata;
		if (sub_id == pnsub->sub_id) {
			double_list_remove(&pdb->nsub_list, pnode);
			if (NULL != pnsub->remote_id) {
				free(pnsub->remote_id);
			}
			free(pnsub);
			break;
		}
	}
	return TRUE;
}

BOOL exmdb_server_transport_new_mail(const char *dir, uint64_t folder_id,
	uint64_t message_id, uint32_t message_flags, const char *pstr_class)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	db_engine_transport_new_mail(pdb, rop_util_get_gc_value(folder_id),
		rop_util_get_gc_value(message_id), message_flags, pstr_class);
	return TRUE;
}

static BOOL table_check_address_in_contact_folder(
	sqlite3_stmt *pstmt_subfolder, sqlite3_stmt *pstmt_search,
	uint64_t folder_id, const char *paddress, BOOL *pb_found)
{
	DOUBLE_LIST_NODE *pnode;
	DOUBLE_LIST folder_list;
	
	sqlite3_reset(pstmt_search);
	sqlite3_bind_int64(pstmt_search, 1, folder_id);
	sqlite3_bind_text(pstmt_search, 2, paddress, -1, SQLITE_STATIC);
	if (SQLITE_ROW == sqlite3_step(pstmt_search)) {
		*pb_found = TRUE;
		return TRUE;
	}
	double_list_init(&folder_list);
	sqlite3_reset(pstmt_subfolder);
	sqlite3_bind_int64(pstmt_subfolder, 1, folder_id);
	while (SQLITE_ROW == sqlite3_step(pstmt_subfolder)) {
		pnode = cu_alloc<DOUBLE_LIST_NODE>();
		if (NULL == pnode) {
			return FALSE;
		}
		pnode->pdata = cu_alloc<uint64_t>();
		if (NULL == pnode->pdata) {
			return FALSE;
		}
		*(uint64_t*)pnode->pdata =
			sqlite3_column_int64(pstmt_subfolder, 0);
		double_list_append_as_tail(&folder_list, pnode);
	}
	while ((pnode = double_list_pop_front(&folder_list)) != nullptr) {
		if (FALSE == table_check_address_in_contact_folder(pstmt_subfolder,
			pstmt_search, *(uint64_t*)pnode->pdata, paddress, pb_found)) {
			return FALSE;	
		}
		if (TRUE == *pb_found) {
			return TRUE;
		}
	}
	*pb_found = FALSE;
	return TRUE;
}

BOOL exmdb_server_check_contact_address(const char *dir,
	const char *paddress, BOOL *pb_found)
{
	uint32_t proptags[3];
	char sql_string[512];
	PROPID_ARRAY propids;
	PROPNAME_ARRAY propnames;
	PROPERTY_NAME propname_buff[3];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	propnames.count = 3;
	propnames.ppropname = propname_buff;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[0].guid);
	propname_buff[0].kind = MNID_ID;
	propname_buff[0].lid = PidLidEmail1EmailAddress;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[1].guid);
	propname_buff[1].kind = MNID_ID;
	propname_buff[1].lid = PidLidEmail2EmailAddress;
	rop_util_get_common_pset(PSETID_ADDRESS, &propname_buff[2].guid);
	propname_buff[2].kind = MNID_ID;
	propname_buff[2].lid = PidLidEmail3EmailAddress;
	if (FALSE == common_util_get_named_propids(pdb->psqlite,
		FALSE, &propnames, &propids) || 3 != propids.count) {
		return FALSE;	
	}
	proptags[0] = PROP_TAG(PT_UNICODE, propids.ppropid[0]);
	proptags[1] = PROP_TAG(PT_UNICODE, propids.ppropid[1]);
	proptags[2] = PROP_TAG(PT_UNICODE, propids.ppropid[2]);
	sprintf(sql_string, "SELECT folder_id"
				" FROM folders WHERE parent_id=?");
	auto pstmt1 = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt1 == nullptr) {
		return FALSE;
	}
	sprintf(sql_string, "SELECT messages.message_id"
		" FROM messages JOIN message_properties ON "
		"messages.message_id=message_properties.message_id "
		"WHERE parent_fid=? AND (message_properties.proptag=%u"
		" OR message_properties.proptag=%u"
		" OR message_properties.proptag=%u)"
		" AND message_properties.propval=?"
		" LIMIT 1", proptags[0], proptags[1],
		proptags[2]);
	auto pstmt2 = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt2 == nullptr) {
		return FALSE;
	}
	return table_check_address_in_contact_folder(pstmt1, pstmt2,
	       PRIVATE_FID_CONTACTS, paddress, pb_found);
}

BOOL exmdb_server_unload_store(const char *dir)
{
	return db_engine_unload_db(dir);
}
