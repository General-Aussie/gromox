// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020 grommunio GmbH
// This file is part of Gromox.
#include <cstdint>
#include <gromox/defs.h>
#include <gromox/guid.hpp>
#include <gromox/mapidefs.h>
#include <gromox/rop_util.hpp>
#include <gromox/oxvcard.hpp>
#include <gromox/vcard.hpp>
#include <gromox/util.hpp>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>

using namespace gromox;

static const uint32_t g_n_proptags[] = 
	{PROP_TAG_SURNAME, PROP_TAG_GIVENNAME, PROP_TAG_MIDDLENAME,
	PR_DISPLAY_NAME_PREFIX, PROP_TAG_GENERATION};
static const uint32_t g_workaddr_proptags[] =
	{0x8000001F, 0x8001001F, 0x8002001F, 0x8003001F, 0x8004001F, 0x8005001F};
static constexpr uint32_t g_homeaddr_proptags[] =
	{PR_HOME_ADDRESS_POST_OFFICE_BOX, PR_HOME_ADDRESS_STREET,
	PR_HOME_ADDRESS_CITY, PR_HOME_ADDRESS_STATE_OR_PROVINCE,
	PR_HOME_ADDRESS_POSTAL_CODE, PR_HOME_ADDRESS_COUNTRY};
static constexpr uint32_t g_otheraddr_proptags[] =
	{PR_OTHER_ADDRESS_POST_OFFICE_BOX, PR_OTHER_ADDRESS_STREET,
	PR_OTHER_ADDRESS_CITY, PR_OTHER_ADDRESS_STATE_OR_PROVINCE,
	PR_OTHER_ADDRESS_POSTAL_CODE, PR_OTHER_ADDRESS_COUNTRY};
static const uint32_t g_email_proptags[] =
	{0x8006001F, 0x8007001F, 0x8008001F};
static const uint32_t g_im_proptag = 0x8009001F;
static const uint32_t g_categories_proptag = 0x800A101F;
static const uint32_t g_bcd_proptag = 0x800B0102;
static const uint32_t g_ufld_proptags[] = 
	{0x800C001F, 0x800D001F, 0x800E001F, 0x800F001F};
static const uint32_t g_fbl_proptag = 0x8010001F;
static const uint32_t g_vcarduid_proptag = 0x8011001F;

static BOOL oxvcard_check_compatible(const VCARD *pvcard)
{
	BOOL b_version;
	DOUBLE_LIST *plist;
	VCARD_LINE *pvline;
	const char *pstring;
	DOUBLE_LIST_NODE *pnode;
	
	b_version = FALSE;
	plist = (DOUBLE_LIST*)pvcard;
	for (pnode=double_list_get_head(plist); NULL!=pnode;
		pnode=double_list_get_after(plist, pnode)) {
		pvline = (VCARD_LINE*)pnode->pdata;
		if (0 == strcasecmp(pvline->name, "VERSION")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				return FALSE;
			}
			if (strcmp(pstring, "3.0") != 0 &&
			    strcmp(pstring, "4.0") != 0)
				return FALSE;
			b_version = TRUE;
		}
	}
	return b_version ? TRUE : FALSE;
}

static BOOL oxvcard_get_propids(PROPID_ARRAY *ppropids,
	GET_PROPIDS get_propids)
{
	PROPERTY_NAME bf[18];
	size_t z = 0;
	
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidWorkAddressPostOfficeBox;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidWorkAddressStreet;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidWorkAddressCity;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidWorkAddressState;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidWorkAddressPostalCode;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidWorkAddressCountry;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidEmail1EmailAddress;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidEmail2EmailAddress;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidEmail3EmailAddress;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidInstantMessagingAddress;
	rop_util_get_common_pset(PS_PUBLIC_STRINGS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidCategories;
	rop_util_get_common_pset(PS_PUBLIC_STRINGS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidBusinessCardDisplayDefinition;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidContactUserField1;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidContactUserField2;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidContactUserField3;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidContactUserField4;
	rop_util_get_common_pset(PSETID_ADDRESS, &bf[z].guid);
	bf[z].kind = MNID_ID;
	bf[z++].lid = PidLidFreeBusyLocation;
	rop_util_get_common_pset(PSETID_GROMOX, &bf[z].guid);
	bf[z].kind = MNID_STRING;
	bf[z++].pname = deconst("vcarduid");

	PROPNAME_ARRAY propnames;
	propnames.count = z;
	propnames.ppropname = bf;
	return get_propids(&propnames, ppropids);
}

MESSAGE_CONTENT* oxvcard_import(
	const VCARD *pvcard, GET_PROPIDS get_propids)
{
	int i;
	int count;
	int tmp_len;
	int ufld_count;
	int mail_count;
	int list_count;
	BINARY tmp_bin;
	uint16_t propid;
	BOOL b_encoding;
	uint32_t proptag;
	struct tm tmp_tm;
	uint8_t tmp_byte;
	size_t decode_len;
	uint32_t tmp_int32;
	uint64_t tmp_int64;
	DOUBLE_LIST *plist;
	VCARD_LINE *pvline;
	const char *pstring;
	char* child_buff[16];
	VCARD_VALUE *pvvalue;
	VCARD_PARAM *pvparam;
	PROPID_ARRAY propids;
	MESSAGE_CONTENT *pmsg;
	BINARY_ARRAY bin_array;
	const char *photo_type;
	DOUBLE_LIST_NODE *pnode;
	DOUBLE_LIST_NODE *pnode1;
	DOUBLE_LIST_NODE *pnode2;
	const char *address_type;
	STRING_ARRAY child_strings;
	STRING_ARRAY strings_array;
	ATTACHMENT_LIST *pattachments;
	ATTACHMENT_CONTENT *pattachment;
	char tmp_buff[VCARD_MAX_BUFFER_LEN];
	
	mail_count = 0;
	ufld_count = 0;
	child_strings.count = 0;
	child_strings.ppstr = child_buff;
	if (FALSE == oxvcard_check_compatible(pvcard)) {
		return NULL;
	}
	pmsg = message_content_init();
	if (NULL == pmsg) {
		return NULL;
	}
	if (pmsg->proplist.set(PR_MESSAGE_CLASS, "IPM.Contact") != 0)
		goto IMPORT_FAILURE;
	plist = (DOUBLE_LIST*)pvcard;
	for (pnode=double_list_get_head(plist); NULL!=pnode;
		pnode=double_list_get_after(plist, pnode)) {
		pvline = (VCARD_LINE*)pnode->pdata;
		if (strcasecmp(pvline->name, "UID") == 0) {
			/* Deviation from MS-OXVCARD v8.3 §2.1.3.7.7 */
			pstring = vcard_get_first_subvalue(pvline);
			if (pstring == nullptr)
				goto IMPORT_FAILURE;
			if (pmsg->proplist.set(g_vcarduid_proptag, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "FN")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				goto IMPORT_FAILURE;
			}
			if (pmsg->proplist.set(PR_DISPLAY_NAME, pstring) != 0 ||
			    pmsg->proplist.set(PR_NORMALIZED_SUBJECT, tmp_buff) != 0 ||
			    pmsg->proplist.set(PROP_TAG_CONVERSATIONTOPIC, tmp_buff) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "N")) {
			count = 0;
			for (pnode1=double_list_get_head(&pvline->value_list);
				NULL!=pnode1; pnode1=double_list_get_after(
				&pvline->value_list, pnode1)) {
				if (count > 4) {
					break;
				}
				pvvalue = (VCARD_VALUE*)pnode1->pdata;
				list_count = double_list_get_nodes_num(&pvvalue->subval_list);
				if (list_count > 1) {
					goto IMPORT_FAILURE;
				}
				pnode2 = double_list_get_head(&pvvalue->subval_list);
				if (pnode2 != nullptr && pnode2->pdata != nullptr &&
				    pmsg->proplist.set(g_n_proptags[count], pnode2->pdata) != 0)
					goto IMPORT_FAILURE;
				count ++;
			}
		} else if (0 == strcasecmp(pvline->name, "NICKNAME")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (pstring != nullptr &&
			    pmsg->proplist.set(PROP_TAG_NICKNAME, pstring) != 0)
					goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "PHOTO")) {
			if (NULL != pmsg->children.pattachments) {
				goto IMPORT_FAILURE;
			}
			b_encoding = FALSE;
			photo_type = NULL;
			for (pnode1=double_list_get_head(&pvline->param_list);
				NULL!=pnode1; pnode1=double_list_get_after(
				&pvline->param_list, pnode1)) {
				pvparam = (VCARD_PARAM*)pnode1->pdata;
				if (0 == strcasecmp(pvparam->name, "ENCODING")) {
					if (NULL == pvparam->pparamval_list) {
						goto IMPORT_FAILURE;
					}
					pnode2 = double_list_get_head(pvparam->pparamval_list);
					if (pnode2 == nullptr || pnode2->pdata == nullptr ||
					    strcasecmp(static_cast<char *>(pnode2->pdata), "b") != 0)
						goto IMPORT_FAILURE;
					b_encoding = TRUE;
				} else if (0 == strcasecmp(pvparam->name, "TYPE")) {
					if (NULL == pvparam->pparamval_list) {
						goto IMPORT_FAILURE;
					}
					pnode2 = double_list_get_head(pvparam->pparamval_list);
					if (NULL == pnode2 || NULL == pnode2->pdata) {
						goto IMPORT_FAILURE;
					}
					photo_type = static_cast<char *>(pnode2->pdata);
				}
			}
			if (FALSE == b_encoding || NULL == photo_type) {
				goto IMPORT_FAILURE;
			}
			if (0 != strcasecmp(photo_type, "jpeg") &&
				0 != strcasecmp(photo_type, "jpg") &&
				0 != strcasecmp(photo_type, "bmp") &&
				0 != strcasecmp(photo_type, "gif") &&
				0 != strcasecmp(photo_type, "png")) {
				goto IMPORT_FAILURE;
			}
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				goto IMPORT_FAILURE;
			}
			pattachments = attachment_list_init();
			if (NULL == pattachments) {
				goto IMPORT_FAILURE;
			}
			message_content_set_attachments_internal(pmsg, pattachments);
			pattachment = attachment_content_init();
			if (NULL == pattachment) {
				goto IMPORT_FAILURE;
			}
			if (FALSE == attachment_list_append_internal(
				pattachments, pattachment)) {
				attachment_content_free(pattachment);
				goto IMPORT_FAILURE;
			}
			tmp_len = strlen(pstring);
			if (0 != decode64(pstring, tmp_len, tmp_buff, &decode_len)) {
				goto IMPORT_FAILURE;
			}
			tmp_bin.pc = tmp_buff;
			tmp_bin.cb = decode_len;
			if (pattachment->proplist.set(PR_ATTACH_DATA_BIN, &tmp_bin) != 0 ||
			    pattachment->proplist.set(PR_ATTACH_EXTENSION, photo_type) != 0)
				goto IMPORT_FAILURE;
			snprintf(tmp_buff, arsizeof(tmp_buff), "ContactPhoto.%s", photo_type);
			if (pattachment->proplist.set(PR_ATTACH_LONG_FILENAME, tmp_buff) != 0)
				goto IMPORT_FAILURE;
			tmp_byte = 1;
			if (pmsg->proplist.set(PR_ATTACHMENT_CONTACTPHOTO, &tmp_byte) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "BDAY")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (pstring == nullptr)
				continue;
			memset(&tmp_tm, 0, sizeof(tmp_tm));
			if (NULL != strptime(pstring, "%Y-%m-%d", &tmp_tm)) {
				tmp_int64 = rop_util_unix_to_nttime(
							mktime(&tmp_tm) - timezone);
				if (pmsg->proplist.set(PROP_TAG_BIRTHDAY, &tmp_int64) != 0)
					goto IMPORT_FAILURE;
			}
		} else if (0 == strcasecmp(pvline->name, "ADR")) {
			pnode1 = double_list_get_head(&pvline->param_list);
			if (NULL == pnode1) {
				goto IMPORT_FAILURE;
			}
			pvparam = (VCARD_PARAM*)pnode1->pdata;
			if (0 != strcasecmp(pvparam->name, "TYPE") ||
				NULL == pvparam->pparamval_list) {
				goto IMPORT_FAILURE;
			}
			pnode2 = double_list_get_head(pvparam->pparamval_list);
			address_type = pnode2 != nullptr && pnode2->pdata != nullptr ? static_cast<char *>(pnode2->pdata) : "";
			count = 0;
			for (pnode1=double_list_get_head(&pvline->value_list);
				NULL!=pnode1; pnode1=double_list_get_after(
				&pvline->value_list, pnode1)) {
				if (count > 5) {
					break;
				}
				pvvalue = (VCARD_VALUE*)pnode1->pdata;
				list_count = double_list_get_nodes_num(&pvvalue->subval_list);
				if (list_count > 1) {
					goto IMPORT_FAILURE;
				}
				pnode2 = double_list_get_head(&pvvalue->subval_list);
				if (NULL != pnode2) {
					if (NULL == pnode2->pdata) {
						continue;
					}
					uint32_t tag;
					if (0 == strcasecmp(address_type, "work")) {
						tag = g_workaddr_proptags[count];
					} else if (0 == strcasecmp(address_type, "home")) {
						tag = g_homeaddr_proptags[count];
					} else {
						tag = g_otheraddr_proptags[count];
					}
					if (pmsg->proplist.set(tag, pnode2->pdata) != 0)
						goto IMPORT_FAILURE;
				}
				count ++;
			}
		} else if (0 == strcasecmp(pvline->name, "TEL")) {
			pnode1 = double_list_get_head(&pvline->param_list);
			if (NULL == pnode1) {
				goto IMPORT_FAILURE;
			}
			pvparam = (VCARD_PARAM*)pnode1->pdata;
			if (0 != strcasecmp(pvparam->name, "TYPE") ||
				NULL == pvparam->pparamval_list) {
				goto IMPORT_FAILURE;
			}
			pnode2 = double_list_get_head(pvparam->pparamval_list);
			if (NULL == pnode2 || NULL == pnode2->pdata) {
				goto IMPORT_FAILURE;
			}
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			uint32_t tag = 0;
			auto keyword = static_cast<char *>(pnode2->pdata);
			if (strcasecmp(keyword, "home") == 0) {
				pnode1 = double_list_get_after(
					&pvline->param_list, pnode1);
				if (NULL == pnode1) {
					tag = pmsg->proplist.has(PR_HOME_TELEPHONE_NUMBER) ?
					      PR_HOME2_TELEPHONE_NUMBER :
					      PR_HOME_TELEPHONE_NUMBER;
				} else if (0 == strcasecmp(((VCARD_PARAM*)
					pnode1->pdata)->name, "fax")) {
					tag = PR_HOME_FAX_NUMBER;
				} else {
					goto IMPORT_FAILURE;
				}
			} else if (strcasecmp(keyword, "voice") == 0) {
				tag = PR_OTHER_TELEPHONE_NUMBER;
			} else if (strcasecmp(keyword, "work") == 0) {
				pnode1 = double_list_get_after(
					&pvline->param_list, pnode1);
				if (NULL == pnode1) {
					tag = pmsg->proplist.has(PR_BUSINESS_TELEPHONE_NUMBER) ?
					      PR_BUSINESS2_TELEPHONE_NUMBER :
					      PR_BUSINESS_TELEPHONE_NUMBER;
				} else if (0 == strcasecmp(((VCARD_PARAM*)
					pnode1->pdata)->name, "fax")) {
					tag = PR_BUSINESS_FAX_NUMBER;
				} else {
					goto IMPORT_FAILURE;
				}
			} else if (strcasecmp(keyword, "cell") == 0) {
				tag = PR_MOBILE_TELEPHONE_NUMBER;
			} else if (strcasecmp(keyword, "pager") == 0) {
				tag = PR_PAGER_TELEPHONE_NUMBER;
			} else if (strcasecmp(keyword, "car") == 0) {
				tag = PR_CAR_TELEPHONE_NUMBER;
			} else if (strcasecmp(keyword, "isdn") == 0) {
				tag = PR_ISDN_NUMBER;
			} else if (strcasecmp(keyword, "pref") == 0) {
				tag = PR_PRIMARY_TELEPHONE_NUMBER;
			} else {
				continue;
			}
			if (pmsg->proplist.set(tag, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "EMAIL")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (mail_count > 2)
				continue;
			if (pmsg->proplist.set(g_email_proptags[mail_count++], pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "TITLE")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (pmsg->proplist.set(PROP_TAG_TITLE, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "ROLE")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (pmsg->proplist.set(PROP_TAG_PROFESSION, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "ORG")) {
			pnode1 = double_list_get_head(&pvline->value_list);
			if (NULL == pnode1) {
				continue;
			}
			if (NULL != pnode1->pdata) {
				pvvalue = (VCARD_VALUE*)pnode1->pdata;
				pnode2 = double_list_get_head(&pvvalue->subval_list);
				if (pnode2 != nullptr && pnode2->pdata != nullptr &&
				    pmsg->proplist.set(PROP_TAG_COMPANYNAME, pnode2->pdata) != 0)
					goto IMPORT_FAILURE;
			}
			pnode1 = double_list_get_after(&pvline->value_list, pnode1);
			if (NULL != pnode1 && NULL != pnode1->pdata) {
				pvvalue = (VCARD_VALUE*)pnode1->pdata;
				pnode2 = double_list_get_head(&pvvalue->subval_list);
				if (pnode2 != nullptr && pnode2->pdata != nullptr &&
				    pmsg->proplist.set(PROP_TAG_DEPARTMENTNAME, pnode2->pdata) != 0)
					goto IMPORT_FAILURE;
			}
		} else if (strcasecmp(pvline->name, "CATEGORIES") == 0) {
			pnode1 = double_list_get_head(&pvline->value_list);
			if (NULL == pnode1) {
				continue;
			}
			pvvalue = (VCARD_VALUE*)pnode1->pdata;
			strings_array.count = 0;
			strings_array.ppstr = (char**)tmp_buff;
			for (pnode2=double_list_get_head(&pvvalue->subval_list);
				NULL!=pnode2; pnode2=double_list_get_after(
				&pvvalue->subval_list, pnode2)) {
				if (NULL == pnode2->pdata) {
					continue;
				}
				strings_array.ppstr[strings_array.count] = static_cast<char *>(pnode2->pdata);
				strings_array.count ++;
			}
			if (strings_array.count != 0 && strings_array.count < 128 &&
			    pmsg->proplist.set(g_categories_proptag, &strings_array) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "NOTE")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (pmsg->proplist.set(PR_BODY, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "REV")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			memset(&tmp_tm, 0, sizeof(tmp_tm));
			if (NULL != strptime(pstring, "%Y-%m-%dT%H:%M:%S", &tmp_tm)) {
				tmp_int64 = rop_util_unix_to_nttime(
						mktime(&tmp_tm) - timezone);
				if (pmsg->proplist.set(PR_LAST_MODIFICATION_TIME, &tmp_int64) != 0)
					goto IMPORT_FAILURE;
			}
		} else if (0 == strcasecmp(pvline->name, "URL")) {
			pnode1 = double_list_get_head(&pvline->param_list);
			if (NULL == pnode1) {
				goto IMPORT_FAILURE;
			}
			pvparam = (VCARD_PARAM*)pnode1->pdata;
			if (0 != strcasecmp(pvparam->name, "TYPE") ||
				NULL == pvparam->pparamval_list) {
				goto IMPORT_FAILURE;
			}
			pnode2 = double_list_get_head(pvparam->pparamval_list);
			if (NULL == pnode2 || NULL == pnode2->pdata) {
				goto IMPORT_FAILURE;
			}
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			uint32_t tag;
			auto keyword = static_cast<char *>(pnode2->pdata);
			if (strcasecmp(keyword, "home") == 0) {
				tag = PROP_TAG_PERSONALHOMEPAGE;
			} else if (strcasecmp(keyword, "work") == 0) {
				tag = PR_BUSINESS_HOME_PAGE;
			} else {
				continue;
			}
			if (pmsg->proplist.set(tag, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (strcasecmp(pvline->name, "CLASS") == 0 &&
		    (pstring = vcard_get_first_subvalue(pvline)) != nullptr) {
			if (0 == strcasecmp(pstring, "PRIVATE")) {
				tmp_int32 = SENSITIVITY_PRIVATE;
			} else if (0 == strcasecmp(pstring, "CONFIDENTIAL")) {
				tmp_int32 = SENSITIVITY_COMPANY_CONFIDENTIAL;
			} else {
				tmp_int32 = SENSITIVITY_NONE;
			}
			if (pmsg->proplist.set(PR_SENSITIVITY, &tmp_int32) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "KEY")) {
			pnode1 = double_list_get_head(&pvline->param_list);
			if (NULL == pnode1) {
				goto IMPORT_FAILURE;
			}
			pvparam = (VCARD_PARAM*)pnode1->pdata;
			if (0 != strcasecmp(pvparam->name, "ENCODING") ||
				NULL == pvparam->pparamval_list) {
				goto IMPORT_FAILURE;
			}
			pnode2 = double_list_get_head(pvparam->pparamval_list);
			if (pnode2 == nullptr || pnode2->pdata == nullptr ||
			    strcasecmp(static_cast<char *>(pnode2->pdata), "b") != 0)
				goto IMPORT_FAILURE;
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				goto IMPORT_FAILURE;
			}
			tmp_len = strlen(pstring);
			if (0 != decode64(pstring, tmp_len, tmp_buff, &decode_len)) {
				goto IMPORT_FAILURE;
			}
			bin_array.count = 1;
			bin_array.pbin = &tmp_bin;
			tmp_bin.pc = tmp_buff;
			tmp_bin.cb = decode_len;
			if (pmsg->proplist.set(PROP_TAG_USERX509CERTIFICATE, &bin_array) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "X-MS-OL-DESIGN")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			tmp_bin.cb = strlen(pstring);
			tmp_bin.pv = deconst(pstring);
			if (pmsg->proplist.set(g_bcd_proptag, &tmp_bin) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "X-MS-CHILD")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (child_strings.count >= GX_ARRAY_SIZE(child_buff))
				goto IMPORT_FAILURE;
			child_strings.ppstr[child_strings.count] = deconst(pstring);
			child_strings.count ++;
		} else if (0 == strcasecmp(pvline->name, "X-MS-TEXT")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (ufld_count > 3) {
				goto IMPORT_FAILURE;
			}
			if (pmsg->proplist.set(g_ufld_proptags[ufld_count++], pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "X-MS-IMADDRESS") ||
			0 == strcasecmp(pvline->name, "X-MS-RM-IMACCOUNT")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (pmsg->proplist.set(g_im_proptag, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "X-MS-TEL")) {
			pnode1 = double_list_get_head(&pvline->param_list);
			if (NULL == pnode1) {
				goto IMPORT_FAILURE;
			}
			pvparam = (VCARD_PARAM*)pnode1->pdata;
			if (NULL != pvparam->pparamval_list) {
				goto IMPORT_FAILURE;
			}
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			uint32_t tag;
			if (0 == strcasecmp(pvparam->name, "ASSISTANT")) {
				tag = PR_ASSISTANT_TELEPHONE_NUMBER;
			} else if (0 == strcasecmp(pvparam->name, "CALLBACK")) {
				tag = PR_CALLBACK_TELEPHONE_NUMBER;
			} else if (0 == strcasecmp(pvparam->name, "COMPANY")) {
				tag = PR_COMPANY_MAIN_PHONE_NUMBER;
			} else if (0 != strcasecmp(pvparam->name, "RADIO")) {
				tag = PR_RADIO_TELEPHONE_NUMBER;
			} else if (0 == strcasecmp(pvparam->name, "TTYTTD")) {
				tag = PR_TTYTDD_PHONE_NUMBER;
			} else {
				goto IMPORT_FAILURE;
			}
			if (pmsg->proplist.set(tag, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "X-MS-ANNIVERSARY")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (pstring == nullptr)
				continue;
			memset(&tmp_tm, 0, sizeof(tmp_tm));
			if (NULL != strptime(pstring, "%Y-%m-%d", &tmp_tm)) {
				tmp_int64 = rop_util_unix_to_nttime(
							mktime(&tmp_tm) - timezone);
				if (pmsg->proplist.set(PROP_TAG_WEDDINGANNIVERSARY, &tmp_int64) != 0)
					goto IMPORT_FAILURE;
			}	
		} else if (0 == strcasecmp(pvline->name, "X-MS-SPOUSE")) {
			pnode1 = double_list_get_head(&pvline->param_list);
			if (NULL == pnode1) {
				goto IMPORT_FAILURE;
			}
			pvparam = (VCARD_PARAM*)pnode1->pdata;
			if (0 != strcasecmp(pvparam->name, "N") ||
				NULL != pvparam->pparamval_list) {
				goto IMPORT_FAILURE;
			}
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (pmsg->proplist.set(PROP_TAG_SPOUSENAME, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "X-MS-MANAGER")) {
			pnode1 = double_list_get_head(&pvline->param_list);
			if (NULL == pnode1) {
				goto IMPORT_FAILURE;
			}
			pvparam = (VCARD_PARAM*)pnode1->pdata;
			if (0 != strcasecmp(pvparam->name, "N") ||
				NULL != pvparam->pparamval_list) {
				goto IMPORT_FAILURE;
			}
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (pmsg->proplist.set(PROP_TAG_MANAGERNAME, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "X-MS-ASSISTANT")) {
			pnode1 = double_list_get_head(&pvline->param_list);
			if (NULL == pnode1) {
				goto IMPORT_FAILURE;
			}
			pvparam = (VCARD_PARAM*)pnode1->pdata;
			if (0 != strcasecmp(pvparam->name, "N") ||
				NULL != pvparam->pparamval_list) {
				goto IMPORT_FAILURE;
			}
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (pmsg->proplist.set(PROP_TAG_ASSISTANT, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "FBURL")) {
			pstring = vcard_get_first_subvalue(pvline);
			if (NULL == pstring) {
				continue;
			}
			if (pmsg->proplist.set(g_fbl_proptag, pstring) != 0)
				goto IMPORT_FAILURE;
		} else if (0 == strcasecmp(pvline->name, "X-MS-INTERESTS")) {
			pnode1 = double_list_get_head(&pvline->value_list);
			if (NULL == pnode1) {
				continue;
			}
			pvvalue = (VCARD_VALUE*)pnode1->pdata;
			strings_array.count = 0;
			strings_array.ppstr = (char**)tmp_buff;
			for (pnode2=double_list_get_head(&pvvalue->subval_list);
				NULL!=pnode2; pnode2=double_list_get_after(
				&pvvalue->subval_list, pnode2)) {
				if (NULL == pnode2->pdata) {
					continue;
				}
				strings_array.ppstr[strings_array.count] = static_cast<char *>(pnode2->pdata);
				strings_array.count ++;
			}
			if (strings_array.count != 0 && strings_array.count < 128 &&
			    pmsg->proplist.set(PROP_TAG_HOBBIES, &strings_array) != 0)
				goto IMPORT_FAILURE;
		}
	}
	if (child_strings.count != 0 &&
	    pmsg->proplist.set(PROP_TAG_CHILDRENSNAMES, &child_strings) != 0)
		goto IMPORT_FAILURE;
	for (i=0; i<pmsg->proplist.count; i++) {
		proptag = pmsg->proplist.ppropval[i].proptag;
		propid = PROP_ID(proptag);
		if (is_nameprop_id(propid))
			break;
	}
	if (i >= pmsg->proplist.count) {
		return pmsg;
	}
	if (FALSE == oxvcard_get_propids(&propids, get_propids)) {
		goto IMPORT_FAILURE;
	}
	for (i=0; i<pmsg->proplist.count; i++) {
		proptag = pmsg->proplist.ppropval[i].proptag;
		propid = PROP_ID(proptag);
		if (!is_nameprop_id(propid))
			continue;
		proptag = propids.ppropid[propid - 0x8000];
		pmsg->proplist.ppropval[i].proptag =
			PROP_TAG(PROP_TYPE(pmsg->proplist.ppropval[i].proptag), proptag);
	}
	return pmsg;
	
 IMPORT_FAILURE:
	message_content_free(pmsg);
	return NULL;
}

BOOL oxvcard_export(MESSAGE_CONTENT *pmsg, VCARD *pvcard, GET_PROPIDS get_propids)
{
	BINARY *pbin;
	const char *pvalue;
	STRING_ARRAY *saval = nullptr;
	size_t out_len;
	uint16_t propid;
	uint32_t proptag;
	time_t unix_time;
	struct tm tmp_tm;
	VCARD_LINE *pvline;
	VCARD_VALUE *pvvalue;
	VCARD_PARAM *pvparam;
	PROPID_ARRAY propids;
	const char *photo_type;
	ATTACHMENT_CONTENT *pattachment;
	char tmp_buff[VCARD_MAX_BUFFER_LEN];
	const char* tel_types[] =
		{"HOME", "HOME", "VOICE", "WORK", "WORK",
		"CELL", "PAGER", "CAR", "ISDN", "PREF"};
	const char* ms_tel_types[] =
		{"ASSISTANT", "CALLBACK", "COMPANY", "RADIO", "TTYTTD"};
	static constexpr uint32_t tel_proptags[] =
		{PR_HOME_TELEPHONE_NUMBER, PR_HOME2_TELEPHONE_NUMBER,
		PR_OTHER_TELEPHONE_NUMBER, PR_BUSINESS_TELEPHONE_NUMBER,
		PR_BUSINESS2_TELEPHONE_NUMBER, PR_MOBILE_TELEPHONE_NUMBER,
		PR_PAGER_TELEPHONE_NUMBER, PR_CAR_TELEPHONE_NUMBER,
		PR_ISDN_NUMBER, PR_PRIMARY_TELEPHONE_NUMBER};
	static constexpr uint32_t ms_tel_proptags[] =
		{PR_ASSISTANT_TELEPHONE_NUMBER, PR_CALLBACK_TELEPHONE_NUMBER,
		PR_COMPANY_MAIN_PHONE_NUMBER, PR_RADIO_TELEPHONE_NUMBER,
		PR_TTYTDD_PHONE_NUMBER};
	
	
	if (FALSE == oxvcard_get_propids(&propids, get_propids)) {
		return FALSE;
	}
	vcard_init(pvcard);
	pvline = vcard_new_simple_line("PROFILE", "VCARD");
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	
	pvline = vcard_new_simple_line("VERSION", "4.0");
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	
	pvline = vcard_new_simple_line("MAILER", "gromox-oxvcard");
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	
	pvline = vcard_new_simple_line("PRODID", "gromox-oxvcard");
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	pvalue = pmsg->proplist.get<char>(PR_DISPLAY_NAME);
	if (NULL == pvalue) {
		pvalue = pmsg->proplist.get<char>(PR_NORMALIZED_SUBJECT);
	}
	if (NULL != pvalue) {
		pvline = vcard_new_simple_line("FN", pvalue);
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
	}
	
	pvline = vcard_new_line("N");
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	for (size_t i = 0; i < 5; ++i) {
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		pvalue = pmsg->proplist.get<char>(g_n_proptags[i]);
		if (NULL == pvalue) {
			continue;
		}
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG_NICKNAME);
	if (NULL != pvalue) {
		pvline = vcard_new_simple_line("NICKNAME", pvalue);
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
	}
	
	for (size_t i = 0; i < 3; ++i) {
		propid = PROP_ID(g_email_proptags[i]);
		proptag = PROP_TAG(PROP_TYPE(g_email_proptags[i]), propids.ppropid[propid - 0x8000]);
		pvalue = pmsg->proplist.get<char>(proptag);
		if (NULL == pvalue) {
			continue;
		}
		pvline = vcard_new_line("EMAIL");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("TYPE");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(pvparam, "INTERNET")) {
			goto EXPORT_FAILURE;
		}
		if (i == 0 && !vcard_append_paramval(pvparam, "PREF"))
			goto EXPORT_FAILURE;
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PR_ATTACHMENT_CONTACTPHOTO);
	if (pvalue != nullptr && *reinterpret_cast<const uint8_t *>(pvalue) != 0 &&
		NULL != pmsg->children.pattachments) {
		for (size_t i = 0; i < pmsg->children.pattachments->count; ++i) {
			pattachment = pmsg->children.pattachments->pplist[i];
			pvalue = pattachment->proplist.get<char>(PR_ATTACH_EXTENSION);
			if (NULL == pvalue) {
				continue;
			}
			if (0 != strcasecmp(pvalue, "jpeg") &&
				0 != strcasecmp(pvalue, "jpg") &&
				0 != strcasecmp(pvalue, "bmp") &&
				0 != strcasecmp(pvalue, "gif") &&
				0 != strcasecmp(pvalue, "png")) {
				continue;
			}
			photo_type = pvalue;
			auto bv = pattachment->proplist.get<BINARY>(PR_ATTACH_DATA_BIN);
			if (bv == nullptr)
				continue;
			pvline = vcard_new_line("PHOTO");
			if (NULL == pvline) {
				goto EXPORT_FAILURE;
			}
			vcard_append_line(pvcard, pvline);
			pvparam = vcard_new_param("TYPE");
			if (NULL == pvparam) {
				goto EXPORT_FAILURE;
			}
			vcard_append_param(pvline, pvparam);
			if (FALSE == vcard_append_paramval(
				pvparam, photo_type)) {
				goto EXPORT_FAILURE;
			}
			pvparam = vcard_new_param("ENCODING");
			if (NULL == pvparam) {
				goto EXPORT_FAILURE;
			}
			vcard_append_param(pvline, pvparam);
			if (FALSE == vcard_append_paramval(pvparam, "B")) {
				goto EXPORT_FAILURE;
			}
			pvvalue = vcard_new_value();
			if (NULL == pvvalue) {
				goto EXPORT_FAILURE;
			}
			vcard_append_value(pvline, pvvalue);
			if (encode64(bv->pb, bv->cb, tmp_buff, VCARD_MAX_BUFFER_LEN - 1, &out_len) != 0)
				goto EXPORT_FAILURE;
			tmp_buff[out_len] = '\0';
			if (FALSE == vcard_append_subval(pvvalue, tmp_buff)) {
				goto EXPORT_FAILURE;
			}
			break;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PR_BODY);
	if (NULL != pvalue) {
		pvline = vcard_new_simple_line("NOTE", pvalue);
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
	}
	
	pvline = vcard_new_line("ORG");
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	pvvalue = vcard_new_value();
	if (NULL == pvvalue) {
		goto EXPORT_FAILURE;
	}
	vcard_append_value(pvline, pvvalue);
	pvalue = pmsg->proplist.get<char>(PROP_TAG_COMPANYNAME);
	vcard_append_subval(pvvalue, pvalue);
	pvvalue = vcard_new_value();
	if (NULL == pvvalue) {
		goto EXPORT_FAILURE;
	}
	vcard_append_value(pvline, pvvalue);
	pvalue = pmsg->proplist.get<char>(PROP_TAG_DEPARTMENTNAME);
	vcard_append_subval(pvvalue, pvalue);
	
	pvalue = pmsg->proplist.get<char>(PR_SENSITIVITY);
	if (NULL == pvalue) {
		pvalue = "PUBLIC";
	} else {
		switch (*(uint32_t*)pvalue) {
		case SENSITIVITY_PRIVATE:
			pvalue = "PRIVATE";
			break;
		case SENSITIVITY_COMPANY_CONFIDENTIAL:
			pvalue = "CONFIDENTIAL";
			break;
		default:
			pvalue = "PUBLIC";
			break;
		}
	}
	pvline = vcard_new_simple_line("CLASS", pvalue);
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	
	pvline = vcard_new_line("ADR");
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	pvparam = vcard_new_param("TYPE");
	if (NULL == pvparam) {
		goto EXPORT_FAILURE;
	}
	vcard_append_param(pvline, pvparam);
	if (FALSE == vcard_append_paramval(pvparam, "WORK")) {
		goto EXPORT_FAILURE;
	}
	for (size_t i = 0; i < 6; ++i) {
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		propid = PROP_ID(g_workaddr_proptags[i]);
		proptag = PROP_TAG(PROP_TYPE(g_workaddr_proptags[i]), propids.ppropid[propid - 0x8000]);
		pvalue = pmsg->proplist.get<char>(proptag);
		if (NULL == pvalue) {
			continue;
		}
		vcard_append_subval(pvvalue, pvalue);
	}
	pvvalue = vcard_new_value();
	if (NULL == pvvalue) {
		goto EXPORT_FAILURE;
	}
	vcard_append_value(pvline, pvvalue);
	
	pvline = vcard_new_line("ADR");
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	pvparam = vcard_new_param("TYPE");
	if (NULL == pvparam) {
		goto EXPORT_FAILURE;
	}
	vcard_append_param(pvline, pvparam);
	if (FALSE == vcard_append_paramval(pvparam, "HOME")) {
		goto EXPORT_FAILURE;
	}
	for (size_t i = 0; i < 6; ++i) {
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		pvalue = pmsg->proplist.get<char>(g_homeaddr_proptags[i]);
		if (NULL == pvalue) {
			continue;
		}
		vcard_append_subval(pvvalue, pvalue);
	}
	pvvalue = vcard_new_value();
	if (NULL == pvvalue) {
		goto EXPORT_FAILURE;
	}
	vcard_append_value(pvline, pvvalue);
	
	pvline = vcard_new_line("ADR");
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	pvparam = vcard_new_param("TYPE");
	if (NULL == pvparam) {
		goto EXPORT_FAILURE;
	}
	vcard_append_param(pvline, pvparam);
	if (FALSE == vcard_append_paramval(pvparam, "POSTAL")) {
		goto EXPORT_FAILURE;
	}
	for (size_t i = 0; i < 6; ++i) {
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		pvalue = pmsg->proplist.get<char>(g_otheraddr_proptags[i]);
		if (NULL == pvalue) {
			continue;
		}
		vcard_append_subval(pvvalue, pvalue);
	}
	pvvalue = vcard_new_value();
	if (NULL == pvvalue) {
		goto EXPORT_FAILURE;
	}
	vcard_append_value(pvline, pvvalue);
	
	for (size_t i = 0; i < 10; ++i) {
		pvalue = pmsg->proplist.get<char>(tel_proptags[i]);
		if (NULL == pvalue) {
			continue;
		}
		pvline = vcard_new_line("TEL");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("TYPE");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(pvparam, tel_types[i])) {
			goto EXPORT_FAILURE;
		}
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PR_HOME_FAX_NUMBER);
	if (NULL != pvalue) {
			pvline = vcard_new_line("TEL");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("TYPE");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(pvparam, "HOME")) {
			goto EXPORT_FAILURE;
		}
		pvparam = vcard_new_param("FAX");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PR_BUSINESS_FAX_NUMBER);
	if (NULL != pvalue) {
			pvline = vcard_new_line("TEL");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("TYPE");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(pvparam, "WORK")) {
			goto EXPORT_FAILURE;
		}
		pvparam = vcard_new_param("FAX");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	propid = PROP_ID(g_categories_proptag);
	proptag = PROP_TAG(PROP_TYPE(g_categories_proptag), propids.ppropid[propid - 0x8000]);
	saval = pmsg->proplist.get<STRING_ARRAY>(proptag);
	if (saval != nullptr) {
		pvline = vcard_new_line("CATEGORIES");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		for (size_t i = 0; i < saval->count; ++i)
			if (!vcard_append_subval(pvvalue, saval->ppstr[i]))
				goto EXPORT_FAILURE;
	}
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG_PROFESSION);
	if (NULL != pvalue) {
		pvline = vcard_new_simple_line("ROLE", pvalue);
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
	}
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG_PERSONALHOMEPAGE);
	if (NULL != pvalue) {
		pvline = vcard_new_line("URL");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("TYPE");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(pvparam, "HOME")) {
			goto EXPORT_FAILURE;
		}
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PR_BUSINESS_HOME_PAGE);
	if (NULL != pvalue) {
		pvline = vcard_new_line("URL");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("TYPE");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(pvparam, "WORK")) {
			goto EXPORT_FAILURE;
		}
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	propid = PROP_ID(g_bcd_proptag);
	proptag = PROP_TAG(PROP_TYPE(g_bcd_proptag), propids.ppropid[propid - 0x8000]);
	pvalue = pmsg->proplist.get<char>(proptag);
	if (NULL != pvalue) {
		pvline = vcard_new_simple_line("X-MS-OL-DESIGN", pvalue);
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
	}
	
	saval = pmsg->proplist.get<STRING_ARRAY>(PROP_TAG_CHILDRENSNAMES);
	if (NULL != pvalue) {
		for (size_t i = 0; i < saval->count; ++i) {
			pvline = vcard_new_simple_line("X-MS-CHILD", saval->ppstr[i]);
			if (NULL == pvline) {
				goto EXPORT_FAILURE;
			}
			vcard_append_line(pvcard, pvline);
		}
	}
	
	for (size_t i = 0; i < 4; ++i) {
		propid = PROP_ID(g_ufld_proptags[i]);
		proptag = PROP_TAG(PROP_TYPE(g_ufld_proptags[i]), propids.ppropid[propid - 0x8000]);
		pvalue = pmsg->proplist.get<char>(proptag);
		if (NULL == pvalue) {
			continue;
		}
		pvline = vcard_new_simple_line("X-MS-TEXT", pvalue);
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
	}
	
	for (size_t i = 0; i < 5; ++i) {
		pvalue = pmsg->proplist.get<char>(ms_tel_proptags[i]);
		if (NULL == pvalue) {
			continue;
		}
		pvline = vcard_new_line("X-MS-TEL");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("TYPE");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(
			pvparam, ms_tel_types[i])) {
			goto EXPORT_FAILURE;
		}
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG_SPOUSENAME);
	if (NULL != pvalue) {
		pvline = vcard_new_line("X-MS-SPOUSE");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("N");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG_MANAGERNAME);
	if (NULL != pvalue) {
		pvline = vcard_new_line("X-MS-MANAGER");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("N");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG_ASSISTANT);
	if (NULL != pvalue) {
		pvline = vcard_new_line("X-MS-ASSISTANT");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("N");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, pvalue)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG(PROP_TYPE(g_vcarduid_proptag), propids.ppropid[PROP_ID(g_vcarduid_proptag)-0x8000]));
	if (pvalue == nullptr) {
		auto guid = guid_random_new();
		auto gstr = "uuid:" + bin2hex(&guid, sizeof(guid));
		uint32_t tag = PROP_TAG(PROP_TYPE(g_vcarduid_proptag), propids.ppropid[PROP_ID(g_vcarduid_proptag)-0x8000]);
		pmsg->proplist.set(proptag, gstr.c_str());
		pvalue = pmsg->proplist.get<char>(tag);
	}
	if (pvalue != nullptr) {
		pvline = vcard_new_simple_line("UID", pvalue);
		if (pvline == nullptr)
			goto EXPORT_FAILURE;
		vcard_append_line(pvcard, pvline);
	}

	propid = PROP_ID(g_fbl_proptag);
	proptag = PROP_TAG(PROP_TYPE(g_fbl_proptag), propids.ppropid[propid - 0x8000]);
	pvalue = pmsg->proplist.get<char>(proptag);
	if (NULL != pvalue) {
		pvline = vcard_new_simple_line("FBURL", pvalue);
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
	}
	
	saval = pmsg->proplist.get<STRING_ARRAY>(PROP_TAG_HOBBIES);
	if (NULL != pvalue) {
		pvline = vcard_new_line("X-MS-INTERESTS");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		for (size_t i = 0; i < saval->count; ++i)
			if (!vcard_append_subval(pvvalue, saval->ppstr[i]))
				goto EXPORT_FAILURE;
	}
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG_USERX509CERTIFICATE);
	if (NULL != pvalue && 0 != ((BINARY_ARRAY*)pvalue)->count) {
		pvline = vcard_new_line("KEY");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("ENCODING");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(pvparam, "B")) {
			goto EXPORT_FAILURE;
		}
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		pbin = ((BINARY_ARRAY*)pvalue)->pbin;
		if (0 != encode64(pbin->pb, pbin->cb, tmp_buff,
			VCARD_MAX_BUFFER_LEN - 1, &out_len)) {
			goto EXPORT_FAILURE;
		}
		tmp_buff[out_len] = '\0';
		if (FALSE == vcard_append_subval(pvvalue, tmp_buff)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG_TITLE);
	pvline = vcard_new_simple_line("TITLE", pvalue);
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	
	propid = PROP_ID(g_im_proptag);
	proptag = PROP_TAG(PROP_TYPE(g_im_proptag), propids.ppropid[propid - 0x8000]);
	pvalue = pmsg->proplist.get<char>(proptag);
	pvline = vcard_new_simple_line("X-MS-IMADDRESS", pvalue);
	if (NULL == pvline) {
		goto EXPORT_FAILURE;
	}
	vcard_append_line(pvcard, pvline);
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG_BIRTHDAY);
	if (NULL != pvalue) {
		unix_time = rop_util_nttime_to_unix(*(uint64_t*)pvalue);
		gmtime_r(&unix_time, &tmp_tm);
		strftime(tmp_buff, 1024, "%Y-%m-%d", &tmp_tm);
		pvline = vcard_new_line("BDAY");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("VALUE");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(pvparam, "DATE")) {
			goto EXPORT_FAILURE;
		}
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, tmp_buff)) {
			goto EXPORT_FAILURE;
		}
	}
	
	pvalue = pmsg->proplist.get<char>(PR_LAST_MODIFICATION_TIME);
	if (NULL != pvalue) {
		unix_time = rop_util_nttime_to_unix(*(uint64_t*)pvalue);
		gmtime_r(&unix_time, &tmp_tm);
		strftime(tmp_buff, 1024, "%Y-%m-%dT%H:%M:%SZ", &tmp_tm);
		pvline = vcard_new_line("REV");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("VALUE");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(pvparam, "DATE-TIME")) {
			goto EXPORT_FAILURE;
		}
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (!vcard_append_subval(pvvalue, tmp_buff))
			goto EXPORT_FAILURE;
	}
	
	pvalue = pmsg->proplist.get<char>(PROP_TAG_WEDDINGANNIVERSARY);
	if (NULL != pvalue) {
		unix_time = rop_util_nttime_to_unix(*(uint64_t*)pvalue);
		gmtime_r(&unix_time, &tmp_tm);
		strftime(tmp_buff, 1024, "%Y-%m-%d", &tmp_tm);
		pvline = vcard_new_line("X-MS-ANNIVERSARY");
		if (NULL == pvline) {
			goto EXPORT_FAILURE;
		}
		vcard_append_line(pvcard, pvline);
		pvparam = vcard_new_param("VALUE");
		if (NULL == pvparam) {
			goto EXPORT_FAILURE;
		}
		vcard_append_param(pvline, pvparam);
		if (FALSE == vcard_append_paramval(pvparam, "DATE")) {
			goto EXPORT_FAILURE;
		}
		pvvalue = vcard_new_value();
		if (NULL == pvvalue) {
			goto EXPORT_FAILURE;
		}
		vcard_append_value(pvline, pvvalue);
		if (FALSE == vcard_append_subval(pvvalue, tmp_buff)) {
			goto EXPORT_FAILURE;
		}
	}
	
	return TRUE;
	
 EXPORT_FAILURE:
	vcard_free(pvcard);
	return FALSE;
}
