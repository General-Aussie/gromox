// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <gromox/mapidefs.h>

TARRAY_SET* tarray_set_init()
{
	auto pset = gromox::me_alloc<TARRAY_SET>();
	if (NULL == pset) {
		return NULL;
	}
	pset->count = 0;
	auto count = strange_roundup(pset->count, SR_GROW_TPROPVAL_ARRAY);
	pset->pparray = gromox::me_alloc<TPROPVAL_ARRAY *>(count);
	if (NULL == pset->pparray) {
		free(pset);
		return NULL;
	}
	return pset;
}

void tarray_set_free(TARRAY_SET *pset)
{
	for (size_t i = 0; i < pset->count; ++i)
		if (NULL != pset->pparray[i]) {
			tpropval_array_free(pset->pparray[i]);
		}
	free(pset->pparray);
	free(pset);
}

void tarray_set::erase(uint32_t index)
{
	auto pset = this;
	TPROPVAL_ARRAY *parray;
	
	if (index >= pset->count) {
		return;
	}
	parray = pset->pparray[index];
	pset->count --;
	if (index != pset->count) {
		memmove(pset->pparray + index, pset->pparray +
			index + 1, sizeof(void*)*(pset->count - index));
	}
	tpropval_array_free(parray);
}

int tarray_set::append_move(TPROPVAL_ARRAY *pproplist)
{
	auto pset = this;
	
	if (pset->count >= 0xFF00) {
		return ENOSPC;
	}
	auto z = strange_roundup(pset->count, SR_GROW_TPROPVAL_ARRAY);
	if (pset->count + 1 >= z) {
		z += SR_GROW_TPROPVAL_ARRAY;
		auto list = gromox::re_alloc<TPROPVAL_ARRAY *>(pset->pparray, z);
		if (list == nullptr)
			return ENOMEM;
		pset->pparray = list;
	}
	pset->pparray[pset->count++] = pproplist;
	return 0;
}

tarray_set *tarray_set::dup() const
{
	auto pset = this;
	TARRAY_SET *pset1;
	
	pset1 = tarray_set_init();
	if (NULL == pset1) {
		return NULL;
	}
	for (size_t i = 0; i < pset->count; ++i) {
		auto pproplist = pset->pparray[i]->dup();
		if (NULL == pproplist) {
			tarray_set_free(pset1);
			return NULL;
		}
		auto ret = pset1->append_move(pproplist);
		if (ret != 0) {
			tpropval_array_free(pproplist);
			tarray_set_free(pset1);
			errno = ret;
			return NULL;
		}
	}
	return pset1;
}
