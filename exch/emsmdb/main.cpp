// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/paths.h>
#include <gromox/guid.hpp>
#include <gromox/util.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/mail_func.hpp>
#include "emsmdb_ndr.h"
#include <gromox/proc_common.h>
#include "common_util.h"
#include <gromox/config_file.hpp>
#include "logon_object.h"
#include "exmdb_client.h"
#include "rop_processor.h"
#include "bounce_producer.h"
#include "msgchg_grouping.h"
#include "asyncemsmdb_ndr.h"
#include "emsmdb_interface.h"
#include "asyncemsmdb_interface.h"
#include <cstring>
#include <cstdio>
#include "rop_dispatch.h"

using namespace std::string_literals;
using namespace gromox;

enum {
	ecDoDisconnect = 1,
	ecRRegisterPushNotification = 4,
	ecDummyRpc = 6,
	ecDoConnectEx = 10,
	ecDoRpcExt2 = 11,
	ecDoAsyncConnectEx = 14,
};

static int exchange_emsmdb_ndr_pull(int opnum, NDR_PULL* pndr, void **pin);

static int exchange_emsmdb_dispatch(int opnum, const GUID *pobject,
	uint64_t handle, void *pin, void **ppout);

static int exchange_emsmdb_ndr_push(int opnum, NDR_PUSH *pndr, void *pout);

static void exchange_emsmdb_unbind(uint64_t handle);

static int exchange_async_emsmdb_ndr_pull(int opnum,
	NDR_PULL* pndr, void **pin);

static int exchange_async_emsmdb_dispatch(int opnum, const GUID *pobject,
	uint64_t handle, void *pin, void **ppout);

static int exchange_async_emsmdb_ndr_push(int opnum,
	NDR_PUSH *pndr, void *pout);

static void exchange_async_emsmdb_reclaim(uint32_t async_id);

DECLARE_PROC_API();
static DCERPC_ENDPOINT *ep_6001;

static bool exch_emsmdb_reload(std::shared_ptr<CONFIG_FILE> pconfig) try
{
	if (pconfig == nullptr)
		pconfig = config_file_initd("exchange_emsmdb.cfg", get_config_path());
	if (pconfig == nullptr) {
		printf("[exmdb_provider]: config_file_initd exmdb_provider.cfg: %s\n",
		       strerror(errno));
		return false;
	}
	g_rop_debug = pconfig->get_ll("rop_debug");
	return true;
} catch (const cfg_error &) {
	return false;
}

static constexpr DCERPC_INTERFACE interface_emsmdb = {
	"exchangeEMSMDB",
	/* {a4f1db00-ca47-1067-b31f-00dd010662da} */
	{0xa4f1db00, 0xca47, 0x1067, {0xb3, 0x1f}, {0x00, 0xdd, 0x01, 0x06, 0x62, 0xda}},
	0x510000, exchange_emsmdb_ndr_pull, exchange_emsmdb_dispatch,
	exchange_emsmdb_ndr_push, exchange_emsmdb_unbind,
};

static constexpr DCERPC_INTERFACE interface_async_emsmdb = {
	"exchangeAsyncEMSMDB",
	/* {5261574a-4572-206e-b268-6b199213b4e4} */
	{0x5261574a, 0x4572, 0x206e, {0xb2, 0x68}, {0x6b, 0x19, 0x92, 0x13, 0xb4, 0xe4}},
	0x10000, exchange_async_emsmdb_ndr_pull, exchange_async_emsmdb_dispatch,
	exchange_async_emsmdb_ndr_push, nullptr, exchange_async_emsmdb_reclaim,
};

static BOOL proc_exchange_emsmdb(int reason, void **ppdata) try
{
	int max_mail;
	int max_rcpt;
	int async_num;
	uint16_t smtp_port;
	int max_length;
	int max_rule_len;
	char smtp_ip[40];
	int ping_interval;
	int average_blocks;
	char size_buff[32];
	char separator[16];
	char org_name[256];
	int average_handles;
	char temp_buff[256];
	char file_name[256];
	char submit_command[1024], *psearch;
	
	/* path contains the config files directory */
	switch (reason) {
	case PLUGIN_RELOAD:
		exch_emsmdb_reload(nullptr);
		return TRUE;
	case PLUGIN_INIT: {
		LINK_PROC_API(ppdata);
		gx_strlcpy(file_name, get_plugin_name(), GX_ARRAY_SIZE(file_name));
		psearch = strrchr(file_name, '.');
		if (NULL != psearch) {
			*psearch = '\0';
		}
		auto cfg_path = file_name + ".cfg"s;
		auto pfile = config_file_initd(cfg_path.c_str(), get_config_path());
		if (NULL == pfile) {
			printf("[exchange_emsmdb]: config_file_initd %s: %s\n",
			       cfg_path.c_str(), strerror(errno));
			return FALSE;
		}
		static constexpr cfg_directive cfg_default_values[] = {
			{"async_threads_num", "4", CFG_SIZE, "1", "20"},
			{"average_handles", "1000", CFG_SIZE, "100"},
			{"average_mem", "4K", CFG_SIZE, "4K"},
			{"mailbox_ping_interval", "5min", CFG_TIME, "60s", "1h"},
			{"max_ext_rule_length", "510K", CFG_SIZE, "1"},
			{"max_mail_length", "64M", CFG_SIZE, "1"},
			{"max_mail_num", "1000000", CFG_SIZE, "1"},
			{"max_rcpt_num", "256", CFG_SIZE, "1"},
			{"rop_debug", "0"},
			{"separator_for_bounce", " "},
			{"submit_command", "/usr/bin/php " PKGDATADIR "/sa/submit.php"},
			{"smtp_server_ip", "::1"},
			{"smtp_server_port", "25"},
			{"x500_org_name", "Gromox default"},
			{},
		};
		config_file_apply(*pfile, cfg_default_values);
		gx_strlcpy(separator, pfile->get_value("separator_for_bounce"), arsizeof(separator));
		gx_strlcpy(org_name, pfile->get_value("x500_org_name"), arsizeof(org_name));
		printf("[exchange_emsmdb]: x500 org name is \"%s\"\n", org_name);
		average_handles = pfile->get_ll("average_handles");
		printf("[exchange_emsmdb]: average handles number "
			"per context is %d\n", average_handles);
		average_blocks = pfile->get_ll("average_mem") / 256;
		printf("[exchange_emsmdb]: average memory per"
				" context is %d*256\n", average_blocks);
		max_rcpt = pfile->get_ll("max_rcpt_num");
		printf("[exchange_emsmdb]: maximum rcpt number is %d\n", max_rcpt);
		max_mail = pfile->get_ll("max_mail_num");
		printf("[exchange_emsmdb]: maximum mail number is %d\n", max_mail);
		max_length = pfile->get_ll("max_mail_length");
		bytetoa(max_length, size_buff);
		printf("[exchange_emsmdb]: maximum mail length is %s\n", size_buff);
		max_rule_len = pfile->get_ll("max_ext_rule_length");
		bytetoa(max_rule_len, size_buff);
		printf("[exchange_emsmdb]: maximum extended rule length is %s\n", size_buff);
		ping_interval = pfile->get_ll("mailbox_ping_interval");
		itvltoa(ping_interval, temp_buff);
		printf("[exchange_emsmdb]: mailbox ping interval is %s\n",
			temp_buff);
		gx_strlcpy(smtp_ip, pfile->get_value("smtp_server_ip"), arsizeof(smtp_ip));
		smtp_port = pfile->get_ll("smtp_server_port");
		printf("[exchange_emsmdb]: smtp server is [%s]:%hu\n", smtp_ip, smtp_port);
		gx_strlcpy(submit_command, pfile->get_value("submit_command"), arsizeof(submit_command));
		async_num = pfile->get_ll("async_threads_num");
		printf("[exchange_emsmdb]: async threads number is %d\n", async_num);
		if (!exch_emsmdb_reload(pfile))
			return false;
		
#define regsvr(f) register_service(#f, f)
		if (!regsvr(asyncemsmdb_interface_async_wait) ||
		    !regsvr(asyncemsmdb_interface_register_active) ||
		    !regsvr(asyncemsmdb_interface_remove) ||
		    !regsvr(emsmdb_interface_connect_ex) ||
		    !regsvr(emsmdb_interface_disconnect) ||
		    !regsvr(emsmdb_interface_rpc_ext2) ||
		    !regsvr(emsmdb_interface_touch_handle)) {
			printf("[exchange_emsmdb]: service interface registration failure\n");
			return false;
		}
#undef regsvr

		/* host can include wildcard */
		ep_6001 = register_endpoint("*", 6001);
		if (ep_6001 == nullptr) {
			printf("[exchange_emsmdb]: failed to register endpoint with port 6001\n");
			return FALSE;
		}
		if (!register_interface(ep_6001, &interface_emsmdb) ||
		    !register_interface(ep_6001, &interface_async_emsmdb)) {
			printf("[exchange_emsmdb]: failed to register emsmdb interface\n");
			return FALSE;
		}
		bounce_producer_init(separator);
		common_util_init(org_name, average_blocks, max_rcpt, max_mail,
			max_length, max_rule_len, smtp_ip, smtp_port, submit_command);
		msgchg_grouping_init(get_data_path());
		emsmdb_interface_init();
		asyncemsmdb_interface_init(async_num);
		rop_processor_init(average_handles, ping_interval);
		if (bounce_producer_run(get_data_path()) != 0) {
			printf("[exchange_emsmdb]: failed to run bounce producer\n");
			return FALSE;
		}
		if (0 != common_util_run()) {
			printf("[exchange_emsmdb]: failed to run common util\n");
			return FALSE;
		}
		if (0 != exmdb_client_run()) {
			printf("[exchange_emsmdb]: failed to run exmdb client\n");
			return FALSE;
		}
		if (0 != msgchg_grouping_run()) {
			printf("[exchange_emsmdb]: failed to run msgchg grouping\n");
			return FALSE;
		}
		if (0 != emsmdb_interface_run()) {
			printf("[exchange_emsmdb]: failed to run emsmdb interface\n");
			return FALSE;
		}
		if (0 != asyncemsmdb_interface_run()) {
			printf("[exchange_emsmdb]: failed to run asyncemsmdb interface\n");
			return FALSE;
		}
		if (0 != rop_processor_run()) {
			printf("[exchange_emsmdb]: failed to run rop processor\n");
			return FALSE;
		}
		printf("[exchange_emsmdb]: plugin is loaded into system\n");
		return TRUE;
	}
	case PLUGIN_FREE:
		rop_processor_stop();
		asyncemsmdb_interface_stop();
		emsmdb_interface_stop();
		msgchg_grouping_stop();
		common_util_stop();
		asyncemsmdb_interface_free();
		emsmdb_interface_free();
		msgchg_grouping_free();
		common_util_free();
		return TRUE;
	}
	return TRUE;
} catch (const cfg_error &) {
	return false;
}
PROC_ENTRY(proc_exchange_emsmdb);

static int exchange_emsmdb_ndr_pull(int opnum, NDR_PULL* pndr, void **ppin)
{
	switch (opnum) {
	case ecDoDisconnect:
		*ppin = ndr_stack_anew<ECDODISCONNECT_IN>(NDR_STACK_IN);
		if (NULL == *ppin) {
			return NDR_ERR_ALLOC;
		}
		return emsmdb_ndr_pull_ecdodisconnect(pndr, static_cast<ECDODISCONNECT_IN *>(*ppin));
	case ecRRegisterPushNotification:
		*ppin = ndr_stack_anew<ECRREGISTERPUSHNOTIFICATION_IN>(NDR_STACK_IN);
		if (NULL == *ppin) {
			return NDR_ERR_ALLOC;
		}
		return emsmdb_ndr_pull_ecrregisterpushnotification(pndr, static_cast<ECRREGISTERPUSHNOTIFICATION_IN *>(*ppin));
	case ecDummyRpc:
		*ppin = NULL;
		return NDR_ERR_SUCCESS;
	case ecDoConnectEx:
		*ppin = ndr_stack_anew<ECDOCONNECTEX_IN>(NDR_STACK_IN);
		if (NULL == *ppin) {
			return NDR_ERR_ALLOC;
		}
		return emsmdb_ndr_pull_ecdoconnectex(pndr, static_cast<ECDOCONNECTEX_IN *>(*ppin));
	case ecDoRpcExt2:
		*ppin = ndr_stack_anew<ECDORPCEXT2_IN>(NDR_STACK_IN);
		if (NULL == *ppin) {
			return NDR_ERR_ALLOC;
		}
		return emsmdb_ndr_pull_ecdorpcext2(pndr, static_cast<ECDORPCEXT2_IN *>(*ppin));
	case ecDoAsyncConnectEx:
		*ppin = ndr_stack_anew<ECDOASYNCCONNECTEX_IN>(NDR_STACK_IN);
		if (NULL == *ppin) {
			return NDR_ERR_ALLOC;
		}
		return emsmdb_ndr_pull_ecdoasyncconnectex(pndr, static_cast<ECDOASYNCCONNECTEX_IN *>(*ppin));
	default:
		return NDR_ERR_BAD_SWITCH;
	}
}

static int exchange_emsmdb_dispatch(int opnum, const GUID *pobject,
	uint64_t handle, void *pin, void **ppout)
{
	switch (opnum) {
	case ecDoDisconnect: {
		auto in  = static_cast<ECDOASYNCCONNECTEX_IN *>(pin);
		auto out = ndr_stack_anew<ECDODISCONNECT_OUT>(NDR_STACK_OUT);
		if (out == nullptr)
			return DISPATCH_FAIL;
		*ppout = out;
		out->result = emsmdb_interface_disconnect(&in->cxh);
		out->cxh = in->cxh;
		return DISPATCH_SUCCESS;
	}
	case ecRRegisterPushNotification: {
		auto in  = static_cast<ECRREGISTERPUSHNOTIFICATION_IN *>(pin);
		auto out = ndr_stack_anew<ECRREGISTERPUSHNOTIFICATION_OUT>(NDR_STACK_OUT);
		if (out == nullptr)
			return DISPATCH_FAIL;
		*ppout = out;
		out->result = emsmdb_interface_register_push_notification(&in->cxh,
		              in->rpc, in->pctx, in->cb_ctx, in->advise_bits,
		              in->paddr, in->cb_addr, &out->hnotification);
		out->cxh = in->cxh;
		return DISPATCH_SUCCESS;
	}
	case ecDummyRpc:
		*ppout = ndr_stack_anew<int32_t>(NDR_STACK_OUT);
		if (NULL == *ppout) {
			return DISPATCH_FAIL;
		}
		*(int32_t*)*ppout = emsmdb_interface_dummy_rpc(handle);
		return DISPATCH_SUCCESS;
	case ecDoConnectEx: {
		auto in  = static_cast<ECDOCONNECTEX_IN *>(pin);
		auto out = ndr_stack_anew<ECDOCONNECTEX_OUT>(NDR_STACK_OUT);
		if (out == nullptr)
			return DISPATCH_FAIL;
		*ppout = out;
		out->result = emsmdb_interface_connect_ex(handle, &out->cxh,
		              in->puserdn, in->flags, in->conmod, in->limit,
		              in->cpid, in->lcid_string, in->lcid_sort,
		              in->cxr_link, in->cnvt_cps, &out->max_polls,
		              &out->max_retry, &out->retry_delay, &out->cxr,
		              out->pdn_prefix, out->pdisplayname,
		              in->pclient_vers, out->pserver_vers,
		              out->pbest_vers, &in->timestamp, in->pauxin,
		              in->cb_auxin, out->pauxout, &in->cb_auxout);
		out->timestamp = in->timestamp;
		out->cb_auxout = in->cb_auxout;
		return DISPATCH_SUCCESS;
	}
	case ecDoRpcExt2: {
		auto in  = static_cast<ECDORPCEXT2_IN *>(pin);
		auto out = ndr_stack_anew<ECDORPCEXT2_OUT>(NDR_STACK_OUT);
		if (out == nullptr)
			return DISPATCH_FAIL;
		*ppout = out;
		out->result = emsmdb_interface_rpc_ext2(&in->cxh, &in->flags,
		              in->pin, in->cb_in, out->pout, &in->cb_out,
		              in->pauxin, in->cb_auxin, out->pauxout,
		              &in->cb_auxout, &out->trans_time);
		out->cxh = in->cxh;
		out->flags = in->flags;
		out->cb_out = in->cb_out;
		out->cb_auxout = in->cb_auxout;
		return DISPATCH_SUCCESS;
	}
	case ecDoAsyncConnectEx: {
		auto in  = static_cast<ECDOASYNCCONNECTEX_IN *>(pin);
		auto out = ndr_stack_anew<ECDOASYNCCONNECTEX_OUT>(NDR_STACK_OUT);
		if (out == nullptr)
			return DISPATCH_FAIL;
		*ppout = out;
		out->result = emsmdb_interface_async_connect_ex(in->cxh, &out->acxh);
		return DISPATCH_SUCCESS;
	}
	default:
		return DISPATCH_FAIL;
	}
}

static int exchange_emsmdb_ndr_push(int opnum, NDR_PUSH *pndr, void *pout)
{
	switch (opnum) {
	case ecDoDisconnect:
		return emsmdb_ndr_push_ecdodisconnect(pndr, static_cast<ECDODISCONNECT_OUT *>(pout));
	case ecRRegisterPushNotification:
		return emsmdb_ndr_push_ecrregisterpushnotification(pndr, static_cast<ECRREGISTERPUSHNOTIFICATION_OUT *>(pout));
	case ecDummyRpc:
		return emsmdb_ndr_push_ecdummyrpc(pndr, static_cast<int32_t *>(pout));
	case ecDoConnectEx:
		return emsmdb_ndr_push_ecdoconnectex(pndr, static_cast<ECDOCONNECTEX_OUT *>(pout));
	case ecDoRpcExt2:
		return emsmdb_ndr_push_ecdorpcext2(pndr, static_cast<ECDORPCEXT2_OUT *>(pout));
	case ecDoAsyncConnectEx:
		return emsmdb_ndr_push_ecdoasyncconnectex(pndr, static_cast<ECDOASYNCCONNECTEX_OUT *>(pout));
	default:
		return NDR_ERR_BAD_SWITCH;
	}
}

static void exchange_emsmdb_unbind(uint64_t handle)
{
	emsmdb_interface_unbind_rpc_handle(handle);
}

static int exchange_async_emsmdb_ndr_pull(int opnum,
	NDR_PULL* pndr, void **ppin)
{
	switch (opnum) {
	case 0:
		*ppin = ndr_stack_anew<ECDOASYNCWAITEX_IN>(NDR_STACK_IN);
		if (NULL == *ppin) {
			return NDR_ERR_ALLOC;
		}
		return asyncemsmdb_ndr_pull_ecdoasyncwaitex(pndr, static_cast<ECDOASYNCWAITEX_IN *>(*ppin));
	default:
		return NDR_ERR_BAD_SWITCH;
	}
}

static int exchange_async_emsmdb_dispatch(int opnum, const GUID *pobject,
	uint64_t handle, void *pin, void **ppout)
{
	int result;
	uint32_t async_id;
	
	switch (opnum) {
	case 0:
		*ppout = ndr_stack_anew<ECDOASYNCWAITEX_OUT>(NDR_STACK_OUT);
		if (NULL == *ppout) {
			return DISPATCH_FAIL;
		}
		async_id = apply_async_id();
		if (0 == async_id) {
			return DISPATCH_FAIL;
		}
		result = asyncemsmdb_interface_async_wait(async_id, static_cast<ECDOASYNCWAITEX_IN *>(pin),
		         static_cast<ECDOASYNCWAITEX_OUT *>(*ppout));
		if (DISPATCH_PENDING == result) {
			activate_async_id(async_id);
		} else {
			cancel_async_id(async_id);
		}
		return result;
	default:
		return DISPATCH_FAIL;
	}
}

static int exchange_async_emsmdb_ndr_push(int opnum,
	NDR_PUSH *pndr, void *pout)
{
	switch (opnum) {
	case 0:
		return asyncemsmdb_ndr_push_ecdoasyncwaitex(pndr, static_cast<ECDOASYNCWAITEX_OUT *>(pout));
	default:
		return NDR_ERR_BAD_SWITCH;
	}
}

static void exchange_async_emsmdb_reclaim(uint32_t async_id)
{
	asyncemsmdb_interface_reclaim(async_id);
}
