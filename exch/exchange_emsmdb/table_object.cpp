// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cstdint>
#include <gromox/restriction.hpp>
#include <gromox/proc_common.h>
#include "exmdb_client.h"
#include "table_object.h"
#include <gromox/sortorder_set.hpp>
#include "folder_object.h"
#include <gromox/proptag_array.hpp>
#include "rop_processor.h"
#include "message_object.h"
#include "processor_types.h"
#include "emsmdb_interface.h"
#include <cstdlib>
#include <cstring>
#include "common_util.h"

namespace {

struct BOOKMARK_NODE {
	DOUBLE_LIST_NODE node;
	uint32_t index;
	uint64_t inst_id;
	uint32_t row_type;
	uint32_t inst_num;
	uint32_t position;
};

}

static void table_object_set_table_id(
	TABLE_OBJECT *ptable, uint32_t table_id)
{
	auto dir = ptable->plogon->get_dir();
	if (ptable->m_table_id != 0) {
		exmdb_client_unload_table(dir, ptable->m_table_id);
		if (ptable->rop_id == ropGetContentsTable ||
		    ptable->rop_id == ropGetHierarchyTable)
			emsmdb_interface_remove_table_notify(dir, ptable->m_table_id);
	}
	if (0 != table_id) {
		if (ptable->rop_id == ropGetContentsTable ||
		    ptable->rop_id == ropGetHierarchyTable)
			emsmdb_interface_add_table_notify(
				dir, table_id, ptable->handle,
				ptable->logon_id, &ptable->cxh.guid);
	}
	ptable->m_table_id = table_id;
}

BOOL TABLE_OBJECT::check_loaded()
{
	auto ptable = this;
	if (ptable->rop_id == ropGetAttachmentTable)
		return TRUE;
	return m_table_id == 0 ? false : TRUE;
}

BOOL TABLE_OBJECT::check_to_load()
{
	auto ptable = this;
	uint32_t row_num;
	uint32_t table_id;
	uint32_t permission;
	
	if (ptable->rop_id == ropGetAttachmentTable)
		return TRUE;
	if (m_table_id != 0)
		return TRUE;
	switch (ptable->rop_id) {
	case ropGetHierarchyTable: {
		auto rpc_info = get_rpc_info();
		auto username = ptable->plogon->logon_mode == LOGON_MODE_OWNER ?
		                nullptr : rpc_info.username;
		if (!exmdb_client_load_hierarchy_table(ptable->plogon->get_dir(),
		    static_cast<FOLDER_OBJECT *>(ptable->pparent_obj)->folder_id,
		    username, ptable->table_flags, m_restriction,
		    &table_id, &row_num))
			return FALSE;
		break;
	}
	case ropGetContentsTable: {
		auto rpc_info = get_rpc_info();
		auto pinfo = emsmdb_interface_get_emsmdb_info();
		if (NULL == pinfo) {
			return FALSE;
		}
		const char *username = nullptr;
		if (ptable->plogon != LOGON_MODE_OWNER) {
			if (!ptable->plogon->check_private()) {
				username = rpc_info.username;
			} else {
				if (!exmdb_client_check_folder_permission(
				    ptable->plogon->get_dir(),
				    static_cast<FOLDER_OBJECT *>(ptable->pparent_obj)->folder_id,
				    rpc_info.username, &permission))
					return FALSE;	
				if (!(permission & (frightsReadAny | frightsOwner)))
					username = rpc_info.username;
			}
		}
		if (!exmdb_client_load_content_table(ptable->plogon->get_dir(),
		    pinfo->cpid, static_cast<FOLDER_OBJECT *>(ptable->pparent_obj)->folder_id,
		    username, ptable->table_flags, m_restriction,
		    m_sorts, &table_id, &row_num))
			return FALSE;
		break;
	}
	case ropGetPermissionsTable:
		if (!exmdb_client_load_permission_table(ptable->plogon->get_dir(),
		    static_cast<FOLDER_OBJECT *>(ptable->pparent_obj)->folder_id,
		    ptable->table_flags, &table_id, &row_num))
			return FALSE;
		break;
	case ropGetRulesTable:
		if (!exmdb_client_load_rule_table(ptable->plogon->get_dir(),
		    static_cast<FOLDER_OBJECT *>(ptable->pparent_obj)->folder_id,
		    ptable->table_flags, m_restriction, &table_id, &row_num))
			return FALSE;
		break;
	default:
		fprintf(stderr, "%s - not calling table_object_set_table_id\n", __func__);
		return TRUE;
	}
	table_object_set_table_id(ptable, table_id);
	return TRUE;
}

void TABLE_OBJECT::unload()
{
	table_object_set_table_id(this, 0);
}

BOOL TABLE_OBJECT::query_rows(BOOL b_forward, uint16_t row_count, TARRAY_SET *pset)
{
	auto ptable = this;
	DCERPC_INFO rpc_info;
	const char *username;
	
	if (m_columns == nullptr)
		return FALSE;
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	if (NULL == pinfo) {
		return FALSE;
	}
	if (m_position == 0 && !b_forward) {
		pset->count = 0;
		return TRUE;
	}
	if (m_position >= ptable->get_total() && b_forward) {
		pset->count = 0;
		return TRUE;
	}
	int32_t row_needed = b_forward ? row_count : -row_count; /* XXX */
	if (ptable->rop_id == ropGetAttachmentTable) {
		return message_object_query_attachment_table(
		       static_cast<MESSAGE_OBJECT *>(ptable->pparent_obj),
		       m_columns, m_position, row_needed, pset);
	}
	if (!ptable->plogon->check_private()) {
		rpc_info = get_rpc_info();
		username = rpc_info.username;
	} else {
		username = NULL;
	}
	return exmdb_client_query_table(ptable->plogon->get_dir(), username,
	       pinfo->cpid, m_table_id, m_columns,
	       m_position, row_needed, pset);
}

void TABLE_OBJECT::seek_current(BOOL b_forward, uint16_t row_count)
{
	auto ptable = this;
	
	if (TRUE == b_forward) {
		m_position += row_count;
		auto total_rows = ptable->get_total();
		if (m_position > total_rows)
			m_position = total_rows;
	} else {
		if (m_position < row_count) {
			m_position = 0;
			return;
		}
		m_position -= row_count;
	}
}

BOOL TABLE_OBJECT::set_columns(const PROPTAG_ARRAY *pcolumns)
{
	if (m_columns != nullptr)
		proptag_array_free(m_columns);
	if (NULL == pcolumns) {
		m_columns = nullptr;
		return TRUE;
	}
	m_columns = proptag_array_dup(pcolumns);
	return m_columns != nullptr ? TRUE : false;
}

BOOL TABLE_OBJECT::set_sorts(const SORTORDER_SET *psorts)
{
	if (m_sorts != nullptr)
		sortorder_set_free(m_sorts);
	if (NULL == psorts) {
		m_sorts = nullptr;
		return TRUE;
	}
	m_sorts = sortorder_set_dup(psorts);
	return m_sorts != nullptr ? TRUE : false;
}

BOOL TABLE_OBJECT::set_restriction(const RESTRICTION *prestriction)
{
	if (m_restriction != nullptr)
		restriction_free(m_restriction);
	if (NULL == prestriction) {
		m_restriction = nullptr;
		return TRUE;
	}
	m_restriction = restriction_dup(prestriction);
	return m_restriction != nullptr ? TRUE : false;
}

void TABLE_OBJECT::set_position(uint32_t position)
{
	auto ptable = this;
	auto total_rows = ptable->get_total();
	if (position > total_rows) {
		position = total_rows;
	}
	m_position = position;
}

uint32_t TABLE_OBJECT::get_total() const
{
	auto ptable = this;
	uint16_t num;
	uint32_t total_rows;
	
	if (ptable->rop_id == ropGetAttachmentTable) {
		num = 0;
		message_object_get_attachments_num(static_cast<MESSAGE_OBJECT *>(ptable->pparent_obj), &num);
		return num;
	}
	exmdb_client_sum_table(ptable->plogon->get_dir(),
		m_table_id, &total_rows);
	return total_rows;
}

std::unique_ptr<TABLE_OBJECT> table_object_create(LOGON_OBJECT *plogon,
	void *pparent_obj, uint8_t table_flags,
	uint8_t rop_id, uint8_t logon_id)
{
	std::unique_ptr<TABLE_OBJECT> ptable;
	try {
		ptable = std::make_unique<TABLE_OBJECT>();
	} catch (const std::bad_alloc &) {
		return NULL;
	}
	if (FALSE == emsmdb_interface_get_cxh(&ptable->cxh)) {
		return NULL;
	}
	ptable->plogon = plogon;
	ptable->pparent_obj = pparent_obj;
	ptable->handle = 0;
	ptable->rop_id = rop_id;
	ptable->table_flags = table_flags;
	ptable->logon_id = logon_id;
	ptable->m_columns = nullptr;
	ptable->m_sorts = nullptr;
	ptable->m_restriction = nullptr;
	ptable->m_position = 0;
	ptable->m_table_id = 0;
	ptable->bookmark_index = 0;
	double_list_init(&ptable->bookmark_list);
	return ptable;
}

TABLE_OBJECT::~TABLE_OBJECT()
{
	auto ptable = this;
	ptable->reset();
	double_list_free(&ptable->bookmark_list);
}

BOOL TABLE_OBJECT::create_bookmark(uint32_t *pindex)
{
	auto ptable = this;
	uint64_t inst_id;
	uint32_t row_type;
	uint32_t inst_num;
	
	if (!exmdb_client_mark_table(ptable->plogon->get_dir(), m_table_id,
	    m_position, &inst_id, &inst_num, &row_type))
		return FALSE;
	auto pbookmark = me_alloc<BOOKMARK_NODE>();
	if (NULL == pbookmark) {
		return FALSE;
	}
	pbookmark->node.pdata = pbookmark;
	pbookmark->index = ptable->bookmark_index;
	ptable->bookmark_index ++;
	pbookmark->inst_id = inst_id;
	pbookmark->row_type = row_type;
	pbookmark->inst_num = inst_num;
	pbookmark->position = m_position;
	double_list_append_as_tail(&ptable->bookmark_list, &pbookmark->node);
	*pindex = pbookmark->index;
	return TRUE;
}

BOOL TABLE_OBJECT::retrieve_bookmark(uint32_t index, BOOL *pb_exist)
{
	auto ptable = this;
	uint64_t inst_id;
	uint32_t row_type;
	uint32_t inst_num;
	uint32_t position;
	uint32_t tmp_type;
	int32_t tmp_position;
	DOUBLE_LIST_NODE *pnode;
	
	for (pnode=double_list_get_head(&ptable->bookmark_list); NULL!=pnode;
		pnode=double_list_get_after(&ptable->bookmark_list, pnode)) {
		if (index == ((BOOKMARK_NODE*)pnode->pdata)->index) {
			inst_id = ((BOOKMARK_NODE*)pnode->pdata)->inst_id;
			row_type = ((BOOKMARK_NODE*)pnode->pdata)->row_type;
			inst_num = ((BOOKMARK_NODE*)pnode->pdata)->inst_num;
			position = ((BOOKMARK_NODE*)pnode->pdata)->position;
			break;
		}
	}
	if (NULL == pnode) {
		return FALSE;
	}
	if (!exmdb_client_locate_table(ptable->plogon->get_dir(),
	    m_table_id, inst_id, inst_num, &tmp_position, &tmp_type))
		return FALSE;
	*pb_exist = FALSE;
	if (tmp_position >= 0) {
		if (tmp_type == row_type) {
			*pb_exist = TRUE;
		}
		m_position = tmp_position;
	} else {
		m_position = position;
	}
	auto total_rows = ptable->get_total();
	if (m_position > total_rows)
		m_position = total_rows;
	return TRUE;
}

void TABLE_OBJECT::remove_bookmark(uint32_t index)
{
	auto ptable = this;
	DOUBLE_LIST_NODE *pnode;
	
	for (pnode=double_list_get_head(&ptable->bookmark_list); NULL!=pnode;
		pnode=double_list_get_after(&ptable->bookmark_list, pnode)) {
		if (index == ((BOOKMARK_NODE*)pnode->pdata)->index) {
			double_list_remove(&ptable->bookmark_list, pnode);
			free(pnode->pdata);
			break;
		}
	}
}

void TABLE_OBJECT::clear_bookmarks()
{
	DOUBLE_LIST_NODE *pnode;
	
	while ((pnode = double_list_pop_front(&bookmark_list)) != nullptr)
		free(pnode->pdata);
}

void TABLE_OBJECT::reset()
{
	auto ptable = this;
	if (m_columns != nullptr) {
		proptag_array_free(m_columns);
		m_columns = nullptr;
	}
	if (m_sorts != nullptr) {
		sortorder_set_free(m_sorts);
		m_sorts = nullptr;
	}
	if (m_restriction != nullptr) {
		restriction_free(m_restriction);
		m_restriction = nullptr;
	}
	m_position = 0;
	table_object_set_table_id(ptable, 0);
	ptable->clear_bookmarks();
}

BOOL TABLE_OBJECT::get_all_columns(PROPTAG_ARRAY *pcolumns)
{
	auto ptable = this;
	if (ptable->rop_id == ropGetAttachmentTable)
		return message_object_get_attachment_table_all_proptags(
		       static_cast<MESSAGE_OBJECT *>(ptable->pparent_obj), pcolumns);
	return exmdb_client_get_table_all_proptags(ptable->plogon->get_dir(),
	       m_table_id, pcolumns);
}

BOOL TABLE_OBJECT::match_row(BOOL b_forward, const RESTRICTION *pres,
	int32_t *pposition, TPROPVAL_ARRAY *ppropvals)
{
	auto ptable = this;
	DCERPC_INFO rpc_info;
	const char *username;
	
	if (m_columns == nullptr)
		return FALSE;
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	if (!ptable->plogon->check_private()) {
		rpc_info = get_rpc_info();
		username = rpc_info.username;
	} else {
		username = NULL;
	}
	return exmdb_client_match_table(ptable->plogon->get_dir(), username,
	       pinfo->cpid, m_table_id, b_forward, m_position,
	       pres, m_columns, pposition, ppropvals);
}

BOOL TABLE_OBJECT::read_row(uint64_t inst_id, uint32_t inst_num,
	TPROPVAL_ARRAY *ppropvals)
{
	auto ptable = this;
	DCERPC_INFO rpc_info;
	const char *username;
	
	if (m_columns == nullptr)
		return FALSE;
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	if (!ptable->plogon->check_private()) {
		rpc_info = get_rpc_info();
		username = rpc_info.username;
	} else {
		username = NULL;
	}
	return exmdb_client_read_table_row(ptable->plogon->get_dir(), username,
	       pinfo->cpid, m_table_id, m_columns,
	       inst_id, inst_num, ppropvals);
}

BOOL TABLE_OBJECT::expand(uint64_t inst_id, BOOL *pb_found, int32_t *pposition,
    uint32_t *prow_count)
{
	return exmdb_client_expand_table(plogon->get_dir(),
	       m_table_id, inst_id, pb_found, pposition, prow_count);
}

BOOL TABLE_OBJECT::collapse(uint64_t inst_id, BOOL *pb_found,
    int32_t *pposition, uint32_t *prow_count)
{
	return exmdb_client_collapse_table(plogon->get_dir(),
	       m_table_id, inst_id, pb_found, pposition, prow_count);
}

BOOL TABLE_OBJECT::store_state(uint64_t inst_id, uint32_t inst_num,
    uint32_t *pstate_id)
{
	return exmdb_client_store_table_state(plogon->get_dir(),
	       m_table_id, inst_id, inst_num, pstate_id);
}

BOOL TABLE_OBJECT::restore_state(uint32_t state_id, uint32_t *pindex)
{
	int32_t position;
	uint64_t inst_id;
	uint32_t inst_num;
	uint32_t tmp_type;
	uint32_t new_position;
	auto ptable = this;
	
	if (!exmdb_client_mark_table(ptable->plogon->get_dir(),
	    m_table_id, m_position, &inst_id, &inst_num, &tmp_type))
		return FALSE;
	if (!exmdb_client_restore_table_state(ptable->plogon->get_dir(),
	    m_table_id, state_id, &position))
		return FALSE;	
	if (!exmdb_client_locate_table(ptable->plogon->get_dir(),
	    m_table_id, inst_id, inst_num,
	    reinterpret_cast<int32_t *>(&new_position), &tmp_type))
		return FALSE;
	if (position < 0) {
		/* assign an invalid bookmark index */
		*pindex = ptable->bookmark_index;
		ptable->bookmark_index ++;
		return TRUE;
	}
	m_position = position;
	if (!ptable->create_bookmark(pindex)) {
		m_position = new_position;
		return FALSE;
	}
	m_position = new_position;
	return TRUE;
}
