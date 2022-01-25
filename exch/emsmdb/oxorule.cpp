// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cstdint>
#include <gromox/defs.h>
#include <gromox/rop_util.hpp>
#include <gromox/ext_buffer.hpp>
#include "common_util.h"
#include <gromox/proc_common.h>
#include "exmdb_client.h"
#include "logon_object.h"
#include "table_object.h"
#include "folder_object.h"
#include "rop_funcs.hpp"
#include "rop_ids.h"
#include "rop_processor.h"
#include "processor_types.h"

uint32_t rop_modifyrules(uint8_t flags, uint16_t count, const RULE_DATA *prow,
    LOGMAP *plogmap, uint8_t logon_id, uint32_t hin)
{
	int i, j;
	BOOL b_exceed;
	int object_type;
	uint32_t permission;
	
	/* MS-OXORULE 3.2.5.2 */
	if (flags & ~MODIFY_RULES_FLAG_REPLACE) {
		return ecInvalidParam;
	}
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (plogon == nullptr)
		return ecError;
	auto pfolder = rop_proc_get_obj<folder_object>(plogmap, logon_id, hin, &object_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (object_type != OBJECT_TYPE_FOLDER)
		return ecNotSupported;
	auto rpc_info = get_rpc_info();
	if (plogon->logon_mode != LOGON_MODE_OWNER) {
		if (!exmdb_client_check_folder_permission(plogon->get_dir(),
		    pfolder->folder_id, rpc_info.username, &permission))
			return ecError;
		if (!(permission & frightsOwner))
			return ecAccessDenied;
	}
	if (MODIFY_RULES_FLAG_REPLACE & flags) {
		for (i=0; i<count; i++) {
			if (prow[i].flags != ROW_ADD)
				return ecInvalidParam;
		}
		if (!exmdb_client_empty_folder_rule(plogon->get_dir(),
		    pfolder->folder_id))
			return ecError;
	}
	for (i=0; i<count; i++) {
		for (j=0; j<prow[i].propvals.count; j++) {
			if (FALSE == common_util_convert_tagged_propval(
				TRUE, prow[i].propvals.ppropval + j)) {
				return ecError;
			}
		}
	}
	if (!exmdb_client_update_folder_rule(plogon->get_dir(),
	    pfolder->folder_id, count, prow, &b_exceed))
		return ecError;
	if (TRUE == b_exceed) {
		return ecMAPIOOM;
	}
	return ecSuccess;
}

uint32_t rop_getrulestable(uint8_t flags, LOGMAP *plogmap, uint8_t logon_id,
    uint32_t hin, uint32_t *phout)
{
	int object_type;
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (plogon == nullptr)
		return ecError;
	auto pfolder = rop_proc_get_obj<folder_object>(plogmap, logon_id, hin, &object_type);
	if (pfolder == nullptr)
		return ecNullObject;
	if (object_type != OBJECT_TYPE_FOLDER)
		return ecNotSupported;
	auto ptable = table_object::create(plogon, pfolder,
	              flags, ropGetRulesTable, logon_id);
	if (ptable == nullptr)
		return ecMAPIOOM;
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_TABLE, ptable.get());
	if (hnd < 0) {
		return ecError;
	}
	ptable->set_handle(hnd);
	ptable.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_updatedeferredactionmessages(const BINARY *pserver_entry_id,
    const BINARY *pclient_entry_id, LOGMAP *plogmap,
    uint8_t logon_id, uint32_t hin)
{
	uint8_t tmp_byte;
	uint32_t table_id;
	uint32_t row_count;
	TARRAY_SET tmp_set;
	uint32_t permission;
	uint64_t fid_deferred;
	PROBLEM_ARRAY problems;
	PROPTAG_ARRAY proptags;
	RESTRICTION restriction;
	TPROPVAL_ARRAY propvals;
	TAGGED_PROPVAL propval_buff[2];
	RESTRICTION_PROPERTY res_property;
	
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (plogon == nullptr)
		return ecError;
	if (!plogon->check_private())
		return ecNotSupported;
	fid_deferred = rop_util_make_eid_ex(1, PRIVATE_FID_DEFERRED_ACTION);
	auto rpc_info = get_rpc_info();
	if (plogon->logon_mode != LOGON_MODE_OWNER) {
		if (!exmdb_client_check_folder_permission(plogon->get_dir(),
		    fid_deferred, rpc_info.username, &permission))
			return ecError;
		if (!(permission & frightsEditAny))
			return ecAccessDenied;
	}
	
	restriction.rt = RES_PROPERTY;
	restriction.pres = &res_property;
	res_property.relop = RELOP_EQ;
	res_property.proptag = PR_DAM_ORIG_MSG_SVREID;
	res_property.propval.proptag = res_property.proptag;
	res_property.propval.pvalue = (void*)pserver_entry_id;
	if (!exmdb_client_load_content_table(plogon->get_dir(), 0, fid_deferred,
	    nullptr, TABLE_FLAG_NONOTIFICATIONS, &restriction, nullptr,
	    &table_id, &row_count))
		return ecError;
	
	uint32_t tmp_proptag = PidTagMid;
	proptags.count = 1;
	proptags.pproptag = &tmp_proptag;
	
	if (!exmdb_client_query_table(plogon->get_dir(), nullptr, 0,
	    table_id, &proptags, 0, row_count, &tmp_set))
		return ecError;
	exmdb_client_unload_table(plogon->get_dir(), table_id);
	
	propvals.count = 2;
	propvals.ppropval = propval_buff;
	propval_buff[0].proptag = PR_DAM_ORIG_MSG_SVREID;
	propval_buff[0].pvalue = (void*)pclient_entry_id;
	propval_buff[1].proptag = PROP_TAG_DAMBACKPATCHED;
	propval_buff[1].pvalue = &tmp_byte;
	tmp_byte = 1;
	
	for (size_t i = 0; i < tmp_set.count; ++i) {
		auto pmid = tmp_set.pparray[i]->get<uint64_t>(PidTagMid);
		if (NULL == pmid) {
			continue;
		}
		exmdb_client_set_message_properties(plogon->get_dir(), nullptr,
			0, *pmid, &propvals, &problems);
	}
	return ecSuccess;
}
