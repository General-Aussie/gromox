#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <gromox/defs.h>
#include <gromox/mail.hpp>
#include <gromox/common_types.hpp>
#include <gromox/element_data.hpp>
#include <sqlite3.h>
#include "../mysql_adaptor/mysql_adaptor.h"

#define SOCKET_TIMEOUT										60
#define MAXIMUM_PROPNAME_NUMBER								0x7000
#define MAX_DIGLEN											256*1024
#define MAX_RULE_RECIPIENTS									256
#define MAX_DAMS_PER_RULE_FOLDER							128
#define MAX_FAI_COUNT										1024

#define ID_TAG_BODY 												0x00010014
#define ID_TAG_BODY_STRING8											0x00020014
#define ID_TAG_HTML													0x00040014
#define ID_TAG_RTFCOMPRESSED										0x00050014
#define ID_TAG_TRANSPORTMESSAGEHEADERS								0x00060014
#define ID_TAG_TRANSPORTMESSAGEHEADERS_STRING8						0x00070014
#define ID_TAG_ATTACHDATABINARY										0x000B0014
#define ID_TAG_ATTACHDATAOBJECT										0x000F0014

enum {
	STORE_PROPERTIES_TABLE,
	FOLDER_PROPERTIES_TABLE,
	MESSAGE_PROPERTIES_TABLE,
	RECIPIENT_PROPERTIES_TABLE,
	ATTACHMENT_PROPERTIES_TABLE
};

enum {
	COMMON_UTIL_MAX_RULE_NUMBER,
	COMMON_UTIL_MAX_EXT_RULE_NUMBER
};

extern BOOL (*common_util_lang_to_charset)(
	const char *lang, char *charset);
extern const char* (*common_util_cpid_to_charset)(uint32_t cpid);
#define E(s) extern decltype(mysql_adaptor_ ## s) *common_util_ ## s;
E(check_mlist_include)
E(get_domain_ids)
E(get_homedir_by_id)
E(get_id_from_homedir)
E(get_id_from_maildir)
E(get_id_from_username)
E(get_maildir)
E(get_timezone)
E(get_user_displayname)
E(get_user_lang)
#undef E
extern BOOL (*common_util_send_mail)(MAIL *pmail,
	const char *sender, DOUBLE_LIST *prcpt_list);
extern std::shared_ptr<MIME_POOL> (*common_util_get_mime_pool)();
extern void (*common_util_log_info)(unsigned int level, const char *format, ...) __attribute__((format(printf, 2, 3)));
extern const GUID *(*common_util_get_handle)();

void common_util_set_propvals(TPROPVAL_ARRAY *parray,
	const TAGGED_PROPVAL *ppropval);
void common_util_remove_propvals(
	TPROPVAL_ARRAY *parray, uint32_t proptag);
extern BOOL common_util_essdn_to_username(const char *pessdn, char *username, size_t);
extern BOOL common_util_username_to_essdn(const char *username, char *dn, size_t);
void common_util_pass_service(int service_id, void *func);
void common_util_init(const char *org_name, unsigned int max_msg,
	unsigned int max_rule_num, unsigned int max_ext_rule_num);
extern void common_util_free();
extern void common_util_build_tls();
void common_util_set_tls_var(const void *pvar);
extern const void *common_util_get_tls_var();
extern int common_util_sequence_ID();
void* common_util_alloc(size_t size);
template<typename T> T *cu_alloc()
{
	static_assert(std::is_trivially_destructible_v<T>);
	return static_cast<T *>(common_util_alloc(sizeof(T)));
}
template<typename T> T *cu_alloc(size_t elem)
{
	static_assert(std::is_trivially_destructible_v<T>);
	return static_cast<T *>(common_util_alloc(sizeof(T) * elem));
}
template<typename T> T *me_alloc() { return static_cast<T *>(malloc(sizeof(T))); }
template<typename T> T *me_alloc(size_t elem) { return static_cast<T *>(malloc(sizeof(T) * elem)); }
char* common_util_dup(const char *pstr);
char* common_util_convert_copy(BOOL to_utf8,
	uint32_t cpid, const char *pstring);
extern STRING_ARRAY *common_util_convert_copy_string_array(BOOL to_utf8, uint32_t cpid, const STRING_ARRAY *);
BOOL common_util_allocate_eid(sqlite3 *psqlite, uint64_t *peid);
BOOL common_util_allocate_eid_from_folder(sqlite3 *psqlite,
	uint64_t folder_id, uint64_t *peid);
BOOL common_util_allocate_cn(sqlite3 *psqlite, uint64_t *pcn);
BOOL common_util_allocate_folder_art(sqlite3 *psqlite, uint32_t *part);
BOOL common_util_check_allocated_eid(sqlite3 *psqlite,
	uint64_t eid_val, BOOL *pb_result);
BOOL common_util_allocate_cid(sqlite3 *psqlite, uint64_t *pcid);
BOOL common_util_get_proptags(int table_type, uint64_t id,
	sqlite3 *psqlite, PROPTAG_ARRAY *pproptags);
BOOL common_util_get_mapping_guid(sqlite3 *psqlite,
	uint16_t replid, BOOL *pb_found, GUID *pguid);
BOOL common_util_begin_message_optimize(sqlite3 *psqlite);
extern void common_util_end_message_optimize();
BOOL common_util_get_property(int table_type, uint64_t id,
	uint32_t cpid, sqlite3 *psqlite, uint32_t proptag,
	void **ppvalue);
BOOL common_util_get_properties(int table_type,
	uint64_t id, uint32_t cpid, sqlite3 *psqlite,
	const PROPTAG_ARRAY *pproptags, TPROPVAL_ARRAY *ppropvals);
BOOL common_util_set_property(int table_type,
	uint64_t id, uint32_t cpid, sqlite3 *psqlite,
	const TAGGED_PROPVAL *ppropval, BOOL *pb_result); 
BOOL common_util_set_properties(int table_type,
	uint64_t id, uint32_t cpid, sqlite3 *psqlite,
	const TPROPVAL_ARRAY *ppropvals, PROBLEM_ARRAY *pproblems);
BOOL common_util_remove_property(int table_type,
	uint64_t id, sqlite3 *psqlite, uint32_t proptag);
BOOL common_util_remove_properties(int table_type, uint64_t id,
	sqlite3 *psqlite,const PROPTAG_ARRAY *pproptags);
BOOL common_util_get_rule_property(uint64_t rule_id,
	sqlite3 *psqlite, uint32_t proptag, void **ppvalue);
BOOL common_util_get_permission_property(uint64_t member_id,
	sqlite3 *psqlite, uint32_t proptag, void **ppvalue);
BOOL common_util_check_msgcnt_overflow(sqlite3 *psqlite);
extern BOOL cu_check_msgsize_overflow(sqlite3 *psqlite, uint32_t qtag);
uint32_t common_util_get_folder_unread_count(
	sqlite3 *psqlite, uint64_t folder_id);
extern BOOL common_util_get_folder_type(sqlite3 *, uint64_t folder_id, uint32_t *type, const char *dir = nullptr);
uint64_t common_util_get_folder_parent_fid(
	sqlite3 *psqlite, uint64_t folder_id);
BOOL common_util_get_folder_by_name(
	sqlite3 *psqlite, uint64_t parent_id,
	const char *str_name, uint64_t *pfolder_id);
BOOL common_util_check_message_associated(
	sqlite3 *psqlite, uint64_t message_id);
BOOL common_util_get_message_flags(sqlite3 *psqlite,
	uint64_t message_id, BOOL b_native,
	uint32_t **ppmessage_flags);
void common_util_set_message_read(sqlite3 *psqlite,
	uint64_t message_id, uint8_t is_read);
extern BOOL common_util_addressbook_entryid_to_username(const BINARY *eid, char *username, size_t);
extern BOOL common_util_addressbook_entryid_to_essdn(const BINARY *eid, char *dn, size_t);
BINARY* common_util_username_to_addressbook_entryid(
	const char *username);
extern BOOL common_util_entryid_to_username(const BINARY *, char *username, size_t);
extern BOOL common_util_parse_addressbook_entryid(const BINARY *, char *address_type, size_t atsize, char *email_address, size_t emsize);
BINARY* common_util_to_private_folder_entryid(
	sqlite3 *psqlite, const char *username,
	uint64_t folder_id);
BINARY* common_util_to_private_message_entryid(
	sqlite3 *psqlite, const char *username,
	uint64_t folder_id, uint64_t message_id);
BOOL common_util_check_folder_permission(
	sqlite3 *psqlite, uint64_t folder_id,
	const char *username, uint32_t *ppermission);
extern BOOL common_util_check_descendant(sqlite3 *, uint64_t inner_fid, uint64_t outer_fid, BOOL *pb_included);
BOOL common_util_get_message_parent_folder(sqlite3 *psqlite,
	uint64_t message_id, uint64_t *pfolder_id);
BOOL common_util_load_search_scopes(sqlite3 *psqlite,
	uint64_t folder_id, LONGLONG_ARRAY *pfolder_ids);
BOOL common_util_evaluate_folder_restriction(sqlite3 *psqlite,
	uint64_t folder_id, const RESTRICTION *pres);
BOOL common_util_evaluate_message_restriction(sqlite3 *psqlite,
	uint32_t cpid, uint64_t message_id, const RESTRICTION *pres);
BOOL common_util_check_search_result(sqlite3 *psqlite,
	uint64_t folder_id, uint64_t message_id, BOOL *pb_exist);
BOOL common_util_get_mid_string(sqlite3 *psqlite,
	uint64_t message_id, char **ppmid_string);
BOOL common_util_set_mid_string(sqlite3 *psqlite,
	uint64_t message_id, const char *pmid_string);
BOOL common_util_check_message_owner(sqlite3 *psqlite,
	uint64_t message_id, const char *username, BOOL *pb_owner);
BOOL common_util_copy_message(sqlite3 *psqlite, int account_id,
	uint64_t message_id, uint64_t folder_id, uint64_t *pdst_mid,
	BOOL *pb_result, uint32_t *pmessage_size);
BOOL common_util_get_named_propids(sqlite3 *psqlite,
	BOOL b_create, const PROPNAME_ARRAY *ppropnames,
	PROPID_ARRAY *ppropids);
BOOL common_util_get_named_propnames(sqlite3 *psqlite,
	const PROPID_ARRAY *ppropids, PROPNAME_ARRAY *ppropnames);
BOOL common_util_check_folder_id(sqlite3 *psqlite,
	uint64_t folder_id, BOOL *pb_exist);
BOOL common_util_increase_deleted_count(sqlite3 *psqlite,
	uint64_t folder_id, uint32_t del_count);
BOOL common_util_increase_store_size(sqlite3 *psqlite,
	uint64_t normal_size, uint64_t fai_size);
BOOL common_util_decrease_store_size(sqlite3 *psqlite,
	uint64_t normal_size, uint64_t fai_size);
BOOL common_util_recipients_to_list(
	TARRAY_SET *prcpts, DOUBLE_LIST *plist);
extern BINARY *cu_xid_to_bin(const XID &);
BOOL common_util_binary_to_xid(const BINARY *pbin, XID *pxid);
BINARY* common_util_pcl_append(const BINARY *pbin_pcl,
	const BINARY *pchange_key);
BOOL common_util_copy_file(const char *src_file, const char *dst_file);
BOOL common_util_bind_sqlite_statement(sqlite3_stmt *pstmt,
	int bind_index, uint16_t proptype, void *pvalue);
void* common_util_column_sqlite_statement(sqlite3_stmt *pstmt,
	int column_index, uint16_t proptype);
BOOL common_util_indexing_sub_contents(
	uint32_t step, sqlite3_stmt *pstmt,
	sqlite3_stmt *pstmt1, uint32_t *pidx);
uint32_t common_util_calculate_message_size(
	const MESSAGE_CONTENT *pmsgctnt);
uint32_t common_util_calculate_attachment_size(
	const ATTACHMENT_CONTENT *pattachment);
unsigned int common_util_get_param(int param);
extern const char *exmdb_rpc_idtoname(unsigned int i);
