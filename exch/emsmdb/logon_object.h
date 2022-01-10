#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <gromox/mapi_types.hpp>
#include <gromox/str_hash.hpp>
#define LOGON_MODE_OWNER				0
#define LOGON_MODE_DELEGATE				1
#define LOGON_MODE_GUEST				2

struct INT_HASH_TABLE;
struct property_groupinfo;

struct logon_object {
	protected:
	logon_object() = default;
	NOMOVE(logon_object);

	public:
	~logon_object();
	static std::unique_ptr<logon_object> create(uint8_t logon_flags, uint32_t open_flags, int logon_mode, int account_id, const char *account, const char *dir, GUID mailbox_guid);
	BOOL check_private() const { return (logon_flags & LOGON_FLAG_PRIVATE) ? TRUE : false; }
	GUID guid() const;
	const char *get_account() const { return account; }
	const char *get_dir() const { return dir; }
	BOOL get_named_propname(uint16_t propid, PROPERTY_NAME *);
	BOOL get_named_propnames(const PROPID_ARRAY *, PROPNAME_ARRAY *);
	BOOL get_named_propid(BOOL create, const PROPERTY_NAME *, uint16_t *propid);
	BOOL get_named_propids(BOOL create, const PROPNAME_ARRAY *, PROPID_ARRAY *);
	/*
	 * Used for message partial change information when saving 
	 * message, the return value is maintained by logon object,
	 * do not free it outside.
	 */
	const property_groupinfo *get_last_property_groupinfo();
	/* same as logon_object_get_last_property_groupinfo, do not free it outside */
	const property_groupinfo *get_property_groupinfo(uint32_t group_id);
	BOOL get_all_proptags(PROPTAG_ARRAY *);
	BOOL get_properties(const PROPTAG_ARRAY *, TPROPVAL_ARRAY *);
	BOOL set_properties(const TPROPVAL_ARRAY *, PROBLEM_ARRAY *);
	BOOL remove_properties(const PROPTAG_ARRAY *, PROBLEM_ARRAY *);

	uint8_t logon_flags = 0;
	uint32_t open_flags = 0;
	int logon_mode = 0, account_id = 0;
	char account[UADDR_SIZE]{};
	char dir[256]{};
	GUID mailbox_guid{};
	std::unique_ptr<property_groupinfo> m_gpinfo;
	std::vector<property_groupinfo> group_list;
	std::unique_ptr<INT_HASH_TABLE> ppropid_hash;
	std::unique_ptr<STR_HASH_TABLE> ppropname_hash;
};
