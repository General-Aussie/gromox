// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020 grammm GmbH
// This file is part of Gromox.
#include "vcard.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gromox/fileio.h>
#define MAX_LINE							73

typedef struct _LINE_ITEM {
	char *ptag;
	char *pvalue;
} LINE_ITEM;

static char* vcard_get_comma(char *pstring)
{
	char *ptoken;
	
	ptoken = strchr(pstring, ',');
	if (NULL == ptoken) {
		return NULL;
	}
	*ptoken = '\0';
	return ptoken + 1;
}

static char* vcard_get_semicolon(char *pstring)
{
	int i;
	int tmp_len;
	
	tmp_len = strlen(pstring);
	for (i=0; i<tmp_len; i++) {
		if ('\\' == pstring[i]) {
			if ('\\' == pstring[i + 1] || ';' == pstring[i + 1] ||
				',' == pstring[i + 1]) {
				memmove(pstring + i, pstring + i + 1, tmp_len - i - 1);
				pstring[tmp_len] = '\0';
				tmp_len --;
			} else if ('n' == pstring[i + 1] || 'N' == pstring[i + 1]) {
				pstring[i] = '\r';
				pstring[i + 1] = '\n';
			}
		} else if (';' == pstring[i]) {
			pstring[i] = '\0';
			for (i+=1; i<tmp_len; i++) {
				if (' ' != pstring[i] && '\t' != pstring[i]) {
					break;
				}
			}
			return pstring + i;
		}
	}
	return NULL;
}

void vcard_init(VCARD *pvcard)
{
	double_list_init(pvcard);
}

static void vcard_free_param(VCARD_PARAM *pvparam)
{
	DOUBLE_LIST_NODE *pnode;
	
	if (NULL == pvparam->pparamval_list) {
		free(pvparam);
		return;
	}
	while ((pnode = double_list_get_from_head(pvparam->pparamval_list)) != NULL) {
		free(pnode->pdata);
		free(pnode);
	}
	double_list_free(pvparam->pparamval_list);
	free(pvparam->pparamval_list);
	free(pvparam);
}

static void vcard_free_value(VCARD_VALUE *pvvalue)
{
	DOUBLE_LIST_NODE *pnode;
	
	while ((pnode = double_list_get_from_head(&pvvalue->subval_list)) != NULL) {
		if (NULL != pnode->pdata) {
			free(pnode->pdata);
		}
		free(pnode);
	}
	double_list_free(&pvvalue->subval_list);
	free(pvvalue);
}

static void vcard_free_line(VCARD_LINE *pvline)
{
	DOUBLE_LIST_NODE *pnode;
	
	while ((pnode = double_list_get_from_head(&pvline->param_list)) != NULL)
		vcard_free_param(pnode->pdata);
	double_list_free(&pvline->param_list);
	while ((pnode = double_list_get_from_head(&pvline->value_list)) != NULL)
		vcard_free_value(pnode->pdata);
	double_list_free(&pvline->value_list);
	free(pvline);
}

void vcard_free(VCARD *pvcard)
{
	DOUBLE_LIST_NODE *pnode;
	
	while ((pnode = double_list_get_from_head(pvcard)) != NULL)
		vcard_free_line(pnode->pdata);
	double_list_free(pvcard);
}

static BOOL vcard_retrieve_line_item(char *pline, LINE_ITEM *pitem)
{
	BOOL b_value;
	pitem->ptag = NULL;
	pitem->pvalue = NULL;
	
	b_value = FALSE;
	while ('\0' != *pline) {
		if ((NULL == pitem->ptag ||
			(TRUE == b_value && NULL == pitem->pvalue))
			&& (' ' == *pline || '\t' == *pline)) {
			pline ++;
			continue;
		}
		if (NULL == pitem->ptag) {
			pitem->ptag = pline;
			pline ++;
			continue;
		}
		if (FALSE == b_value) {
			if (':' == *pline) {
				*pline = '\0';
				b_value = TRUE;
			}
		} else {
			if (NULL == pitem->pvalue) {
				pitem->pvalue = pline;
				break;
			}
		}
		pline ++;
	}
	if (NULL == pitem->ptag) {
		return FALSE;
	}
	return TRUE;
}

static char* vcard_get_line(char *pbuff, size_t max_length)
{
	size_t i;
	char *pnext;
	BOOL b_quoted;
	BOOL b_searched = false;
	
	b_quoted = FALSE;
	for (i=0; i<max_length; i++) {
		if ('\r' == pbuff[i]) {
			pbuff[i] = '\0';
			if (!b_searched) {
				b_searched = TRUE;
				if (NULL != strcasestr(pbuff, "QUOTED-PRINTABLE")) {
					b_quoted = TRUE;
				} else {
					b_quoted = FALSE;
				}
			}
			if (TRUE == b_quoted) {
				if ('=' == pbuff[i - 1]) {
					memmove(pbuff + i - 1, pbuff + i, max_length - i);
					pbuff[max_length-1] = '\0';
					max_length --;
					i --;
				} else {
					if ('\n' == pbuff[i + 1]) {
						if (i + 2 < max_length) {
							return pbuff + i + 2;
						}
					} else {
						if (i + 1 < max_length) {
							return pbuff + i + 1;
						}
					}
					return NULL;
				}
			}
			if (i + 1 < max_length && '\n' == pbuff[i + 1]) {
				pnext = pbuff + i + 2;
				if (TRUE == b_quoted) {
					memmove(pbuff + i, pnext, pbuff + max_length - pnext);
					size_t bytes = pbuff + max_length - pnext;
					pbuff[i+bytes] = '\0';
					max_length -= pnext - (pbuff + i);
					continue;
				}
				if (' ' == *pnext || '\t' == *pnext) {
					for (; pnext<pbuff+max_length; pnext++) {
						if (' ' == *pnext || '\t' == *pnext) {
							continue;
						}
						break;
					}
					memmove(pbuff + i, pnext, pbuff + max_length - pnext);
					size_t bytes = pbuff + max_length - pnext;
					pbuff[i+bytes] = '\0';
					max_length -= pnext - (pbuff + i);
					continue;
				}
			} else {
				pnext = pbuff + i + 1;
				if (TRUE == b_quoted) {
					memmove(pbuff + i, pnext, pbuff + max_length - pnext);
					size_t bytes = pbuff + max_length - pnext;
					pbuff[i+bytes] = '\0';
					max_length -= pnext - (pbuff + i);
					continue;
				}
				if (' ' == *pnext || '\t' == *pnext) {
					for (; pnext<pbuff+max_length; pnext++) {
						if (' ' == *pnext || '\t' == *pnext) {
							continue;
						}
						break;
					}
					memmove(pbuff + i, pnext, pbuff + max_length - pnext);
					size_t bytes = pbuff + max_length - pnext;
					pbuff[i+bytes] = '\0';
					max_length -= pnext - (pbuff + i);
					continue;
				}
			}
			return pnext;
		} else if ('\n' == pbuff[i]) {
			pbuff[i] = '\0';
			if (!b_searched) {
				b_searched = TRUE;
				if (NULL != strcasestr(pbuff, "QUOTED-PRINTABLE")) {
					b_quoted = TRUE;
				} else {
					b_quoted = FALSE;
				}
			}
			if (TRUE == b_quoted) {
				if ('=' == pbuff[i - 1]) {
					memmove(pbuff + i - 1, pbuff + i, max_length - i);
					pbuff[max_length-1] = '\0';
					max_length --;
					i --;
				} else {
					if (i + 1 < max_length) {
						return pbuff + i + 1;
					}
				}
			}
			pnext = pbuff + i + 1;
			if (TRUE == b_quoted) {
				memmove(pbuff + i, pnext, pbuff + max_length - pnext);
				size_t bytes = pbuff + max_length - pnext;
				pbuff[i+bytes] = '\0';
				max_length -= pnext - (pbuff + i);
				continue;
			}
			if (' ' == *pnext || '\t' == *pnext) {
				for (; pnext<pbuff+max_length; pnext++) {
					if (' ' == *pnext || '\t' == *pnext) {
						continue;
					}
					break;
				}
				memmove(pbuff + i, pnext, pbuff + max_length - pnext);
				size_t bytes = pbuff + max_length - pnext;
				pbuff[i+bytes] = '\0';
				max_length -= pnext - (pbuff + i);
				continue;
			}
			return pnext;
		}
	}
	return NULL;
}

static BOOL vcard_check_empty_line(const char *pline)
{	
	while ('\0' != *pline) {
		if (' ' != *pline && '\t' != *pline) {
			return FALSE;
		}
	}
	return TRUE;
}

static VCARD_PARAM* vcard_retrieve_param(char *ptag)
{
	char *ptr;
	char *pnext;
	VCARD_PARAM *pvparam;
	
	ptr = strchr(ptag, '=');
	if (NULL != ptr) {
		*ptr = '\0';
	}
	pvparam = vcard_new_param(ptag);
	if (NULL == pvparam) {
		return NULL;
	}
	if (NULL == ptr) {
		return pvparam;
	}
	ptr ++;
	do {
		pnext = vcard_get_comma(ptr);
		if (FALSE == vcard_append_paramval(pvparam, ptr)) {
			vcard_free_param(pvparam);
			return FALSE;
		}
	} while ((ptr = pnext) != NULL);
	return pvparam;
}

static VCARD_LINE* vcard_retrieve_tag(char *ptag)
{
	char *ptr;
	char *pnext;
	VCARD_LINE *pvline;
	VCARD_PARAM *pvparam;
	
	ptr = strchr(ptag, ';');
	if (NULL != ptr) {
		*ptr = '\0';
	}
	pvline = vcard_new_line(ptag);
	if (NULL == pvline) {
		return NULL;
	}
	if (NULL == ptr) {
		return pvline;
	}
	ptr ++;
	do {
		pnext = vcard_get_semicolon(ptr);
		pvparam = vcard_retrieve_param(ptr);
		if (NULL == pvparam) {
			return FALSE;
		}
		vcard_append_param(pvline, pvparam);
	} while ((ptr = pnext) != NULL);
	return pvline;
}

static BOOL vcard_retrieve_value(VCARD_LINE *pvline, char *pvalue)
{
	char *ptr;
	char *ptr1;
	char *pnext;
	char *pnext1;
	VCARD_VALUE *pvvalue;
	
	ptr = pvalue;
	do {
		pvvalue = vcard_new_value();
		if (NULL == pvalue) {
			return FALSE;
		}
		vcard_append_value(pvline, pvvalue);
		pnext = vcard_get_semicolon(ptr);
		ptr1 = ptr;
		do {
			pnext1 = vcard_get_comma(ptr1);
			if ('\0' == *ptr1) {
				if (FALSE == vcard_append_subval(pvvalue, NULL)) {
					return FALSE;
				}
			} else {
				if (FALSE == vcard_append_subval(pvvalue, ptr1)) {
					return FALSE;
				}
			}
		} while ((ptr1 = pnext1) != NULL);
	} while ((ptr = pnext) != NULL);
	return TRUE;
}

static void vcard_unescape_string(char *pstring)
{
	int i;
	int tmp_len;
	
	tmp_len = strlen(pstring);
	for (i=0; i<tmp_len; i++) {
		if ('\\' == pstring[i]) {
			if ('\\' == pstring[i + 1] || ';' == pstring[i + 1] ||
				',' == pstring[i + 1]) {
				memmove(pstring + i, pstring + i + 1, tmp_len - i);
				pstring[tmp_len] = '\0';
				tmp_len --;
			} else if ('n' == pstring[i + 1] || 'N' == pstring[i + 1]) {
				pstring[i] = '\r';
				pstring[i + 1] = '\n';
			}
		}
	}
}

BOOL vcard_retrieve(VCARD *pvcard, char *in_buff)
{
	char *pline;
	char *pnext;
	BOOL b_begin;
	size_t length;
	VCARD_LINE *pvline;
	LINE_ITEM tmp_item;
	VCARD_VALUE *pvvalue;
	DOUBLE_LIST_NODE *pnode;
	
	while ((pnode = double_list_get_from_head(pvcard)) != NULL)
		vcard_free_line(pnode->pdata);
	b_begin = FALSE;
	pline = in_buff;
	length = strlen(in_buff);
	do {
		pnext = vcard_get_line(pline, length - (pline - in_buff));
		if (TRUE == vcard_check_empty_line(pline)) {
			continue;
		}
		if (FALSE == vcard_retrieve_line_item(pline, &tmp_item)) {
			break;
		}
		if (FALSE == b_begin) {
			if (0 == strcasecmp(tmp_item.ptag, "BEGIN") &&
				(NULL != tmp_item.pvalue &&
				0 == strcasecmp(tmp_item.pvalue, "VCARD"))) {
				b_begin = TRUE;
				continue;
			} else {
				break;
			}
		}
		if (0 == strcasecmp(tmp_item.ptag, "END") &&
			(NULL != tmp_item.pvalue &&
			0 == strcasecmp(tmp_item.pvalue, "VCARD"))) {
			return TRUE;
		}
		pvline = vcard_retrieve_tag(tmp_item.ptag);
		if (NULL == pvline) {
			break;
		}
		vcard_append_line(pvcard, pvline);
		if (NULL != tmp_item.pvalue) {
			if (0 == strcasecmp(pvline->name, "ORG") ||
				0 == strcasecmp(pvline->name, "UID") ||
				0 == strcasecmp(pvline->name, "KEY") ||
				0 == strcasecmp(pvline->name, "ADDR") ||
				0 == strcasecmp(pvline->name, "NOTE") ||
				0 == strcasecmp(pvline->name, "LOGO") ||
				0 == strcasecmp(pvline->name, "ROLE") ||
				0 == strcasecmp(pvline->name, "LABLE") ||
				0 == strcasecmp(pvline->name, "PHOTO") ||
				0 == strcasecmp(pvline->name, "SOUND") ||
				0 == strcasecmp(pvline->name, "TITLE") ||
				0 == strcasecmp(pvline->name, "PRODID") ||
				0 == strcasecmp(pvline->name, "VERSION")) {
				pvvalue = vcard_new_value();
				if (NULL == pvvalue) {
					break;
				}
				vcard_append_value(pvline, pvvalue);
				vcard_unescape_string(tmp_item.pvalue);
				if (FALSE == vcard_append_subval(pvvalue, tmp_item.pvalue)) {
					break;
				}
			} else {
				if (FALSE == vcard_retrieve_value(pvline, tmp_item.pvalue)) {
					break;
				}
			}
		}
		
	} while ((pline = pnext) != NULL);
	while ((pnode = double_list_get_from_head(pvcard)) != NULL)
		vcard_free_line(pnode->pdata);
	return FALSE;
}

static size_t vcard_serialize_string(char *pbuff,
	size_t max_length, int line_offset, const char *string)
{
	size_t i;
	size_t offset;
	size_t tmp_len;
	
	if (line_offset >= MAX_LINE) {
		line_offset %= MAX_LINE;
	}
	offset = 0;
	tmp_len = strlen(string);
	for (i=0; i<tmp_len; i++) {
		if (offset >= max_length) {
			return offset;
		}
		if (line_offset >= MAX_LINE) {
			if (offset + 3 >= max_length) {
				return max_length;
			}
			memcpy(pbuff + offset, "\r\n ", 3);
			offset += 3;
			line_offset = 0;
		}
		if ('\\' == string[i] || ';' == string[i] || ',' == string[i]) {
			if (offset + 1 >= max_length) {
				return max_length;
			}
			pbuff[offset] = '\\';
			offset ++;
			if (line_offset >= 0) {
				line_offset ++;
			}
		} else if ('\r' == string[i] && '\n' == string[i + 1]) {
			if (offset + 1 >= max_length) {
				return max_length;
			}
			pbuff[offset] = '\\';
			offset ++;
			pbuff[offset] = 'n';
			offset ++;
			i ++;
			if (line_offset >= 0) {
				line_offset += 2;
			}
			continue;
		}
		pbuff[offset] = string[i];
		offset ++;
		if (line_offset >= 0) {
			line_offset ++;
		}
	}
	return offset;
}

BOOL vcard_serialize(VCARD *pvcard, char *out_buff, size_t max_length)
{
	size_t offset;
	BOOL need_comma;
	size_t line_begin;
	VCARD_LINE *pvline;
	BOOL need_semicolon;
	VCARD_PARAM *pvparam;
	VCARD_VALUE *pvvalue;
	DOUBLE_LIST_NODE *pnode;
	DOUBLE_LIST_NODE *pnode1;
	DOUBLE_LIST_NODE *pnode2;
	
	if (max_length <= 13) {
		return FALSE;
	}
	memcpy(out_buff, "BEGIN:VCARD\r\n", 13);
	offset = 13;
	for (pnode=double_list_get_head(pvcard); NULL!=pnode;
		pnode=double_list_get_after(pvcard, pnode)) {
		line_begin = offset;
		pvline = (VCARD_LINE*)pnode->pdata;
		offset += gx_snprintf(out_buff + offset,
			max_length - offset, "%s", pvline->name);
		if (offset >= max_length) {
			return FALSE;
		}
		for (pnode1=double_list_get_head(&pvline->param_list); NULL!=pnode1;
			pnode1=double_list_get_after(&pvline->param_list, pnode1)) {
			pvparam = (VCARD_PARAM*)pnode1->pdata;
			if (offset + 1 >= max_length) {
				return FALSE;
			}
			out_buff[offset] = ';';
			offset ++;
			if (NULL == pvparam->pparamval_list) {
				offset += gx_snprintf(out_buff + offset,
					max_length - offset, "%s", pvparam->name);
				if (offset >= max_length) {
					return FALSE;
				}
				continue;
			}
			offset += gx_snprintf(out_buff + offset,
				max_length - offset, "%s=", pvparam->name);
			if (offset >= max_length) {
				return FALSE;
			}
			need_comma = FALSE;
			for (pnode2=double_list_get_head(pvparam->pparamval_list);
				NULL!=pnode2; pnode2=double_list_get_after(
				pvparam->pparamval_list, pnode2)) {
				if (FALSE == need_comma) {
					need_comma = TRUE;
				} else {
					if (offset + 1 >= max_length) {
						return FALSE;
					}
					out_buff[offset] = ',';
					offset ++;
				}
				offset += vcard_serialize_string(out_buff + offset,
							max_length - offset, -1, pnode2->pdata);
				if (offset >= max_length) {
					return FALSE;
				}
			}
		}
		out_buff[offset] = ':';
		offset ++;
		if (offset >= max_length) {
			return FALSE;
		}
		need_semicolon = FALSE;
		for (pnode1=double_list_get_head(&pvline->value_list); NULL!=pnode1;
			pnode1=double_list_get_after(&pvline->value_list, pnode1)) {
			pvvalue = (VCARD_VALUE*)pnode1->pdata;
			if (FALSE == need_semicolon) {
				need_semicolon = TRUE;
			} else {
				if (offset + 1 >= max_length) {
					return FALSE;
				}
				out_buff[offset] = ';';
				offset ++;
			}
			need_comma = FALSE;
			for (pnode2=double_list_get_head(&pvvalue->subval_list);
				NULL!=pnode2; pnode2=double_list_get_after(
				&pvvalue->subval_list, pnode2)) {
				if (FALSE == need_comma) {
					need_comma = TRUE;
				} else {
					if (offset + 1 >= max_length) {
						return FALSE;
					}
					out_buff[offset] = ',';
					offset ++;
				}
				if (NULL != pnode2->pdata) {
					offset += vcard_serialize_string(out_buff + offset,
						max_length - offset, offset - line_begin, pnode2->pdata);
					if (offset >= max_length) {
						return FALSE;
					}
				}
			}
		}
		if (offset + 2 >= max_length) {
			return FALSE;
		}
		out_buff[offset] = '\r';
		offset ++;
		out_buff[offset] = '\n';
		offset ++;
	}
	if (offset + 12 > max_length) {
		return FALSE;
	}
	memcpy(out_buff + offset, "END:VCARD\r\n", 12);
	return TRUE;
}

VCARD_LINE* vcard_new_line(const char *name)
{
	VCARD_LINE *pvline;
	
	pvline = malloc(sizeof(VCARD_LINE));
	if (NULL == pvline) {
		return NULL;
	}
	pvline->node.pdata = pvline;
	strncpy(pvline->name, name, VCARD_NAME_LEN);
	double_list_init(&pvline->param_list);
	double_list_init(&pvline->value_list);
	return pvline;
}

void vcard_append_line(VCARD *pvcard, VCARD_LINE *pvline)
{
	double_list_append_as_tail(pvcard, &pvline->node);
}

void vcard_delete_line(VCARD *pvcard, VCARD_LINE *pvline)
{
	double_list_remove(pvcard, &pvline->node);
	vcard_free_line(pvline);
}

VCARD_PARAM* vcard_new_param(const char*name)
{
	VCARD_PARAM *pvparam;
	
	pvparam = malloc(sizeof(VCARD_PARAM));
	if (NULL == pvparam) {
		return NULL;
	}
	pvparam->node.pdata = pvparam;
	strncpy(pvparam->name, name, VCARD_NAME_LEN);
	pvparam->pparamval_list = NULL;
	return pvparam;
}

BOOL vcard_append_paramval(VCARD_PARAM *pvparam, const char *paramval)
{
	BOOL b_list;
	DOUBLE_LIST_NODE *pnode;
	
	if (NULL == pvparam->pparamval_list) {
		b_list = TRUE;
		pvparam->pparamval_list = malloc(sizeof(DOUBLE_LIST));
		if (NULL == pvparam->pparamval_list) {
			return FALSE;
		}
		double_list_init(pvparam->pparamval_list);
	} else {
		b_list = FALSE;
	}
	pnode = malloc(sizeof(DOUBLE_LIST_NODE));
	if (NULL == pnode) {
		if (TRUE == b_list) {
			double_list_free(pvparam->pparamval_list);
			free(pvparam->pparamval_list);
			pvparam->pparamval_list = NULL;
		}
		return FALSE;
	}
	pnode->pdata = strdup(paramval);
	if (NULL == pnode->pdata) {
		free(pnode);
		if (TRUE == b_list) {
			double_list_free(pvparam->pparamval_list);
			free(pvparam->pparamval_list);
			pvparam->pparamval_list = NULL;
		}
		return FALSE;
	}
	double_list_append_as_tail(pvparam->pparamval_list, pnode);
	return TRUE;
}

void vcard_append_param(VCARD_LINE *pvline, VCARD_PARAM *pvparam)
{
	double_list_append_as_tail(&pvline->param_list, &pvparam->node);
}

VCARD_VALUE* vcard_new_value()
{
	VCARD_VALUE *pvvalue;
	
	pvvalue = malloc(sizeof(VCARD_VALUE));
	if (NULL == pvvalue) {
		return NULL;
	}
	pvvalue->node.pdata = pvvalue;
	double_list_init(&pvvalue->subval_list);
	return pvvalue;
}

BOOL vcard_append_subval(VCARD_VALUE *pvvalue, const char *subval)
{
	DOUBLE_LIST_NODE *pnode;
	
	pnode = malloc(sizeof(DOUBLE_LIST_NODE));
	if (NULL == pnode) {
		return FALSE;
	}
	if (NULL != subval) {
		pnode->pdata = strdup(subval);
		if (NULL == pnode->pdata) {
			free(pnode);
			return FALSE;
		}
	} else {
		pnode->pdata = NULL;
	}
	double_list_append_as_tail(&pvvalue->subval_list, pnode);
	return TRUE;
}

void vcard_append_value(VCARD_LINE *pvline, VCARD_VALUE *pvvalue)
{
	double_list_append_as_tail(&pvline->value_list, &pvvalue->node);
}

const char* vcard_get_first_subvalue(VCARD_LINE *pvline)
{
	VCARD_VALUE *pvvalue;
	DOUBLE_LIST_NODE *pnode;
	DOUBLE_LIST_NODE *pnode1;
	
	pnode = double_list_get_head(&pvline->value_list);
	if (NULL == pnode) {
		return NULL;
	}
	pvvalue = (VCARD_VALUE*)pnode->pdata;
	pnode1 = double_list_get_head(&pvvalue->subval_list);
	if (NULL == pnode1) {
		return NULL;
	}
	return pnode1->pdata;
}

VCARD_LINE* vcard_new_simple_line(const char *name, const char *value)
{
	VCARD_LINE *pvline;
	VCARD_VALUE *pvvalue;
	
	pvline = vcard_new_line(name);
	if (NULL == pvline) {
		return NULL;
	}
	pvvalue = vcard_new_value();
	if (NULL == pvvalue) {
		vcard_free_line(pvline);
		return NULL;
	}
	vcard_append_value(pvline, pvvalue);
	if (FALSE == vcard_append_subval(pvvalue, value)) {
		vcard_free_line(pvline);
		return NULL;
	}
	return pvline;
}
