// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <memory>
#include "common_util.h"
#include "ics_state.h"
#include <gromox/rop_util.hpp>
#include <gromox/idset.hpp>
#include <cstdlib>
#include <cstring>

ICS_STATE::~ICS_STATE()
{
	auto pstate = this;
	if (NULL != pstate->pgiven) {
		idset_free(pstate->pgiven);
		pstate->pgiven = NULL;
	}
	if (NULL != pstate->pseen) {
		idset_free(pstate->pseen);
		pstate->pseen = NULL;
	}
	if (NULL != pstate->pseen_fai) {
		idset_free(pstate->pseen_fai);
		pstate->pseen_fai = NULL;
	}
	if (NULL != pstate->pread) {
		idset_free(pstate->pread);
		pstate->pread = NULL;
	}
}

std::unique_ptr<ics_state> ics_state::create(logon_object *plogon, int type)
{
	std::unique_ptr<ICS_STATE> pstate;
	BINARY tmp_bin;
	try {
		pstate.reset(new ics_state);
	} catch (const std::bad_alloc &) {
		return NULL;
	}
	tmp_bin.cb = sizeof(void*);
	tmp_bin.pv = &plogon;
	pstate->pseen = idset_init(TRUE, REPL_TYPE_GUID);
	if (NULL == pstate->pseen) {
		return NULL;
	}
	if (!pstate->pseen->register_mapping(&tmp_bin, common_util_mapping_replica))
		return NULL;
	switch (type) {
	case ICS_STATE_CONTENTS_DOWN:
		pstate->pgiven = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pgiven) {
			return NULL;
		}
		if (!pstate->pgiven->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		pstate->pseen_fai = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pseen_fai) {
			return NULL;
		}
		if (!pstate->pseen_fai->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		pstate->pread = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pread) {
			return NULL;
		}
		if (!pstate->pread->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		break;
	case ICS_STATE_HIERARCHY_DOWN:
		pstate->pgiven = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pgiven) {
			return NULL;
		}
		if (!pstate->pgiven->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		break;
	case ICS_STATE_CONTENTS_UP:
		pstate->pgiven = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pgiven) {
			return NULL;
		}
		if (!pstate->pgiven->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		pstate->pseen_fai = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pseen_fai) {
			return NULL;
		}
		if (!pstate->pseen_fai->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		pstate->pread = idset_init(TRUE, REPL_TYPE_GUID);
		if (NULL == pstate->pread) {
			return NULL;
		}
		if (!pstate->pread->register_mapping(&tmp_bin,
		    common_util_mapping_replica))
			return NULL;
		break;
	case ICS_STATE_HIERARCHY_UP:
		break;
	}
	pstate->type = type;
	return pstate;
}

BOOL ICS_STATE::append_idset(uint32_t state_property, IDSET *pset)
{
	auto pstate = this;
	switch (state_property) {
	case MetaTagIdsetGiven:
	case MetaTagIdsetGiven1:
		if (NULL != pstate->pgiven) {
			idset_free(pstate->pgiven);
		}
		pstate->pgiven = pset;
		return TRUE;
	case MetaTagCnsetSeen:
		if (NULL != pstate->pseen) {
			if ((ICS_STATE_CONTENTS_UP == pstate->type ||
				ICS_STATE_HIERARCHY_UP == pstate->type) &&
			    !pstate->pseen->check_empty() &&
			    !pset->concatenate(pstate->pseen))
				return FALSE;
			idset_free(pstate->pseen);
		}
		pstate->pseen = pset;
		return TRUE;
	case MetaTagCnsetSeenFAI:
		if (NULL != pstate->pseen_fai) {
			if (ICS_STATE_CONTENTS_UP == pstate->type &&
			    !pstate->pseen_fai->check_empty() &&
			    !pset->concatenate(pstate->pseen_fai))
				return FALSE;
			idset_free(pstate->pseen_fai);
		}
		pstate->pseen_fai = pset;
		return TRUE;
	case MetaTagCnsetRead:
		if (NULL != pstate->pread) {
			if (ICS_STATE_CONTENTS_UP == pstate->type &&
			    !pstate->pread->check_empty() &&
			    !pset->concatenate(pstate->pread))
				return FALSE;
			idset_free(pstate->pread);
		}
		pstate->pread = pset;
		return TRUE;
	}
	return FALSE;
}

TPROPVAL_ARRAY *ICS_STATE::serialize()
{
	struct mdel {
		inline void operator()(BINARY *x) const { rop_util_free_binary(x); }
		inline void operator()(TPROPVAL_ARRAY *x) const { tpropval_array_free(x); }
	};
	auto pstate = this;
	std::unique_ptr<TPROPVAL_ARRAY, mdel> pproplist(tpropval_array_init());
	if (NULL == pproplist) {
		return NULL;
	}
	
	if (ICS_STATE_CONTENTS_DOWN == pstate->type ||
		ICS_STATE_HIERARCHY_DOWN == pstate->type ||
		(ICS_STATE_CONTENTS_UP == pstate->type &&
	    !pstate->pgiven->check_empty())) {
		auto pbin = pstate->pgiven->serialize();
		if (NULL == pbin) {
			return NULL;
		}
		if (pproplist->set(MetaTagIdsetGiven1, pbin) != 0) {
			rop_util_free_binary(pbin);
			return NULL;
		}
		rop_util_free_binary(pbin);
	}
	
	std::unique_ptr<BINARY, mdel> ser(pstate->pseen->serialize());
	if (ser == nullptr || pproplist->set(MetaTagCnsetSeen, ser.get()) != 0)
		return NULL;
	
	if (ICS_STATE_CONTENTS_DOWN == pstate->type ||
		ICS_STATE_CONTENTS_UP == pstate->type) {
		decltype(ser) s(pstate->pseen_fai->serialize());
		if (s == nullptr ||
		    pproplist->set(MetaTagCnsetSeenFAI, s.get()) != 0)
			return NULL;
	}
	
	if (ICS_STATE_CONTENTS_DOWN == pstate->type ||
		(ICS_STATE_CONTENTS_UP == pstate->type &&
	    !pstate->pread->check_empty())) {
		decltype(ser) s(pstate->pread->serialize());
		if (s == nullptr ||
		    pproplist->set(MetaTagCnsetRead, s.get()) != 0)
			return NULL;
	}
	return pproplist.release();
}
