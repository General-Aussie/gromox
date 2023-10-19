// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 grommunio GmbH
// This file is part of Gromox.
#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <fmt/core.h>
#include <fmt/format.h>
#include <gromox/element_data.hpp>
#include <gromox/exmdb_client.hpp>
#include <gromox/exmdb_server.hpp>
#include <gromox/exmdb_rpc.hpp>
#include <gromox/ext_buffer.hpp>
#include <gromox/mapidefs.h>
#include <gromox/freebusy.hpp>
#include <gromox/mapierr.hpp>
#include <gromox/mapitags.hpp>
#include <gromox/pcl.hpp>
#include <gromox/propval.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/scope.hpp>
#include <gromox/util.hpp>

// Extended MAPI Definitions
#define POLICY_PROCESS_MEETING_REQUESTS              0x0001
#define POLICY_DECLINE_RECURRING_MEETING_REQUESTS    0x0002
#define POLICY_DECLINE_CONFLICTING_MEETING_REQUESTS  0x0004

using namespace gromox;
namespace exmdb_client = exmdb_client_remote;

namespace {
/**
 * @rule_id:	if @extended, message id of the extrule, else rule id.
 * @folder_id:	(Current) enclosing folder for @msg_id,
 * 		can change upon %OP_MOVE.
 * @msg_id:	(Current) identifier of the message being processed,
 * 		will change upon %OP_MOVE.
 * @name:	Name for debugging.
 * @provider:	Provider field from the rule, just copied to DAM/DEM.
 * @cond:	Rule conditions; may point to @xcond or something else.
 */
struct rule_node {
	rule_node() = default;
	rule_node(rule_node &&);
	rule_node &operator=(rule_node &&);

	int32_t seq = 0;
	uint32_t state = 0;
	bool extended = false;
	uint64_t rule_id = 0;
	std::string name, provider;
	RESTRICTION xcond{};
	EXT_RULE_ACTIONS xact{};
	NAMEDPROPERTY_INFO xcnames{}, xanames{};
	RESTRICTION *cond = nullptr;
	RULE_ACTIONS *act = nullptr;
	/* XXX: Who frees this? */

	bool operator<(const struct rule_node &o) const { return seq < o.seq; }
};

struct folder_node {
	std::string dir;
	eid_t fid = 0;
	inline bool operator<(const struct folder_node &o) const { return std::tie(dir, fid) < std::tie(o.dir, o.fid); }
	inline bool operator==(const struct folder_node &o) const { return dir == o.dir && fid == o.fid; }
};

struct message_node : public folder_node {
	eid_t mid = 0;
	void operator<(const struct message_node &) = delete;
	void operator==(const struct message_node &) = delete;
};

struct rx_delete {
	void operator()(BINARY *x) const { rop_util_free_binary(x); }
	void operator()(MESSAGE_CONTENT *x) const { message_content_free(x); }
};

/**
 * @ev_to:	Envelope-To, and thus also the rule executing identity
 * @cur:	current pointer to message
 */
struct rxparam {
	const char *ev_from = nullptr, *ev_to = nullptr;
	message_node cur;
	std::set<folder_node> loop_check;
	MESSAGE_CONTENT *ctnt = nullptr;
	bool del = false, exit = false;
};

using message_content_ptr = std::unique_ptr<MESSAGE_CONTENT, rx_delete>;

}

static unsigned int g_ruleproc_debug;

rule_node::rule_node(rule_node &&o) :
	seq(o.seq), state(o.state), extended(o.extended), rule_id(o.rule_id),
	name(std::move(o.name)), provider(std::move(o.provider)),
	xcond(std::move(o.xcond)), xact(std::move(o.xact)),
	xcnames(std::move(o.xcnames)), xanames(std::move(o.xanames)),
	cond(o.cond == std::addressof(o.xcond) ? std::addressof(xcond) : o.cond), act(o.act)
{
	o.cond = nullptr;
	o.act = nullptr;
}

rule_node &rule_node::operator=(rule_node &&o)
{
	seq = o.seq;
	state = o.state;
	extended = o.extended;
	rule_id = o.rule_id;
	name = std::move(o.name);
	provider = std::move(o.provider);
	cond = o.cond == std::addressof(o.xcond) ? std::addressof(xcond) : o.cond;
	xcond = std::move(o.xcond);
	xact = std::move(o.xact);
	xcnames = std::move(o.xcnames);
	xanames = std::move(o.xanames);
	o.cond = nullptr;
	act = o.act;
	o.act = nullptr;
	return *this;
}

static void rx_delete_local(PROPNAME_ARRAY &x)
{
	if (x.ppropname == nullptr)
		return;
	for (unsigned int i = 0; i < x.count; ++i)
		if (x.ppropname[i].kind == MNID_STRING)
			exmdb_rpc_free(x.ppropname[i].pname);
	exmdb_rpc_free(x.ppropname);
}

static void rx_npid_collect(const TPROPVAL_ARRAY &props, std::set<uint16_t> &m)
{
	for (unsigned int i = 0; i < props.count; ++i) {
		auto id = PROP_ID(props.ppropval[i].proptag);
		if (id >= 0x8000)
			m.emplace(id);
	}
}

static void rx_npid_collect(const MESSAGE_CONTENT &ctnt, std::set<uint16_t> &m)
{
	rx_npid_collect(ctnt.proplist, m);
	if (ctnt.children.prcpts != nullptr)
		for (unsigned int i = 0; i < ctnt.children.prcpts->count; ++i)
			rx_npid_collect(*ctnt.children.prcpts->pparray[i], m);
	if (ctnt.children.pattachments != nullptr) {
		for (unsigned int i = 0; i < ctnt.children.pattachments->count; ++i) {
			auto at = ctnt.children.pattachments->pplist[i];
			if (at == nullptr)
				continue;
			rx_npid_collect(at->proplist, m);
			if (at->pembedded != nullptr)
				rx_npid_collect(*at->pembedded, m);
		}
	}
}

static void rx_npid_transform(TPROPVAL_ARRAY &props,
    const std::vector<uint16_t> &src, const PROPID_ARRAY &dst)
{
	for (unsigned int i = 0; i < props.count; ++i) {
		auto oldtag = props.ppropval[i].proptag;
		if (PROP_ID(oldtag) < 0x8000)
			continue;
		auto it = std::find(src.begin(), src.end(), PROP_ID(oldtag));
		if (it == src.end())
			continue;
		props.ppropval[i].proptag = PROP_TAG(PROP_TYPE(oldtag), dst.ppropid[it-src.begin()]);
	}
}

static void rx_npid_transform(MESSAGE_CONTENT &ctnt,
    const std::vector<uint16_t> &src, const PROPID_ARRAY &dst)
{
	rx_npid_transform(ctnt.proplist, src, dst);
	if (ctnt.children.prcpts != nullptr)
		for (unsigned int i = 0; i < ctnt.children.prcpts->count; ++i)
			rx_npid_transform(*ctnt.children.prcpts->pparray[i], src, dst);
	if (ctnt.children.pattachments != nullptr) {
		for (unsigned int i = 0; i < ctnt.children.pattachments->count; ++i) {
			auto at = ctnt.children.pattachments->pplist[i];
			if (at == nullptr)
				continue;
			rx_npid_transform(at->proplist, src, dst);
			if (at->pembedded != nullptr)
				rx_npid_transform(*at->pembedded, src, dst);
		}
	}
}

static ec_error_t rx_npid_replace(rxparam &par, MESSAGE_CONTENT &ctnt,
    const char *newdir)
{
	std::set<uint16_t> src_id_set;
	std::vector<uint16_t> src_id_vec;
	rx_npid_collect(ctnt, src_id_set);
	if (src_id_set.size() == 0)
		return ecSuccess;
	for (auto id : src_id_set)
		src_id_vec.push_back(id);
	PROPID_ARRAY src_id_arr, dst_id_arr{};
	src_id_arr.count = src_id_vec.size();
	src_id_arr.ppropid = src_id_vec.data();
	PROPNAME_ARRAY src_name_arr{};
	auto cl_0 = make_scope_exit([&]() {
		rx_delete_local(src_name_arr);
		free(dst_id_arr.ppropid);
	});
	if (!exmdb_client::get_named_propnames(par.cur.dir.c_str(),
	    &src_id_arr, &src_name_arr)) {
		mlog(LV_DEBUG, "ruleproc: get_named_propnames(%s) failed",
			par.cur.dir.c_str());
		return ecRpcFailed;
	}
	if (src_name_arr.count != src_id_arr.count) {
		mlog(LV_ERR, "ruleproc: np(src) counts are fishy");
		return ecError;
	}
	if (!exmdb_client::get_named_propids(newdir, TRUE,
	    &src_name_arr, &dst_id_arr)) {
		mlog(LV_DEBUG, "ruleproc: get_named_propids(%s) failed", newdir);
		return ecRpcFailed;
	}
	if (dst_id_arr.count != src_name_arr.count) {
		mlog(LV_ERR, "ruleproc: np(dst) counts are fishy");
		return ecError;
	}
	rx_npid_transform(ctnt, src_id_vec, dst_id_arr);
	return ecSuccess;
}

static ec_error_t rx_is_oof(const char *dir, bool *oof)
{
	static constexpr uint32_t tags[] = {PR_OOF_STATE};
	static constexpr PROPTAG_ARRAY pt = {std::size(tags), deconst(tags)};
	TPROPVAL_ARRAY props{};
	if (!exmdb_client::get_store_properties(dir, CP_UTF8, &pt, &props))
		return ecError;
	auto flag = props.get<uint8_t>(PR_OOF_STATE);
	*oof = flag != nullptr ? *flag : 0;
	return ecSuccess;
}

static ec_error_t rx_load_std_rules(const char *dir, eid_t fid, bool oof,
    std::vector<rule_node> &rule_list)
{
	uint32_t table_id = 0, row_count = 0;

	RESTRICTION_BITMASK rst_1 = {BMR_NEZ, PR_RULE_STATE, ST_ENABLED};
	RESTRICTION_BITMASK rst_2 = {BMR_NEZ, PR_RULE_STATE, oof ? ST_ONLY_WHEN_OOF : 0U};
	RESTRICTION rst_3[]       = {{RES_BITMASK, {&rst_1}}, {RES_BITMASK, {&rst_2}}};
	RESTRICTION_AND_OR rst_4  = {std::size(rst_3), rst_3};

	RESTRICTION_EXIST rst_5   = {PR_RULE_STATE};
	RESTRICTION rst_6[]       = {{RES_EXIST, {&rst_5}}, {RES_OR, {&rst_4}}};
	RESTRICTION_AND_OR rst_7  = {std::size(rst_6), rst_6};
	RESTRICTION rst_8         = {RES_AND, {&rst_7}};

	/* XXX: Where is my sort order parameter */
	if (!exmdb_client::load_rule_table(dir, fid, 0, &rst_8,
	    &table_id, &row_count))
		return ecError;
	auto cl_0 = make_scope_exit([&]() { exmdb_client::unload_table(dir, table_id); });
	static constexpr uint32_t tags[] = {
		PR_RULE_STATE, PR_RULE_ID, PR_RULE_SEQUENCE, PR_RULE_NAME,
		PR_RULE_PROVIDER, PR_RULE_CONDITION, PR_RULE_ACTIONS,
	};
	const PROPTAG_ARRAY ptags = {std::size(tags), deconst(tags)};
	tarray_set output_rows{};
	if (!exmdb_client::query_table(dir, nullptr, CP_ACP, table_id, &ptags,
	    0, row_count, &output_rows))
		return ecError;

	for (unsigned int i = 0; i < output_rows.count; ++i) {
		auto row   = output_rows.pparray[i];
		if (row == nullptr)
			continue;
		auto seq   = row->get<const int32_t>(PR_RULE_SEQUENCE);
		auto state = row->get<const uint32_t>(PR_RULE_STATE);
		auto id    = row->get<const uint64_t>(PR_RULE_ID);
		if (seq == nullptr || state == nullptr || id == nullptr)
			continue;
		rule_node rule;
		rule.seq = *seq;
		rule.state = *state;
		rule.rule_id = *id;
		rule.name = znul(row->get<const char>(PR_RULE_NAME));
		rule.provider = znul(row->get<const char>(PR_RULE_PROVIDER));
		rule.cond = row->get<RESTRICTION>(PR_RULE_CONDITION);
		rule.act  = row->get<RULE_ACTIONS>(PR_RULE_ACTIONS);
		rule_list.push_back(std::move(rule));
	}
	return ecSuccess;
}

static ec_error_t rx_load_ext_rules(const char *dir, eid_t fid, bool oof,
    std::vector<rule_node> &rule_list)
{
	uint32_t table_id = 0, row_count = 0;

	RESTRICTION_BITMASK rst_1 = {BMR_NEZ, PR_RULE_MSG_STATE, ST_ENABLED};
	RESTRICTION_BITMASK rst_2 = {BMR_NEZ, PR_RULE_MSG_STATE, oof ? ST_ONLY_WHEN_OOF : 0U};
	RESTRICTION rst_3[2]      = {{RES_BITMASK, {&rst_1}}, {RES_BITMASK, {&rst_2}}};
	RESTRICTION_AND_OR rst_4  = {std::size(rst_3), rst_3};

	RESTRICTION_EXIST rst_5   = {PR_RULE_MSG_STATE};
	RESTRICTION_EXIST rst_6   = {PR_MESSAGE_CLASS};
	RESTRICTION_CONTENT rst_7 = {FL_FULLSTRING | FL_IGNORECASE, PR_MESSAGE_CLASS, {PR_MESSAGE_CLASS, deconst("IPM.ExtendedRule.Message")}};
	RESTRICTION rst_8[]       = {{RES_EXIST, {&rst_5}}, {RES_OR, {&rst_4}}, {RES_EXIST, {&rst_6}}, {RES_CONTENT, {&rst_7}}};
	RESTRICTION_AND_OR rst_9  = {std::size(rst_8), rst_8};
	RESTRICTION rst_10        = {RES_AND, {&rst_9}};

	static constexpr SORT_ORDER sort_spec[] = {{PT_LONG, PROP_ID(PR_RULE_MSG_SEQUENCE), TABLE_SORT_ASCEND}};
	static constexpr SORTORDER_SET sort_order = {std::size(sort_spec), 0, 0, deconst(sort_spec)};
	if (!exmdb_client::load_content_table(dir, CP_ACP, fid, nullptr,
	    TABLE_FLAG_ASSOCIATED, &rst_10, &sort_order, &table_id, &row_count))
		return ecError;
	auto cl_0 = make_scope_exit([&]() { exmdb_client::unload_table(dir, table_id); });

	static constexpr uint32_t tags[] = {
		PR_RULE_MSG_STATE, PidTagMid, PR_RULE_MSG_SEQUENCE,
		PR_RULE_MSG_PROVIDER,
	};
	static constexpr uint32_t tags2[] = {
		PR_EXTENDED_RULE_MSG_CONDITION, PR_EXTENDED_RULE_MSG_ACTIONS,
	};
	const PROPTAG_ARRAY ptags = {std::size(tags), deconst(tags)};
	const PROPTAG_ARRAY ptags2 = {std::size(tags2), deconst(tags2)};
	tarray_set output_rows{};
	if (!exmdb_client::query_table(dir, nullptr, CP_ACP, table_id, &ptags,
	    0, row_count, &output_rows))
		return ecError;

	for (unsigned int i = 0; i < output_rows.count; ++i) {
		auto row   = output_rows.pparray[i];
		if (row == nullptr)
			continue;
		auto seq   = row->get<const int32_t>(PR_RULE_MSG_SEQUENCE);
		auto state = row->get<const uint32_t>(PR_RULE_MSG_STATE);
		auto mid   = row->get<const uint64_t>(PidTagMid);
		if (seq == nullptr || state == nullptr || mid == nullptr)
			continue;

		rule_node rule;
		rule.seq = *seq;
		rule.state = *state;
		rule.extended = true;
		rule.rule_id = *mid;
		rule.name = znul(row->get<const char>(PR_RULE_MSG_NAME));
		rule.provider = znul(row->get<const char>(PR_RULE_MSG_PROVIDER));
		TPROPVAL_ARRAY vals2{};
		if (!exmdb_client::get_message_properties(dir, nullptr, CP_ACP,
		    *mid, &ptags2, &vals2))
			continue;
		auto cond = vals2.get<const BINARY>(PR_EXTENDED_RULE_MSG_CONDITION);
		auto act  = vals2.get<const BINARY>(PR_EXTENDED_RULE_MSG_ACTIONS);
		if (act == nullptr || act->cb == 0)
			continue;
		EXT_PULL ep;
		if (cond != nullptr && cond->cb != 0) {
			ep.init(cond->pb, cond->cb, exmdb_rpc_alloc,
				EXT_FLAG_WCOUNT | EXT_FLAG_UTF16);
			if (ep.g_namedprop_info(&rule.xcnames) != EXT_ERR_SUCCESS ||
			    ep.g_restriction(&rule.xcond) != EXT_ERR_SUCCESS)
				return ecError;
			rule.cond = &rule.xcond;
		}
		uint32_t version = 0;
		ep.init(act->pb, act->cb, exmdb_rpc_alloc,
			EXT_FLAG_WCOUNT | EXT_FLAG_UTF16);
		if (ep.g_namedprop_info(&rule.xanames) != EXT_ERR_SUCCESS ||
		    ep.g_uint32(&version) != EXT_ERR_SUCCESS ||
		    version != 1 ||
		    ep.g_ext_rule_actions(&rule.xact) != EXT_ERR_SUCCESS)
			return ecError;
		rule_list.emplace_back(std::move(rule));
	}
	return ecSuccess;
}

static bool rx_eval_props(const MESSAGE_CONTENT *ct, const TPROPVAL_ARRAY &props, const RESTRICTION &res);

static bool rx_eval_msgsub(const MESSAGE_CHILDREN &ch, uint32_t tag,
    const RESTRICTION &res)
{
	uint32_t count = 0;
	if (tag == PR_MESSAGE_RECIPIENTS && ch.prcpts != nullptr) {
		for (size_t i = 0; i < ch.prcpts->count; ++i) {
			auto rcpt = ch.prcpts->pparray[i];
			if (res.rt == RES_COUNT) {
				if (rx_eval_props(nullptr, *rcpt,
				    static_cast<RESTRICTION_COUNT *>(res.pres)->sub_res))
					++count;
			} else {
				if (rx_eval_props(nullptr, *rcpt, res))
					return true;
			}
		}
	} else if (tag == PR_MESSAGE_ATTACHMENTS && ch.pattachments != nullptr) {
		for (size_t i = 0; i < ch.pattachments->count; ++i) {
			auto atx = ch.pattachments->pplist[i];
			if (res.rt == RES_COUNT) {
				if (rx_eval_props(nullptr, atx[i].proplist,
				    static_cast<RESTRICTION_COUNT *>(res.pres)->sub_res))
					++count;
			} else {
				if (rx_eval_props(nullptr, atx[i].proplist, res))
					return true;
			}
		}
	}
	return res.rt == RES_COUNT && res.count->count == count;
}

static bool rx_eval_sub(const MESSAGE_CONTENT *ct, uint32_t tag, const RESTRICTION &res)
{
	switch (res.rt) {
	case RES_OR:
		for (size_t i = 0; i < res.andor->count; ++i)
			if (rx_eval_sub(ct, tag, res.andor->pres[i]))
				return true;
		return false;
	case RES_AND:
		for (size_t i = 0; i < res.andor->count; ++i)
			if (!rx_eval_sub(ct, tag, res.andor->pres[i]))
				return false;
		return true;
	case RES_NOT:
		return !rx_eval_sub(ct, tag, res.xnot->res);
	case RES_CONTENT:
	case RES_PROPERTY:
	case RES_PROPCOMPARE:
	case RES_BITMASK:
	case RES_SIZE:
	case RES_EXIST:
	case RES_COMMENT:
	case RES_ANNOTATION:
	case RES_COUNT: {
		MESSAGE_CHILDREN none{};
		auto &ch = ct != nullptr ? ct->children : none;
		return rx_eval_msgsub(ch, tag, res);
	}
	default:
		return false;
	}
}

static bool rx_eval_props(const MESSAGE_CONTENT *ct, const TPROPVAL_ARRAY &props,
    const RESTRICTION &res)
{
	switch (res.rt) {
	case RES_OR:
		for (size_t i = 0; i < res.andor->count; ++i)
			if (rx_eval_props(ct, props, res.andor->pres[i]))
				return true;
		return false;
	case RES_AND:
		for (size_t i = 0; i < res.andor->count; ++i)
			if (!rx_eval_props(ct, props, res.andor->pres[i]))
				return false;
		return true;
	case RES_NOT:
		return !rx_eval_props(ct, props, res.xnot->res);
	case RES_CONTENT: {
		auto &rcon = *res.cont;
		return rcon.comparable() && rcon.eval(props.getval(rcon.proptag));
	}
	case RES_PROPERTY: {
		auto &rprop = *res.prop;
		// XXX: special-case PR_ANR?
		return rprop.comparable() && rprop.eval(props.getval(rprop.proptag));
	}
	case RES_PROPCOMPARE: {
		auto &rprop = *res.pcmp;
		if (!rprop.comparable())
			return false;
		auto lhs = props.getval(rprop.proptag1);
		auto rhs = props.getval(rprop.proptag2);
		return propval_compare_relop_nullok(rprop.relop,
		       PROP_TYPE(rprop.proptag1), lhs, rhs);
	}
	case RES_BITMASK: {
		auto &rbm = *res.bm;
		return rbm.comparable() &&
		       rbm.eval(props.getval(rbm.proptag));
	}
	case RES_SIZE: {
		auto &rsize = *res.size;
		return rsize.eval(props.getval(rsize.proptag));
	}
	case RES_EXIST:
		return props.has(res.exist->proptag);
	case RES_SUBRESTRICTION: {
		auto &rsub = *res.sub;
		if (rsub.subobject == PR_MESSAGE_RECIPIENTS ||
		    rsub.subobject == PR_MESSAGE_ATTACHMENTS)
			return rx_eval_sub(ct, rsub.subobject, rsub.res);
		return false;
	}
	case RES_COMMENT:
	case RES_ANNOTATION:
		if (res.comment->pres == nullptr)
			return TRUE;
		return rx_eval_props(ct, props, *res.comment->pres);
	case RES_COUNT: {
		auto &rcnt = *res.count;
		if (rcnt.count == 0)
			return false;
		if (!rx_eval_props(ct, props, rcnt.sub_res))
			return false;
		--rcnt.count;
		return true;
	}
	case RES_NULL:
		return true;
	}
	return false;
}

static ec_error_t op_copy_other(rxparam &par, const rule_node &rule,
    const MOVECOPY_ACTION &mc, uint8_t act_type)
{
	using LLU = unsigned long long;
	/* Resolve store */
	if (mc.pstore_eid == nullptr)
		return ecNotFound;
	auto &other_store = *static_cast<const STORE_ENTRYID *>(mc.pstore_eid);
	if (other_store.pserver_name == nullptr)
		return ecNotFound;
	unsigned int user_id = 0, domain_id = 0;
	char *newdir = nullptr;
	if (!exmdb_client::store_eid_to_user(par.cur.dir.c_str(), &other_store,
	    &newdir, &user_id, &domain_id))
		return ecRpcFailed;
	if (newdir == nullptr)
		return ecNotFound;

	/* Resolve folder */
	auto tgt_public = other_store.wrapped_provider_uid == g_muidStorePublic;
	if (!tgt_public && other_store.wrapped_provider_uid != g_muidStorePrivate)
		/* try parsing as FOLDER_ENTRYID directly? (cf. cu_entryid_to_fid) */
		return ecNotFound;
	auto &fid_bin = *static_cast<const BINARY *>(mc.pfolder_eid);
	uint64_t dst_fid, dst_mid = 0, dst_cn = 0;
	if (fid_bin.cb == 0) {
		dst_fid = rop_util_make_eid_ex(1, tgt_public ?
		          PUBLIC_FID_IPMSUBTREE : PRIVATE_FID_INBOX);
	} else {
		FOLDER_ENTRYID folder_eid{};
		EXT_PULL ep;
		ep.init(fid_bin.pb, fid_bin.cb, malloc, EXT_FLAG_WCOUNT | EXT_FLAG_UTF16);
		if (ep.g_folder_eid(&folder_eid) != pack_result::success)
			return ecNotFound;
		else if (folder_eid.folder_type == EITLT_PUBLIC_FOLDER && !tgt_public)
			return ecNotFound;
		else if (folder_eid.folder_type == EITLT_PRIVATE_FOLDER && tgt_public)
			return ecNotFound;
		dst_fid = rop_util_make_eid_ex(1, rop_util_gc_to_value(folder_eid.global_counter));
	}

	/* Loop & permission checks. */
	if (par.loop_check.find({newdir, dst_fid}) != par.loop_check.end())
		return ecRootFolder;
	uint32_t permission = 0;
	if (!exmdb_client::get_folder_perm(newdir, dst_fid, par.ev_to, &permission))
		return ecRpcFailed;
	if (!(permission & (frightsOwner | frightsCreate)))
		return ecAccessDenied;

	/* Prepare write */
	message_content_ptr dst(par.ctnt->dup());
	if (dst == nullptr)
		return ecMAPIOOM;
	auto err = rx_npid_replace(par, *dst, newdir);
	if (err != ecSuccess)
		return err;
	if (!exmdb_client::allocate_message_id(newdir, dst_fid, &dst_mid) ||
	    !exmdb_client::allocate_cn(newdir, &dst_cn))
		return ecRpcFailed;
	XID zxid{tgt_public ? rop_util_make_domain_guid(user_id) :
	         rop_util_make_user_guid(domain_id), dst_cn};
	char xidbuf[22];
	BINARY xidbin;
	EXT_PUSH ep;
	if (!ep.init(xidbuf, std::size(xidbuf), 0) ||
	    ep.p_xid(zxid) != pack_result::success)
		return ecMAPIOOM;
	xidbin.pv = xidbuf;
	xidbin.cb = ep.m_offset;
	PCL pcl;
	if (!pcl.append(zxid))
		return ecMAPIOOM;
	std::unique_ptr<BINARY, rx_delete> pclbin(pcl.serialize());
	if (pclbin == nullptr)
		return ecMAPIOOM;
	auto &props = dst->proplist;
	if (!props.has(PR_LAST_MODIFICATION_TIME)) {
		auto last_time = rop_util_current_nttime();
		auto ret = props.set(PR_LAST_MODIFICATION_TIME, &last_time);
		if (ret != 0)
			return ecError;
	}
	int ret;
	if ((ret = props.set(PidTagMid, &dst_mid)) != 0 ||
	    (ret = props.set(PidTagChangeNumber, &dst_cn)) != 0 ||
	    (ret = props.set(PR_CHANGE_KEY, &xidbin)) != 0 ||
	    (ret = props.set(PR_PREDECESSOR_CHANGE_LIST, pclbin.get())) != 0) {
		return ecError;
	}

	/* Writeout */
	ec_error_t e_result = ecRpcFailed;
	if (!exmdb_client::write_message(newdir, other_store.pserver_name, CP_UTF8,
	    dst_fid, dst.get(), &e_result)) {
		mlog(LV_DEBUG, "ruleproc: write_message failed");
		return ecRpcFailed;
	} else if (e_result != ecSuccess) {
		mlog(LV_DEBUG, "ruleproc: write_message: %s\n", mapi_strerror(e_result));
		return ecRpcFailed;
	}
	if (g_ruleproc_debug)
		mlog(LV_DEBUG, "ruleproc: OP_COPY/MOVE to %s:%llxh\n", newdir, LLU{dst_fid});
	if (act_type != OP_MOVE)
		return ecSuccess;

	/* Copy done, delete original message object */
	EID_ARRAY del_mids{};
	del_mids.count = 1;
	del_mids.pids = reinterpret_cast<uint64_t *>(&par.cur.mid);
	BOOL partial = false;
	if (!exmdb_client::delete_messages(par.cur.dir.c_str(),
	    tgt_public ? domain_id : user_id, CP_UTF8,
	    nullptr, par.cur.fid, &del_mids, true, &partial))
		mlog(LV_ERR, "ruleproc: OP_MOVE del_msg %s:%llxh failed",
			par.cur.dir.c_str(), LLU{rop_util_get_gc_value(par.cur.mid)});
	par.cur.dir = newdir;
	par.cur.fid = eid_t(dst_fid);
	par.cur.mid = eid_t(dst_mid);
	return ecSuccess;
}

static ec_error_t op_copy(rxparam &par, const rule_node &rule,
    const MOVECOPY_ACTION &mc, uint8_t act_type)
{
	if (mc.pfolder_eid == nullptr)
		return ecSuccess;
	if (!mc.same_store)
		return op_copy_other(par, rule, mc, act_type);
	auto &svreid = *static_cast<const SVREID *>(mc.pfolder_eid);
	auto dst_fid = svreid.folder_id;
	if (rop_util_get_replid(dst_fid) != 1)
		return ecNotFound;
	if (par.loop_check.find({par.cur.dir, dst_fid}) != par.loop_check.end())
		return ecSuccess;
	uint64_t dst_mid = 0;
	BOOL result = false;
	if (!exmdb_client::allocate_message_id(par.cur.dir.c_str(), dst_fid, &dst_mid))
		return ecRpcFailed;
	if (!exmdb_client::movecopy_message(par.cur.dir.c_str(), 0, CP_ACP,
	    par.cur.mid, dst_fid, dst_mid, act_type == OP_MOVE ? TRUE : false, &result))
		return ecRpcFailed;
	if (act_type == OP_MOVE) {
		par.cur.fid = eid_t(dst_fid);
		par.cur.mid = eid_t(dst_mid);
	}
	return ecSuccess;
}

static BINARY *xid_to_bin(const XID &xid)
{
	EXT_PUSH ext_push;
	auto bin = static_cast<BINARY *>(exmdb_rpc_alloc(sizeof(BINARY)));
	if (bin == nullptr)
		return nullptr;
	bin->pv = exmdb_rpc_alloc(24);
	if (bin->pv == nullptr)
		return nullptr;
	if (!ext_push.init(bin->pv, 24, 0) ||
	    ext_push.p_xid(xid) != EXT_ERR_SUCCESS)
		return nullptr;
	bin->cb = ext_push.m_offset;
	return bin;
}

static ec_error_t op_tag(rxparam &par, const rule_node &rule,
    const TAGGED_PROPVAL *setval)
{
	if (setval == nullptr)
		return ecSuccess;
	uint64_t change_num = 0, modtime = 0;
	if (!exmdb_client::allocate_cn(par.cur.dir.c_str(), &change_num))
		return ecRpcFailed;
	auto change_key = xid_to_bin({GUID{}, change_num});
	if (change_key == nullptr)
		return ecServerOOM;
	const TAGGED_PROPVAL valdata[] = {
		{PidTagChangeNumber, &change_num},
		{PR_CHANGE_KEY, change_key},
		{PR_LOCAL_COMMIT_TIME, &modtime},
		{PR_LAST_MODIFICATION_TIME, &modtime},
		{setval->proptag, setval->pvalue},
	};
	const TPROPVAL_ARRAY valhdr = {std::size(valdata), deconst(valdata)};
	if (valdata[1].pvalue == nullptr)
		return ecServerOOM;
	PROBLEM_ARRAY problems{};
	if (!exmdb_client::set_message_properties(par.cur.dir.c_str(),
	    nullptr, CP_ACP, par.cur.mid, &valhdr, &problems))
		return ecRpcFailed;
	return ecSuccess;
}

static ec_error_t op_read(rxparam &par, const rule_node &rule)
{
	uint64_t cn = 0;
	/* XXX: this RPC cannot cope with nullptr username on public stores */
	if (!exmdb_client::set_message_read_state(par.cur.dir.c_str(),
	    nullptr, par.cur.mid, true, &cn))
		return ecRpcFailed;
	return ecSuccess;
}

static ec_error_t op_switch(rxparam &par, const rule_node &rule,
    const ACTION_BLOCK &act, size_t act_idx)
{
	if (g_ruleproc_debug)
		mlog(LV_DEBUG, "Rule_Action %s", act.repr().c_str());
	switch (act.type) {
	case OP_MOVE:
	case OP_COPY: {
		auto mc = static_cast<MOVECOPY_ACTION *>(act.pdata);
		return mc != nullptr ? op_copy(par, rule, *mc, act.type) : ecSuccess;
	}
	case OP_MARK_AS_READ:
		return op_read(par, rule);
	case OP_TAG:
		return op_tag(par, rule, static_cast<TAGGED_PROPVAL *>(act.pdata));
	case OP_DELETE:
		par.del = true;
		return ecSuccess;
	default:
		return ecSuccess;
	}
}

static ec_error_t op_process(rxparam &par, const rule_node &rule)
{
	if (par.exit && !(rule.state & ST_ONLY_WHEN_OOF))
		return ecSuccess;
	if (rule.cond != nullptr) {
		if (g_ruleproc_debug)
			mlog(LV_DEBUG, "Rule_Condition %s", rule.cond->repr().c_str());
		if (!rx_eval_props(par.ctnt, par.ctnt->proplist, *rule.cond))
			return ecSuccess;
	}
	if (rule.state & ST_EXIT_LEVEL)
		par.exit = true;
	if (rule.act == nullptr)
		return ecSuccess;
	for (size_t i = 0; i < rule.act->count; ++i) {
		auto ret = op_switch(par, rule, rule.act->pblock[i], i);
		if (ret != ecSuccess)
			return ret;
	}
	return ecSuccess;
}

static ec_error_t opx_switch(rxparam &par, const rule_node &rule,
    const EXT_ACTION_BLOCK &act, size_t act_idx)
{
	switch (act.type) {
	case OP_MARK_AS_READ:
		return op_read(par, rule);
	case OP_TAG:
		return op_tag(par, rule, static_cast<TAGGED_PROPVAL *>(act.pdata));
	case OP_DELETE:
		par.del = true;
		return ecSuccess;
	default:
		return ecSuccess;
	}
}

static ec_error_t opx_process(rxparam &par, const rule_node &rule)
{
	if (par.exit && !(rule.state & ST_ONLY_WHEN_OOF))
		return ecSuccess;
	if (rule.cond != nullptr &&
	    !rx_eval_props(par.ctnt, par.ctnt->proplist, *rule.cond))
		return ecSuccess;
	if (rule.state & ST_EXIT_LEVEL)
		par.exit = true;
	for (size_t i = 0; i < rule.xact.count; ++i) {
		auto ret = opx_switch(par, rule, rule.xact.pblock[i], i);
		if (ret != ecSuccess)
			return ret;
	}
	return ecSuccess;
}

static int get_policy_from_message_content(rxparam par)
{
	mlog(LV_ERR, "W-PREC: check policy 1 %s", par.cur.dir.c_str());
	int flags = 0;
	mlog(LV_ERR, "W-PREC: check policy 2 %s", par.cur.dir.c_str());

	mlog(LV_ERR, "W-PREC: check class %s", par.cur.dir.c_str());

    for (size_t i = 0; i < par.ctnt->proplist.count; ++i)
    {
		mlog(LV_ERR, "W-PREC: check policy loop %s", par.cur.dir.c_str());
        auto prop = par.ctnt->proplist.ppropval[i];
		// Check for PR_PROCESS_MEETING_REQUESTS, PR_DECLINE_CONFLICTING_MEETING_REQUESTS,
		// and PR_DECLINE_RECURRING_MEETING_REQUESTS properties
		switch (prop.proptag)
		{
			case PR_SCHDINFO_AUTO_ACCEPT_APPTS:
				if (prop.pvalue)
					flags |= POLICY_PROCESS_MEETING_REQUESTS;
				break;

			case PR_SCHDINFO_DISALLOW_OVERLAPPING_APPTS:
				if (prop.pvalue)
					flags |= POLICY_DECLINE_CONFLICTING_MEETING_REQUESTS;
				break;

			case PR_SCHDINFO_DISALLOW_RECURRING_APPTS:
				if (prop.pvalue)
					flags |= POLICY_DECLINE_RECURRING_MEETING_REQUESTS;
				break;
		}
	}
	mlog(LV_ERR, "W-PREC: check policy done %s", par.cur.dir.c_str());
	mlog(LV_ERR, "W-PREC: check policy done %d", flags);
    return flags;
}

static ec_error_t rx_resource_type(rxparam par, bool *isEquipmentMailbox, bool *isRoomMailbox)
{
    mlog(LV_ERR, "W-PREC: entering rx resource type %s", par.cur.dir.c_str());
    auto pmsg = par.ctnt;

    if (pmsg->children.prcpts != nullptr) {
        mlog(LV_ERR, "W-PREC: recipients is not null %s", par.cur.dir.c_str());
        for (unsigned int i = 0; i < pmsg->children.prcpts->count; ++i) {
            mlog(LV_ERR, "W-PREC: recipient count is: %d", pmsg->children.prcpts->count); 
            auto addrtype =  pmsg->children.prcpts->pparray[i]->get<const char>(PR_ADDRTYPE);
            if (addrtype != nullptr) {
                mlog(LV_ERR, "W-PREC: successfully got address type %s", par.cur.dir.c_str()); 
                auto disptype = pmsg->children.prcpts->pparray[i]->get<const uint32_t>(PR_DISPLAY_TYPE);
				if (*disptype == static_cast<unsigned int>(DT_ROOM)) {
					*isRoomMailbox = true;
				} else if (*disptype == static_cast<unsigned int>(DT_EQUIPMENT)) {
					*isEquipmentMailbox = true;
				}
            }
        }
    }
    mlog(LV_ERR, "W-PREC: equipment mailbox is: %d", *isEquipmentMailbox);
    mlog(LV_ERR, "W-PREC: room mailbox is: %d", *isRoomMailbox);
    mlog(LV_ERR, "W-PREC: resource type checked successfully %s", par.cur.dir.c_str());
    return ecSuccess;
}

static ec_error_t process_meeting_requests(rxparam &par, const char* dir, int policy, bool *meetingresponse) {
	mlog(LV_ERR, "W-PREC: process meeting request starts here %s", par.cur.dir.c_str());
	TARRAY_SET *prcpts;
	const uint8_t responseDeclined = olResponseDeclined;
	const uint8_t responseAccepted = olResponseAccepted;
	const uint8_t notresponded = olResponseNotResponded;
	const uint8_t busy = olBusy;
    std::vector<freebusy_event> intersect;
	char buffer[100];

	auto msg = message_content_init();	
	auto pmsg = par.ctnt; 
	mlog(LV_ERR, "W-PREC: setting pmsg to par.ctnt %s", par.cur.dir.c_str());
	if (pmsg == nullptr)
		return ecError;

	if (pmsg->proplist.count == 0){
		mlog(LV_ERR, "W-PREC: proplist count is 0 %s", par.cur.dir.c_str());
		return ecError;
	}
	mlog(LV_ERR, "W-PREC: proplist count is not zero %s", par.cur.dir.c_str());
	
	auto pmessage_class = pmsg->proplist.get<const char>(PR_MESSAGE_CLASS);
	mlog(LV_ERR, "W-PREC: PR_MESSAGE_CLASS: %s", pmessage_class);
	if (pmessage_class == nullptr){
		pmessage_class = pmsg->proplist.get<char>(PR_MESSAGE_CLASS_A);
		mlog(LV_ERR, "W-PREC: PR_MESSAGE_CLASS_A: %s", pmessage_class);
	}
	if (pmessage_class == nullptr){
		pmessage_class = "IPM.Note";
		mlog(LV_ERR, "W-PREC: IPM for note: %s", pmessage_class);
	}
	/* ignore ATTENDEE when METHOD is "PUBLIC" */
	if (strcasecmp(pmessage_class, "IPM.Appointment") == 0)
		return ecSuccess;
	
	prcpts = tarray_set_init();
	if (prcpts == nullptr)
		return ecError;
	pmsg->set_rcpts_internal(prcpts);

	auto pdisplay_name = par.ctnt->proplist.get<char>(PR_DISPLAY_NAME);
	mlog(LV_ERR, "W-PREC: PR_DISPLAY_NAME: %s", pdisplay_name);

    auto cal_eid = rop_util_make_eid_ex(1, PRIVATE_FID_CALENDAR);
    if (!cal_eid) {
		snprintf(buffer, sizeof(buffer), "Unable to open calendar");
        mlog(LV_ERR, "Unable to open calendar.\n");
        return ecError;
    }

	mlog(LV_ERR, "W-PREC: setting tags %s", par.cur.dir.c_str());
	static constexpr uint32_t tags[] = {
		PR_ENTRYID, PR_MESSAGE_CLASS, PR_START_DATE, PR_END_DATE, PR_RESPONSE_REQUESTED,
		PR_REPLY_REQUESTED, PR_RECIPIENT_TRACKSTATUS, PidLidRecurring, PidLidResponseStatus,
		PidLidBusyStatus, PR_PROCESSED, PR_DISPLAY_NAME,
	};
	static constexpr PROPTAG_ARRAY pt = {std::size(tags), deconst(tags)};
	mlog(LV_ERR, "W-PREC: setting props %s", par.cur.dir.c_str());
	TPROPVAL_ARRAY props{};
	if (!exmdb_client::get_store_properties(dir, CP_UTF8, &pt, &props))
		return ecError;
	mlog(LV_ERR, "W-PREC: store properties gotten successfully %s", par.cur.dir.c_str());

	mlog(LV_ERR, "W-PREC: checking propname buff %s", par.cur.dir.c_str());
	PROPERTY_NAME propname_buff[] = {
		{MNID_ID, PSETID_APPOINTMENT, PidLidRecurring},
		{MNID_ID, PSETID_APPOINTMENT, PidLidResponseStatus},
		{MNID_ID, PSETID_APPOINTMENT, PidLidBusyStatus},
		{MNID_ID, PSETID_MEETING,     PidLidGlobalObjectId},
		{MNID_ID, PSETID_APPOINTMENT, PidLidBusyStatus},
		{MNID_ID, PSETID_COMMON, PidLidCommonStart},
		{MNID_ID, PSETID_COMMON, PidLidCommonEnd},

	};

	mlog(LV_ERR, "W-PREC: load propnames %s", par.cur.dir.c_str());

	const PROPNAME_ARRAY propnames = {std::size(propname_buff), deconst(propname_buff)};
	PROPID_ARRAY propids;
	if (!exmdb_client::get_named_propids(dir, false, &propnames, &propids)){
		mlog(LV_ERR, "W-PREC: cannot get names propids %s", par.cur.dir.c_str());
		return ecError;
	}

	PROPERTY_NAME GlobalObjectId = {MNID_ID, PSETID_MEETING, PidLidGlobalObjectId};

	auto goid = PROP_TAG(PT_BINARY, propids.ppropid[3]);
	auto response_stat = PROP_TAG(PT_LONG, propids.ppropid[1]);
	auto busy_stat = PROP_TAG(PT_LONG, propids.ppropid[2]);

	// RESTRICTION_EXIST rst_1 = {GlobalObjectId.lid};
	// RESTRICTION_PROPERTY prop_goid = {RELOP_EQ, GlobalObjectId.lid, {GlobalObjectId.lid, (&goid)}};
	// RESTRICTION rst_2[2] = {{RES_EXIST, {&rst_1}}, {RES_PROPERTY, {&prop_goid}}};
	// RESTRICTION_AND_OR rst_3 = {std::size(rst_2), rst_2};
	// RESTRICTION rst_4 = {RES_AND, {&rst_3}};

	static constexpr uint8_t fixed_true = 1;

	RESTRICTION_EXIST rst_11 = {PR_MESSAGE_CLASS};
	RESTRICTION_PROPERTY rst_6 = {RELOP_EQ, PR_MESSAGE_CLASS, {PR_MESSAGE_CLASS, deconst("IPM.Appointment")}};
	RESTRICTION_PROPERTY rst_12 = {RELOP_EQ, PR_RESPONSE_REQUESTED, {PR_RESPONSE_REQUESTED, deconst(&fixed_true)}};
	RESTRICTION rst_C3[3] = {{RES_EXIST, {&rst_11}}, {RES_PROPERTY, {&rst_6}}, {RES_PROPERTY, {&rst_12}}};
	RESTRICTION_AND_OR rst_C3_and = {std::size(rst_C3), rst_C3};
	RESTRICTION rst_10 = {RES_AND, {&rst_C3_and}};

	mlog(LV_ERR, "W-PREC: creating the filter: %s", par.cur.dir.c_str());

	uint32_t table_id = 0, row_count = 0;
	if (!exmdb_client::load_content_table(par.cur.dir.c_str(), CP_ACP, cal_eid, nullptr, TABLE_FLAG_NONOTIFICATIONS, &rst_10, nullptr, &table_id, &row_count))
		mlog(LV_ERR, "W-PREC: cannot load table content: %s", par.cur.dir.c_str());
	mlog(LV_ERR, "W-PREC: returned number of rows is: %d", row_count);
	auto cl_0 = make_scope_exit([&]() { exmdb_client::unload_table(par.cur.dir.c_str(), table_id); });

	uint32_t proptag_buff[] = {
		response_stat, busy_stat,
	};
	const PROPTAG_ARRAY proptags = {std::size(proptag_buff), deconst(proptag_buff)};
	TARRAY_SET rows;
	if (!exmdb_client::query_table(par.cur.dir.c_str(), nullptr, CP_ACP, table_id,
	    &proptags, 0, row_count, &rows))
		mlog(LV_ERR, "W-PREC: cannot query table: %s", par.cur.dir.c_str());
	mlog(LV_ERR, "W-PREC: returned number of rows for query table is: %d", rows.count);

	for (size_t i = 0; i < rows.count; ++i) {
		auto ts = rows.pparray[i]->get<const uint8_t>(response_stat);
		if (ts == nullptr)
			mlog(LV_ERR, "W-PREC: cannot get the response status: %s", par.cur.dir.c_str());
		mlog(LV_ERR, "W-PREC: got response status: %u", response_stat);	
		if (response_stat == notresponded){
			mlog(LV_ERR, "W-PREC: not responded: %s", par.cur.dir.c_str());
		} else {
			mlog(LV_ERR, "W-PREC: got response status: %u", response_stat);
		}
	
		auto num = rows.pparray[i]->get<const uint32_t>(busy_stat);
		uint32_t busy_type = num == nullptr || *num > olWorkingElsewhere ? 0 : *num;
	}
	
	cl_0.release();
	if (!exmdb_client::unload_table(par.cur.dir.c_str(), table_id))
		mlog(LV_ERR, "W-PREC: cannot unload table: %s", par.cur.dir.c_str());

	auto use_name = props.get<char>(PR_DISPLAY_NAME);
	mlog(LV_ERR, "W-PREC: PR_DISPLAY_NAME using props: %s", use_name);
	if (!par.ctnt->proplist.get<const uint8_t>(PROP_TAG(PT_LONG, propids.ppropid[1])))
		mlog(LV_ERR, "W-PREC: cannot get the response status of user: %s", use_name);
	if (!msg->proplist.get<const uint8_t>(PROP_TAG(PT_LONG, propids.ppropid[1])))
		mlog(LV_ERR, "W-PREC: cannot get the response status of user: %s", use_name);
	auto cur_resp = par.ctnt->proplist.get<const uint8_t>(PROP_TAG(PT_LONG, propids.ppropid[1]));
	mlog(LV_ERR, "W-PREC: cannot get the response status of user: %s", cur_resp);



	uint32_t out_status = 0;
	mlog(LV_ERR, "W-PREC: check for start date and end date %s", par.cur.dir.c_str());	
	if (par.ctnt->proplist.has(PR_START_DATE) && par.ctnt->proplist.has(PR_END_DATE)){
		auto start = par.ctnt->proplist.get<const uint64_t>(PR_START_DATE);
		auto end = par.ctnt->proplist.get<const uint64_t>(PR_END_DATE);
		mlog(LV_ERR, "Start date: %lu", *start);
		mlog(LV_ERR, "End date: %lu", *end);
		auto start_whole = rop_util_nttime_to_unix(*start);
		auto end_whole = rop_util_nttime_to_unix(*end);
		auto startt = rop_util_unix_to_nttime(start_whole);
		auto endd = rop_util_unix_to_nttime(end_whole);
		
		if (!exmdb_client::appt_meetreq_overlap(dir, use_name, startt, endd, &out_status)){
			snprintf(buffer, sizeof(buffer), "Meeting overlap error");
			mlog(LV_ERR, "W-PREC: Cannot check for meeting overlap %s", par.cur.dir.c_str());
		}
	}
	auto response_requested = par.ctnt->proplist.get<const uint8_t>(PR_RESPONSE_REQUESTED);
	mlog(LV_ERR, "W-PREC: Checking for if response is requested %d", *response_requested);
	if(response_requested){
		*meetingresponse = true;
	}
	
    auto flags = par.ctnt->proplist.get<uint8_t>(PROP_TAG(PT_BOOLEAN, propids.ppropid[0]));

	auto propmessage_class = props.get<const char>(PR_MESSAGE_CLASS);
	mlog(LV_ERR, "W-PREC: PR_MESSAGE_CLASS with prop: %s", propmessage_class);

	// TPROPVAL_ARRAY itemProps{};
    // if (!exmdb_client::get_store_properties(par.cur.dir.c_str(), CP_UTF8, &pt, &itemProps))
    //     return ecError;

    // static const PROPERTY_NAME ResponseStatus = {MNID_ID, PSETID_APPOINTMENT, PidLidResponseStatus};
    // const char* username = itemProps.get<char>(user_name_tag);

	mlog(LV_ERR, "W-PREC: outstatus is: %u", out_status);
	mlog(LV_ERR, "W-PREC: check meeting overlap successful %s", par.cur.dir.c_str());

	bool isEquipmentMailbox = false;
	bool isRoomMailbox = false;
	if(!rx_resource_type(par, &isEquipmentMailbox, &isRoomMailbox))
		mlog(LV_DEBUG, "W-1554: cannot check resource type %s", par.cur.dir.c_str());

    if (isRoomMailbox || isEquipmentMailbox) {
		mlog(LV_ERR, "W-PREC: entering if statement %s", par.cur.dir.c_str());
        // Room mailbox specific processing
        if (par.ctnt->proplist.get<char>(PR_MESSAGE_CLASS) &&
            strcmp(static_cast<const char*>(par.ctnt->proplist.getval(PR_MESSAGE_CLASS)), deconst("IPM.Schedule.Meeting.Request")) == 0) {
				mlog(LV_ERR, "W-PREC: message is a meeting request %s", par.cur.dir.c_str());
                if (flags != nullptr && (policy & POLICY_DECLINE_RECURRING_MEETING_REQUESTS)) {
					if (par.ctnt->proplist.set(PR_MESSAGE_CLASS, "IPM.Schedule.Meeting.Resp.Neg") != 0)
						return ecError;
					snprintf(buffer, sizeof(buffer), "Meeting Declined");
                    mlog(LV_INFO, "Declined due to recurrence against non-recurring policy.\n");
                }
                
				if (policy & POLICY_DECLINE_CONFLICTING_MEETING_REQUESTS) {
					// non-recurring appointments
					if (flags == nullptr || *flags == 0) {
						// At least one conflict
						if (out_status == 1) {
							// itemProps.set(ResponseStatus.lid, &responseDeclined);
							props.set(PROP_TAG(PT_LONG, propids.ppropid[1]), &responseDeclined);
							// if (itemProps.set(PR_MESSAGE_CLASS, "IPM.Schedule.Meeting.Resp.Neg") != 0)
							// 	return ecError;
							if (par.ctnt->proplist.set(PROP_TAG(PT_LONG, propids.ppropid[1]), &responseDeclined) != 0)
								return ecError;
							if (par.ctnt->proplist.set(PR_MESSAGE_CLASS, "IPM.Schedule.Meeting.Resp.Neg") != 0)
								return ecError;
							snprintf(buffer, sizeof(buffer), "Meeting Declined");
							mlog(LV_INFO, "Declined due to conflict.\n");
						}   
					}       
					// recurring appointments
					if(out_status == 1) {
						// At least one conflict
						// itemProps.set(ResponseStatus.lid, &responseDeclined);
						// itemProps.set(ResponseStatus, &olResponseDeclined);
						// if (itemProps.set(PR_MESSAGE_CLASS, "IPM.Schedule.Meeting.Resp.Neg") != 0)
							// return ecError;
						if (par.ctnt->proplist.set(PROP_TAG(PT_LONG, propids.ppropid[1]), &responseDeclined) != 0)
							return ecError;
						if (par.ctnt->proplist.set(PR_MESSAGE_CLASS, "IPM.Schedule.Meeting.Resp.Neg") != 0)
							return ecError;
						snprintf(buffer, sizeof(buffer), "Meeting Declined");
						mlog(LV_INFO, "Declined due to conflict.\n");
					}   
				}
				// itemProps.set(ResponseStatus.lid, &responseAccepted);
				if (par.ctnt->proplist.set(PROP_TAG(PT_LONG, propids.ppropid[1]), &responseAccepted) != 0)
					return ecError;
				if (par.ctnt->proplist.set(PR_MESSAGE_CLASS, "IPM.Schedule.Meeting.Resp.Pos") != 0)
					return ecError;
				mlog(LV_ERR, "W-PREC: PR_MESSAGE_CLASS set to accepted %s", par.cur.dir.c_str());
				if (par.ctnt->proplist.set(PROP_TAG(PT_LONG, propids.ppropid[2]), &busy) != 0)
					return ecError;
				mlog(LV_ERR, "W-PREC: busy status set %s", par.cur.dir.c_str());
				props.set(PROP_TAG(PT_LONG, propids.ppropid[1]), &responseAccepted);
				if (props.set(PR_MESSAGE_CLASS, "IPM.Schedule.Meeting.Resp.Pos") != 0)
					return ecError;
				mlog(LV_ERR, "W-PREC: PR_MESSAGE_CLASS set to accepted using props %s", par.cur.dir.c_str());
				snprintf(buffer, sizeof(buffer), "Meeting Accepted");
				mlog(LV_ERR, "Accepted\n");
			}
		mlog(LV_ERR, "W-PREC: create a response for the tracking status %s", par.cur.dir.c_str());
			
		uint64_t change_num = 0, modtime = 0;
		if (!exmdb_client::allocate_cn(par.cur.dir.c_str(), &change_num))
			return ecRpcFailed;
		auto change_key = xid_to_bin({GUID{}, change_num});
		if (change_key == nullptr)
			return ecServerOOM;
		const TAGGED_PROPVAL valdata[] = {
			{PidTagChangeNumber, &change_num},
			{PR_CHANGE_KEY, change_key},
			{PR_LOCAL_COMMIT_TIME, &modtime},
			{PR_LAST_MODIFICATION_TIME, &modtime},
		};
		// const TPROPVAL_ARRAY valhdr = {std::size(valdata), deconst(valdata)};
		if (valdata[1].pvalue == nullptr)
			return ecServerOOM;
		PROBLEM_ARRAY problems{};
		if (!exmdb_client::set_message_properties(par.cur.dir.c_str(),
			nullptr, CP_ACP, par.cur.mid, &props, &problems))
			return ecRpcFailed;
	}
	mlog(LV_ERR, "W-PREC: finshed the if statement %s", par.cur.dir.c_str());
    return ecSuccess;
}

ec_error_t exmdb_local_rules_execute(const char *dir, const char *ev_from,
    const char *ev_to, eid_t folder_id, eid_t msg_id) try
{
	bool oof = false;
	auto err = rx_is_oof(dir, &oof);
	if (err != ecSuccess)
		return err;
	std::vector<rule_node> rule_list;
	err = rx_load_std_rules(dir, folder_id, oof, rule_list);
	if (err != ecSuccess)
		return err;
	err = rx_load_ext_rules(dir, folder_id, oof, rule_list);
	if (err != ecSuccess)
		return err;
	std::sort(rule_list.begin(), rule_list.end());

	rxparam par = {ev_from, ev_to, {{dir, folder_id}, msg_id}, {{dir, folder_id}}};
	if (!exmdb_client::read_message(par.cur.dir.c_str(), nullptr, CP_ACP,
	    par.cur.mid, &par.ctnt))
		return ecError;
	
	mlog(LV_ERR, "W-PREC: check resource type %s", par.cur.dir.c_str());
	mlog(LV_ERR, "W-PREC: check resource type %s", dir);

	int policy = get_policy_from_message_content(par);
	mlog(LV_ERR, "W-PREC: check policy finished %s", par.cur.dir.c_str());

	mlog(LV_DEBUG, "W-1554: Process meeting request %s", par.cur.dir.c_str());
	mlog(LV_ERR, "W-PREC: Process meeting request %s", par.cur.dir.c_str());
	bool meetingresponse = false;
	err = process_meeting_requests(par, dir, policy, &meetingresponse);
	if (err != ecSuccess){
		return err;
		mlog(LV_WARN, "W-1554: Meeting Processed Done but not successful %s", strerror(ecSuccess));
	}
	mlog(LV_ERR, "W-PREC: Process meeting request done %s", par.cur.dir.c_str());
	auto pmessage_class = par.ctnt->proplist.get<const char>(PR_MESSAGE_CLASS);
	mlog(LV_ERR, "W-PREC: PR_MESSAGE_CLASS: %s", pmessage_class);	

	for (auto &&rule : rule_list) {
		err = rule.extended ? opx_process(par, rule) : op_process(par, rule);
		if (err != ecSuccess)
			return err;
		if (par.del)
			break;
	}
	pmessage_class = par.ctnt->proplist.get<const char>(PR_MESSAGE_CLASS);
	mlog(LV_ERR, "W-PREC: PR_MESSAGE_CLASS: %s", pmessage_class);
	if (par.del) {
		const EID_ARRAY ids = {1, reinterpret_cast<uint64_t *>(&par.cur.mid)};
		BOOL partial;
		if (!exmdb_client::delete_messages(par.cur.dir.c_str(), 0,
		    CP_ACP, nullptr, par.cur.fid, &ids, true/*hard*/, &partial))
			mlog(LV_DEBUG, "ruleproc: deletion unsuccessful");
		return ecSuccess;
	}
	pmessage_class = par.ctnt->proplist.get<const char>(PR_MESSAGE_CLASS);
	mlog(LV_ERR, "W-PREC: PR_MESSAGE_CLASS: %s", pmessage_class);
	if (!exmdb_client::notify_new_mail(par.cur.dir.c_str(),
	    par.cur.fid, par.cur.mid))
		mlog(LV_ERR, "ruleproc: newmail notification unsuccessful");
	pmessage_class = par.ctnt->proplist.get<const char>(PR_MESSAGE_CLASS);
	mlog(LV_ERR, "W-PREC: PR_MESSAGE_CLASS: %s", pmessage_class);
	return ecSuccess;
} catch (const std::bad_alloc &) {
	mlog(LV_ERR, "E-1121: ENOMEM");
	return ecServerOOM;
}
