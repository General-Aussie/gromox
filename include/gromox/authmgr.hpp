#pragma once
#include <cstdint>
#include <string>

/**
 * %EXCH:	can login via emsmdb or zcore
 * %IMAP:	can login via IMAP & POP3
 * %CHGPASSWD:	user is allowed to change their own password via zcore
 * %PUBADDR:	(unused)
 * %SMTP, %CHAT, %VIDEO, %FILES, %ARCHIVE:
 * 		pam_gromox with service=...
 * %WANTPRIV_METAONLY: Indicator for callers of auth_meta that only account
 * metadata is desired, but no login checks on address_status or dtypx.
 */
enum {
	USER_PRIVILEGE_EXCH = 0,
	USER_PRIVILEGE_IMAP = 1U << 0,
	USER_PRIVILEGE_POP3 = USER_PRIVILEGE_IMAP,
	USER_PRIVILEGE_SMTP = 1U << 1,
	USER_PRIVILEGE_CHGPASSWD = 1U << 2,
	USER_PRIVILEGE_PUBADDR = 1U << 3,
	USER_PRIVILEGE_CHAT = 1U << 4,
	USER_PRIVILEGE_VIDEO = 1U << 5,
	USER_PRIVILEGE_FILES = 1U << 6,
	USER_PRIVILEGE_ARCHIVE = 1U << 7,
	WANTPRIV_METAONLY = 0x10000U,
};

/**
 * Outputs from mysql_adaptor_meta
 * @username:	Primary, e-mail-address-based username
 * @maildir:	Mailbox location
 * @lang:	Preferred language for mailbox
 * @enc_passwd:	Encrypted password right from the SQL column,
 * 		used by authmgr to perform authentication.
 * @errstr:	Error message, if any. This is for the system log only,
 * 		it must not be sent to any peer.
 * @have_xid:	Whether an externid is set
 * 		(0=no / 1=yes / 0xFF=indeterminate)
 */
struct sql_meta_result {
	std::string username, maildir, lang, enc_passwd, errstr;
	std::string ldap_uri, ldap_binddn, ldap_bindpw, ldap_basedn;
	std::string ldap_mail_attr;
	bool ldap_start_tls = false;
	uint8_t have_xid = 0xFF;
};

using authmgr_login_t = bool (*)(const char *username, const char *password, unsigned int wantprivs, sql_meta_result &);
using authmgr_login_t2 = bool (*)(const char *token, unsigned int wantprivs, sql_meta_result &);
