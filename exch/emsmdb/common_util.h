#pragma once
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <gromox/mapi_types.hpp>
#include "../mysql_adaptor/mysql_adaptor.h"
#define NOTIFY_RECEIPT_READ							1
#define NOTIFY_RECEIPT_NON_READ						2
#define MAX_HANDLES_ON_CONTEXT						10
#define MINIMUM_COMPRESS_SIZE						0x100

enum {
	COMMON_UTIL_MAX_RCPT,
	COMMON_UTIL_MAX_MESSAGE,
	COMMON_UTIL_MAX_MAIL_LENGTH,
	COMMON_UTIL_MAX_EXTRULE_LENGTH
};

struct LIB_BUFFER;
struct logon_object;
struct MAIL;
struct MESSAGE_CONTENT;
struct MIME_POOL;

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
int common_util_mb_from_utf8(uint32_t cpid,
	const char *src, char *dst, size_t len);
int common_util_mb_to_utf8(uint32_t cpid,
	const char *src, char *dst, size_t len);
int common_util_convert_string(BOOL to_utf8,
	const char *src, char *dst, size_t len);
void common_util_obfuscate_data(uint8_t *data, uint32_t size);
extern BOOL common_util_essdn_to_username(const char *pessdn, char *username, size_t);
BOOL common_util_username_to_essdn(const char *username, char *pessdn, size_t);
BOOL common_util_essdn_to_public(const char *pessdn, char *domainname);
BOOL common_util_public_to_essdn(const char *username, char *pessdn, size_t);
const char* common_util_essdn_to_domain(const char *pessdn);
void common_util_domain_to_essdn(const char *pdomain, char *pessdn, size_t);
extern BOOL common_util_entryid_to_username(const BINARY *, char *username, size_t);
void common_util_get_domain_server(const char *account_name, char *pserver);
BINARY* common_util_username_to_addressbook_entryid(const char *username);
BINARY* common_util_public_to_addressbook_entryid(const char *domainname);
extern BINARY *common_util_to_folder_entryid(logon_object *, uint64_t folder_id);
extern BINARY *common_util_calculate_folder_sourcekey(logon_object *, uint64_t folder_id);
extern BINARY *common_util_to_message_entryid(logon_object *, uint64_t folder_id, uint64_t msg_id);
extern BOOL common_util_from_folder_entryid(logon_object *, BINARY *, uint64_t *folder_id);
extern BOOL common_util_from_message_entryid(logon_object *, BINARY *, uint64_t *folder_id, uint64_t *msg_id);
extern BINARY *common_util_calculate_message_sourcekey(logon_object *, uint64_t msg_id);
extern BINARY *cu_xid_to_bin(const XID &);
BOOL common_util_binary_to_xid(const BINARY *pbin, XID *pxid);
BINARY* common_util_guid_to_binary(GUID guid);
BOOL common_util_pcl_compare(const BINARY *pbin_pcl1,
	const BINARY *pbin_pcl2, uint32_t *presult);
BINARY* common_util_pcl_append(const BINARY *pbin_pcl,
	const BINARY *pchange_key);
BINARY* common_util_pcl_merge(const BINARY *pbin_pcl1,
	const BINARY *pbin_pcl2);
BINARY* common_util_to_folder_replica(
	const LONG_TERM_ID *plongid, const char *essdn);
BOOL common_util_check_message_class(const char *str_class);
GUID common_util_get_mapping_guid(BOOL b_private, int account_id);
BOOL common_util_mapping_replica(BOOL to_guid,
	void *pparam, uint16_t *preplid, GUID *pguid);
/* must ensure there's enough buffer in ppropval */
void common_util_set_propvals(TPROPVAL_ARRAY *parray,
	const TAGGED_PROPVAL *ppropval);
void common_util_remove_propvals(
	TPROPVAL_ARRAY *parray, uint32_t proptag);
BOOL common_util_retag_propvals(TPROPVAL_ARRAY *parray,
	uint32_t orignal_proptag, uint32_t new_proptag);
void common_util_reduce_proptags(PROPTAG_ARRAY *pproptags_minuend,
	const PROPTAG_ARRAY *pproptags_subtractor);
PROPTAG_ARRAY* common_util_trim_proptags(const PROPTAG_ARRAY *pproptags);
int common_util_problem_compare(const void *pproblem1,
	const void *pproblem2);
BOOL common_util_propvals_to_row(
	const TPROPVAL_ARRAY *ppropvals,
	const PROPTAG_ARRAY *pcolumns, PROPERTY_ROW *prow);
BOOL common_util_convert_unspecified(uint32_t cpid,
	BOOL b_unicode, TYPED_PROPVAL *ptyped);
BOOL common_util_propvals_to_row_ex(uint32_t cpid,
	BOOL b_unicode, const TPROPVAL_ARRAY *ppropvals,
	const PROPTAG_ARRAY *pcolumns, PROPERTY_ROW *prow);
BOOL common_util_propvals_to_openrecipient(uint32_t cpid,
	TPROPVAL_ARRAY *ppropvals, const PROPTAG_ARRAY *pcolumns,
	OPENRECIPIENT_ROW *prow);
BOOL common_util_propvals_to_readrecipient(uint32_t cpid,
	TPROPVAL_ARRAY *ppropvals, const PROPTAG_ARRAY *pcolumns,
	READRECIPIENT_ROW *prow);
BOOL common_util_row_to_propvals(
	const PROPERTY_ROW *prow, const PROPTAG_ARRAY *pcolumns,
	TPROPVAL_ARRAY *ppropvals);
BOOL common_util_modifyrecipient_to_propvals(
	 uint32_t cpid, const MODIFYRECIPIENT_ROW *prow,
	const PROPTAG_ARRAY *pcolumns, TPROPVAL_ARRAY *ppropvals);
BOOL common_util_convert_tagged_propval(
	BOOL to_unicode, TAGGED_PROPVAL *ppropval);
BOOL common_util_convert_restriction(BOOL to_unicode, RESTRICTION *pres);
BOOL common_util_convert_rule_actions(BOOL to_unicode, RULE_ACTIONS *pactions);
void common_util_notify_receipt(const char *username,
	int type, MESSAGE_CONTENT *pbrief);
extern BOOL common_util_save_message_ics(logon_object *plogon, uint64_t msg_id, PROPTAG_ARRAY *changed_tags);
BOOL common_util_send_mail(MAIL *pmail,
	const char *sender, DOUBLE_LIST *prcpt_list);
extern BOOL common_util_send_message(logon_object *, uint64_t msg_id, BOOL submit);

#define E(s) extern decltype(mysql_adaptor_ ## s) *common_util_ ## s;
E(check_mlist_include)
E(check_same_org)
E(get_domain_ids)
E(get_homedir)
E(get_homedir_by_id)
E(get_id_from_homedir)
E(get_id_from_maildir)
E(get_id_from_username)
E(get_maildir)
E(get_timezone)
E(get_user_displayname)
E(get_user_ids)
E(get_user_lang)
E(get_username_from_id)
#undef E
extern BOOL (*common_util_lang_to_charset)(
	const char *lang, char *charset);
extern const char* (*common_util_cpid_to_charset)(uint32_t cpid);
extern bool (*common_util_verify_cpid)(uint32_t cpid);
extern int (*common_util_add_timer)(const char *command, int interval);
extern BOOL (*common_util_cancel_timer)(int timer_id);

extern LIB_BUFFER *common_util_get_allocator();
extern void common_util_init(const char *org_name, int avg_blocks, int max_rcpt, int max_msg, unsigned int max_mail_len, unsigned int max_rule_len, const char *smtp_host, uint16_t smtp_port, const char *submit_cmd);
extern int common_util_run();
extern void common_util_stop();
extern void common_util_free();
unsigned int common_util_get_param(int param);
void common_util_set_param(int param, unsigned int value);
extern const char *common_util_get_submit_command();
extern uint32_t common_util_get_ftstream_id();
extern std::shared_ptr<MIME_POOL> common_util_get_mime_pool();
