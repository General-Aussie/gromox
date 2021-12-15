// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2021 grommunio GmbH
// This file is part of Gromox.
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <libHX/ctype_helper.h>
#include <libHX/string.h>
#include <gromox/atomic.hpp>
#include <gromox/database.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/util.hpp>
#include <gromox/mail.hpp>
#include <gromox/midb.hpp>
#include <gromox/mjson.hpp>
#include <gromox/oxcmail.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/scope.hpp>
#include <gromox/mail_func.hpp>
#include <gromox/mime_pool.hpp>
#include "cmd_parser.h"
#include "common_util.h"
#include "mail_engine.h"
#include <gromox/double_list.hpp>
#include <gromox/single_list.hpp>
#include "exmdb_client.h"
#include "system_services.h"
#include <ctime>
#include <iconv.h>
#include <cstdio>
#include <fcntl.h>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <sqlite3.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#define LLU(x) static_cast<unsigned long long>(x)
#define S2A(x) reinterpret_cast<const char *>(x)

#define FILENUM_PER_MIME				8

#define CONFIG_ID_USERNAME				1

#define MAX_DIGLEN						256*1024

#define CONDITION_TREE					DOUBLE_LIST

#define RELOAD_INTERVAL					3600
#define MAX_DB_WAITING_THREADS			5

using namespace std::string_literals;
using namespace gromox;

enum {
	CONDITION_ALL,
	CONDITION_ANSWERED,
	CONDITION_BCC,
	CONDITION_BEFORE,
	CONDITION_BODY,
	CONDITION_CC,
	CONDITION_DELETED,
	CONDITION_DRAFT,
	CONDITION_FLAGGED,
	CONDITION_FROM,
	CONDITION_HEADER,
	CONDITION_ID,
	CONDITION_KEYWORD,
	CONDITION_LARGER,
	CONDITION_NEW,
	CONDITION_OLD,
	CONDITION_ON,
	CONDITION_RECENT,
	CONDITION_SEEN,
	CONDITION_SENTBEFORE,
	CONDITION_SENTON,
	CONDITION_SENTSINCE,
	CONDITION_SINCE,
	CONDITION_SMALLER,
	CONDITION_SUBJECT,
	CONDITION_TEXT,
	CONDITION_TO,
	CONDITION_UNANSWERED,
	CONDITION_UID,
	CONDITION_UNDELETED,
	CONDITION_UNDRAFT,
	CONDITION_UNFLAGGED,
	CONDITION_UNKEYWORD,
	CONDITION_UNSEEN
};

enum {
	CONJUNCTION_AND,
	CONJUNCTION_OR,
	CONJUNCTION_NOT
};

namespace {

struct CONDITION_RESULT {
	SINGLE_LIST list;
	SINGLE_LIST_NODE *pcur_node;
};

struct CONDITION_TREE_NODE {
	DOUBLE_LIST_NODE node;
	int conjunction;
	DOUBLE_LIST *pbranch;
	int condition;
	void *pstatment;
};

struct SEQUENCE_NODE {
	DOUBLE_LIST_NODE node;
	unsigned int min;
	unsigned int max;
};

struct KEYWORD_ENUM {
	MJSON *pjson;
	BOOL b_result;
	const char *charset;
	const char *keyword;
};

struct IDB_ITEM {
	IDB_ITEM() = default;
	~IDB_ITEM();
	NOMOVE(IDB_ITEM);

	sqlite3 *psqlite = nullptr;
	/* client reference count, item can be flushed into file system only count is 0 */
	std::string username;
	time_t last_time = 0, load_time = 0;
	uint32_t sub_id = 0;
	std::atomic<int> reference{0};
	std::timed_mutex lock;
};

struct idb_item_del {
	void operator()(IDB_ITEM *);
};

}

using IDB_REF = std::unique_ptr<IDB_ITEM, idb_item_del>;

enum {
	FIELD_NONE = 0,
	FIELD_UID,
	FIELD_RECEIVED,
	FIELD_SUBJECT,
	FIELD_FROM,
	FIELD_RCPT,
	FIELD_SIZE,
	FIELD_READ,
	FIELD_FLAG
};

namespace {

struct IDL_NODE {
	DOUBLE_LIST_NODE node;
	char *mid_string;
	uint32_t size;
};

struct DTLU_NODE {
	DOUBLE_LIST_NODE node;
	uint32_t idx;
	char *mid_string;
};

struct SIMU_NODE {
	DOUBLE_LIST_NODE node;
	uint32_t idx;
	uint32_t uid;
	char *mid_string;
	char *flags_buff;
};

struct SUB_NODE {
	DOUBLE_LIST_NODE node;
	char maildir[256];
	uint32_t sub_id;
};

}

static constexpr auto DB_LOCK_TIMEOUT = std::chrono::seconds(60);
static BOOL g_wal;
static BOOL g_async;
static int g_mime_num;
static size_t g_table_size;
static std::atomic<int> g_sequence_id;
static gromox::atomic_bool g_notify_stop; /* stop signal for scaning thread */
static uint64_t g_mmap_size;
static pthread_t g_scan_tid;
static int g_cache_interval;          /* maximum living interval in table */
static char g_org_name[256];
static std::shared_ptr<MIME_POOL> g_mime_pool;
static LIB_BUFFER *g_alloc_mjson;      /* mjson allocator */
static char g_default_charset[32];
static char g_default_timezone[64];
static std::mutex g_hash_lock;
static std::unordered_map<std::string, IDB_ITEM> g_hash_table;

static DOUBLE_LIST *mail_engine_ct_parse_sequence(char *string);
static BOOL mail_engine_ct_hint_sequence(DOUBLE_LIST *plist, unsigned int num, unsigned int max_uid);
static void mail_engine_ct_free_sequence(DOUBLE_LIST *plist);

static int mail_engine_get_sequence_id()
{
	int old = 0, nu = 0;
	do {
		old = g_sequence_id.load(std::memory_order_relaxed);
		nu  = old != INT_MAX ? old + 1 : 1;
	} while (!g_sequence_id.compare_exchange_weak(old, nu));
	return nu;
}

static char* mail_engine_ct_to_utf8(const char *charset, const char *string)
{
	int length;
	iconv_t conv_id;
	size_t in_len, out_len;

	if (0 == strcasecmp(charset, "UTF-8") ||
		0 == strcasecmp(charset, "US-ASCII")) {
		return strdup(string);
	}	
	length = strlen(string) + 1;
	auto ret_string = me_alloc<char>(2 * length);
	if (NULL == ret_string) {
		return NULL;
	}
	conv_id = iconv_open("UTF-8", charset);
	if ((iconv_t)-1 == conv_id) {
		free(ret_string);
		return NULL;
	}
	auto pin = deconst(string);
	auto pout = ret_string;
	in_len = length;
	out_len = 2*length;
	if (iconv(conv_id, &pin, &in_len, &pout, &out_len) == static_cast<size_t>(-1)) {
		iconv_close(conv_id);
		free(ret_string);
		return NULL;
	}
	iconv_close(conv_id);
	return ret_string;
}

static uint64_t mail_engine_get_digest(sqlite3 *psqlite,
	const char *mid_string, char *digest_buff)
{
	MAIL imail;
	size_t size;
	int tmp_len;
	char *ptoken;
	const char *pext;
	uint64_t folder_id;
	char tmp_buff[128];
	char temp_path[256];
	struct stat node_stat;
	
	snprintf(temp_path, 256, "%s/ext/%s",
		common_util_get_maildir(), mid_string);
	wrapfd fd = open(temp_path, O_RDONLY);
	if (fd.get() < 0) {
		snprintf(temp_path, 256, "%s/eml/%s",
			common_util_get_maildir(), mid_string);
		fd = open(temp_path, O_RDONLY);
		if (fd.get() < 0 || fstat(fd.get(), &node_stat) < 0) {
			fprintf(stderr, "%s: %s: %s\n", __func__, temp_path, strerror(errno));
			return 0;
		}
		if (!S_ISREG(node_stat.st_mode))
			return 0;
		std::unique_ptr<char[], stdlib_delete> pbuff(me_alloc<char>(node_stat.st_size));
		if (NULL == pbuff) {
			return 0;
		}
		if (read(fd.get(), pbuff.get(), node_stat.st_size) != node_stat.st_size)
			return 0;
		fd.close();
		MAIL imail(g_mime_pool);
		if (!imail.retrieve(pbuff.get(), node_stat.st_size))
			return 0;
		tmp_len = sprintf(digest_buff, "{\"file\":\"\",");
		if (imail.get_digest(&size, digest_buff + tmp_len,
		    MAX_DIGLEN - tmp_len - 1) <= 0)
			return 0;
		imail.clear();
		pbuff.reset();
		tmp_len = strlen(digest_buff);
		memcpy(digest_buff + tmp_len, "}", 2);
		tmp_len ++;
		snprintf(temp_path, 256, "%s/ext/%s",
			common_util_get_maildir(), mid_string);
		fd = open(temp_path, O_CREAT|O_TRUNC|O_WRONLY, 0666);
		if (fd.get() >= 0) {
			write(fd.get(), digest_buff, tmp_len);
			fd.close();
		}
	} else {
		if (fstat(fd.get(), &node_stat) != 0 || !S_ISREG(node_stat.st_mode) ||
		    node_stat.st_size >= MAX_DIGLEN)
			return 0;
		fd = open(temp_path, O_RDONLY);
		if (fd.get() < 0 || read(fd.get(), digest_buff, node_stat.st_size) != node_stat.st_size)
			return 0;
		digest_buff[node_stat.st_size] = '\0';
		fd.close();
	}
	auto pstmt = gx_sql_prep(psqlite, "SELECT uid, recent, read,"
	             " unsent, flagged, replied, forwarded, deleted, ext,"
	             " folder_id FROM messages WHERE mid_string=?");
	if (pstmt == nullptr)
		return 0;
	sqlite3_bind_text(pstmt, 1, mid_string, -1, SQLITE_STATIC);
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		return 0;
	}
	folder_id = sqlite3_column_int64(pstmt, 9);
	snprintf(tmp_buff, arsizeof(tmp_buff), "\"%s\"", mid_string);
	set_digest(digest_buff, MAX_DIGLEN, "file", tmp_buff);
	snprintf(tmp_buff, arsizeof(tmp_buff), "%llu", sqlite3_column_int64(pstmt, 0));
	set_digest(digest_buff, MAX_DIGLEN, "uid", tmp_buff);
	snprintf(tmp_buff, arsizeof(tmp_buff), "%llu", sqlite3_column_int64(pstmt, 1));
	set_digest(digest_buff, MAX_DIGLEN, "recent", tmp_buff);
	snprintf(tmp_buff, arsizeof(tmp_buff), "%llu", sqlite3_column_int64(pstmt, 2));
	set_digest(digest_buff, MAX_DIGLEN, "read", tmp_buff);
	snprintf(tmp_buff, arsizeof(tmp_buff), "%llu", sqlite3_column_int64(pstmt, 3));
	set_digest(digest_buff, MAX_DIGLEN, "unsent", tmp_buff);
	snprintf(tmp_buff, arsizeof(tmp_buff), "%llu", sqlite3_column_int64(pstmt, 4));
	set_digest(digest_buff, MAX_DIGLEN, "flag", tmp_buff);
	snprintf(tmp_buff, arsizeof(tmp_buff), "%llu", sqlite3_column_int64(pstmt, 5));
	set_digest(digest_buff, MAX_DIGLEN, "replied", tmp_buff);
	snprintf(tmp_buff, arsizeof(tmp_buff), "%llu", sqlite3_column_int64(pstmt, 6));
	set_digest(digest_buff, MAX_DIGLEN, "forwarded", tmp_buff);
	snprintf(tmp_buff, arsizeof(tmp_buff), "%llu", sqlite3_column_int64(pstmt, 7));
	set_digest(digest_buff, MAX_DIGLEN, "deleted", tmp_buff);
	if (SQLITE_NULL == sqlite3_column_type(pstmt, 8)) {
		return folder_id;
	}
	pext = S2A(sqlite3_column_text(pstmt, 8));
	ptoken = strrchr(digest_buff, '}');
	if (NULL == ptoken) {
		return 0;
	}
	*ptoken = ',';
	if (ptoken + 1 - digest_buff + strlen(pext + 1) >= MAX_DIGLEN) {
		return 0;
	}
	strcpy(ptoken + 1, pext + 1);
	return folder_id;
}

static char* mail_engine_ct_decode_mime(
	const char *charset, const char *mime_string)
{
	int i, buff_len;
	int offset;
	int last_pos, begin_pos, end_pos;
	ENCODE_STRING encode_string;
	char *tmp_string;
	char temp_buff[1024];

	buff_len = strlen(mime_string);
	auto ret_string = me_alloc<char>(2 * (buff_len + 1));
	if (NULL == ret_string) {
		return NULL;
	}
	auto in_buff = deconst(mime_string);
	auto out_buff = ret_string;
	offset = 0;
	begin_pos = -1;
	end_pos = -1;
	last_pos = 0;
	for (i=0; i<buff_len-1&&offset<2*buff_len+1; i++) {
		if (-1 == begin_pos && '=' == in_buff[i] && '?' == in_buff[i + 1]) {
			begin_pos = i;
			if (i > last_pos) {
				memcpy(temp_buff, in_buff + last_pos, begin_pos - last_pos);
				temp_buff[begin_pos - last_pos] = '\0';
				HX_strltrim(temp_buff);
				tmp_string = mail_engine_ct_to_utf8(charset, temp_buff);
				if (NULL == tmp_string) {
					free(ret_string);
					return NULL;
				}
				auto tmp_len = strlen(tmp_string);
				memcpy(out_buff + offset, tmp_string, tmp_len);
				free(tmp_string);
				offset += tmp_len;
				last_pos = i;
			}
		}
		if (end_pos == -1 && begin_pos != -1 && in_buff[i] == '?' &&
		    in_buff[i+1] == '=' && ((in_buff[i-1] != 'q' &&
		    in_buff[i-1] != 'Q') || in_buff[i-2] != '?'))
			end_pos = i + 1;
		if (-1 != begin_pos && -1 != end_pos) {
			parse_mime_encode_string(in_buff + begin_pos, 
				end_pos - begin_pos + 1, &encode_string);
			auto tmp_len = strlen(encode_string.title);
			if (0 == strcmp(encode_string.encoding, "base64")) {
				size_t decode_len = 0;
				decode64(encode_string.title, tmp_len, temp_buff, &decode_len);
				temp_buff[decode_len] = '\0';
				tmp_string = mail_engine_ct_to_utf8(encode_string.charset, temp_buff);
			} else if (0 == strcmp(encode_string.encoding, "quoted-printable")){
				auto decode_len = qp_decode(temp_buff, encode_string.title, tmp_len);
				temp_buff[decode_len] = '\0';
				tmp_string = mail_engine_ct_to_utf8(encode_string.charset, temp_buff);
			} else {
				tmp_string = mail_engine_ct_to_utf8(charset, encode_string.title);
			}
			if (NULL == tmp_string) {
				free(ret_string);
				return NULL;
			}
			tmp_len = strlen(tmp_string);
			memcpy(out_buff + offset, tmp_string, tmp_len);
			free(tmp_string);
			offset += tmp_len;
			
			last_pos = end_pos + 1;
			i = end_pos;
			begin_pos = -1;
			end_pos = -1;
			continue;
		}
	}
	if (i > last_pos) {
		tmp_string = mail_engine_ct_to_utf8(charset, in_buff + last_pos);
		if (NULL == tmp_string) {
			free(ret_string);
			return NULL;
		}
		auto tmp_len = strlen(tmp_string);
		memcpy(out_buff + offset, tmp_string, tmp_len);
		free(tmp_string);
		offset += tmp_len;
	} 
	out_buff[offset] = '\0';
	return ret_string;

}

static void mail_engine_ct_enum_mime(MJSON_MIME *pmime, KEYWORD_ENUM *penum)
{
	char *pbuff;
	size_t length;
	size_t temp_len;
	char *ret_string;
	const char *charset;
	const char *filename;
	
	if (TRUE == penum->b_result) {
		return;
	}
	if (pmime->get_mtype() != MJSON_MIME_SINGLE)
		return;

	if (strncmp(pmime->get_ctype(), "text/", 5) == 0) {
		length = pmime->get_length(MJSON_MIME_CONTENT);
		pbuff = me_alloc<char>(2 * length + 1);
		if (NULL == pbuff) {
			return;
		}
		auto fd = penum->pjson->seek_fd(pmime->get_id(), MJSON_MIME_CONTENT);
		if (-1 == fd) {
			free(pbuff);
			return;
		}
		auto read_len = read(fd, pbuff, length);
		if (read_len < 0 || static_cast<size_t>(read_len) != length) {
			free(pbuff);
			return;
		}
		if (strcasecmp(pmime->get_encoding(), "base64") == 0) {
			if (0 != decode64_ex(pbuff, length,
				pbuff + length, length, &temp_len)) {
				free(pbuff);
				return;
			}
			pbuff[length + temp_len] = '\0';
		} else if (strcasecmp(pmime->get_encoding(), "quoted-printable") == 0) {
			temp_len = qp_decode(pbuff + length, pbuff, length);
			pbuff[length + temp_len] = '\0';
		} else {
			memcpy(pbuff + length, pbuff, length);
			pbuff[2*length] = '\0';
		}
			
		charset = pmime->get_charset();
		if ('\0' != charset[0]) {
			ret_string = mail_engine_ct_to_utf8(
						charset, pbuff + length);
		} else {
			ret_string = mail_engine_ct_to_utf8(
				penum->charset, pbuff + length);
		}
		if (NULL != ret_string) {
			if (NULL != search_string(ret_string,
				penum->keyword, strlen(ret_string))) {
				penum->b_result = TRUE;
			}
			free(ret_string);
		}
		free(pbuff);			
	} else {
		filename = pmime->get_filename();
		if ('\0' != filename[0]) {
			ret_string = mail_engine_ct_decode_mime(
							penum->charset, filename);
			if (NULL != ret_string) {
				if (NULL != search_string(ret_string,
					penum->keyword, strlen(ret_string))) {
					penum->b_result = TRUE;
				}
				free(ret_string);
			}
		}
	}
}

static BOOL mail_engine_ct_search_head(const char *charset,
	const char *file_path, const char *tag, const char *value)
{
	FILE * fp;
	char *str_mime;
	BOOL stat_head;
	size_t head_offset = 0, offset = 0, len;
	MIME_FIELD mime_field;
	char head_buff[64*1024];
	
	stat_head = FALSE;
	fp = fopen(file_path, "r");
	if (NULL == fp) {
		return FALSE;
	}
	while (NULL != fgets(head_buff + head_offset,
		64*1024 - head_offset, fp)) {
		len = strlen(head_buff + head_offset);
		head_offset += len;
		
		if (head_offset >= 64*1024 - 1) {
			break;
		}
		if (2 == len && 0 == strcmp("\r\n", head_buff + head_offset - 2)) {
			stat_head = TRUE;
			break;
		}
	}
	fclose(fp);
	if (FALSE == stat_head) {
		return FALSE;
	}
	auto tag_len = strlen(tag);
	while ((len = parse_mime_field(head_buff + offset,
	       head_offset - offset, &mime_field)) != 0) {
		offset += len;
		if (tag_len == mime_field.field_name_len &&
			0 == strncasecmp(tag, mime_field.field_name, tag_len)) {
			mime_field.field_value[mime_field.field_value_len] = '\0';
			str_mime = mail_engine_ct_decode_mime(
				charset, mime_field.field_value);
			if (NULL != str_mime) {
				if (NULL != search_string(str_mime,
					value, strlen(str_mime))) {
					free(str_mime);
					return TRUE;
				}
				free(str_mime);
			}
		}
	}
	return FALSE;
}

static BOOL mail_engine_ct_match_mail(sqlite3 *psqlite,
	const char *charset, sqlite3_stmt *pstmt_message,
	const char *mid_string, int id, int total_mail,
	uint32_t uidnext, CONDITION_TREE *ptree)
{
	int sp = 0;
	BOOL b_loaded;
	BOOL b_result;
	BOOL b_result1;
	int conjunction;
	time_t tmp_time;
	size_t temp_len;
	char *ret_string;
	int results[1024];
	char temp_buff[1024];
	char temp_buff1[1024];
	int conjunctions[1024];
	DOUBLE_LIST_NODE *pnode;
	KEYWORD_ENUM keyword_enum;
	CONDITION_TREE* trees[1024];
	char digest_buff[MAX_DIGLEN];
	DOUBLE_LIST_NODE* nodes[1024];
	CONDITION_TREE_NODE *ptree_node;
	
#define PUSH_MATCH(TREE, NODE, CONJUNCTION, RESULT) \
		{trees[sp]=TREE;nodes[sp]=NODE;conjunctions[sp]=CONJUNCTION;results[sp]=RESULT;sp++;}
	
#define POP_MATCH(TREE, NODE, CONJUNCTION, RESULT) \
		{sp--;TREE=trees[sp];NODE=nodes[sp];CONJUNCTION=conjunctions[sp];RESULT=results[sp];}

/* begin of recursion procedure */
	while (true) {
 PROC_BEGIN:
	b_result = TRUE;
	b_loaded = FALSE;
	for (pnode=double_list_get_head(ptree);	NULL!=pnode;
		pnode=double_list_get_after(ptree, pnode)) {
		ptree_node = (CONDITION_TREE_NODE*)pnode->pdata;
		conjunction = ptree_node->conjunction;
		if ((TRUE == b_result && CONJUNCTION_OR == conjunction) ||
			(FALSE == b_result && CONJUNCTION_AND == conjunction)) {
			continue;
		}
		b_result1 = FALSE;
		if (NULL != ptree_node->pbranch) {
			PUSH_MATCH(ptree, pnode, conjunction, b_result)
			ptree = ptree_node->pbranch;
			goto PROC_BEGIN;
		} else {
			switch (ptree_node->condition) {
			case CONDITION_ALL:
			case CONDITION_KEYWORD:
			case CONDITION_UNKEYWORD:
				b_result1 = TRUE;
				break;
			case CONDITION_ANSWERED:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 != sqlite3_column_int64(pstmt_message, 7)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_BCC:
				/* we do not support BCC field in mail digest,
					BCC should not recorded in mail head */
				break;
			case CONDITION_BEFORE:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				tmp_time = rop_util_nttime_to_unix(
					sqlite3_column_int64(pstmt_message, 10));
				if (tmp_time < (time_t)ptree_node->pstatment) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_BODY: {
				if (FALSE == b_loaded) {
					if (0 == mail_engine_get_digest(
						psqlite, mid_string, digest_buff)) {
						break;
					}
					b_loaded = TRUE;
				}
				MJSON temp_mjson(g_alloc_mjson);
				snprintf(temp_buff, 256, "%s/eml",
						common_util_get_maildir());
				if (temp_mjson.retrieve(digest_buff, strlen(digest_buff), temp_buff)) {
					keyword_enum.pjson = &temp_mjson;
					keyword_enum.b_result = FALSE;
					keyword_enum.charset = charset;
					keyword_enum.keyword = (const char*)ptree_node->pstatment;
					temp_mjson.enum_mime(reinterpret_cast<MJSON_MIME_ENUM>(mail_engine_ct_enum_mime), &keyword_enum);
					if (TRUE == keyword_enum.b_result) {
						b_result1 = TRUE;
					}
				}
				break;
			}
			case CONDITION_CC:
				if (FALSE == b_loaded) {
					if (0 == mail_engine_get_digest(
						psqlite, mid_string, digest_buff)) {
						break;
					}
					b_loaded = TRUE;
				}
				if (get_digest(digest_buff, "cc", temp_buff, arsizeof(temp_buff)) &&
				    decode64(temp_buff, strlen(temp_buff), temp_buff1, &temp_len) == 0) {
					temp_buff1[temp_len] = '\0';
					ret_string = mail_engine_ct_decode_mime(charset, temp_buff1);
					if (NULL != ret_string) {
						if (NULL != search_string(ret_string, (char*)
							ptree_node->pstatment, strlen(ret_string))) {
							b_result1 = TRUE;
						}
						free(ret_string);
					}
				}
				break;
			case CONDITION_DELETED:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 != sqlite3_column_int64(pstmt_message, 9)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_DRAFT:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 != sqlite3_column_int64(pstmt_message, 5)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_FLAGGED:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 != sqlite3_column_int64(pstmt_message, 6)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_FROM:
				if (FALSE == b_loaded) {
					if (0 == mail_engine_get_digest(
						psqlite, mid_string, digest_buff)) {
						break;
					}
					b_loaded = TRUE;
				}
				if (get_digest(digest_buff, "from", temp_buff, arsizeof(temp_buff)) &&
				    decode64(temp_buff, strlen(temp_buff), temp_buff1, &temp_len) == 0) {
					temp_buff1[temp_len] = '\0';
					ret_string = mail_engine_ct_decode_mime(charset, temp_buff1);
					if (NULL != ret_string) {
						if (NULL != search_string(ret_string, (char*)
							ptree_node->pstatment, strlen(ret_string))) {
							b_result1 = TRUE;
						}
						free(ret_string);
					}
				}
				break;
			case CONDITION_HEADER:
				snprintf(temp_buff1, 256, "%s/eml/%s",
					common_util_get_maildir(), mid_string);
				b_result1 = mail_engine_ct_search_head(charset,
					temp_buff1, ((char**)ptree_node->pstatment)[0],
					((char**)ptree_node->pstatment)[1]);
				break;
			case CONDITION_ID:
				b_result1 = mail_engine_ct_hint_sequence(
					(DOUBLE_LIST*)ptree_node->pstatment,
					id, total_mail);
				break;
			case CONDITION_LARGER:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (gx_sql_col_uint64(pstmt_message, 13) >
				    reinterpret_cast<size_t>(ptree_node->pstatment))
					b_result1 = TRUE;
				break;
			case CONDITION_NEW:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 != sqlite3_column_int64(pstmt_message, 3) &&
					0 == sqlite3_column_int64(pstmt_message, 4)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_OLD:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 == sqlite3_column_int64(pstmt_message, 3)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_ON:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				tmp_time = rop_util_nttime_to_unix(
					sqlite3_column_int64(pstmt_message, 10));
				if (tmp_time >= (time_t)ptree_node->pstatment &&
					tmp_time < (time_t)ptree_node->pstatment + 24*60*60) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_RECENT:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 != sqlite3_column_int64(pstmt_message, 3)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_SEEN:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 != sqlite3_column_int64(pstmt_message, 4)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_SENTBEFORE:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				tmp_time = rop_util_nttime_to_unix(
					sqlite3_column_int64(pstmt_message, 1));
				if (tmp_time < (time_t)ptree_node->pstatment) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_SENTON:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				tmp_time = rop_util_nttime_to_unix(
					sqlite3_column_int64(pstmt_message, 1));
				if (tmp_time >= (time_t)ptree_node->pstatment &&
					tmp_time < (time_t)ptree_node->pstatment + 24*60*60) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_SENTSINCE:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				tmp_time = rop_util_nttime_to_unix(
					sqlite3_column_int64(pstmt_message, 1));
				if (tmp_time >= (time_t)ptree_node->pstatment) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_SINCE:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				tmp_time = rop_util_nttime_to_unix(
					sqlite3_column_int64(pstmt_message, 10));
				if (tmp_time >= (time_t)ptree_node->pstatment) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_SMALLER:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (gx_sql_col_uint64(pstmt_message, 13) <
				    reinterpret_cast<size_t>(ptree_node->pstatment))
					b_result1 = TRUE;
				break;
			case CONDITION_SUBJECT:
				if (FALSE == b_loaded) {
					if (0 == mail_engine_get_digest(
						psqlite, mid_string, digest_buff)) {
						break;
					}
					b_loaded = TRUE;
				}
				if (get_digest(digest_buff, "subject", temp_buff, arsizeof(temp_buff)) &&
				    decode64(temp_buff, strlen(temp_buff), temp_buff1, &temp_len) == 0) {
					temp_buff1[temp_len] = '\0';
					ret_string = mail_engine_ct_decode_mime(charset, temp_buff1);
					if (NULL != ret_string) {
						if (NULL != search_string(ret_string, (char*)
							ptree_node->pstatment, strlen(ret_string))) {
							b_result1 = TRUE;
						}
						free(ret_string);
					}
				}
				break;
			case CONDITION_TEXT: {
				if (FALSE == b_loaded) {
					if (0 == mail_engine_get_digest(
						psqlite, mid_string, digest_buff)) {
						break;
					}
					b_loaded = TRUE;
				}
				if (get_digest(digest_buff, "cc", temp_buff, arsizeof(temp_buff)) &&
				    decode64(temp_buff, strlen(temp_buff), temp_buff1, &temp_len) == 0) {
					temp_buff1[temp_len] = '\0';
					ret_string = mail_engine_ct_decode_mime(
										charset, temp_buff1);
					if (NULL != ret_string) {
						if (NULL != search_string(ret_string, (char*)
							ptree_node->pstatment, strlen(ret_string))) {
							b_result1 = TRUE;
						}
						free(ret_string);
					}
				}
				if (TRUE == b_result1) {
					break;
				}
				if (get_digest(digest_buff, "from", temp_buff, arsizeof(temp_buff)) &&
				    decode64(temp_buff, strlen(temp_buff), temp_buff1, &temp_len) == 0) {
					temp_buff1[temp_len] = '\0';
					ret_string = mail_engine_ct_decode_mime(
										charset, temp_buff1);
					if (NULL != ret_string) {
						if (NULL != search_string(ret_string, (char*)
							ptree_node->pstatment, strlen(ret_string))) {
							b_result1 = TRUE;
						}
						free(ret_string);
					}
				}
				if (TRUE == b_result1) {
					break;
				}
				if (get_digest(digest_buff, "subject", temp_buff, arsizeof(temp_buff)) &&
				    decode64(temp_buff, strlen(temp_buff), temp_buff1, &temp_len) == 0) {
					temp_buff1[temp_len] = '\0';
					ret_string = mail_engine_ct_decode_mime(
										charset, temp_buff1);
					if (NULL != ret_string) {
						if (NULL != search_string(ret_string, (char*)
							ptree_node->pstatment, strlen(ret_string))) {
							b_result1 = TRUE;
						}
						free(ret_string);
					}
				}
				if (TRUE == b_result1) {
					break;
				}
				if (get_digest(digest_buff, "to", temp_buff, arsizeof(temp_buff)) &&
				    decode64(temp_buff, strlen(temp_buff), temp_buff1, &temp_len) == 0) {
					temp_buff1[temp_len] = '\0';
					ret_string = mail_engine_ct_decode_mime(
										charset, temp_buff1);
					if (NULL != ret_string) {
						if (NULL != search_string(ret_string, (char*)
							ptree_node->pstatment, strlen(ret_string))) {
							b_result1 = TRUE;
						}
						free(ret_string);
					}
				}
				if (TRUE == b_result1) {
					break;
				}
				MJSON temp_mjson(g_alloc_mjson);
				snprintf(temp_buff, 256, "%s/eml",
						common_util_get_maildir());
				if (temp_mjson.retrieve(digest_buff, strlen(digest_buff), temp_buff)) {
					keyword_enum.pjson = &temp_mjson;
					keyword_enum.b_result = FALSE;
					keyword_enum.charset = charset;
					keyword_enum.keyword = (const char*)ptree_node->pstatment;
					temp_mjson.enum_mime(reinterpret_cast<MJSON_MIME_ENUM>(mail_engine_ct_enum_mime), &keyword_enum);
					if (TRUE == keyword_enum.b_result) {
						b_result1 = TRUE;
					}
				}
				break;
			}
			case CONDITION_TO:
				if (FALSE == b_loaded) {
					if (0 == mail_engine_get_digest(
						psqlite, mid_string, digest_buff)) {
						break;
					}
					b_loaded = TRUE;
				}
				if (get_digest(digest_buff, "to", temp_buff, arsizeof(temp_buff)) &&
				    decode64(temp_buff, strlen(temp_buff), temp_buff1, &temp_len) == 0) {
					temp_buff1[temp_len] = '\0';
					ret_string = mail_engine_ct_decode_mime(charset, temp_buff1);
					if (NULL != ret_string) {
						if (NULL != search_string(ret_string, (char*)
							ptree_node->pstatment, strlen(ret_string))) {
							b_result1 = TRUE;
						}
						free(ret_string);
					}
				}
				break;
			case CONDITION_UNANSWERED:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 == sqlite3_column_int64(pstmt_message, 7)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_UID:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				b_result1 = mail_engine_ct_hint_sequence(
					(DOUBLE_LIST*)ptree_node->pstatment,
					sqlite3_column_int64(pstmt_message, 2),
					uidnext);
				break;
			case CONDITION_UNDELETED:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 == sqlite3_column_int64(pstmt_message, 9)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_UNDRAFT:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 == sqlite3_column_int64(pstmt_message, 5)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_UNFLAGGED:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 == sqlite3_column_int64(pstmt_message, 6)) {
					b_result1 = TRUE;
				}
				break;
			case CONDITION_UNSEEN:
				sqlite3_reset(pstmt_message);
				sqlite3_bind_text(pstmt_message,
					1, mid_string, -1, SQLITE_STATIC);
				if (SQLITE_ROW != sqlite3_step(pstmt_message)) {
					break;
				}
				if (0 == sqlite3_column_int64(pstmt_message, 4)) {
					b_result1 = TRUE;
				}
				break;
			default:
				debug_info("[mail_engine]: condition stat %d unknown!",
												ptree_node->condition);
				break;
			}
		}
		
 RECURSION_POINT:
		switch (conjunction) {
		case CONJUNCTION_AND:
			b_result = (b_result&&b_result1)?TRUE:FALSE;
			break;
		case CONJUNCTION_OR:
			b_result = (b_result||b_result1)?TRUE:FALSE;
			break;
		case CONJUNCTION_NOT:
			b_result = (b_result&&(!b_result1))?TRUE:FALSE;
			break;
		}
	}
	if (sp > 0) {
		b_result1 = b_result;
		POP_MATCH(ptree, pnode, conjunction, b_result)
		goto RECURSION_POINT;
	} else {
		return b_result;
	}
}
/* end of recursion procedure */

}

static int mail_engine_ct_compile_criteria(int argc,
	char **argv, int offset, char **argv_out)
{
	int i;
	int tmp_argc;
	int tmp_argc1;
	
	i = offset;
	if (argc < i + 1) {
		return -1;
	}
	argv_out[0] = argv[i];
	if (0 == strcasecmp(argv[i], "OR")) {
		i ++;
		if (argc < i + 1) {
			return -1;
		}
		tmp_argc = mail_engine_ct_compile_criteria(
						argc, argv, i, argv_out + 1);
		if (-1 == tmp_argc) {
			return -1;
		}
		
		i += tmp_argc;
		if (argc < i + 1) {
			return -1;
		}
		tmp_argc1 = mail_engine_ct_compile_criteria(
			argc, argv, i, argv_out + 1 + tmp_argc);
		if (-1 == tmp_argc1) {
			return -1;
		}
		return tmp_argc + tmp_argc1 + 1;
	} else if (0 == strcasecmp(argv[i], "ALL") ||
		0 == strcasecmp(argv[i], "ANSWERED") ||
		0 == strcasecmp(argv[i], "DELETED") ||
		0 == strcasecmp(argv[i], "DRAFT") ||
		0 == strcasecmp(argv[i], "FLAGGED") ||
		0 == strcasecmp(argv[i], "NEW") ||
		0 == strcasecmp(argv[i], "OLD") ||
		0 == strcasecmp(argv[i], "RECENT") ||
		0 == strcasecmp(argv[i], "SEEN") ||
		0 == strcasecmp(argv[i], "UNANSWERED") ||
		0 == strcasecmp(argv[i], "UNDELETED") ||
		0 == strcasecmp(argv[i], "UNDRAFT") ||
		0 == strcasecmp(argv[i], "UNFLAGGED") ||
		0 == strcasecmp(argv[i], "UNSEEN")) {
		return 1;
	} else if (0 == strcasecmp(argv[i], "BCC") ||
		0 == strcasecmp(argv[i], "BEFORE") ||
		0 == strcasecmp(argv[i], "BODY") ||
		0 == strcasecmp(argv[i], "CC") ||
		0 == strcasecmp(argv[i], "FROM") ||
		0 == strcasecmp(argv[i], "KEYWORD") ||
		0 == strcasecmp(argv[i], "LARGER") ||
		0 == strcasecmp(argv[i], "ON") ||
		0 == strcasecmp(argv[i], "SENTBEFORE") ||
		0 == strcasecmp(argv[i], "SENTON") ||
		0 == strcasecmp(argv[i], "SENTSINCE") ||
		0 == strcasecmp(argv[i], "SINCE") ||
		0 == strcasecmp(argv[i], "SMALLER") ||
		0 == strcasecmp(argv[i], "SUBJECT") ||
		0 == strcasecmp(argv[i], "TEXT") ||
		0 == strcasecmp(argv[i], "TO") ||
		0 == strcasecmp(argv[i], "UID") ||
		0 == strcasecmp(argv[i], "UNKEYWORD")) {
		i ++;
		if (argc < i + 1) {
			return -1;
		}
		argv_out[1] = argv[i];
		return 2;
	} else if (0 == strcasecmp(argv[i], "HEADER")) {
		i ++;
		if (argc < i + 1) {
			return -1;
		}
		argv_out[1] = argv[i];
		i++;
		if (argc < i + 1) {
			return -1;
		}
		argv_out[2] = argv[i];
		return 3;
	} else if (0 == strcasecmp(argv[i], "NOT")) {
		i ++;
		if (argc < i + 1) {
			return -1;
		}
		tmp_argc = mail_engine_ct_compile_criteria(
						argc, argv, i, argv_out + 1);
		if (-1 == tmp_argc) {
			return -1;
		}
		return tmp_argc + 1;
	} else {
		/* <sequence set> or () as default */
		return 1;
	}
}

static void mail_engine_ct_destroy_internal(DOUBLE_LIST *plist)
{
	DOUBLE_LIST_NODE *pnode;
	CONDITION_TREE_NODE *ptree_node;
	
	while ((pnode = double_list_pop_front(plist)) != nullptr) {
		ptree_node = (CONDITION_TREE_NODE*)pnode->pdata;
		if (NULL != ptree_node->pbranch) {
			mail_engine_ct_destroy_internal(ptree_node->pbranch);
			ptree_node->pbranch = NULL;
		} else {
			if (CONDITION_ID == ptree_node->condition ||
				CONDITION_UID == ptree_node->condition) {
				mail_engine_ct_free_sequence(
					(DOUBLE_LIST*)ptree_node->pstatment);
				ptree_node->pstatment = NULL;
			} else if (CONDITION_BCC == ptree_node->condition ||
				CONDITION_BODY == ptree_node->condition ||
				CONDITION_CC == ptree_node->condition ||
				CONDITION_FROM == ptree_node->condition ||
				CONDITION_KEYWORD == ptree_node->condition ||
				CONDITION_SUBJECT == ptree_node->condition ||
				CONDITION_TEXT == ptree_node->condition ||
				CONDITION_TO == ptree_node->condition ||
				CONDITION_UNKEYWORD == ptree_node->condition) {
				free(ptree_node->pstatment);
				ptree_node->pstatment = NULL;
			} else if (CONDITION_HEADER == ptree_node->condition) {
				free(((void**)ptree_node->pstatment)[0]);
				free(((void**)ptree_node->pstatment)[1]);
				free(ptree_node->pstatment);
				ptree_node->pstatment = NULL;
			}
		}
		free(ptree_node);
	}
	double_list_free(plist);
	free(plist);
}

static DOUBLE_LIST* mail_engine_ct_build_internal(
	const char *charset, int argc, char **argv)
{
	int i, len;
	int tmp_argc;
	int tmp_argc1;
	struct tm tmp_tm;
	char* tmp_argv[256];
	DOUBLE_LIST *plist1;
	DOUBLE_LIST_NODE *pnode;
	CONDITION_TREE_NODE *ptree_node;

	auto plist = me_alloc<DOUBLE_LIST>();
	if (NULL == plist) {
		return NULL;
	}
	double_list_init(plist);
	for (i=0; i<argc; i++) {
		ptree_node = me_alloc<CONDITION_TREE_NODE>();
		if (NULL == ptree_node) {
			mail_engine_ct_destroy_internal(plist);
			return NULL;
		}
		ptree_node->node.pdata = ptree_node;
		ptree_node->pbranch = NULL;
		if (0 == strcasecmp(argv[i], "NOT")) {
			ptree_node->conjunction = CONJUNCTION_NOT;
			i ++;
			if (i >= argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
		} else {
			ptree_node->conjunction = CONJUNCTION_AND;
		}
		if (0 == strcasecmp(argv[i], "BCC") ||
			0 == strcasecmp(argv[i], "BODY") ||
			0 == strcasecmp(argv[i], "CC") ||
			0 == strcasecmp(argv[i], "FROM") ||
			0 == strcasecmp(argv[i], "KEYWORD") ||
			0 == strcasecmp(argv[i], "SUBJECT") ||
			0 == strcasecmp(argv[i], "TEXT") ||
			0 == strcasecmp(argv[i], "TO") ||
			0 == strcasecmp(argv[i], "UNKEYWORD")) {
			if (0 == strcasecmp(argv[i], "BCC")) {
				ptree_node->condition = CONDITION_BCC;
			} else if (0 == strcasecmp(argv[i], "BODY")) {
				ptree_node->condition = CONDITION_BODY;
			} else if (0 == strcasecmp(argv[i], "CC")) {
				ptree_node->condition = CONDITION_CC;
			} else if (0 == strcasecmp(argv[i], "FROM")) {
				ptree_node->condition = CONDITION_FROM;
			} else if (0 == strcasecmp(argv[i], "KEYWORD")) {
				ptree_node->condition = CONDITION_KEYWORD;
			} else if (0 == strcasecmp(argv[i], "SUBJECT")) {
				ptree_node->condition = CONDITION_SUBJECT;
			} else if (0 == strcasecmp(argv[i], "TEXT")) {
				ptree_node->condition = CONDITION_TEXT;
			} else if (0 == strcasecmp(argv[i], "TO")) {
				ptree_node->condition = CONDITION_TO;
			} else if (0 == strcasecmp(argv[i], "UNKEYWORD")) {
				ptree_node->condition = CONDITION_UNKEYWORD;
			}
			i ++;
			if (i + 1 > argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			ptree_node->pstatment = mail_engine_ct_to_utf8(charset, argv[i]);
			if (NULL == ptree_node->pstatment) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
		} else if (0 == strcasecmp(argv[i], "BEFORE") ||
			0 == strcasecmp(argv[i], "ON") ||
			0 == strcasecmp(argv[i], "SENTBEFORE") ||
			0 == strcasecmp(argv[i], "SENTON") ||
			0 == strcasecmp(argv[i], "SENTSINCE") ||
			0 == strcasecmp(argv[i], "SINCE")) {
			if (i + 1 > argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			if (0 == strcasecmp(argv[i], "BEFORE")) {
				ptree_node->condition = CONDITION_BEFORE;
			} else if (0 == strcasecmp(argv[i], "ON")) {
				ptree_node->condition = CONDITION_ON;
			} else if (0 == strcasecmp(argv[i], "SENTBEFORE")) {
				ptree_node->condition = CONDITION_SENTBEFORE;
			} else if (0 == strcasecmp(argv[i], "SENTON")) {
				ptree_node->condition = CONDITION_SENTON;
			} else if (0 == strcasecmp(argv[i], "SENTSINCE")) {
				ptree_node->condition = CONDITION_SENTSINCE;
			} else if (0 == strcasecmp(argv[i], "SINCE")) {
				ptree_node->condition = CONDITION_SINCE;
			}
			i ++;
			if (i + 1 > argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			memset(&tmp_tm, 0, sizeof(tmp_tm));
			if (NULL == strptime(argv[i], "%d-%b-%Y", &tmp_tm)) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			ptree_node->pstatment = (void*)mktime(&tmp_tm);
		} else if ('(' == argv[i][0]) {
			len = strlen(argv[i]);
			argv[i][len - 1] = '\0';
			tmp_argc = parse_imap_args(argv[i] + 1,
				len - 2, tmp_argv, sizeof(tmp_argv));
			if (-1 == tmp_argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			plist1 = mail_engine_ct_build_internal(
						charset, tmp_argc, tmp_argv);
			if (NULL == plist1) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			ptree_node->pbranch = plist1;
		} else if (0 == strcasecmp(argv[i], "OR")) {
			i ++;
			if (i + 1 > argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			tmp_argc = mail_engine_ct_compile_criteria(
								argc, argv, i, tmp_argv);
			if (-1 == tmp_argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			i += tmp_argc;
			if (i + 1 > argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			tmp_argc1 = mail_engine_ct_compile_criteria(
					argc, argv, i, tmp_argv + tmp_argc);
			if (-1 == tmp_argc1) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			plist1 = mail_engine_ct_build_internal(charset,
							tmp_argc + tmp_argc1, tmp_argv);
			if (NULL == plist1) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			if (2 != double_list_get_nodes_num(plist1) ||
				NULL == (pnode = double_list_get_tail(plist1))) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				mail_engine_ct_destroy_internal(plist1);
				return NULL;
			}
			((CONDITION_TREE_NODE*)pnode->pdata)->conjunction = CONJUNCTION_OR;
			ptree_node->pbranch = plist1;
			i += tmp_argc1 - 1;
		} else if (0 == strcasecmp(argv[i], "ALL")) {
			ptree_node->condition = CONDITION_ALL;
		} else if (0 == strcasecmp(argv[i], "ANSWERED")) {
			ptree_node->condition = CONDITION_ANSWERED;
		} else if (0 == strcasecmp(argv[i], "DELETED")) {
			ptree_node->condition = CONDITION_DELETED;
		} else if (0 == strcasecmp(argv[i], "DRAFT")) {
			ptree_node->condition = CONDITION_DRAFT;
		} else if (0 == strcasecmp(argv[i], "FLAGGED")) {
			ptree_node->condition = CONDITION_FLAGGED;
		} else if (0 == strcasecmp(argv[i], "NEW")) {
			ptree_node->condition = CONDITION_NEW;
		} else if (0 == strcasecmp(argv[i], "OLD")) {
			ptree_node->condition = CONDITION_OLD;
		} else if (0 == strcasecmp(argv[i], "RECENT")) {
			ptree_node->condition = CONDITION_RECENT;
		} else if (0 == strcasecmp(argv[i], "SEEN")) {
			ptree_node->condition = CONDITION_SEEN;
		} else if (0 == strcasecmp(argv[i], "UNANSWERED")) {
			ptree_node->condition = CONDITION_UNANSWERED;
		} else if (0 == strcasecmp(argv[i], "UNDELETED")) {
			ptree_node->condition = CONDITION_UNDELETED;
		} else if (0 == strcasecmp(argv[i], "UNDRAFT")) {
			ptree_node->condition = CONDITION_UNDRAFT;
		} else if (0 == strcasecmp(argv[i], "UNFLAGGED")) {
			ptree_node->condition = CONDITION_UNFLAGGED;
		} else if (0 == strcasecmp(argv[i], "UNSEEN")) {
			ptree_node->condition = CONDITION_UNSEEN;
		} else if (0 == strcasecmp(argv[i], "HEADER")) {
			ptree_node->condition = CONDITION_HEADER;
			ptree_node->pstatment = me_alloc<char *>(2);
			if (NULL == ptree_node->pstatment) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			i ++;
			if (i + 1 > argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			((char**)ptree_node->pstatment)[0] = strdup(argv[i]);
			i ++;
			if (i + 1 > argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			((char**)ptree_node->pstatment)[1] = strdup(argv[i]);
		} else if (0 == strcasecmp(argv[i], "LARGER") ||
			0 == strcasecmp(argv[i], "SMALLER")) {
			if (0 == strcasecmp(argv[i], "LARGER")) {
				ptree_node->condition = CONDITION_LARGER;
			} else {
				ptree_node->condition = CONDITION_SMALLER;
			}
			i ++;
			if (i + 1 > argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			ptree_node->pstatment = reinterpret_cast<void *>(strtol(argv[i], nullptr, 0));
		} else if (0 == strcasecmp(argv[i], "UID")) {
			ptree_node->condition = CONDITION_UID;
			i ++;
			if (i + 1 > argc) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			plist1 = mail_engine_ct_parse_sequence(argv[i]);
			if (NULL == plist1) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			ptree_node->pstatment = plist1;
		} else {
			plist1 = mail_engine_ct_parse_sequence(argv[i]);
			if (NULL == plist1) {
				free(ptree_node);
				mail_engine_ct_destroy_internal(plist);
				return NULL;
			}
			ptree_node->condition = CONDITION_ID;
			ptree_node->pstatment = plist1;
		}
		double_list_append_as_tail(plist, &ptree_node->node);
	}
	return plist;
}

static CONDITION_TREE* mail_engine_ct_build(int argc, char **argv)
{
	if (0 == strcasecmp(argv[0], "CHARSET")) {
		if (argc < 3) {
			return NULL;
		}
		return mail_engine_ct_build_internal(argv[1], argc - 2, argv + 2);
		
	} else {
		return mail_engine_ct_build_internal("UTF-8", argc, argv);
	}
}

static void mail_engine_ct_destroy(CONDITION_TREE *ptree)
{
	mail_engine_ct_destroy_internal(ptree);
}

static DOUBLE_LIST *mail_engine_ct_parse_sequence(char *string)
{
	int i, len, temp;
	char *last_colon;
	char *last_break;
	SEQUENCE_NODE *pseq;
	
	len = strlen(string);
	if (',' == string[len - 1]) {
		len --;
	} else {
		string[len] = ',';
	}
	auto plist = me_alloc<DOUBLE_LIST>();
	if (NULL == plist) {
		return NULL;
	}
	double_list_init(plist);
	last_break = string;
	last_colon = NULL;
	for (i=0; i<=len; i++) {
		if (!HX_isdigit(string[i]) && string[i] != '*'
			&& ',' != string[i] && ':' != string[i]) {
			mail_engine_ct_free_sequence(plist);
			return NULL;
		}
		if (':' == string[i]) {
			if (NULL != last_colon) {
				mail_engine_ct_free_sequence(plist);
				return NULL;
			} else {
				last_colon = string + i;
				*last_colon = '\0';
			}
		} else if (',' == string[i]) {
			if (0 == string + i - last_break) {
				mail_engine_ct_free_sequence(plist);
				return NULL;
			}
			string[i] = '\0';
			pseq = me_alloc<SEQUENCE_NODE>();
			if (NULL == pseq) {
				mail_engine_ct_free_sequence(plist);
				return NULL;
			}
			pseq->node.pdata = pseq;
			if (NULL != last_colon) {
				if (0 == strcmp(last_break, "*")) {
					pseq->max = -1;
					if (0 == strcmp(last_colon + 1, "*")) {
						pseq->min = -1;
					} else {
						pseq->min = strtol(last_colon + 1, nullptr, 0);
						if (pseq->min <= 0) {
							free(pseq);
							mail_engine_ct_free_sequence(plist);
							return NULL;
						}
					}
				} else {
					pseq->min = strtol(last_break, nullptr, 0);
					if (pseq->min <= 0) {
						free(pseq);
						mail_engine_ct_free_sequence(plist);
						return NULL;
					}
					if (0 == strcmp(last_colon + 1, "*")) {
						pseq->max = -1;
					} else {
						pseq->max = strtol(last_colon + 1, nullptr, 0);
						if (pseq->max <= 0) {
							free(pseq);
							mail_engine_ct_free_sequence(plist);
							return NULL;
						}
					}
				}
				last_colon = NULL;
			} else {
				if (*last_break == '*' ||
				    (pseq->min = strtol(last_break, nullptr, 0)) <= 0) {
					free(pseq);
					mail_engine_ct_free_sequence(plist);
					return NULL;
				}
				pseq->max = pseq->min;
			}
			if (pseq->max < pseq->min) {
				temp = pseq->max;
				pseq->max = pseq->min;
				pseq->min = temp;
			}
			last_break = string + i + 1;
			double_list_append_as_tail(plist, &pseq->node);
		}
	}
	return plist;
}

static void mail_engine_ct_free_sequence(DOUBLE_LIST *plist)
{
	DOUBLE_LIST_NODE *pnode;
	
	while ((pnode = double_list_pop_front(plist)) != nullptr)
		free(pnode->pdata);
	double_list_free(plist);
	free(plist);
}

static BOOL mail_engine_ct_hint_sequence(DOUBLE_LIST *plist,
	unsigned int num, unsigned int max_uid)
{
	DOUBLE_LIST_NODE *pnode;
	
	for (pnode=double_list_get_head(plist); NULL!=pnode;
		pnode=double_list_get_after(plist, pnode)) {
		auto pseq = static_cast<SEQUENCE_NODE *>(pnode->pdata);
		if (pseq->max == static_cast<unsigned int>(-1)) {
			if (pseq->min == static_cast<unsigned int>(-1)) {
				if (num == max_uid) {
					return TRUE;
				}
			} else {
				if (num >= pseq->min) {
					return TRUE;
				}
			}
		} else {
			if (pseq->max >= num && pseq->min <= num) {
				return TRUE;
			}
		}
	}
	return FALSE;
}

static CONDITION_RESULT* mail_engine_ct_match(const char *charset,
	sqlite3 *psqlite, uint64_t folder_id, CONDITION_TREE *ptree,
	BOOL b_uid)
{
	int i;
	uint32_t uid;
	int total_mail;
	uint32_t uidnext;
	char sql_string[1024];
	const char *mid_string;
	SINGLE_LIST_NODE *pnode;

	snprintf(sql_string, arsizeof(sql_string), "SELECT count(message_id) "
	          "FROM messages WHERE folder_id=%llu", LLU(folder_id));
	auto pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return NULL;
	total_mail = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "SELECT uidnext FROM"
	          " folders WHERE folder_id=%llu", LLU(folder_id));
	pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return NULL;
	uidnext = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	auto pstmt_message = gx_sql_prep(psqlite, "SELECT message_id, mod_time, "
	                     "uid, recent, read, unsent, flagged, replied, forwarded,"
	                     "deleted, received, ext, folder_id, size FROM messages "
	                     "WHERE mid_string=?");
	if (pstmt_message == nullptr)
		return NULL;
	auto presult = me_alloc<CONDITION_RESULT>();
	if (NULL == presult) {
		return NULL;
	}
	single_list_init(&presult->list);
	presult->pcur_node = NULL;
	snprintf(sql_string, arsizeof(sql_string), "SELECT mid_string, uid FROM "
	          "messages WHERE folder_id=%llu ORDER BY uid", LLU(folder_id));
	pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr) {
		free(presult);
		return NULL;
	}
	i = 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		mid_string = S2A(sqlite3_column_text(pstmt, 0));
		uid = sqlite3_column_int64(pstmt, 1);
		if (TRUE == mail_engine_ct_match_mail(psqlite,
			charset, pstmt_message, mid_string, i + 1,
			total_mail, uidnext, ptree)) {
			pnode = me_alloc<SINGLE_LIST_NODE>();
			if (NULL == pnode) {
				continue;
			}
			if (FALSE == b_uid) {
				pnode->pdata = (void*)(long)(i + 1);
			} else {
				pnode->pdata = (void*)(long)uid;
			}
			single_list_append_as_tail(&presult->list, pnode);
		}
		i ++;
	}
	return presult;
}

static int mail_engine_ct_fetch_result(CONDITION_RESULT *presult)
{
    SINGLE_LIST_NODE *pnode;

    if (NULL == presult->pcur_node) {
        pnode = single_list_get_head(&presult->list);
    } else {
        pnode = single_list_get_after(&presult->list, presult->pcur_node);
    }
    if (NULL == pnode) {
        return -1;
    } else {
        presult->pcur_node = pnode;
        return (int)(long)pnode->pdata;
    }
}

static void mail_engine_ct_free_result(CONDITION_RESULT *presult)
{
	SINGLE_LIST_NODE *pnode;
	
	while ((pnode = single_list_pop_front(&presult->list)) != nullptr)
		free(pnode);
	free(presult);
}

static uint64_t mail_engine_get_folder_id(IDB_ITEM *pidb, const char *name)
{
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT "
	             "folder_id FROM folders WHERE name=?");
	if (pstmt == nullptr)
		return 0;
	sqlite3_bind_text(pstmt, 1, name, -1, SQLITE_STATIC);
	return sqlite3_step(pstmt) != SQLITE_ROW ? 0 :
	       sqlite3_column_int64(pstmt, 0);
}

static BOOL mail_engine_sort_folder(IDB_ITEM *pidb,
	const char *folder_name, int sort_field)
{
	uint32_t idx;
	uint64_t folder_id;
	char field_name[16];
	char sql_string[1024];
	
	switch (sort_field) {
	case FIELD_RECEIVED:
		strcpy(field_name, "received");
		break;
	case FIELD_SUBJECT:
		strcpy(field_name, "subject");
		break;
	case FIELD_FROM:
		strcpy(field_name, "sender");
		break;
	case FIELD_RCPT:
		strcpy(field_name, "rcpt");
		break;
	case FIELD_SIZE:
		strcpy(field_name, "size");
		break;
	case FIELD_READ:
		strcpy(field_name, "read");
		break;
	case FIELD_FLAG:
		strcpy(field_name, "flagged");
		break;
	default:
		strcpy(field_name, "uid");
		sort_field = FIELD_UID;
		break;
	}
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT folder_id,"
	             " sort_field FROM folders WHERE name=?");
	if (pstmt == nullptr)
		return FALSE;
	sqlite3_bind_text(pstmt, 1, folder_name, -1, SQLITE_STATIC);
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		return FALSE;
	}
	folder_id = sqlite3_column_int64(pstmt, 0);
	if (sort_field == sqlite3_column_int64(pstmt, 1)) {
		return TRUE;
	}
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "SELECT message_id FROM messages"
	          " WHERE folder_id=%llu ORDER BY %s", LLU(folder_id), field_name);
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr)
		return FALSE;
	auto pstmt1 = gx_sql_prep(pidb->psqlite, "UPDATE messages"
	              " SET idx=? WHERE message_id=?");
	if (pstmt1 == nullptr) {
		return FALSE;
	}
	idx = 1;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		sqlite3_reset(pstmt1);
		sqlite3_bind_int64(pstmt1, 1, idx);
		sqlite3_bind_int64(pstmt1, 2,
			sqlite3_column_int64(pstmt, 0));
		if (SQLITE_DONE != sqlite3_step(pstmt1)) {
			return FALSE;
		}
		idx ++;
	}
	pstmt.finalize();
	pstmt1.finalize();
	snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET sort_field=%d "
	        "WHERE folder_id=%llu", sort_field, LLU(folder_id));
	gx_sql_exec(pidb->psqlite, sql_string);
	return TRUE;
}

static void mail_engine_extract_digest_fields(const char *digest,
	char *subject, char *from, char *rcpt, size_t *psize)
{
	size_t out_len;
	char temp_buff[64*1024];
	char temp_buff1[64*1024];
	EMAIL_ADDR temp_address;
	
	subject[0] = '\0';
	if (get_digest(digest, "subject", temp_buff, arsizeof(temp_buff))) {
		if (decode64(temp_buff, strlen(temp_buff), subject, &out_len) != 0)
			/* Decode failed */
			subject[0] = '\0';
	}
	from[0] = '\0';
	if (get_digest(digest, "from", temp_buff, arsizeof(temp_buff))) {
		if (0 == decode64(temp_buff, strlen(temp_buff),
			temp_buff1, &out_len)) {
			memset(&temp_address, 0, sizeof(temp_address));
			parse_email_addr(&temp_address, temp_buff1);
			snprintf(from, UADDR_SIZE, "%s@%s",
				temp_address.local_part, temp_address.domain);
		}
	}
	rcpt[0] = '\0';
	if (get_digest(digest, "to", temp_buff, arsizeof(temp_buff))) {
		if (0 == decode64(temp_buff, strlen(temp_buff),
			temp_buff1, &out_len)) {
			for (size_t i = 0; i < out_len; ++i) {
				if (',' == temp_buff1[i] ||
					';' == temp_buff1[i]) {
					temp_buff1[i] = '\0';
					break;
				}
			}
			HX_strrtrim(temp_buff1);
			memset(&temp_address, 0, sizeof(temp_address));
			parse_email_addr(&temp_address, temp_buff1);
			snprintf(rcpt, UADDR_SIZE, "%s@%s",
				temp_address.local_part, temp_address.domain);
		}
	}
	*psize = 0;
	if (get_digest(digest, "size", temp_buff, arsizeof(temp_buff)))
		*psize = strtoull(temp_buff, nullptr, 0);
}

static void mail_engine_insert_message(sqlite3_stmt *pstmt,
	uint32_t *puidnext, uint64_t message_id, const char *mid_string,
	uint32_t message_flags, uint64_t received_time, uint64_t mod_time)
{
	size_t size;
	int tmp_len;
	char from[UADDR_SIZE], rcpt[UADDR_SIZE];
	uint8_t b_read;
	const char *dir;
	uint8_t b_unsent;
	char subject[1024];
	char temp_path[256];
	char temp_path1[256];
	char mid_string1[128];
	struct stat node_stat;
	MESSAGE_CONTENT *pmsgctnt;
	char temp_buff[MAX_DIGLEN];
	
	temp_path[0] = '\0';
	temp_path1[0] = '\0';
	dir = common_util_get_maildir();
	if (NULL != mid_string) {
		sprintf(temp_path, "%s/ext/%s", dir, mid_string);
		wrapfd fd = open(temp_path, O_RDONLY);
		if (fd.get() < 0 || fstat(fd.get(), &node_stat) != 0 ||
		    node_stat.st_size >= MAX_DIGLEN ||
		    read(fd.get(), temp_buff, node_stat.st_size) != node_stat.st_size)
			return;
		temp_buff[node_stat.st_size] = '\0';
	} else {
		if (FALSE == common_util_switch_allocator()) {
			return;
		}
		if (!exmdb_client::read_message(dir, NULL, 0,
			rop_util_make_eid_ex(1, message_id), &pmsgctnt)) {
			common_util_switch_allocator();
			return;
		}
		if (NULL == pmsgctnt) {
			common_util_switch_allocator();
			return;
		}
		MAIL imail;
		if (!oxcmail_export(pmsgctnt, false, OXCMAIL_BODY_PLAIN_AND_HTML,
		    g_mime_pool, &imail, common_util_alloc,
		    common_util_get_propids, common_util_get_propname)) {
			common_util_switch_allocator();
			return;
		}
		imail.set_header("X-Mailer", "gromox-midb " PACKAGE_VERSION);
		common_util_switch_allocator();
		tmp_len = sprintf(temp_buff, "{\"file\":\"\",");
		if (imail.get_digest(&size, temp_buff + tmp_len,
		    MAX_DIGLEN - tmp_len - 1) <= 0)
			return;
		tmp_len = strlen(temp_buff);
		memcpy(temp_buff + tmp_len, "}", 2);
		tmp_len ++;
		sprintf(mid_string1, "%ld.%d.midb", time(NULL),
			mail_engine_get_sequence_id());
		mid_string = mid_string1;
		sprintf(temp_path, "%s/ext/%s", dir, mid_string1);
		wrapfd fd = open(temp_path, O_CREAT|O_TRUNC|O_WRONLY, 0666);
		if (fd.get() < 0)
			return;
		if (write(fd.get(), temp_buff, tmp_len) != tmp_len)
			return;
		fd.close();
		sprintf(temp_path1, "%s/eml/%s", dir, mid_string1);
		fd = open(temp_path1, O_CREAT|O_TRUNC|O_WRONLY, 0666);
		if (fd.get() < 0)
			return;
		if (!imail.to_file(fd.get()))
			return;
	}
	(*puidnext) ++;
	b_unsent = !!(message_flags & MSGFLAG_UNSENT);
	b_read   = !!(message_flags & MSGFLAG_READ);
	mail_engine_extract_digest_fields(
		temp_buff, subject, from, rcpt, &size);
	sqlite3_reset(pstmt);
	sqlite3_bind_int64(pstmt, 1, message_id);
	sqlite3_bind_text(pstmt, 2, mid_string, -1, SQLITE_STATIC);
	sqlite3_bind_int64(pstmt, 3, mod_time);
	sqlite3_bind_int64(pstmt, 4, *puidnext);
	sqlite3_bind_int64(pstmt, 5, b_unsent);
	sqlite3_bind_int64(pstmt, 6, b_read);
	sqlite3_bind_text(pstmt, 7, subject, -1, SQLITE_STATIC);
	sqlite3_bind_text(pstmt, 8, from, -1, SQLITE_STATIC);
	sqlite3_bind_text(pstmt, 9, rcpt, -1, SQLITE_STATIC);
	sqlite3_bind_int64(pstmt, 10, size);
	sqlite3_bind_int64(pstmt, 11, received_time);
	sqlite3_step(pstmt);
}

static void mail_engine_sync_message(IDB_ITEM *pidb,
	sqlite3_stmt *pstmt, sqlite3_stmt *pstmt1, uint32_t *puidnext,
	uint64_t message_id, uint64_t received_time, const char *mid_string,
	const char *mid_string1, uint64_t mod_time, uint64_t mod_time1,
	uint32_t message_flags, uint8_t b_unsent, uint8_t b_read)
{
	uint8_t b_read1;
	uint8_t b_unsent1;
	char sql_string[256];
	
	if (NULL != mid_string || mod_time <= mod_time1) {
		b_unsent1 = !!(message_flags & MSGFLAG_UNSENT);
		b_read1   = !!(message_flags & MSGFLAG_READ);
		if (b_unsent != b_unsent1 || b_read != b_read1) {
			sqlite3_reset(pstmt1);
			sqlite3_bind_int64(pstmt1, 1, b_unsent1);
			sqlite3_bind_int64(pstmt1, 2, b_read1);
			sqlite3_bind_int64(pstmt1, 3, message_id);
			if (SQLITE_DONE != sqlite3_step(pstmt1)) {
				return;
			}
		}
		return;
	}
	snprintf(sql_string, arsizeof(sql_string), "DELETE FROM messages"
	        " WHERE message_id=%llu", LLU(message_id));
	if (gx_sql_exec(pidb->psqlite, sql_string) != SQLITE_OK)
		return;	
	mail_engine_insert_message(pstmt, puidnext, message_id,
			NULL, message_flags, received_time, mod_time);
}

static BOOL mail_engine_sync_contents(IDB_ITEM *pidb, uint64_t folder_id)
{
	const char *dir;
	TARRAY_SET rows;
	sqlite3 *psqlite;
	uint32_t uidnext;
	uint32_t uidnext1;
	uint64_t mod_time;
	uint64_t message_id;
	DOUBLE_LIST temp_list;
	char sql_string[1024];
	uint32_t message_flags;
	uint64_t received_time;
	DOUBLE_LIST_NODE *pnode;
	
	dir = common_util_get_maildir();
	fprintf(stderr, "Running sync_contents for %s, folder %llu\n",
	        dir, LLU(folder_id));
	if (!exmdb_client::query_folder_messages(
		dir, rop_util_make_eid_ex(1, folder_id), &rows)) {
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT uidnext FROM"
	          " folders WHERE folder_id=%llu", LLU(folder_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr)
		return FALSE;
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		return TRUE;
	}
	uidnext = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	uidnext1 = uidnext;
	if (SQLITE_OK != sqlite3_open_v2(":memory:", &psqlite,
		SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, NULL)) {
		return FALSE;
	}
	{
	auto cl_0 = make_scope_exit([&]() { sqlite3_close(psqlite); });
	auto sql_transact = gx_sql_begin_trans(psqlite);
	snprintf(sql_string, arsizeof(sql_string), "CREATE TABLE messages "
			"(message_id INTEGER PRIMARY KEY,"
			"mid_string TEXT,"
			"mod_time INTEGER,"
			"message_flags INTEGER,"
			"received INTEGER)");
	if (gx_sql_exec(psqlite, sql_string) != SQLITE_OK)
		return FALSE;
	pstmt = gx_sql_prep(psqlite, "INSERT INTO messages (message_id,"
	        " mid_string, mod_time, message_flags, received) VALUES "
	        "(?, ?, ?, ?, ?)");
	if (pstmt == nullptr) {
		return FALSE;
	}
	for (size_t i = 0; i < rows.count; ++i) {
		auto pvalue = rows.pparray[i]->getval(PidTagMid);
		if (NULL == pvalue) {
			continue;
		}
		message_id = rop_util_get_gc_value(*(uint64_t*)pvalue);
		pvalue = rows.pparray[i]->getval(PR_MESSAGE_FLAGS);
		if (NULL == pvalue) {
			continue;
		}
		message_flags = *(uint64_t*)pvalue;
		pvalue = rows.pparray[i]->getval(PR_LAST_MODIFICATION_TIME);
		if (NULL == pvalue) {
			mod_time = 0;
		} else {
			mod_time = *(uint64_t*)pvalue;
		}
		pvalue = rows.pparray[i]->getval(PROP_TAG_MESSAGEDELIVERYTIME);
		if (NULL == pvalue) {
			received_time = mod_time;
		} else {
			received_time = *(uint64_t*)pvalue;
		}
		pvalue = rows.pparray[i]->getval(PidTagMidString);
		sqlite3_reset(pstmt);
		sqlite3_bind_int64(pstmt, 1, message_id);
		if (NULL == pvalue) {
			sqlite3_bind_null(pstmt, 2);
		} else {
			sqlite3_bind_text(pstmt, 2, static_cast<char *>(pvalue), -1, SQLITE_STATIC);
		}
		sqlite3_bind_int64(pstmt, 3, mod_time);
		sqlite3_bind_int64(pstmt, 4, message_flags);
		sqlite3_bind_int64(pstmt, 5, received_time);
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			return FALSE;
		}
	}
	pstmt.finalize();
	sql_transact.commit();

	pstmt = gx_sql_prep(psqlite, "SELECT COUNT(*) FROM messages");
	size_t totalmsgs = 0, procmsgs = 0;
	if (pstmt != nullptr && sqlite3_step(pstmt) == SQLITE_ROW)
		totalmsgs = sqlite3_column_int64(pstmt, 0);

	snprintf(sql_string, arsizeof(sql_string), "SELECT message_id, "
		"mid_string, mod_time, message_flags, received"
		" FROM messages");
	pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	auto pstmt1 = gx_sql_prep(pidb->psqlite, "SELECT message_id, mid_string,"
	              " mod_time, unsent, read FROM messages WHERE message_id=?");
	if (pstmt1 == nullptr) {
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "INSERT INTO messages (message_id, "
		"folder_id, mid_string, mod_time, uid, unsent, read, subject,"
		" sender, rcpt, size, received) VALUES (?, %llu, ?, ?, ?, ?, "
		"?, ?, ?, ?, ?, ?)", LLU(folder_id));
	auto pstmt2 = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt2 == nullptr) {
		return FALSE;
	}
	auto pstmt3 = gx_sql_prep(pidb->psqlite, "UPDATE messages"
	              " SET unsent=?, read=? WHERE message_id=?");
	if (pstmt3 == nullptr) {
		return FALSE;
	}
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		message_id = sqlite3_column_int64(pstmt, 0);
		sqlite3_reset(pstmt1);
		sqlite3_bind_int64(pstmt1, 1, message_id);
		if (SQLITE_ROW != sqlite3_step(pstmt1)) {
			uidnext ++;
			mail_engine_insert_message(
				pstmt2, &uidnext, message_id,
				S2A(sqlite3_column_text(pstmt, 1)),
				sqlite3_column_int64(pstmt, 3),
				sqlite3_column_int64(pstmt, 4),
				sqlite3_column_int64(pstmt, 2));
		} else {
			mail_engine_sync_message(pidb,
				pstmt2, pstmt3, &uidnext, message_id,
				sqlite3_column_int64(pstmt, 4),
				S2A(sqlite3_column_text(pstmt, 1)),
				S2A(sqlite3_column_text(pstmt1, 1)),
				sqlite3_column_int64(pstmt, 2),
				sqlite3_column_int64(pstmt1, 2),
				sqlite3_column_int64(pstmt, 3),
				sqlite3_column_int64(pstmt1, 3),
				sqlite3_column_int64(pstmt1, 4));
		}
		if (++procmsgs % 512 == 0)
			fprintf(stderr, "sync_contents %s fld %llu progress: %zu/%zu\n",
			        dir, LLU(folder_id), procmsgs, totalmsgs);
	}
	if (procmsgs > 512)
		/* display final value */
			fprintf(stderr, "sync_contents %s fld %llu progress: %zu/%zu\n",
			        dir, LLU(folder_id), procmsgs, totalmsgs);
	pstmt.finalize();
	pstmt1.finalize();
	pstmt2.finalize();
	pstmt3.finalize();
	snprintf(sql_string, arsizeof(sql_string), "SELECT message_id FROM "
	          "messages WHERE folder_id=%llu", LLU(folder_id));
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	pstmt1 = gx_sql_prep(psqlite, "SELECT message_id"
	         " FROM messages WHERE message_id=?");
	if (pstmt1 == nullptr) {
		return FALSE;
	}
	double_list_init(&temp_list);
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		message_id = sqlite3_column_int64(pstmt, 0);
		sqlite3_reset(pstmt1);
		sqlite3_bind_int64(pstmt1, 1, message_id);
		if (SQLITE_ROW != sqlite3_step(pstmt1)) {
			pnode = cu_alloc<DOUBLE_LIST_NODE>();
			if (NULL == pnode) {
				return FALSE;
			}
			pnode->pdata = cu_alloc<uint64_t>();
			if (NULL == pnode->pdata) {
				return FALSE;
			}
			*(uint64_t*)pnode->pdata = message_id;
			double_list_append_as_tail(&temp_list, pnode);
		}
	}
	pstmt.finalize();
	pstmt1.finalize();
	if (0 != double_list_get_nodes_num(&temp_list)) {
		pstmt = gx_sql_prep(pidb->psqlite, "DELETE "
		        "FROM messages WHERE message_id=?");
		if (pstmt == nullptr) {
			return FALSE;
		}
		while ((pnode = double_list_pop_front(&temp_list)) != nullptr) {
			sqlite3_reset(pstmt);
			sqlite3_bind_int64(pstmt, 1, *(uint64_t*)pnode->pdata);
			if (SQLITE_DONE != sqlite3_step(pstmt)) {
				return FALSE;
			}
		}
		pstmt.finalize();
	}
	if (uidnext != uidnext1) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET uidnext=%u "
		        "WHERE folder_id=%llu", uidnext, LLU(folder_id));
		if (gx_sql_exec(pidb->psqlite, sql_string) != SQLITE_OK)
			return FALSE;
	}
	}
	snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET sort_field=%d "
	        "WHERE folder_id=%llu", FIELD_NONE, LLU(folder_id));
	gx_sql_exec(pidb->psqlite, sql_string);
	return TRUE;
}

static BOOL mail_engine_get_encoded_name(sqlite3_stmt *pstmt,
	uint64_t folder_id, char *encoded_name)
{
	int length;
	int offset;
	char temp_name[512];
	DOUBLE_LIST temp_list;
	DOUBLE_LIST_NODE *pnode;
	
	switch (folder_id) {
	case PRIVATE_FID_INBOX:
		strcpy(encoded_name, "inbox");
		return TRUE;
	case PRIVATE_FID_DRAFT:
		strcpy(encoded_name, "draft");
		return TRUE;
	case PRIVATE_FID_SENT_ITEMS:
		strcpy(encoded_name, "sent");
		return TRUE;
	case PRIVATE_FID_DELETED_ITEMS:
		strcpy(encoded_name, "trash");
		return TRUE;
	case PRIVATE_FID_JUNK:
		strcpy(encoded_name, "junk");
		return TRUE;
	}
	double_list_init(&temp_list);
	do {
		sqlite3_reset(pstmt);
		sqlite3_bind_int64(pstmt, 1, folder_id);
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			return FALSE;
		}
		pnode = cu_alloc<DOUBLE_LIST_NODE>();
		if (NULL == pnode) {
			return FALSE;
		}
		folder_id = sqlite3_column_int64(pstmt, 0);
		pnode->pdata = static_cast<char *>(common_util_dup(S2A(sqlite3_column_text(pstmt, 1))));
		if (NULL == pnode->pdata) {
			return FALSE;
		}
		double_list_insert_as_head(&temp_list, pnode);
	} while (PRIVATE_FID_IPMSUBTREE != folder_id);
	offset = 0;
	for (pnode=double_list_get_head(&temp_list); NULL!=pnode;
		pnode=double_list_get_after(&temp_list, pnode)) {
		length = strlen(static_cast<char *>(pnode->pdata));
		if (length >= 256) {
			return FALSE;
		}
		if (0 != offset) {
			temp_name[offset] = '/';
			offset ++;
		}
		if (offset + length >= 512) {
			return FALSE;
		}
		memcpy(temp_name + offset, pnode->pdata, length);
		offset += length;
	}
	encode_hex_binary(temp_name, offset, encoded_name, 1024);
	return TRUE;
}

static uint64_t mail_engine_get_top_folder_id(
	sqlite3_stmt *pstmt, uint64_t folder_id)
{
	uint64_t parent_fid;
	
	while (true) {
		sqlite3_reset(pstmt);
		sqlite3_bind_int64(pstmt, 1, folder_id);
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			return 0;
		}
		parent_fid = sqlite3_column_int64(pstmt, 0);
		if (PRIVATE_FID_IPMSUBTREE == parent_fid) {
			return folder_id;
		}
		folder_id = parent_fid;
	}
}

static BOOL mail_engine_sync_mailbox(IDB_ITEM *pidb)
{
	BOOL b_new;
	const char *dir;
	TARRAY_SET rows;
	sqlite3 *psqlite;
	uint32_t table_id;
	uint32_t row_count;
	uint64_t folder_id;
	uint64_t parent_fid;
	uint64_t commit_max;
	char sql_string[1280];
	DOUBLE_LIST temp_list;
	PROPTAG_ARRAY proptags;
	DOUBLE_LIST_NODE *pnode;
	char encoded_name[1024];
	uint32_t proptag_buff[6];
	
	dir = common_util_get_maildir();
	fprintf(stderr, "Running sync_mailbox for %s\n", dir);
	if (!exmdb_client::load_hierarchy_table(dir,
		rop_util_make_eid_ex(1, PRIVATE_FID_IPMSUBTREE),
		NULL, TABLE_FLAG_DEPTH|TABLE_FLAG_NONOTIFICATIONS,
		NULL, &table_id, &row_count)) {
		return FALSE;	
	}
	proptags.count = 6;
	proptags.pproptag = proptag_buff;
	proptag_buff[0] = PidTagFolderId;
	proptag_buff[1] = PidTagParentFolderId;
	proptag_buff[2] = PR_ATTR_HIDDEN;
	proptag_buff[3] = PR_CONTAINER_CLASS;
	proptag_buff[4] = PR_DISPLAY_NAME;
	proptag_buff[5] = PR_LOCAL_COMMIT_TIME_MAX;
	if (!exmdb_client::query_table(dir, NULL,
		0, table_id, &proptags, 0, row_count, &rows)) {
		exmdb_client::unload_table(dir, table_id);
		return FALSE;
	}
	exmdb_client::unload_table(dir, table_id);
	if (SQLITE_OK != sqlite3_open_v2(":memory:", &psqlite,
		SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, NULL)) {
		return FALSE;
	}
	{
	auto cl_0 = make_scope_exit([&]() { sqlite3_close(psqlite); });
	auto sql_transact = gx_sql_begin_trans(psqlite);
	snprintf(sql_string, arsizeof(sql_string), "CREATE TABLE folders "
			"(folder_id INTEGER PRIMARY KEY,"
			"parent_fid INTEGER,"
			"display_name TEXT,"
			"commit_max INTEGER)");
	if (gx_sql_exec(psqlite, sql_string) != SQLITE_OK)
		return FALSE;
	auto pstmt = gx_sql_prep(psqlite, "INSERT INTO folders (folder_id, "
	             "parent_fid, display_name, commit_max) VALUES (?, ?, ?, ?)");
	if (pstmt == nullptr) {
		return FALSE;
	}
	for (size_t i = 0; i < rows.count; ++i) {
		const void *pvalue = rows.pparray[i]->getval(PR_ATTR_HIDDEN);
		if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
			continue;
		}
		pvalue = rows.pparray[i]->getval(PR_CONTAINER_CLASS);
		if (pvalue == nullptr || strcasecmp(static_cast<const char *>(pvalue), "IPF.Note") != 0)
			continue;
		sqlite3_reset(pstmt);
		pvalue = rows.pparray[i]->getval(PidTagFolderId);
		if (NULL == pvalue) {
			continue;
		}
		folder_id = rop_util_get_gc_value(*(uint64_t*)pvalue);
		sqlite3_bind_int64(pstmt, 1, folder_id);
		pvalue = rows.pparray[i]->getval(PidTagParentFolderId);
		if (NULL == pvalue) {
			continue;
		}
		parent_fid = rop_util_get_gc_value(*(uint64_t*)pvalue);
		sqlite3_bind_int64(pstmt, 2, parent_fid);
		switch (folder_id) {
		case PRIVATE_FID_INBOX:
			pvalue = "inbox";
			break;
		case PRIVATE_FID_DRAFT:
			pvalue = "draft";
			break;
		case PRIVATE_FID_SENT_ITEMS:
			pvalue = "sent";
			break;
		case PRIVATE_FID_DELETED_ITEMS:
			pvalue = "trash";
			break;
		case PRIVATE_FID_JUNK:
			pvalue = "junk";
			break;
		default:
			pvalue = rows.pparray[i]->getval(PR_DISPLAY_NAME);
			if (pvalue == nullptr || strlen(static_cast<const char *>(pvalue)) >= 256)
				continue;
			break;
		}
		sqlite3_bind_text(pstmt, 3, static_cast<const char *>(pvalue), -1, SQLITE_STATIC);
		pvalue = rows.pparray[i]->getval(PR_LOCAL_COMMIT_TIME_MAX);
		if (NULL == pvalue) {
			sqlite3_bind_int64(pstmt, 4, 0);
		} else {
			sqlite3_bind_int64(pstmt, 4, *(uint64_t*)pvalue);
		}
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			return FALSE;
		}
	}
	pstmt.finalize();
	sql_transact.commit();
	auto pidb_transact = gx_sql_begin_trans(pidb->psqlite);
	pstmt = gx_sql_prep(psqlite, "SELECT folder_id, "
	        "parent_fid, commit_max FROM folders");
	if (pstmt == nullptr) {
		return false;
	}
	auto pstmt1 = gx_sql_prep(pidb->psqlite, "SELECT folder_id, parent_fid, "
	              "commit_max, name FROM folders WHERE folder_id=?");
	if (pstmt1 == nullptr) {
		return false;
	}
	auto pstmt2 = gx_sql_prep(pidb->psqlite, "INSERT INTO folders (folder_id, "
				"parent_fid, commit_max, name) VALUES (?, ?, ?, ?)");
	if (pstmt2 == nullptr) {
		return false;
	}
	auto pstmt3 = gx_sql_prep(psqlite, "SELECT parent_fid, "
		"display_name FROM folders WHERE folder_id=?");
	if (pstmt3 == nullptr) {
		return false;
	}
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		folder_id = sqlite3_column_int64(pstmt, 0);
		switch (mail_engine_get_top_folder_id(pstmt3, folder_id)) {
		case PRIVATE_FID_OUTBOX:
		case PRIVATE_FID_SYNC_ISSUES:
			continue;			
		}
		parent_fid = sqlite3_column_int64(pstmt, 1);
		commit_max = sqlite3_column_int64(pstmt, 2);
		if (FALSE == mail_engine_get_encoded_name(
			pstmt3, folder_id, encoded_name)) {
			continue;
		}
		sqlite3_reset(pstmt1);
		sqlite3_bind_int64(pstmt1, 1, folder_id);
		if (SQLITE_ROW != sqlite3_step(pstmt1)) {
			sqlite3_reset(pstmt2);
			sqlite3_bind_int64(pstmt2, 1, folder_id);
			sqlite3_bind_int64(pstmt2, 2, parent_fid);
			sqlite3_bind_int64(pstmt2, 3, commit_max);
			sqlite3_bind_text(pstmt2, 4, encoded_name, -1, SQLITE_STATIC);
			if (SQLITE_DONE != sqlite3_step(pstmt2)) {
				return false;
			}
			b_new = TRUE;
		} else {
			if (gx_sql_col_uint64(pstmt1, 1) != parent_fid) {
				snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET "
					"parent_fid=%llu WHERE folder_id=%llu",
					LLU(parent_fid), LLU(folder_id));
				gx_sql_exec(pidb->psqlite, sql_string);
			}
			if (strcmp(encoded_name, S2A(sqlite3_column_text(pstmt1, 3))) != 0) {
				snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET name='%s' "
				        "WHERE folder_id=%llu", encoded_name, LLU(folder_id));
				gx_sql_exec(pidb->psqlite, sql_string);
			}
			if (gx_sql_col_uint64(pstmt1, 2) == commit_max)
				continue;	
			b_new = FALSE;
		}
		if (FALSE == mail_engine_sync_contents(pidb, folder_id)) {
			return false;
		}
		if (FALSE == b_new) {
			snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET commit_max=%llu"
			        " WHERE folder_id=%llu", LLU(commit_max), LLU(folder_id));
			gx_sql_exec(pidb->psqlite, sql_string);
		}
	}
	pstmt.finalize();
	pstmt1.finalize();
	pstmt2.finalize();
	pstmt3.finalize();
	snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id FROM folders");
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return false;
	}
	pstmt1 = gx_sql_prep(psqlite, "SELECT "
	         "folder_id FROM folders WHERE folder_id=?");
	if (pstmt1 == nullptr) {
		return false;
	}
	double_list_init(&temp_list);
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		folder_id = sqlite3_column_int64(pstmt, 0);
		sqlite3_reset(pstmt1);
		sqlite3_bind_int64(pstmt1, 1, folder_id);
		if (SQLITE_ROW != sqlite3_step(pstmt1)) {
			pnode = cu_alloc<DOUBLE_LIST_NODE>();
			if (NULL == pnode) {
				return false;
			}
			pnode->pdata = cu_alloc<uint64_t>();
			if (NULL == pnode->pdata) {
				return false;
			}
			*(uint64_t*)pnode->pdata = folder_id;
			double_list_append_as_tail(&temp_list, pnode);
		}
	}
	pstmt.finalize();
	pstmt1.finalize();
	if (0 != double_list_get_nodes_num(&temp_list)) {
		pstmt = gx_sql_prep(pidb->psqlite, "DELETE"
		        " FROM folders WHERE folder_id=?");
		if (pstmt == nullptr) {
			return false;
		}
		while ((pnode = double_list_pop_front(&temp_list)) != nullptr) {
			sqlite3_reset(pstmt);
			sqlite3_bind_int64(pstmt, 1, *(uint64_t*)pnode->pdata);
			if (SQLITE_DONE != sqlite3_step(pstmt)) {
				return false;
			}
		}
		pstmt.finalize();
	}
	pidb_transact.commit();
	}
	if (!exmdb_client::subscribe_notification(dir,
		NOTIFICATION_TYPE_OBJECTCREATED|NOTIFICATION_TYPE_OBJECTDELETED|
		NOTIFICATION_TYPE_OBJECTMODIFIED|NOTIFICATION_TYPE_OBJECTMOVED|
		NOTIFICATION_TYPE_OBJECTCOPIED|NOTIFICATION_TYPE_NEWMAIL, TRUE,
		0, 0, &pidb->sub_id)) {
		pidb->sub_id = 0;	
	}
	time(&pidb->load_time);
	fprintf(stderr, "Ended sync_mailbox for %s\n", dir);
	return TRUE;
}

static IDB_REF mail_engine_peek_idb(const char *path)
{
	char htag[256];
	
	swap_string(htag, path);
	std::unique_lock hhold(g_hash_lock);
	auto it = g_hash_table.find(htag);
	if (it == g_hash_table.end())
		return {};
	auto pidb = &it->second;
	pidb->reference ++;
	hhold.unlock();
	pidb->lock.lock();
	if (NULL == pidb->psqlite) {
		pidb->last_time = 0;
		pidb->lock.unlock();
		hhold.lock();
		pidb->reference --;
		hhold.unlock();
		return {};
	}
	return IDB_REF(pidb);
}

static IDB_REF mail_engine_get_idb(const char *path)
{
	BOOL b_load;
	char htag[256];
	char temp_path[256];
	char sql_string[1024];
	
	b_load = FALSE;
	swap_string(htag, path);
	std::unique_lock hhold(g_hash_lock);
	if (g_hash_table.size() >= g_table_size) {
		debug_info("[mail_engine]: W-1295: no room in idb hash table!");
		return {};
	}
	decltype(g_hash_table.try_emplace(htag)) xp;
	try {
		xp = g_hash_table.try_emplace(htag);
	} catch (const std::bad_alloc &) {
		hhold.unlock();
		debug_info("[mail_engine]: W-1294: mail_engine_get_idb ENOMEM");
		return {};
	}
	auto pidb = &xp.first->second;
	if (xp.second) {
		sprintf(temp_path, "%s/exmdb/midb.sqlite3", path);
		auto ret = sqlite3_open_v2(temp_path, &pidb->psqlite, SQLITE_OPEN_READWRITE, nullptr);
		if (ret != SQLITE_OK) {
			g_hash_table.erase(xp.first);
			fprintf(stderr, "E-1438: sqlite3_open %s: %s\n", temp_path, sqlite3_errstr(ret));
			return {};
		}
		gx_sql_exec(pidb->psqlite, "PRAGMA foreign_keys=ON");
		if (FALSE == g_async) {
			gx_sql_exec(pidb->psqlite, "PRAGMA synchronous=OFF");
		} else {
			gx_sql_exec(pidb->psqlite, "PRAGMA synchronous=ON");
		}
		if (FALSE == g_wal) {
			gx_sql_exec(pidb->psqlite, "PRAGMA journal_mode=DELETE");
		} else {
			gx_sql_exec(pidb->psqlite, "PRAGMA journal_mode=WAL");
		}
		if (0 != g_mmap_size) {
			snprintf(sql_string, sizeof(sql_string), "PRAGMA mmap_size=%llu", LLU(g_mmap_size));
			gx_sql_exec(pidb->psqlite, sql_string);
		}
		gx_sql_exec(pidb->psqlite, "DELETE FROM mapping");
		snprintf(sql_string, arsizeof(sql_string), "SELECT config_value FROM "
			"configurations WHERE config_id=%u", CONFIG_ID_USERNAME);
		auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
		if (pstmt == nullptr) {
			g_hash_table.erase(xp.first);
			return {};
		}
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			pstmt.finalize();
			sqlite3_close(pidb->psqlite);
			g_hash_table.erase(xp.first);
			return {};
		}
		try {
			pidb->username = S2A(sqlite3_column_text(pstmt, 0));
		} catch (const std::bad_alloc &) {
			pstmt.finalize();
			g_hash_table.erase(xp.first);
			return {};
		}
		pstmt.finalize();
		b_load = TRUE;
	} else if (pidb->reference > MAX_DB_WAITING_THREADS) {
		hhold.unlock();
		debug_info("[mail_engine]: too many threads waiting on %s\n", path);
		return {};
	}
	pidb->reference ++;
	hhold.unlock();
	if (!pidb->lock.try_lock_for(DB_LOCK_TIMEOUT)) {
		hhold.lock();
		pidb->reference --;
		hhold.unlock();
		return {};
	}
	if (TRUE == b_load) {
		mail_engine_sync_mailbox(pidb);
	} else {
		if (NULL == pidb->psqlite) {
			pidb->last_time = 0;
			pidb->lock.unlock();
			hhold.lock();
			pidb->reference --;
			hhold.unlock();
			return {};
		}
	}
	return IDB_REF(pidb);
}

void idb_item_del::operator()(IDB_ITEM *pidb)
{
	time(&pidb->last_time);
	pidb->lock.unlock();
	std::lock_guard hhold(g_hash_lock);
	pidb->reference --;
}

IDB_ITEM::~IDB_ITEM()
{
	if (psqlite != nullptr)
		sqlite3_close(psqlite);
}

static void *midbme_scanwork(void *param)
{
	int count;
	char path[256];
	SUB_NODE *psub;
	time_t now_time;
	DOUBLE_LIST temp_list;
	DOUBLE_LIST_NODE *pnode;

	count = 0;
	double_list_init(&temp_list);
	while (!g_notify_stop) {
		sleep(1);
		if (count < 10) {
			count ++;
			continue;
		}
		count = 0;
		std::unique_lock hhold(g_hash_lock);
		for (auto it = g_hash_table.begin(); it != g_hash_table.end(); ) {
			auto pidb = &it->second;
			time(&now_time);
			bool clean = pidb->reference == 0 &&
			             (pidb->sub_id == 0 ||
			              now_time - pidb->last_time > g_cache_interval ||
			              now_time - pidb->load_time > RELOAD_INTERVAL);
			if (!clean) {
				++it;
				continue;
			}
			swap_string(path, it->first.c_str());
				if (0 != pidb->sub_id) {
					psub = me_alloc<SUB_NODE>();
					if (NULL != psub) {
						psub->node.pdata = psub;
						strcpy(psub->maildir, path);
						psub->sub_id = pidb->sub_id;
						double_list_append_as_tail(
							&temp_list, &psub->node);
					}
				}
			it = g_hash_table.erase(it);
		}
		hhold.unlock();
		while ((pnode = double_list_pop_front(&temp_list)) != nullptr) {
			psub = (SUB_NODE*)pnode->pdata;
			if (TRUE == common_util_build_environment(psub->maildir)) {
				exmdb_client::unsubscribe_notification(
						psub->maildir, psub->sub_id);
				common_util_free_environment();
			}
			free(psub);
		}
	}
	std::unique_lock hhold(g_hash_lock);
	for (auto it = g_hash_table.begin(); it != g_hash_table.end(); ) {
		auto pidb = &it->second;
		swap_string(path, it->first.c_str());
		if (0 != pidb->sub_id) {
			exmdb_client::unsubscribe_notification(
								path, pidb->sub_id);
		}
		it = g_hash_table.erase(it);
	}
	hhold.unlock();
	double_list_free(&temp_list);
	return nullptr;
}
	
static int mail_engine_mckfl(int argc, char **argv, int sockd)
{
	uint64_t quota;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	uint32_t tmp_proptags[2];
	
	if (2 != argc || strlen(argv[1]) >= 256) {
		return MIDB_E_PARAMETER_ERROR;
	}
	proptags.count = 2;
	proptags.pproptag = tmp_proptags;
	tmp_proptags[0] = PR_PROHIBIT_RECEIVE_QUOTA;
	tmp_proptags[1] = PR_MESSAGE_SIZE_EXTENDED;
	if (!exmdb_client::get_store_properties(
		argv[1], 0, &proptags, &propvals)) {
		return MIDB_E_NO_MEMORY;
	}
	auto ptotal = propvals.get<uint64_t>(PR_MESSAGE_SIZE_EXTENDED);
	auto pmax   = propvals.get<uint32_t>(PR_PROHIBIT_RECEIVE_QUOTA);
	if (NULL != ptotal && NULL != pmax) {
		quota = *pmax;
		quota *= 1024;
		if (*ptotal >= quota) {
			cmd_write(sockd, "TRUE 1\r\n", 8);
			return 0;
		}
	}
	cmd_write(sockd, "TRUE 0\r\n", 8);
	return 0;
}

static int mail_engine_mping(int argc, char **argv, int sockd)
{
	if (2 != argc || strlen(argv[1]) >= 256) {
		return MIDB_E_PARAMETER_ERROR;
	}
	mail_engine_get_idb(argv[1]);
	exmdb_client::ping_store(argv[1]);
	cmd_write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int mail_engine_menum(int argc, char **argv, int sockd)
{
	int count;
	int offset;
	int temp_len;
	char sql_string[1024];
	char temp_buff[256*1024];
	
	if (2 != argc || strlen(argv[1]) >= 256) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id, name FROM folders");
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	temp_len = 32;
	count = 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		switch (sqlite3_column_int64(pstmt, 0)) {
		case PRIVATE_FID_INBOX:
		case PRIVATE_FID_DRAFT:
		case PRIVATE_FID_SENT_ITEMS:
		case PRIVATE_FID_DELETED_ITEMS:
		case PRIVATE_FID_JUNK:
			continue;
		}
		temp_len += gx_snprintf(temp_buff + temp_len,
		            GX_ARRAY_SIZE(temp_buff) - temp_len, "%s\r\n",
					sqlite3_column_text(pstmt, 1));
		count ++;
	}
	pstmt.finalize();
	pidb.reset();
	offset = gx_snprintf(temp_buff, 32, "TRUE %d\r\n", count);
	memmove(temp_buff + 32 - offset, temp_buff, offset);
	cmd_write(sockd, temp_buff + 32 - offset, offset + temp_len - 32);
	return 0;
}

static int mail_engine_mlist(int argc, char **argv, int sockd)
{
	BOOL b_asc;
	int offset;
	int length;
	int temp_len;
	int idx1, idx2;
	int total_mail;
	int sort_field;
	char sql_string[1024];
	char temp_buff[MAX_DIGLEN];
	
	if ((5 != argc && 7 != argc) || strlen(argv[1]) >= 256
		|| strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[3], "RCV")) {
		sort_field = FIELD_RECEIVED;
	} else if (0 == strcasecmp(argv[3], "SUB")) {
		sort_field = FIELD_SUBJECT;	
	} else if (0 == strcasecmp(argv[3], "FRM")) {
		sort_field = FIELD_FROM;
	} else if (0 == strcasecmp(argv[3], "RCP")) {
		sort_field = FIELD_RCPT;
	} else if (0 == strcasecmp(argv[3], "SIZ")) {
		sort_field = FIELD_SIZE;
	} else if (0 == strcasecmp(argv[3], "RED")) {
		sort_field = FIELD_READ;
	} else if (0 == strcasecmp(argv[3], "FLG")) {
		sort_field = FIELD_FLAG;
	} else if (0 == strcasecmp(argv[3], "UID")) {
		sort_field = FIELD_UID;
	} else if (0 == strcasecmp(argv[3], "NON")) {
		sort_field = FIELD_NONE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[4], "ASC")) {
		b_asc = TRUE;
	} else if (0 == strcasecmp(argv[4], "DSC")) {
		b_asc = FALSE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (7 == argc) {
		offset = strtol(argv[5], nullptr, 0);
		length = strtol(argv[6], nullptr, 0);
		if (length < 0) {
			length = 0;
		}
	} else {
		offset = 0;
		length = 0;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	if (!mail_engine_sort_folder(pidb.get(), argv[2], sort_field))
		return MIDB_E_NO_MEMORY;
	snprintf(sql_string, arsizeof(sql_string), "SELECT count(message_id) "
	          "FROM messages WHERE folder_id=%llu", LLU(folder_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		return MIDB_E_NO_FOLDER;
	}
	total_mail = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	if (TRUE == b_asc) {
		if (offset < 0) {
			idx1 = total_mail + 1 + offset;
			if (idx1 < 1) {
				idx1 = 1;
			}
		} else {
			if (offset >= total_mail) {
				pidb.reset();
				cmd_write(sockd, "TRUE 0\r\n", 8);
				return 0;
			}
			idx1 = offset + 1;
		}
		if (0 == length || total_mail - idx1 + 1 < length) {
			length = total_mail - idx1 + 1;
		}
		idx2 = idx1 + length - 1;
		snprintf(sql_string, arsizeof(sql_string), "SELECT mid_string FROM messages "
			"WHERE folder_id=%llu AND idx>=%d AND idx<=%d ORDER BY idx",
			LLU(folder_id), idx1, idx2);
	} else {
		if (offset < 0) {
			idx2 = -offset;
			if (idx2 > total_mail) {
				idx2 = total_mail;
			}
		} else {
			if (offset >= total_mail) {
				pidb.reset();
				cmd_write(sockd, "TRUE 0\r\n", 8);
				return 0;
			}
			idx2 = total_mail - offset;
		}
		if (0 == length || idx2 < length) {
			length = idx2;
		}
		idx1 = idx2 - length + 1;
		snprintf(sql_string, arsizeof(sql_string), "SELECT mid_string FROM messages "
			"WHERE folder_id=%llu AND idx>=%d AND idx<=%d ORDER BY idx "
			"DESC", LLU(folder_id), idx1, idx2);
	}
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	temp_len = sprintf(temp_buff, "TRUE %d\r\n", length);
	cmd_write(sockd, temp_buff, temp_len);
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		if (mail_engine_get_digest(pidb->psqlite,
		    S2A(sqlite3_column_text(pstmt, 0)),
		    temp_buff) == 0) {
			return MIDB_E_DIGEST;
		}
		temp_len = strlen(temp_buff);
		temp_buff[temp_len] = '\r';
		temp_len ++;
		temp_buff[temp_len] = '\n';
		temp_len ++;
		cmd_write(sockd, temp_buff, temp_len);
	}
	return 0;
}

static int mail_engine_muidl(int argc, char **argv, int sockd)
{
	int result;
	int offset;
	int temp_len;
	IDL_NODE *pinode;
	char temp_line[512];
	DOUBLE_LIST tmp_list;
	char sql_string[1024];
	DOUBLE_LIST_NODE *pnode;
	char list_buff[256*1024];
	
	if (3 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT mid_string, size FROM"
	          " messages WHERE folder_id=%llu ORDER BY uid", LLU(folder_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	double_list_init(&tmp_list);
	while (SQLITE_ROW == (result = sqlite3_step(pstmt))) {
		pinode = cu_alloc<IDL_NODE>();
		if (NULL == pinode) {
			return MIDB_E_NO_MEMORY;
		}
		pinode->node.pdata = pinode;
		pinode->mid_string = common_util_dup(S2A(sqlite3_column_text(pstmt, 0)));
		if (NULL == pinode->mid_string) {
			return MIDB_E_NO_MEMORY;
		}
		pinode->size = sqlite3_column_int64(pstmt, 1);
		double_list_append_as_tail(&tmp_list, &pinode->node);
	}
	pstmt.finalize();
	if (SQLITE_DONE != result) {
		return MIDB_E_NO_MEMORY;
	}
	offset = sprintf(list_buff, "TRUE %zu\r\n",
		double_list_get_nodes_num(&tmp_list));
	while ((pnode = double_list_pop_front(&tmp_list)) != nullptr) {
		pinode = (IDL_NODE*)pnode->pdata;
		temp_len = gx_snprintf(temp_line, GX_ARRAY_SIZE(temp_line), "%s %u\r\n",
						pinode->mid_string, pinode->size);
		if (256*1024 - offset < temp_len) {
			cmd_write(sockd, list_buff, offset);
			offset = 0;
		}
		memcpy(list_buff + offset, temp_line, temp_len);
		offset += temp_len;
	}
	cmd_write(sockd, list_buff, offset);
	return 0;
}

static int mail_engine_minst(int argc, char **argv, int sockd)
{
	int tmp_len;
	int user_id;
	char lang[32];
	uint32_t cpid;
	uint8_t b_read;
	size_t mess_len;
	char charset[32], tmzone[64];
	uint8_t b_unsent;
	uint32_t tmp_flags;
	char temp_path[256];
	uint64_t change_num;
	uint64_t message_id;
	char sql_string[1024];
	struct stat node_stat;
	char temp_buff[MAX_DIGLEN];
	
	if (6 != argc || strlen(argv[1]) >= 256
		|| strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (NULL != strchr(argv[4], 'U')) {
		b_unsent = 1;
	} else {
		b_unsent = 0;
	}
	if (NULL != strchr(argv[4], 'S')) {
		b_read = 1;
	} else {
		b_read = 0;
	}
	if (0 == strcmp(argv[2], "draft")) {
		b_unsent = 1;
	}
	sprintf(temp_path, "%s/eml/%s", argv[1], argv[3]);
	wrapfd fd = open(temp_path, O_RDONLY);
	if (fd.get() < 0)
		return MIDB_E_NO_MEMORY;
	if (fstat(fd.get(), &node_stat) != 0 || !S_ISREG(node_stat.st_mode))
		return MIDB_E_PARAMETER_ERROR;
	std::unique_ptr<char[], stdlib_delete> pbuff(me_alloc<char>(node_stat.st_size));
	if (NULL == pbuff) {
		return MIDB_E_NO_MEMORY;
	}
	if (read(fd.get(), pbuff.get(), node_stat.st_size) != node_stat.st_size)
		return MIDB_E_NO_MEMORY;
	fd.close();

	MAIL imail(g_mime_pool);
	if (!imail.retrieve(pbuff.get(), node_stat.st_size))
		return MIDB_E_NO_MEMORY;
	tmp_len = sprintf(temp_buff, "{\"file\":\"\",");
	if (imail.get_digest(&mess_len, temp_buff + tmp_len, MAX_DIGLEN - tmp_len - 1) <= 0)
		return MIDB_E_NO_MEMORY;
	tmp_len = strlen(temp_buff);
	temp_buff[tmp_len] = '}';
	tmp_len ++;
	temp_buff[tmp_len] = '\0';
	sprintf(temp_path, "%s/ext/%s", argv[1], argv[3]);
	fd = open(temp_path, O_CREAT|O_TRUNC|O_WRONLY, 0666);
	if (fd.get() < 0) {
		return MIDB_E_NO_MEMORY;
	}
	write(fd.get(), temp_buff, tmp_len);
	fd.close();
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr) {
		return MIDB_E_HASHTABLE_FULL;
	}
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		pidb.reset();
		return MIDB_E_NO_FOLDER;
	}
	if (!system_services_get_id_from_username(pidb->username.c_str(), &user_id)) {
		pidb.reset();
		return MIDB_E_NO_MEMORY;
	}
	if (!system_services_get_user_lang(pidb->username.c_str(), lang,
	    arsizeof(lang)) || lang[0] == '\0' ||
		FALSE == system_services_lang_to_charset(
		lang, charset) || '\0' == charset[0]) {
		strcpy(charset, g_default_charset);
	}
	if (!system_services_get_timezone(pidb->username.c_str(), tmzone,
	    arsizeof(tmzone)) || tmzone[0] == '\0')
		strcpy(tmzone, g_default_timezone);
	auto pmsgctnt = oxcmail_import(charset, tmzone, &imail,
	                common_util_alloc, common_util_get_propids_create);
	imail.clear();
	pbuff.reset();
	if (NULL == pmsgctnt) {
		return MIDB_E_NO_MEMORY;
	}
	auto cl_msg = make_scope_exit([&]() { message_content_free(pmsgctnt); });
	auto nt_time = rop_util_unix_to_nttime(strtol(argv[5], nullptr, 0));
	if (pmsgctnt->proplist.set(PROP_TAG_MESSAGEDELIVERYTIME, &nt_time) != 0) {
		return MIDB_E_NO_MEMORY;
	}
	if (b_read && pmsgctnt->proplist.set(PR_READ, &b_read) != 0) {
		return MIDB_E_NO_MEMORY;
	}
	if (0 != b_unsent) {
		tmp_flags = MSGFLAG_UNSENT;
		if (pmsgctnt->proplist.set(PR_MESSAGE_FLAGS, &tmp_flags) != 0) {
			return MIDB_E_NO_MEMORY;
		}
	}
	if (!exmdb_client::allocate_message_id(argv[1],
		rop_util_make_eid_ex(1, folder_id), &message_id) ||
		!exmdb_client::allocate_cn(argv[1], &change_num)) {
		return MIDB_E_NO_MEMORY;
	}
	snprintf(sql_string, arsizeof(sql_string), "INSERT INTO mapping"
		" (message_id, mid_string, flag_string) VALUES"
		" (%llu, ?, ?)", LLU(rop_util_get_gc_value(message_id)));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_bind_text(pstmt, 1, argv[3], -1, SQLITE_STATIC);
	sqlite3_bind_text(pstmt, 2, argv[4], -1, SQLITE_STATIC);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		return MIDB_E_NO_MEMORY;
	}
	pstmt.finalize();
	std::string username;
	try {
		username = pidb->username;
	} catch (const std::bad_alloc &) {
		return MIDB_E_NO_MEMORY;
	}
	pidb.reset();
	if (pmsgctnt->proplist.set(PidTagMid, &message_id) != 0 ||
	    pmsgctnt->proplist.set(PidTagChangeNumber, &change_num) != 0) {
		return MIDB_E_NO_MEMORY;
	}
	auto pbin = cu_xid_to_bin({rop_util_make_user_guid(user_id), change_num});
	if (pbin == nullptr ||
	    pmsgctnt->proplist.set(PR_CHANGE_KEY, pbin) != 0) {
		return MIDB_E_NO_MEMORY;
	}
	auto newval = common_util_pcl_append(NULL, pbin);
	if (newval == nullptr ||
	    pmsgctnt->proplist.set(PR_PREDECESSOR_CHANGE_LIST, newval) != 0) {
		return MIDB_E_NO_MEMORY;
	}
	cpid = system_services_charset_to_cpid(charset);
	if (0 == cpid) {
		cpid = 1252;
	}
	gxerr_t e_result = GXERR_CALL_FAILED;
	if (!exmdb_client::write_message(argv[1], username.c_str(), cpid,
	    rop_util_make_eid_ex(1, folder_id), pmsgctnt, &e_result) ||
	    e_result != GXERR_SUCCESS) {
		return MIDB_E_NO_MEMORY;
	}
	cmd_write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int mail_engine_mdele(int argc, char **argv, int sockd)
{
	int i;
	int user_id;
	BOOL b_partial;
	EID_ARRAY message_ids;

	if (argc < 4 || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	message_ids.count = 0;
	message_ids.pids = cu_alloc<uint64_t>(argc - 3);
	if (NULL == message_ids.pids) {
		return MIDB_E_NO_MEMORY;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	if (!system_services_get_id_from_username(pidb->username.c_str(), &user_id))
		return MIDB_E_NO_MEMORY;
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT message_id,"
	             " folder_id FROM messages WHERE mid_string=?");
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	for (i=3; i<argc; i++) {
		sqlite3_reset(pstmt);
		sqlite3_bind_text(pstmt, 1, argv[i], -1, SQLITE_STATIC);
		if (SQLITE_ROW != sqlite3_step(pstmt) ||
		    gx_sql_col_uint64(pstmt, 1) != folder_id)
			continue;
		message_ids.pids[message_ids.count] = rop_util_make_eid_ex(
								1, sqlite3_column_int64(pstmt, 0));
		message_ids.count ++;
	}
	pstmt.finalize();
	pidb.reset();
	if (!exmdb_client::delete_messages(argv[1],
		user_id, 0, NULL, rop_util_make_eid_ex(1, folder_id),
		&message_ids, TRUE, &b_partial)) {
		return MIDB_E_NO_MEMORY;
	}
	cmd_write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int mail_engine_mcopy(int argc, char **argv, int sockd)
{
	int user_id;
	char lang[32];
	uint32_t cpid;
	int flags_len;
	uint8_t b_read;
	uint64_t nt_time;
	char charset[32], tmzone[64];
	uint8_t b_unsent;
	uint32_t tmp_flags;
	char flags_buff[16];
	uint64_t change_num;
	uint64_t message_id;
	char sql_string[1024];
	struct stat node_stat;

	if (5 != argc || strlen(argv[1]) >= 256 ||
		strlen(argv[2]) >= 1024 || strlen(argv[4]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	std::string eml_path;
	try {
		eml_path = argv[1] + "/eml/"s + argv[3];
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "E-1486: ENOMEM\n");
		return MIDB_E_NO_MEMORY;
	}
	wrapfd fd = open(eml_path.c_str(), O_RDONLY);
	if (fd.get() < 0)
		return MIDB_E_NO_MEMORY;
	if (fstat(fd.get(), &node_stat) != 0 || !S_ISREG(node_stat.st_mode))
		return MIDB_E_PARAMETER_ERROR;
	std::unique_ptr<char[], stdlib_delete> pbuff(me_alloc<char>(node_stat.st_size));
	if (NULL == pbuff) {
		return MIDB_E_NO_MEMORY;
	}
	if (read(fd.get(), pbuff.get(), node_stat.st_size) != node_stat.st_size)
		return MIDB_E_NO_MEMORY;
	fd.close();

	MAIL imail(g_mime_pool);
	if (!imail.retrieve(pbuff.get(), node_stat.st_size))
		return MIDB_E_NO_MEMORY;
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr) {
		return MIDB_E_HASHTABLE_FULL;
	}
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	auto folder_id1 = mail_engine_get_folder_id(pidb.get(), argv[4]);
	if (0 == folder_id1) {
		return MIDB_E_NO_FOLDER;
	}
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT message_id, mod_time, "
	             "uid, recent, read, unsent, flagged, replied, forwarded,"
	             "deleted, received, ext, folder_id, size FROM messages "
	             "WHERE mid_string=?");
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_bind_text(pstmt, 1, argv[3], -1, SQLITE_STATIC);
	if (SQLITE_ROW != sqlite3_step(pstmt) ||
	    gx_sql_col_uint64(pstmt, 12) != folder_id) {
		return MIDB_E_NO_MESSAGE;
	}
	b_read = 0;
	b_unsent = 0;
	flags_buff[0] = '(';
	flags_len = 1;
	if (0 != sqlite3_column_int64(pstmt, 7)) {
		flags_buff[flags_len] = 'A';
		flags_len ++;
	}
	if (0 != sqlite3_column_int64(pstmt, 6)) {
		flags_buff[flags_len] = 'F';
		flags_len ++;
	}
	if (0 != sqlite3_column_int64(pstmt, 8)) {
		flags_buff[flags_len] = 'W';
		flags_len ++;
	}
	flags_buff[flags_len] = ')';
	flags_len ++;
	flags_buff[flags_len] = '\0';
	if (0 != sqlite3_column_int64(pstmt, 5)) {
		b_unsent = 1;
	}
	if (0 != sqlite3_column_int64(pstmt, 4)) {
		b_read = 1;
	}
	nt_time = sqlite3_column_int64(pstmt, 10);
	pstmt.finalize();
	if (!system_services_get_id_from_username(pidb->username.c_str(), &user_id)) {
		return MIDB_E_NO_MEMORY;
	}
	if (!system_services_get_user_lang(pidb->username.c_str(), lang,
	    arsizeof(lang)) || lang[0] == '\0' ||
		FALSE == system_services_lang_to_charset(
		lang, charset) || '\0' == charset[0]) {
		strcpy(charset, g_default_charset);
	}
	if (!system_services_get_timezone(pidb->username.c_str(), tmzone,
	    arsizeof(tmzone)) || tmzone[0] == '\0')
		strcpy(tmzone, g_default_timezone);
	auto pmsgctnt = oxcmail_import(charset, tmzone, &imail,
	                common_util_alloc, common_util_get_propids_create);
	imail.clear();
	pbuff.reset();
	if (NULL == pmsgctnt) {
		return MIDB_E_NO_MEMORY;
	}
	auto cl_msg = make_scope_exit([&]() { message_content_free(pmsgctnt); });
	if (pmsgctnt->proplist.set(PROP_TAG_MESSAGEDELIVERYTIME, &nt_time) != 0)
		return MIDB_E_NO_MEMORY;
	if (b_read && pmsgctnt->proplist.set(PR_READ, &b_read) != 0)
		return MIDB_E_NO_MEMORY;
	if (0 != b_unsent) {
		tmp_flags = MSGFLAG_UNSENT;
		if (pmsgctnt->proplist.set(PR_MESSAGE_FLAGS, &tmp_flags) != 0)
			return MIDB_E_NO_MEMORY;
	}
	if (!exmdb_client::allocate_message_id(argv[1],
		rop_util_make_eid_ex(1, folder_id), &message_id) ||
		!exmdb_client::allocate_cn(argv[1], &change_num)) {
		return MIDB_E_NO_MEMORY;
	}
	std::string mid_string;
	try {
		mid_string = std::to_string(time(nullptr)) + "." +
		             std::to_string(mail_engine_get_sequence_id()) + ".midb";
		eml_path = argv[1] + "/eml/"s + argv[3];
		auto eml_path1 = argv[1] + "/eml/"s + mid_string;
		link(eml_path.c_str(), eml_path1.c_str());
		eml_path = argv[1] + "/ext/"s + argv[3];
		eml_path1 = argv[1] + "/ext/"s + mid_string;
		link(eml_path.c_str(), eml_path1.c_str());
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "E-1487: ENOMEM\n");
		return MIDB_E_NO_MEMORY;
	}
	snprintf(sql_string, arsizeof(sql_string), "INSERT INTO mapping"
		" (message_id, mid_string, flag_string) VALUES"
		" (%llu, ?, ?)", LLU(rop_util_get_gc_value(message_id)));
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_bind_text(pstmt, 1, mid_string.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(pstmt, 2, flags_buff, -1, SQLITE_STATIC);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		return MIDB_E_NO_MEMORY;
	}
	pstmt.finalize();
	std::string username;
	try {
		username = pidb->username;
	} catch (const std::bad_alloc &) {
		return MIDB_E_NO_MEMORY;
	}
	pidb.reset();
	if (pmsgctnt->proplist.set(PidTagMid, &message_id) != 0 ||
	    pmsgctnt->proplist.set(PidTagChangeNumber, &change_num) != 0) {
		return MIDB_E_NO_MEMORY;
	}
	auto pbin = cu_xid_to_bin({rop_util_make_user_guid(user_id), change_num});
	if (pbin == nullptr ||
	    pmsgctnt->proplist.set(PR_CHANGE_KEY, pbin) != 0) {
		return MIDB_E_NO_MEMORY;
	}
	auto newval = common_util_pcl_append(NULL, pbin);
	if (newval == nullptr ||
	    pmsgctnt->proplist.set(PR_PREDECESSOR_CHANGE_LIST, newval) != 0) {
		return MIDB_E_NO_MEMORY;
	}
	cpid = system_services_charset_to_cpid(charset);
	if (0 == cpid) {
		cpid = 1252;
	}
	gxerr_t e_result = GXERR_CALL_FAILED;
	if (!exmdb_client::write_message(argv[1], username.c_str(), cpid,
	    rop_util_make_eid_ex(1, folder_id1), pmsgctnt, &e_result) ||
	    e_result != GXERR_SUCCESS) {
		return MIDB_E_NO_MEMORY;
	}
	cl_msg.release();
	message_content_free(pmsgctnt);
	try {
		mid_string.insert(0, "TRUE ");
		mid_string.append("\r\n");
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "E-1488: ENOMEM\n");
		return MIDB_E_NO_MEMORY;
	}
	cmd_write(sockd, mid_string.c_str(), mid_string.size());
	return 0;
}

static int mail_engine_mrenf(int argc, char **argv, int sockd)
{
	int user_id;
	BOOL b_exist;
	char *ptoken;
	BINARY *pbin1;
	char *ptoken1;
	BOOL b_partial;
	uint64_t nt_time;
	uint64_t parent_id;
	uint64_t folder_id;
	uint64_t folder_id1;
	uint64_t folder_id2;
	uint64_t change_num;
	char temp_name[256];
	uint32_t tmp_proptag;
	PROPTAG_ARRAY proptags;
	char decoded_name[512];
	PROBLEM_ARRAY problems;
	char encoded_name[1024];
	TPROPVAL_ARRAY propvals;
	TAGGED_PROPVAL propval_buff[5];

	if (4 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024
		|| strlen(argv[3]) >= 1024 || 0 == strcmp(argv[2], argv[3])) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcmp(argv[2], "inbox") ||
		0 == strcmp(argv[2], "draft") ||
		0 == strcmp(argv[2], "sent") ||
		0 == strcmp(argv[2], "trash") ||
		0 == strcmp(argv[2], "junk")) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (FALSE == decode_hex_binary(argv[3], decoded_name, 512)) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	if (!system_services_get_id_from_username(pidb->username.c_str(), &user_id))
		return MIDB_E_NO_MEMORY;
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT folder_id,"
	             " parent_fid FROM folders WHERE name=?");
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_bind_text(pstmt, 1, argv[2], -1, SQLITE_STATIC);
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		return MIDB_E_NO_MEMORY;
	}
	folder_id = sqlite3_column_int64(pstmt, 0);
	parent_id = sqlite3_column_int64(pstmt, 1);
	pstmt.finalize();
	if (0 != mail_engine_get_folder_id(pidb.get(), argv[3])) {
		return MIDB_E_FOLDER_EXISTS;
	}
	ptoken = decoded_name;
	folder_id1 = PRIVATE_FID_IPMSUBTREE;
	while ((ptoken1 = strchr(ptoken, '/')) != NULL) {
		if (static_cast<size_t>(ptoken1 - ptoken) >= sizeof(temp_name)) {
			return MIDB_E_PARAMETER_ERROR;
		}
		memcpy(temp_name, ptoken, ptoken1 - ptoken);
		temp_name[ptoken1 - ptoken] = '\0';
		if (0 == strcmp(temp_name, "inbox")) {
			folder_id1 = PRIVATE_FID_INBOX;
		} else if (0 == strcmp(temp_name, "draft")) {
			folder_id1 = PRIVATE_FID_DRAFT;
		} else if (0 == strcmp(temp_name, "sent")) {
			folder_id1 = PRIVATE_FID_SENT_ITEMS;
		} else if (0 == strcmp(temp_name, "trash")) {
			folder_id1 = PRIVATE_FID_DELETED_ITEMS;
		} else if (0 == strcmp(temp_name, "junk")) {
			folder_id1 = PRIVATE_FID_JUNK;
		} else {
			encode_hex_binary(decoded_name, ptoken1 - decoded_name,
				encoded_name, 1024);
			folder_id2 = mail_engine_get_folder_id(pidb.get(), encoded_name);
			if (0 == folder_id2) {
				if (FALSE == common_util_create_folder(argv[1],
					user_id, rop_util_make_eid_ex(1, folder_id1),
					temp_name, &folder_id2)) {
					return MIDB_E_NO_MEMORY;
				}
				folder_id1 = rop_util_get_gc_value(folder_id2);
			} else {
				folder_id1 = folder_id2;
			}
		}
		ptoken = ptoken1 + 1;
	}
	pidb.reset();
	if (parent_id != folder_id1) {
		if (!exmdb_client::movecopy_folder(
			argv[1], user_id, 0, FALSE, NULL,
			rop_util_make_eid_ex(1, parent_id),
			rop_util_make_eid_ex(1, folder_id),
			rop_util_make_eid_ex(1, folder_id1),
			ptoken, FALSE, &b_exist, &b_partial)) {
			return MIDB_E_NO_MEMORY;
		}
		if (TRUE == b_exist) {
			return MIDB_E_FOLDER_EXISTS;
		}
		if (TRUE == b_partial) {
			return MIDB_E_NO_MEMORY;
		}
	}
	proptags.count = 1;
	proptags.pproptag = &tmp_proptag;
	tmp_proptag = PR_PREDECESSOR_CHANGE_LIST;
	if (!exmdb_client::allocate_cn(argv[1], &change_num)
		|| !exmdb_client::get_folder_properties(argv[1],
		0, rop_util_make_eid_ex(1, folder_id), &proptags, &propvals)
		||
	     (pbin1 = propvals.get<BINARY>(PR_PREDECESSOR_CHANGE_LIST)) == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	if (parent_id == folder_id1) {
		propvals.count = 5;
	} else {
		propvals.count = 4;
	}
	propvals.ppropval = propval_buff;
	propval_buff[0].proptag = PidTagChangeNumber;
	propval_buff[0].pvalue = &change_num;
	auto pbin = cu_xid_to_bin({rop_util_make_user_guid(user_id), change_num});
	if (NULL == pbin) {
		return MIDB_E_NO_MEMORY;
	}
	propval_buff[1].proptag = PR_CHANGE_KEY;
	propval_buff[1].pvalue = pbin;
	propval_buff[2].proptag = PR_PREDECESSOR_CHANGE_LIST;
	propval_buff[2].pvalue = common_util_pcl_append(pbin1, pbin);
	if (NULL == propval_buff[2].pvalue) {
		return MIDB_E_NO_MEMORY;
	}
	nt_time = rop_util_current_nttime();
	propval_buff[3].proptag = PR_LAST_MODIFICATION_TIME;
	propval_buff[3].pvalue = &nt_time;
	if (parent_id == folder_id1) {
		propval_buff[4].proptag = PR_DISPLAY_NAME;
		propval_buff[4].pvalue = ptoken;
	}
	if (!exmdb_client::set_folder_properties(
		argv[1], 0, rop_util_make_eid_ex(1, folder_id),
		&propvals, &problems)) {
		return MIDB_E_NO_MEMORY;
	}
	cmd_write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int mail_engine_mmakf(int argc, char **argv, int sockd)
{
	int user_id;
	char *ptoken;
	char *ptoken1;
	uint64_t folder_id1;
	uint64_t folder_id2;
	char temp_name[256];
	char decoded_name[512];
	char encoded_name[1024];

	if (3 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (FALSE == decode_hex_binary(argv[2], decoded_name, 512)) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	if (!system_services_get_id_from_username(pidb->username.c_str(), &user_id))
		return MIDB_E_NO_MEMORY;
	if (0 != mail_engine_get_folder_id(pidb.get(), argv[2])) {
		return MIDB_E_FOLDER_EXISTS;
	}
	ptoken = decoded_name;
	folder_id1 = PRIVATE_FID_IPMSUBTREE;
	while ((ptoken1 = strchr(ptoken, '/')) != NULL) {
		if (static_cast<size_t>(ptoken1 - ptoken) >= sizeof(temp_name)) {
			return MIDB_E_PARAMETER_ERROR;
		}
		memcpy(temp_name, ptoken, ptoken1 - ptoken);
		temp_name[ptoken1 - ptoken] = '\0';
		if (0 == strcmp(temp_name, "inbox")) {
			folder_id1 = PRIVATE_FID_INBOX;
		} else if (0 == strcmp(temp_name, "draft")) {
			folder_id1 = PRIVATE_FID_DRAFT;
		} else if (0 == strcmp(temp_name, "sent")) {
			folder_id1 = PRIVATE_FID_SENT_ITEMS;
		} else if (0 == strcmp(temp_name, "trash")) {
			folder_id1 = PRIVATE_FID_DELETED_ITEMS;
		} else if (0 == strcmp(temp_name, "junk")) {
			folder_id1 = PRIVATE_FID_JUNK;
		} else {
			encode_hex_binary(decoded_name, ptoken1 - decoded_name,
				encoded_name, 1024);
			folder_id2 = mail_engine_get_folder_id(pidb.get(), encoded_name);
			if (0 == folder_id2) {
				if (FALSE == common_util_create_folder(argv[1],
					user_id, rop_util_make_eid_ex(1, folder_id1),
					temp_name, &folder_id2)) {
					return MIDB_E_NO_MEMORY;
				}
				folder_id1 = rop_util_get_gc_value(folder_id2);
			} else {
				folder_id1 = folder_id2;
			}
		}
		ptoken = ptoken1 + 1;
	}
	pidb.reset();
	if (FALSE == common_util_create_folder(argv[1],
		user_id, rop_util_make_eid_ex(1, folder_id1),
		ptoken, &folder_id2) || 0 == folder_id2) {
		return MIDB_E_NO_MEMORY;
	}
	cmd_write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int mail_engine_mremf(int argc, char **argv, int sockd)
{
	BOOL b_result;
	BOOL b_partial;
	
	if (3 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcmp(argv[2], "inbox") ||
		0 == strcmp(argv[2], "draft") ||
		0 == strcmp(argv[2], "sent") ||
		0 == strcmp(argv[2], "trash") ||
		0 == strcmp(argv[2], "junk")) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		pidb.reset();
		cmd_write(sockd, "TRUE\r\n", 6);
		return 0;
	}
	pidb.reset();
	folder_id = rop_util_make_eid_ex(1, folder_id);
	if (!exmdb_client::empty_folder(argv[1], 0, NULL, folder_id,
		TRUE, TRUE, TRUE, FALSE, &b_partial) || TRUE == b_partial ||
		!exmdb_client::empty_folder(argv[1], 0, NULL, folder_id,
		TRUE, FALSE, FALSE, TRUE, &b_partial) || TRUE == b_partial ||
		!exmdb_client::delete_folder(argv[1], 0, folder_id, TRUE,
		&b_result) || FALSE == b_result) {
		return MIDB_E_NO_MEMORY;
	}
	cmd_write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int mail_engine_pofst(int argc, char **argv, int sockd)
{
	int idx;
	BOOL b_asc;
	int temp_len;
	int sort_field, total_mail = 0;
	char temp_buff[1024];
	char sql_string[1024];
	
	if (6 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[4], "RCV")) {
		sort_field = FIELD_RECEIVED;
	} else if (0 == strcasecmp(argv[4], "SUB")) {
		sort_field = FIELD_SUBJECT;	
	} else if (0 == strcasecmp(argv[4], "FRM")) {
		sort_field = FIELD_FROM;
	} else if (0 == strcasecmp(argv[4], "RCP")) {
		sort_field = FIELD_RCPT;
	} else if (0 == strcasecmp(argv[4], "SIZ")) {
		sort_field = FIELD_SIZE;
	} else if (0 == strcasecmp(argv[4], "RED")) {
		sort_field = FIELD_READ;
	} else if (0 == strcasecmp(argv[4], "FLG")) {
		sort_field = FIELD_FLAG;
	} else if (0 == strcasecmp(argv[4], "UID")) {
		sort_field = FIELD_UID;
	} else if (0 == strcasecmp(argv[4], "NON")) {
		sort_field = FIELD_NONE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[5], "ASC")) {
		b_asc = TRUE;
	} else if (0 == strcasecmp(argv[5], "DSC")) {
		b_asc = FALSE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	if (!mail_engine_sort_folder(pidb.get(), argv[2], sort_field))
		return MIDB_E_NO_MEMORY;
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT folder_id,"
	             " idx FROM messages WHERE mid_string=?");
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_bind_text(pstmt, 1, argv[3], -1, SQLITE_STATIC);
	if (SQLITE_ROW != sqlite3_step(pstmt) ||
	    gx_sql_col_uint64(pstmt, 0) != folder_id) {
		return MIDB_E_NO_MESSAGE;
	}
	idx = sqlite3_column_int64(pstmt, 1);
	pstmt.finalize();
	if (FALSE == b_asc) {
		snprintf(sql_string, arsizeof(sql_string), "SELECT count(message_id) "
		          "FROM messages WHERE folder_id=%llu", LLU(folder_id));
		pstmt = gx_sql_prep(pidb->psqlite, sql_string);
		if (pstmt == nullptr) {
			return MIDB_E_NO_MEMORY;
		}
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			return MIDB_E_NO_FOLDER;
		}
		total_mail = sqlite3_column_int64(pstmt, 0);
		pstmt.finalize();
	}
	pidb.reset();
	if (TRUE == b_asc) {
		temp_len = sprintf(temp_buff, "TRUE %d\r\n", idx - 1);
	} else {
		temp_len = sprintf(temp_buff, "TRUE %d\r\n", total_mail - idx);
	}
	cmd_write(sockd, temp_buff, temp_len);
	return 0;
}

static int mail_engine_punid(int argc, char **argv, int sockd)
{
	int temp_len;
	uint32_t uid;
	char temp_buff[1024];

	if (4 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT folder_id,"
	             " uid FROM messages WHERE mid_string=?");
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_bind_text(pstmt, 1, argv[3], -1, SQLITE_STATIC);
	if (SQLITE_ROW != sqlite3_step(pstmt) ||
	    gx_sql_col_uint64(pstmt, 0) != folder_id) {
		return MIDB_E_NO_MESSAGE;
	}
	uid = sqlite3_column_int64(pstmt, 1);
	pstmt.finalize();
	pidb.reset();
	temp_len = sprintf(temp_buff, "TRUE %u\r\n", uid);
	cmd_write(sockd, temp_buff, temp_len);
	return 0;
}

static int mail_engine_pfddt(int argc, char **argv, int sockd)
{
	BOOL b_asc;
	int offset;
	int temp_len;
	uint32_t total;
	uint32_t unreads;
	uint32_t recents;
	uint32_t uidnext;
	uint64_t uidvalid;
	uint64_t folder_id;
	char temp_buff[1024];
	char sql_string[1024];
	
	if (5 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[3], "RCV") ||
		0 == strcasecmp(argv[3], "SUB") ||
		0 == strcasecmp(argv[3], "FRM") ||
		0 == strcasecmp(argv[3], "RCP") ||
		0 == strcasecmp(argv[3], "SIZ") ||
		0 == strcasecmp(argv[3], "RED") ||
		0 == strcasecmp(argv[3], "FLG") ||
		0 == strcasecmp(argv[3], "UID") ||
		0 == strcasecmp(argv[3], "NON")) {
		/* do nothing */
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[4], "ASC")) {
		b_asc = TRUE;
	} else if (0 == strcasecmp(argv[4], "DSC")) {
		b_asc = FALSE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT folder_id,"
	             " uidnext FROM folders WHERE name=?");
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_bind_text(pstmt, 1, argv[2], -1, SQLITE_STATIC);
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		return MIDB_E_NO_FOLDER;
	}
	folder_id = sqlite3_column_int64(pstmt, 0);
	uidnext = sqlite3_column_int64(pstmt, 1);
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "SELECT count(message_id) "
	          "FROM messages WHERE folder_id=%llu", LLU(folder_id));
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		return MIDB_E_NO_FOLDER;
	}
	total = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "SELECT count(message_id) FROM "
	          "messages WHERE folder_id=%llu AND read=0", LLU(folder_id));
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	if (SQLITE_ROW == sqlite3_step(pstmt)) {
		unreads = sqlite3_column_int64(pstmt, 0);
	} else {
		unreads = 0;
	}
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "SELECT count(message_id) FROM"
	          " messages WHERE folder_id=%llu AND recent=0", LLU(folder_id));
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	if (SQLITE_ROW == sqlite3_step(pstmt)) {
		recents = sqlite3_column_int64(pstmt, 0);
	} else {
		recents = 0;
	}
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "SELECT min(idx) FROM messages "
	          "WHERE folder_id=%llu AND read=0", LLU(folder_id));
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	if (SQLITE_ROW == sqlite3_step(pstmt)) {
		offset = sqlite3_column_int64(pstmt, 0);
		if (FALSE == b_asc) {
			offset = total - offset;
		} else {
			offset --;
		}
	} else {
		offset = -1;
	}
	pstmt.finalize();
	pidb.reset();
	uidvalid = folder_id;
	temp_len = sprintf(temp_buff, "TRUE %u %u %u %llu %u %d\r\n",
	           total, recents, unreads, LLU(uidvalid), uidnext + 1, offset);
	cmd_write(sockd, temp_buff, temp_len);
	return 0;
}

static int mail_engine_psubf(int argc, char **argv, int sockd)
{
	char sql_string[1024];

	if (3 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET unsub=0"
	        " WHERE folder_id=%llu", LLU(folder_id));
	gx_sql_exec(pidb->psqlite, sql_string);
	pidb.reset();
	cmd_write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int mail_engine_punsf(int argc, char **argv, int sockd)
{
	char sql_string[1024];

	if (3 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET unsub=1"
	        " WHERE folder_id=%llu", LLU(folder_id));
	gx_sql_exec(pidb->psqlite, sql_string);
	pidb.reset();
	cmd_write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int mail_engine_psubl(int argc, char **argv, int sockd)
{
	int count;
	int offset;
	int temp_len;
	char sql_string[1024];
	char temp_buff[256*1024];
	
	if (2 != argc || strlen(argv[1]) >= 256) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	snprintf(sql_string, arsizeof(sql_string), "SELECT name FROM folders WHERE unsub=0");
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_HASHTABLE_FULL;
	}
	count = 0;
	temp_len = 32;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		temp_len += gx_snprintf(temp_buff + temp_len,
		            GX_ARRAY_SIZE(temp_buff) - temp_len,  "%s\r\n",
					sqlite3_column_text(pstmt, 0));
		count ++;
	}
	pstmt.finalize();
	pidb.reset();
	offset = gx_snprintf(temp_buff, 32, "TRUE %d\r\n", count);
	memmove(temp_buff + 32 - offset, temp_buff, offset);
	cmd_write(sockd, temp_buff + 32 - offset, offset + temp_len - 32);
	return 0;
}

static int mail_engine_psiml(int argc, char **argv, int sockd)
{
	BOOL b_asc;
	int offset;
	int length;
	int temp_len;
	int buff_len;
	uint32_t uid;
	int flags_len;
	int idx1, idx2;
	int total_mail;
	int sort_field;
	char flags_buff[16];
	char temp_line[1024];
	char sql_string[1024];
	const char *mid_string;
	char temp_buff[256*1024];
	
	if ((5 != argc && 7 != argc) || strlen(argv[1]) >= 256
		|| strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[3], "RCV")) {
		sort_field = FIELD_RECEIVED;
	} else if (0 == strcasecmp(argv[3], "SUB")) {
		sort_field = FIELD_SUBJECT;	
	} else if (0 == strcasecmp(argv[3], "FRM")) {
		sort_field = FIELD_FROM;
	} else if (0 == strcasecmp(argv[3], "RCP")) {
		sort_field = FIELD_RCPT;
	} else if (0 == strcasecmp(argv[3], "SIZ")) {
		sort_field = FIELD_SIZE;
	} else if (0 == strcasecmp(argv[3], "RED")) {
		sort_field = FIELD_READ;
	} else if (0 == strcasecmp(argv[3], "FLG")) {
		sort_field = FIELD_FLAG;
	} else if (0 == strcasecmp(argv[3], "UID")) {
		sort_field = FIELD_UID;
	} else if (0 == strcasecmp(argv[3], "NON")) {
		sort_field = FIELD_NONE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[4], "ASC")) {
		b_asc = TRUE;
	} else if (0 == strcasecmp(argv[4], "DSC")) {
		b_asc = FALSE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (7 == argc) {
		offset = strtol(argv[5], nullptr, 0);
		length = strtol(argv[6], nullptr, 0);
		if (length < 0) {
			length = 0;
		}
	} else {
		offset = 0;
		length = 0;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	if (!mail_engine_sort_folder(pidb.get(), argv[2], sort_field))
		return MIDB_E_NO_MEMORY;
	snprintf(sql_string, arsizeof(sql_string), "SELECT count(message_id) "
	          "FROM messages WHERE folder_id=%llu", LLU(folder_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		return MIDB_E_NO_FOLDER;
	}
	total_mail = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	if (TRUE == b_asc) {
		if (offset < 0) {
			idx1 = total_mail + 1 + offset;
			if (idx1 < 1) {
				idx1 = 1;
			}
		} else {
			if (offset >= total_mail) {
				pidb.reset();
				cmd_write(sockd, "TRUE 0\r\n", 8);
				return 0;
			}
			idx1 = offset + 1;
		}
		if (0 == length || total_mail - idx1 + 1 < length) {
			length = total_mail - idx1 + 1;
		}
		idx2 = idx1 + length - 1;
		snprintf(sql_string, arsizeof(sql_string), "SELECT mid_string, uid, replied, "
				"unsent, flagged, deleted, read, recent, forwarded FROM "
				"messages WHERE folder_id=%llu AND idx>=%d AND idx<=%d "
				"ORDER BY idx", LLU(folder_id), idx1, idx2);
	} else {
		if (offset < 0) {
			idx2 = offset*(-1);
			if (idx2 > total_mail) {
				idx2 = total_mail;
			}
		} else {
			if (offset >= total_mail) {
				pidb.reset();
				cmd_write(sockd, "TRUE 0\r\n", 8);
				return 0;
			}
			idx2 = total_mail - offset;
		}
		if (0 == length || idx2 < length) {
			length = idx2;
		}
		idx1 = idx2 - length + 1;
		snprintf(sql_string, arsizeof(sql_string), "SELECT mid_string, uid, replied, "
				"unsent, flagged, deleted, read, recent, forwarded FROM "
				"messages WHERE folder_id=%llu AND idx>=%d AND idx<=%d "
				"ORDER BY idx DESC", LLU(folder_id), idx1, idx2);
	}
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	temp_len = sprintf(temp_buff, "TRUE %d\r\n", length);
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		mid_string = S2A(sqlite3_column_text(pstmt, 0));
		uid = sqlite3_column_int64(pstmt, 1);
		flags_buff[0] = '(';
		flags_len = 1;
		if (0 != sqlite3_column_int64(pstmt, 2)) {
			flags_buff[flags_len] = 'A';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 3)) {
			flags_buff[flags_len] = 'U';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 4)) {
			flags_buff[flags_len] = 'F';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 5)) {
			flags_buff[flags_len] = 'D';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 6)) {
			flags_buff[flags_len] = 'S';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 7)) {
			flags_buff[flags_len] = 'R';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 8)) {
			flags_buff[flags_len] = 'W';
			flags_len ++;
		}
		flags_buff[flags_len] = ')';
		flags_len ++;
		flags_buff[flags_len] = '\0';
		buff_len = gx_snprintf(temp_line, GX_ARRAY_SIZE(temp_line),
			"%s %u %s\r\n", mid_string, uid,
			flags_buff);
		if (256*1024 - temp_len < buff_len) {
			cmd_write(sockd, temp_buff, temp_len);
			temp_len = 0;
		}
		memcpy(temp_buff + temp_len, temp_line, buff_len);
		temp_len += buff_len;
	}
	pstmt.finalize();
	pidb.reset();
	cmd_write(sockd, temp_buff, temp_len);
	return 0;
}

static int mail_engine_psimu(int argc, char **argv, int sockd)
{
	BOOL b_asc;
	int buff_len;
	int temp_len;
	int flags_len, total_mail = 0;
	int sort_field;
	char flags_buff[16];
	char temp_line[1024];
	char sql_string[1024];
	DOUBLE_LIST temp_list;
	DOUBLE_LIST_NODE *pnode;
	char temp_buff[256*1024];
	
	if (7 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[3], "RCV")) {
		sort_field = FIELD_RECEIVED;
	} else if (0 == strcasecmp(argv[3], "SUB")) {
		sort_field = FIELD_SUBJECT;	
	} else if (0 == strcasecmp(argv[3], "FRM")) {
		sort_field = FIELD_FROM;
	} else if (0 == strcasecmp(argv[3], "RCP")) {
		sort_field = FIELD_RCPT;
	} else if (0 == strcasecmp(argv[3], "SIZ")) {
		sort_field = FIELD_SIZE;
	} else if (0 == strcasecmp(argv[3], "RED")) {
		sort_field = FIELD_READ;
	} else if (0 == strcasecmp(argv[3], "FLG")) {
		sort_field = FIELD_FLAG;
	} else if (0 == strcasecmp(argv[3], "UID")) {
		sort_field = FIELD_UID;
	} else if (0 == strcasecmp(argv[3], "NON")) {
		sort_field = FIELD_NONE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[4], "ASC")) {
		b_asc = TRUE;
	} else if (0 == strcasecmp(argv[4], "DSC")) {
		b_asc = FALSE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	int first = strtol(argv[5], nullptr, 0), last = strtol(argv[6], nullptr, 0);
	if (first < 1 && first != -1)
		return MIDB_E_PARAMETER_ERROR;
	if (last < 1 && last != -1)
		return MIDB_E_PARAMETER_ERROR;
	if (first == -1 && last != -1)
		return MIDB_E_PARAMETER_ERROR;
	if (last != -1 && last < first)
		return MIDB_E_PARAMETER_ERROR;
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	if (!mail_engine_sort_folder(pidb.get(), argv[2], sort_field))
		return MIDB_E_NO_MEMORY;
	if (TRUE == b_asc) {
		if (-1 == first && -1 == last) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid, "
				"replied, unsent, flagged, deleted, read, recent, forwarded "
				"FROM messages WHERE folder_id=%llu ORDER BY idx", LLU(folder_id));
		} else if (-1 == first) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid, "
				"replied, unsent, flagged, deleted, read, recent, forwarded "
				"FROM messages WHERE folder_id=%llu AND uid<=%u ORDER BY idx",
				LLU(folder_id), last);
		} else if (-1 == last) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid, "
				"replied, unsent, flagged, deleted, read, recent, forwarded "
				"FROM messages WHERE folder_id=%llu AND uid>=%u ORDER BY idx",
				LLU(folder_id), first);
		} else if (last == first) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid, "
				"replied, unsent, flagged, deleted, read, recent, forwarded "
				"FROM messages WHERE folder_id=%llu AND uid=%u",
				LLU(folder_id), first);
		} else {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid, "
				"replied, unsent, flagged, deleted, read, recent, forwarded "
				"FROM messages WHERE folder_id=%llu AND uid>=%u AND uid<=%u "
				"ORDER BY idx", LLU(folder_id), first, last);
		}
	} else {
		snprintf(sql_string, arsizeof(sql_string), "SELECT count(message_id) "
			"FROM messages WHERE folder_id=%llu", LLU(folder_id));
		auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
		if (pstmt == nullptr) {
			return MIDB_E_NO_MEMORY;
		}
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			return MIDB_E_NO_FOLDER;
		}
		total_mail = sqlite3_column_int64(pstmt, 0);
		pstmt.finalize();
		if (-1 == first && -1 == last) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid, "
				"replied, unsent, flagged, deleted, read, recent, forwarded"
				" FROM messages WHERE folder_id=%llu ORDER BY idx DESC",
				LLU(folder_id));
		} else if (-1 == first) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid, "
				"replied, unsent, flagged, deleted, read, recent, forwarded "
				"FROM messages WHERE folder_id=%llu AND uid<=%u ORDER BY idx"
				" DESC", LLU(folder_id), last);
		} else if (-1 == last) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid, "
				"replied, unsent, flagged, deleted, read, recent, forwarded "
				"FROM messages WHERE folder_id=%llu AND uid>=%u ORDER BY idx"
				" DESC", LLU(folder_id), first);
		} else if (last == first) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid, "
				"replied, unsent, flagged, deleted, read, recent, forwarded "
				"FROM messages WHERE folder_id=%llu AND uid=%u",
				LLU(folder_id), first);
		} else {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid, "
				"replied, unsent, flagged, deleted, read, recent, forwarded "
				"FROM messages WHERE folder_id=%llu AND uid>=%u AND uid<=%u "
				"ORDER BY idx DESC", LLU(folder_id), first, last);
		}
	}
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	double_list_init(&temp_list);
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		auto psm_node = cu_alloc<SIMU_NODE>();
		if (NULL == psm_node) {
			return MIDB_E_NO_MEMORY;
		}
		psm_node->node.pdata = psm_node;
		if (TRUE == b_asc) {
			psm_node->idx = sqlite3_column_int64(pstmt, 0);
		} else {
			psm_node->idx = total_mail - sqlite3_column_int64(pstmt, 0) + 1;
		}
		psm_node->mid_string = common_util_dup(S2A(sqlite3_column_text(pstmt, 1)));
		if (NULL == psm_node->mid_string) {
			return MIDB_E_NO_MEMORY;
		}
		psm_node->uid = sqlite3_column_int64(pstmt, 2);
		flags_buff[0] = '(';
		flags_len = 1;
		if (0 != sqlite3_column_int64(pstmt, 3)) {
			flags_buff[flags_len] = 'A';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 4)) {
			flags_buff[flags_len] = 'U';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 5)) {
			flags_buff[flags_len] = 'F';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 6)) {
			flags_buff[flags_len] = 'D';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 7)) {
			flags_buff[flags_len] = 'S';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 8)) {
			flags_buff[flags_len] = 'R';
			flags_len ++;
		}
		if (0 != sqlite3_column_int64(pstmt, 9)) {
			flags_buff[flags_len] = 'W';
			flags_len ++;
		}
		flags_buff[flags_len] = ')';
		flags_len ++;
		flags_buff[flags_len] = '\0';
		psm_node->flags_buff = common_util_dup(flags_buff);
		if (NULL == psm_node->flags_buff) {
			return MIDB_E_NO_MEMORY;
		}
		double_list_append_as_tail(&temp_list, &psm_node->node);
	}
	pstmt.finalize();
	temp_len = sprintf(temp_buff, "TRUE %zu\r\n",
		double_list_get_nodes_num(&temp_list));
	for (pnode=double_list_get_head(&temp_list); NULL!=pnode;
		pnode=double_list_get_after(&temp_list, pnode)) {
		auto psm_node = static_cast<SIMU_NODE *>(pnode->pdata);
		buff_len = gx_snprintf(temp_line, GX_ARRAY_SIZE(temp_line), "%u %s %u %s\r\n",
					psm_node->idx - 1, psm_node->mid_string,
					psm_node->uid, psm_node->flags_buff);
		if (256*1024 - temp_len < buff_len) {
			cmd_write(sockd, temp_buff, temp_len);
			temp_len = 0;
		}
		memcpy(temp_buff + temp_len, temp_line, buff_len);
		temp_len += buff_len;
	}
	pidb.reset();
	cmd_write(sockd, temp_buff, temp_len);
	return 0;
}

static int mail_engine_pdell(int argc, char **argv, int sockd)
{
	BOOL b_asc;
	int length;
	int temp_len;
	int buff_len;
	uint32_t uid;
	uint32_t idx;
	int sort_field;
	char temp_line[1024];
	char sql_string[1024];
	const char *mid_string;
	char temp_buff[256*1024];
	
	if (5 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[3], "RCV")) {
		sort_field = FIELD_RECEIVED;
	} else if (0 == strcasecmp(argv[3], "SUB")) {
		sort_field = FIELD_SUBJECT;	
	} else if (0 == strcasecmp(argv[3], "FRM")) {
		sort_field = FIELD_FROM;
	} else if (0 == strcasecmp(argv[3], "RCP")) {
		sort_field = FIELD_RCPT;
	} else if (0 == strcasecmp(argv[3], "SIZ")) {
		sort_field = FIELD_SIZE;
	} else if (0 == strcasecmp(argv[3], "RED")) {
		sort_field = FIELD_READ;
	} else if (0 == strcasecmp(argv[3], "FLG")) {
		sort_field = FIELD_FLAG;
	} else if (0 == strcasecmp(argv[3], "UID")) {
		sort_field = FIELD_UID;
	} else if (0 == strcasecmp(argv[3], "NON")) {
		sort_field = FIELD_NONE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[4], "ASC")) {
		b_asc = TRUE;
	} else if (0 == strcasecmp(argv[4], "DSC")) {
		b_asc = FALSE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	if (!mail_engine_sort_folder(pidb.get(), argv[2], sort_field))
		return MIDB_E_NO_MEMORY;
	snprintf(sql_string, arsizeof(sql_string), "SELECT count(message_id) FROM "
		"messages WHERE folder_id=%llu AND deleted=1", LLU(folder_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		return MIDB_E_NO_FOLDER;
	}
	length = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	if (TRUE == b_asc) {
		snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid FROM"
			" messages WHERE folder_id=%llu AND deleted=1 ORDER BY idx",
			LLU(folder_id));
	} else {
		snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string, uid FROM"
			" messages WHERE folder_id=%llu AND deleted=1 ORDER BY idx "
			"DESC", LLU(folder_id));
	}
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	temp_len = sprintf(temp_buff, "TRUE %d\r\n", length);
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		idx = sqlite3_column_int64(pstmt, 0);
		mid_string = S2A(sqlite3_column_text(pstmt, 1));
		uid = sqlite3_column_int64(pstmt, 2);
		buff_len = gx_snprintf(temp_line, GX_ARRAY_SIZE(temp_line),
			"%u %s %u\r\n", idx - 1, mid_string, uid);
		if (256*1024 - temp_len < buff_len) {
			cmd_write(sockd, temp_buff, temp_len);
			temp_len = 0;
		}
		memcpy(temp_buff + temp_len, temp_line, buff_len);
		temp_len += buff_len;
	}
	pstmt.finalize();
	pidb.reset();
	cmd_write(sockd, temp_buff, temp_len);
	return 0;
}

static int mail_engine_pdtlu(int argc, char **argv, int sockd)
{
	BOOL b_asc;
	int temp_len, total_mail = 0;
	int sort_field;
	DTLU_NODE *pdt_node;
	char sql_string[1024];
	DOUBLE_LIST temp_list;
	DOUBLE_LIST_NODE *pnode;
	char temp_buff[MAX_DIGLEN + 16];
	
	if (7 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[3], "RCV")) {
		sort_field = FIELD_RECEIVED;
	} else if (0 == strcasecmp(argv[3], "SUB")) {
		sort_field = FIELD_SUBJECT;	
	} else if (0 == strcasecmp(argv[3], "FRM")) {
		sort_field = FIELD_FROM;
	} else if (0 == strcasecmp(argv[3], "RCP")) {
		sort_field = FIELD_RCPT;
	} else if (0 == strcasecmp(argv[3], "SIZ")) {
		sort_field = FIELD_SIZE;
	} else if (0 == strcasecmp(argv[3], "RED")) {
		sort_field = FIELD_READ;
	} else if (0 == strcasecmp(argv[3], "FLG")) {
		sort_field = FIELD_FLAG;
	} else if (0 == strcasecmp(argv[3], "UID")) {
		sort_field = FIELD_UID;
	} else if (0 == strcasecmp(argv[3], "NON")) {
		sort_field = FIELD_NONE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 == strcasecmp(argv[4], "ASC")) {
		b_asc = TRUE;
	} else if (0 == strcasecmp(argv[4], "DSC")) {
		b_asc = FALSE;
	} else {
		return MIDB_E_PARAMETER_ERROR;
	}
	int first = strtol(argv[5], nullptr, 0), last = strtol(argv[6], nullptr, 0);
	if (first < 1 && first != -1)
		return MIDB_E_PARAMETER_ERROR;
	if (last < 1 && last != -1)
		return MIDB_E_PARAMETER_ERROR;
	if (first == -1 && last != -1)
		return MIDB_E_PARAMETER_ERROR;
	if (last != -1 && last < first)
		return MIDB_E_PARAMETER_ERROR;
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	if (!mail_engine_sort_folder(pidb.get(), argv[2], sort_field))
		return MIDB_E_NO_MEMORY;
	if (TRUE == b_asc) {
		if (-1 == first && -1 == last) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string"
				" FROM messages WHERE folder_id=%llu ORDER BY idx",
				LLU(folder_id));
		} else if (-1 == first) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string "
					"FROM messages WHERE folder_id=%llu AND uid<=%u"
					" ORDER BY idx", LLU(folder_id), last);
		} else if (-1 == last) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string "
					"FROM messages WHERE folder_id=%llu AND uid>=%u"
					" ORDER BY idx", LLU(folder_id), first);
		} else if (last == first) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string "
					"FROM messages WHERE folder_id=%llu AND uid=%u",
					LLU(folder_id), first);
		} else {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string "
				"FROM messages WHERE folder_id=%llu AND uid>=%u AND"
				" uid<=%u ORDER BY idx", LLU(folder_id), first, last);
		}
	} else {
		snprintf(sql_string, arsizeof(sql_string), "SELECT count(message_id) "
		          "FROM messages WHERE folder_id=%llu", LLU(folder_id));
		auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
		if (pstmt == nullptr) {
			return MIDB_E_NO_MEMORY;
		}
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			return MIDB_E_NO_FOLDER;
		}
		total_mail = sqlite3_column_int64(pstmt, 0);
		pstmt.finalize();
		if (-1 == first && -1 == last) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string"
				" FROM messages WHERE folder_id=%llu ORDER BY idx"
				" DESC", LLU(folder_id));
		} else if (-1 == first) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string "
					"FROM messages WHERE folder_id=%llu AND uid<=%u"
					" ORDER BY idx DESC", LLU(folder_id), last);
		} else if (-1 == last) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string "
					"FROM messages WHERE folder_id=%llu AND uid>=%u"
					" ORDER BY idx", LLU(folder_id), first);
		} else if (last == first) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string "
					"FROM messages WHERE folder_id=%llu AND uid=%u",
					LLU(folder_id), first);
		} else {
			snprintf(sql_string, arsizeof(sql_string), "SELECT idx, mid_string "
				"FROM messages WHERE folder_id=%llu AND uid>=%u AND "
				"uid<=%u ORDER BY idx DESC", LLU(folder_id), first, last);
		}
	}
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	double_list_init(&temp_list);
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		pdt_node = cu_alloc<DTLU_NODE>();
		if (NULL == pdt_node) {
			return MIDB_E_NO_MEMORY;
		}
		pdt_node->node.pdata = pdt_node;
		if (TRUE == b_asc) {
			pdt_node->idx = sqlite3_column_int64(pstmt, 0);
		} else {
			pdt_node->idx = total_mail - sqlite3_column_int64(pstmt, 0) + 1;
		}
		pdt_node->mid_string = common_util_dup(S2A(sqlite3_column_text(pstmt, 1)));
		if (NULL == pdt_node->mid_string) {
			return MIDB_E_NO_MEMORY;
		}
		double_list_append_as_tail(&temp_list, &pdt_node->node);
	}
	pstmt.finalize();
	temp_len = sprintf(temp_buff, "TRUE %zu\r\n",
		double_list_get_nodes_num(&temp_list));
	cmd_write(sockd, temp_buff, temp_len);
	for (pnode=double_list_get_head(&temp_list); NULL!=pnode;
		pnode=double_list_get_after(&temp_list, pnode)) {
		pdt_node = (DTLU_NODE*)pnode->pdata;
		temp_len = sprintf(temp_buff, "%d ", pdt_node->idx - 1);
		if (0 == mail_engine_get_digest(pidb->psqlite,
			pdt_node->mid_string, temp_buff + temp_len)) {
			return MIDB_E_DIGEST;
		}
		temp_len = strlen(temp_buff);
		temp_buff[temp_len] = '\r';
		temp_len ++;
		temp_buff[temp_len] = '\n';
		temp_len ++;	
		cmd_write(sockd, temp_buff, temp_len);
	}
	return 0;
}

static int mail_engine_psflg(int argc, char **argv, int sockd)
{
	uint64_t read_cn;
	uint64_t message_id;
	uint32_t tmp_proptag;
	char sql_string[1024];
	uint32_t message_flags;
	PROPTAG_ARRAY proptags;
	PROBLEM_ARRAY problems;
	TPROPVAL_ARRAY propvals;

	if (5 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT message_id,"
	             " folder_id FROM messages WHERE mid_string=?");
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_bind_text(pstmt, 1, argv[3], -1, SQLITE_STATIC);
	if (SQLITE_ROW != sqlite3_step(pstmt) ||
	    gx_sql_col_uint64(pstmt, 1) != folder_id) {
		return MIDB_E_NO_MESSAGE;
	}
	message_id = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	if (NULL != strchr(argv[4], 'A')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET replied=1"
		        " WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	if (NULL != strchr(argv[4], 'U')) {
		proptags.count = 1;
		proptags.pproptag = &tmp_proptag;
		tmp_proptag = PR_MESSAGE_FLAGS;
		if (!exmdb_client::get_message_properties(argv[1], NULL,
			0, rop_util_make_eid_ex(1, message_id), &proptags, &propvals)
			|| 0 == propvals.count) {
			return MIDB_E_NO_MEMORY;
		}
		message_flags = *(uint32_t*)propvals.ppropval[0].pvalue;
		if (!(message_flags & MSGFLAG_UNSENT)) {
			message_flags |= MSGFLAG_UNSENT;
			propvals.ppropval[0].pvalue = &message_flags;
			if (!exmdb_client::set_message_properties(argv[1],
				NULL, 0, rop_util_make_eid_ex(1, message_id), &propvals,
				&problems)) {
				return MIDB_E_NO_MEMORY;
			}
		}
	}
	if (NULL != strchr(argv[4], 'F')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET flagged=1"
		        " WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	if (NULL != strchr(argv[4], 'W')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET forwarded=1"
		        " WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	if (NULL != strchr(argv[4], 'D')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET deleted=1"
		        " WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	if (NULL != strchr(argv[4], 'S')) {
		if (!exmdb_client::set_message_read_state(argv[1],
			NULL, rop_util_make_eid_ex(1, message_id), 1, &read_cn)) {
			return MIDB_E_NO_MEMORY;
		}
	}
	if (NULL != strchr(argv[4], 'R')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET recent=1"
		        " WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	pidb.reset();
	cmd_write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int mail_engine_prflg(int argc, char **argv, int sockd)
{
	uint64_t read_cn;
	uint64_t message_id;
	uint32_t tmp_proptag;
	char sql_string[1024];
	uint32_t message_flags;
	PROPTAG_ARRAY proptags;
	PROBLEM_ARRAY problems;
	TPROPVAL_ARRAY propvals;

	if (5 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT message_id,"
	             " folder_id FROM messages WHERE mid_string=?");
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_bind_text(pstmt, 1, argv[3], -1, SQLITE_STATIC);
	if (SQLITE_ROW != sqlite3_step(pstmt) ||
	    gx_sql_col_uint64(pstmt, 1) != folder_id) {
		return MIDB_E_NO_MESSAGE;
	}
	message_id = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	if (NULL != strchr(argv[4], 'A')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET replied=0"
		        " WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	if (NULL != strchr(argv[4], 'U')) {
		proptags.count = 1;
		proptags.pproptag = &tmp_proptag;
		tmp_proptag = PR_MESSAGE_FLAGS;
		if (!exmdb_client::get_message_properties(argv[1], NULL,
			0, rop_util_make_eid_ex(1, message_id), &proptags, &propvals)
			|| 0 == propvals.count) {
			return MIDB_E_NO_MEMORY;
		}
		message_flags = *(uint32_t*)propvals.ppropval[0].pvalue;
		if (message_flags & MSGFLAG_UNSENT) {
			message_flags &= ~MSGFLAG_UNSENT;
			propvals.ppropval[0].pvalue = &message_flags;
			if (!exmdb_client::set_message_properties(argv[1],
				NULL, 0, rop_util_make_eid_ex(1, message_id), &propvals,
				&problems)) {
				return MIDB_E_NO_MEMORY;
			}
		}
	}
	if (NULL != strchr(argv[4], 'F')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET flagged=0"
		        " WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	if (NULL != strchr(argv[4], 'W')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET forwarded=0"
		        " WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	if (NULL != strchr(argv[4], 'D')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET deleted=0"
		        " WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	if (NULL != strchr(argv[4], 'S')) {
		if (!exmdb_client::set_message_read_state(argv[1],
			NULL, rop_util_make_eid_ex(1, message_id), 0, &read_cn)) {
			return MIDB_E_NO_MEMORY;
		}
	}
	if (NULL != strchr(argv[4], 'R')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET recent=0"
		        " WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	pidb.reset();
	cmd_write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int mail_engine_pgflg(int argc, char **argv, int sockd)
{
	int temp_len;
	int flags_len;
	char flags_buff[32];
	char temp_buff[1024];

	if (4 != argc || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr)
		return MIDB_E_HASHTABLE_FULL;
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		return MIDB_E_NO_FOLDER;
	}
	auto pstmt = gx_sql_prep(pidb->psqlite, "SELECT folder_id, recent, "
	             "read, unsent, flagged, replied, forwarded, deleted "
	             "FROM messages WHERE mid_string=?");
	if (pstmt == nullptr) {
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_bind_text(pstmt, 1, argv[3], -1, SQLITE_STATIC);
	if (SQLITE_ROW != sqlite3_step(pstmt) ||
	    gx_sql_col_uint64(pstmt, 0) != folder_id) {
		return MIDB_E_NO_MESSAGE;
	}
	flags_buff[0] = '(';
	flags_len = 1;
	if (0 != sqlite3_column_int64(pstmt, 5)) {
		flags_buff[flags_len] = 'A';
		flags_len ++;
	}
	if (0 != sqlite3_column_int64(pstmt, 3)) {
		flags_buff[flags_len] = 'U';
		flags_len ++;
	}
	if (0 != sqlite3_column_int64(pstmt, 4)) {
		flags_buff[flags_len] = 'F';
		flags_len ++;
	}
	if (0 != sqlite3_column_int64(pstmt, 6)) {
		flags_buff[flags_len] = 'W';
		flags_len ++;
	}
	if (0 != sqlite3_column_int64(pstmt, 7)) {
		flags_buff[flags_len] = 'D';
		flags_len ++;	
	}
	if (0 != sqlite3_column_int64(pstmt, 2)) {
		flags_buff[flags_len] = 'S';
		flags_len ++;
	}
	if (0 != sqlite3_column_int64(pstmt, 1)) {
		flags_buff[flags_len] = 'R';
		flags_len ++;
	}
	pstmt.finalize();
	pidb.reset();
	flags_buff[flags_len] = ')';
	flags_len ++;
	flags_buff[flags_len] = '\0';
	temp_len = sprintf(temp_buff, "TRUE %s\r\n", flags_buff);
	cmd_write(sockd, temp_buff, temp_len);
	return 0;
}

static int mail_engine_psrhl(int argc, char **argv, int sockd)
{
	int result;
	char *parg;
	int tmp_argc;
	sqlite3 *psqlite;
	size_t decode_len;
	char temp_path[256];
	char* tmp_argv[1024];
	CONDITION_TREE *ptree;
	char tmp_buff[16*1024];
	char list_buff[256*1024];
	CONDITION_RESULT *presult;
	
	if (argc != 5 || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto tmp_len = strlen(argv[4]);
	if (tmp_len >= sizeof(tmp_buff)) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 != decode64(argv[4], tmp_len, tmp_buff, &decode_len)) {
		return MIDB_E_PARAMETER_ERROR;
	}
	tmp_argc = 0;
	parg = tmp_buff;
	while (*parg != '\0' && parg - tmp_buff >= 0 &&
	       static_cast<size_t>(parg - tmp_buff) < decode_len &&
	       static_cast<size_t>(tmp_argc) < sizeof(tmp_argv)) {
		tmp_argv[tmp_argc] = parg;
		parg += strlen(parg) + 1;
		tmp_argc ++;
	}
	if (0 == tmp_argc) {
		return MIDB_E_PARAMETER_ERROR;
	}
	ptree = mail_engine_ct_build(tmp_argc, tmp_argv);
	if (NULL == ptree) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr) {
		mail_engine_ct_destroy(ptree);
		return MIDB_E_HASHTABLE_FULL;
	}
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		pidb.reset();
		mail_engine_ct_destroy(ptree);
		return MIDB_E_NO_FOLDER;
	}
	pidb.reset();
	sprintf(temp_path, "%s/exmdb/midb.sqlite3", argv[1]);
	auto ret = sqlite3_open_v2(temp_path, &psqlite, SQLITE_OPEN_READWRITE, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "E-1439: sqlite3_open %s: %s\n", temp_path, sqlite3_errstr(ret));
		mail_engine_ct_destroy(ptree);
		return MIDB_E_HASHTABLE_FULL;
	}
	presult = mail_engine_ct_match(argv[3],
		psqlite, folder_id, ptree, FALSE);
	if (NULL == presult) {
		sqlite3_close(psqlite);
		mail_engine_ct_destroy(ptree);
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_close(psqlite);
	tmp_len = 4;
    memcpy(list_buff, "TRUE", 4);
    while (-1 != (result = mail_engine_ct_fetch_result(presult))) {
		tmp_len += gx_snprintf(list_buff + tmp_len,
		           GX_ARRAY_SIZE(list_buff) - tmp_len, " %d", result);
		if (tmp_len >= 255*1024) {
			cmd_write(sockd, list_buff, tmp_len);
			tmp_len = 0;
		}
    }
    mail_engine_ct_free_result(presult);
    mail_engine_ct_destroy(ptree);
    list_buff[tmp_len] = '\r';
	tmp_len ++;
    list_buff[tmp_len] = '\n';
	tmp_len ++;
	cmd_write(sockd, list_buff, tmp_len);
	return 0;
}

static int mail_engine_psrhu(int argc, char **argv, int sockd)
{
	int result;
	char *parg;
	int tmp_argc;
	sqlite3 *psqlite;
	size_t decode_len;
	char temp_path[256];
	char* tmp_argv[1024];
	char tmp_buff[16*1024];
	char list_buff[256*1024];
	CONDITION_RESULT *presult;
	
	if (argc != 5 || strlen(argv[1]) >= 256 || strlen(argv[2]) >= 1024) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto tmp_len = strlen(argv[4]);
	if (tmp_len >= sizeof(tmp_buff)) {
		return MIDB_E_PARAMETER_ERROR;
	}
	if (0 != decode64(argv[4], tmp_len, tmp_buff, &decode_len)) {
		return MIDB_E_PARAMETER_ERROR;
	}
	tmp_argc = 0;
	parg = tmp_buff;
	while (*parg != '\0' && parg - tmp_buff >= 0 &&
	       static_cast<size_t>(parg - tmp_buff) < decode_len &&
	       static_cast<size_t>(tmp_argc) < sizeof(tmp_argv)) {
		tmp_argv[tmp_argc] = parg;
		parg += strlen(parg) + 1;
		tmp_argc ++;
	}
	if (0 == tmp_argc) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto ptree = mail_engine_ct_build(tmp_argc, tmp_argv);
	if (NULL == ptree) {
		return MIDB_E_PARAMETER_ERROR;
	}
	auto pidb = mail_engine_get_idb(argv[1]);
	if (pidb == nullptr) {
		mail_engine_ct_destroy(ptree);
		return MIDB_E_HASHTABLE_FULL;
	}
	auto folder_id = mail_engine_get_folder_id(pidb.get(), argv[2]);
	if (0 == folder_id) {
		pidb.reset();
		mail_engine_ct_destroy(ptree);
		return MIDB_E_NO_FOLDER;
	}
	pidb.reset();
	sprintf(temp_path, "%s/exmdb/midb.sqlite3", argv[1]);
	auto ret = sqlite3_open_v2(temp_path, &psqlite, SQLITE_OPEN_READWRITE, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "E-1505: sqlite3_open %s: %s\n", temp_path, sqlite3_errstr(ret));
		mail_engine_ct_destroy(ptree);
		return MIDB_E_HASHTABLE_FULL;
	}
	presult = mail_engine_ct_match(argv[3],
		psqlite, folder_id, ptree, TRUE);
	if (NULL == presult) {
		sqlite3_close(psqlite);
		mail_engine_ct_destroy(ptree);
		return MIDB_E_NO_MEMORY;
	}
	sqlite3_close(psqlite);
	tmp_len = 4;
    memcpy(list_buff, "TRUE", 4);
    while (-1 != (result = mail_engine_ct_fetch_result(presult))) {
		tmp_len += gx_snprintf(list_buff + tmp_len,
		           GX_ARRAY_SIZE(list_buff) - tmp_len, " %d", result);
		if (tmp_len >= 255*1024) {
			cmd_write(sockd, list_buff, tmp_len);
			tmp_len = 0;
		}
    }
    mail_engine_ct_free_result(presult);
    mail_engine_ct_destroy(ptree);
    list_buff[tmp_len] = '\r';
	tmp_len ++;
    list_buff[tmp_len] = '\n';
	tmp_len ++;
	cmd_write(sockd, list_buff, tmp_len);
	return 0;
}

static void mail_engine_add_notification_message(
	IDB_ITEM *pidb, uint64_t folder_id, uint64_t message_id)
{
	uint32_t uidnext;
	uint64_t mod_time;
	char flags_buff[16];
	char mid_string[128];
	char sql_string[1024];
	uint32_t message_flags;
	uint64_t received_time;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	uint32_t tmp_proptags[4];
	
	proptags.count = 4;
	proptags.pproptag = tmp_proptags;
	tmp_proptags[0] = PROP_TAG_MESSAGEDELIVERYTIME;
	tmp_proptags[1] = PR_LAST_MODIFICATION_TIME;
	tmp_proptags[2] = PidTagMidString;
	tmp_proptags[3] = PR_MESSAGE_FLAGS;
	if (!exmdb_client::get_message_properties(
		common_util_get_maildir(), NULL, 0,
		rop_util_make_eid_ex(1, message_id),
		&proptags, &propvals)) {
		return;		
	}
	const void *pvalue = propvals.getval(PR_LAST_MODIFICATION_TIME);
	if (NULL == pvalue) {
		mod_time = 0;
	} else {
		mod_time = *(uint64_t*)pvalue;
	}
	pvalue = propvals.getval(PROP_TAG_MESSAGEDELIVERYTIME);
	if (NULL == pvalue) {
		received_time = mod_time;
	} else {
		received_time = *(uint64_t*)pvalue;
	}
	pvalue = propvals.getval(PR_MESSAGE_FLAGS);
	if (NULL == pvalue) {
		message_flags = 0;
	} else {
		message_flags = *(uint32_t*)pvalue;
	}
	flags_buff[0] = '\0';
	pvalue = propvals.getval(PidTagMidString);
	if (NULL == pvalue) {
		snprintf(sql_string, arsizeof(sql_string), "SELECT mid_string, flag_string"
		          " FROM mapping WHERE message_id=%llu", LLU(message_id));
		auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
		if (pstmt == nullptr)
			return;
		if (SQLITE_ROW == sqlite3_step(pstmt)) {
			gx_strlcpy(mid_string, S2A(sqlite3_column_text(pstmt, 0)), sizeof(mid_string));
			pvalue = sqlite3_column_text(pstmt, 1);
			if (NULL != pvalue) {
				gx_strlcpy(flags_buff, static_cast<const char *>(pvalue), GX_ARRAY_SIZE(flags_buff));
			}
			pvalue = mid_string;
		}
		pstmt.finalize();
		if (NULL != pvalue) {
			snprintf(sql_string, arsizeof(sql_string), "DELETE FROM mapping"
			        " WHERE message_id=%llu", LLU(message_id));
			gx_sql_exec(pidb->psqlite, sql_string);
		}
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT uidnext FROM"
	          " folders WHERE folder_id=%llu", LLU(folder_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return;
	uidnext = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET"
		" uidnext=uidnext+1, sort_field=%d "
		"WHERE folder_id=%llu", FIELD_NONE, LLU(folder_id));
	if (gx_sql_exec(pidb->psqlite, sql_string) != SQLITE_OK)
		return;
	snprintf(sql_string, arsizeof(sql_string), "INSERT INTO messages ("
		"message_id, folder_id, mid_string, mod_time, uid, "
		"unsent, read, subject, sender, rcpt, size, received)"
		" VALUES (?, %llu, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
		LLU(folder_id));
	pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr)
		return;	
	mail_engine_insert_message(pstmt, &uidnext, message_id,
		static_cast<const char *>(pvalue), message_flags, received_time, mod_time);
	pstmt.finalize();
	if (NULL != strchr(flags_buff, 'F')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET "
		        "flagged=1 WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	if (NULL != strchr(flags_buff, 'A')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET "
		        "replied=1 WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
	if (NULL != strchr(flags_buff, 'W')) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET "
		        "forwarded=1 WHERE message_id=%llu", LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
	}
}

static void mail_engine_delete_notification_message(
	IDB_ITEM *pidb, uint64_t folder_id, uint64_t message_id)
{
	char sql_string[1024];
	
	snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id FROM "
	          "messages WHERE message_id=%llu", LLU(message_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW ||
	    gx_sql_col_uint64(pstmt, 0) != folder_id)
		return;
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "DELETE FROM messages"
	        " WHERE message_id=%llu", LLU(message_id));
	gx_sql_exec(pidb->psqlite, sql_string);
	snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET sort_field=%d "
	        "WHERE folder_id=%llu", FIELD_NONE, LLU(folder_id));
	gx_sql_exec(pidb->psqlite, sql_string);
}

static BOOL mail_engine_add_notification_folder(
	IDB_ITEM *pidb, uint64_t parent_id, uint64_t folder_id)
{
	BOOL b_wait;
	int tmp_len;
	uint64_t commit_max;
	char sql_string[1280];
	char decoded_name[512];
	PROPTAG_ARRAY proptags;
	char encoded_name[1024];
	TPROPVAL_ARRAY propvals;
	uint32_t tmp_proptags[4];
	
	switch (parent_id) {
	case PRIVATE_FID_IPMSUBTREE:
		break;
	case PRIVATE_FID_INBOX:
		strcpy(decoded_name, "inbox");
		break;
	case PRIVATE_FID_DRAFT:
		strcpy(decoded_name, "draft");
		break;
	case PRIVATE_FID_SENT_ITEMS:
		strcpy(decoded_name, "sent");
		break;
	case PRIVATE_FID_DELETED_ITEMS:
		strcpy(decoded_name, "trash");
		break;
	case PRIVATE_FID_JUNK:
		strcpy(decoded_name, "junk");
		break;
	default: {
		snprintf(sql_string, arsizeof(sql_string), "SELECT name FROM"
		          " folders WHERE folder_id=%llu", LLU(parent_id));
		auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
		if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW ||
		    !decode_hex_binary(S2A(sqlite3_column_text(pstmt, 0)),
		    decoded_name, sizeof(decoded_name)))
			return FALSE;
	}
	}
	proptags.count = 4;
	proptags.pproptag = tmp_proptags;
	tmp_proptags[0] = PR_DISPLAY_NAME;
	tmp_proptags[1] = PR_LOCAL_COMMIT_TIME_MAX;
	tmp_proptags[2] = PR_CONTAINER_CLASS;
	tmp_proptags[3] = PR_ATTR_HIDDEN;
	b_wait = FALSE;
 REQUERY_FOLDER:
	if (!exmdb_client::get_folder_properties(
		common_util_get_maildir(), 0,
		rop_util_make_eid_ex(1, folder_id),
		&proptags, &propvals)) {
		return FALSE;		
	}
	auto pvalue = propvals.getval(PR_ATTR_HIDDEN);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		return FALSE;
	}
	pvalue = propvals.getval(PR_CONTAINER_CLASS);
	if (NULL == pvalue && FALSE == b_wait) {
		/* outlook will set the PR_CONTAINER_CLASS
			after RopCreateFolder, so try to wait! */
		sleep(1);
		b_wait = TRUE;
		goto REQUERY_FOLDER;
	}
	if (NULL == pvalue) {
		return FALSE;
	}
	if (strcasecmp(static_cast<char *>(pvalue), "IPF.Note") != 0)
		return FALSE;
	pvalue = propvals.getval(PR_LOCAL_COMMIT_TIME_MAX);
	if (NULL == pvalue) {
		commit_max = 0;
	} else {
		commit_max = *(uint64_t*)pvalue;
	}
	pvalue = propvals.getval(PR_DISPLAY_NAME);
	if (NULL == pvalue) {
		return FALSE;
	}
	tmp_len = strlen(static_cast<char *>(pvalue));
	if (tmp_len >= 256) {
		return FALSE;
	}
	std::string temp_name;
	try {
		if (parent_id == PRIVATE_FID_IPMSUBTREE) {
			temp_name.assign(static_cast<const char *>(pvalue), tmp_len);
		} else {
			if (tmp_len + strlen(decoded_name) >= 511) {
				return FALSE;
			}
			temp_name = decoded_name + "/"s +
			            static_cast<const char *>(pvalue);
		}
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "E-1477: ENOMEM\n");
		return false;
	}
	encode_hex_binary(temp_name.c_str(), temp_name.size(), encoded_name, arsizeof(encoded_name));
	snprintf(sql_string, arsizeof(sql_string), "INSERT INTO folders (folder_id, parent_fid, "
	        "commit_max, name) VALUES (%llu, %llu, %llu, '%s')", LLU(folder_id),
	        LLU(parent_id), LLU(commit_max), encoded_name);
	if (gx_sql_exec(pidb->psqlite, sql_string) != SQLITE_OK)
		return FALSE;
	return TRUE;
}

static void mail_engine_delete_notification_folder(
	IDB_ITEM *pidb, uint64_t folder_id)
{
	char sql_string[256];
	
	snprintf(sql_string, arsizeof(sql_string), "DELETE FROM folders "
	        "WHERE folder_id=%llu", LLU(folder_id));
	gx_sql_exec(pidb->psqlite, sql_string);
}

static void mail_engine_update_subfolders_name(IDB_ITEM *pidb,
	uint64_t parent_id, const char *parent_name)
{
	int tmp_len;
	char *ptoken;
	uint64_t folder_id;
	char temp_name[512];
	char sql_string[1280];
	char decoded_name[512];
	char encoded_name[1024];
	
	snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id, name"
	          " FROM folders WHERE parent_fid=%llu", LLU(parent_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr)
		return;	
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		folder_id = sqlite3_column_int64(pstmt, 0);
		if (!decode_hex_binary(S2A(sqlite3_column_text(pstmt, 1)),
		    decoded_name, sizeof(decoded_name)))
			continue;
		ptoken = strrchr(decoded_name, '/');
		if (NULL == ptoken) {
			continue;
		}
		if (strlen(ptoken) + strlen(parent_name) >= 512) {
			continue;
		}
		tmp_len = sprintf(temp_name, "%s%s", parent_name, ptoken);
		encode_hex_binary(temp_name, tmp_len, encoded_name, 1024);
		snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET name='%s' "
		        "WHERE folder_id=%llu", encoded_name, LLU(folder_id));
		gx_sql_exec(pidb->psqlite, sql_string);
		mail_engine_update_subfolders_name(pidb, folder_id, temp_name);
	}
}

static void mail_engine_move_notification_folder(
	IDB_ITEM *pidb, uint64_t parent_id, uint64_t folder_id)
{
	int tmp_len;
	void *pvalue;
	uint32_t tmp_proptag;
	char sql_string[1280];
	char decoded_name[512];
	PROPTAG_ARRAY proptags;
	char encoded_name[1024];
	TPROPVAL_ARRAY propvals;
	
	snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id "
	          "FROM folders WHERE folder_id=%llu", LLU(folder_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr)
		return;	
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		pstmt.finalize();
		mail_engine_add_notification_folder(
				pidb, parent_id, folder_id);
		return;
	}
	pstmt.finalize();
	switch (parent_id) {
	case PRIVATE_FID_IPMSUBTREE:
		break;
	case PRIVATE_FID_INBOX:
		strcpy(decoded_name, "inbox");
		break;
	case PRIVATE_FID_DRAFT:
		strcpy(decoded_name, "draft");
		break;
	case PRIVATE_FID_SENT_ITEMS:
		strcpy(decoded_name, "sent");
		break;
	case PRIVATE_FID_DELETED_ITEMS:
		strcpy(decoded_name, "trash");
		break;
	case PRIVATE_FID_JUNK:
		strcpy(decoded_name, "junk");
		break;
	default:
		snprintf(sql_string, arsizeof(sql_string), "SELECT name FROM"
		          " folders WHERE folder_id=%llu", LLU(parent_id));
		pstmt = gx_sql_prep(pidb->psqlite, sql_string);
		if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW ||
		    !decode_hex_binary(S2A(sqlite3_column_text(pstmt, 0)),
		    decoded_name, sizeof(decoded_name)))
			return;
		pstmt.finalize();
	}
	proptags.count = 1;
	proptags.pproptag = &tmp_proptag;
	tmp_proptag = PR_DISPLAY_NAME;
	if (!exmdb_client::get_folder_properties(
		common_util_get_maildir(), 0,
		rop_util_make_eid_ex(1, folder_id),
		&proptags, &propvals)) {
		return;		
	}
	pvalue = propvals.getval(PR_DISPLAY_NAME);
	if (NULL == pvalue) {
		return;
	}
	tmp_len = strlen(static_cast<char *>(pvalue));
	if (tmp_len >= 256) {
		return;
	}
	std::string temp_name;
	try {
		if (parent_id == PRIVATE_FID_IPMSUBTREE) {
			temp_name.assign(static_cast<const char *>(pvalue), tmp_len);
		} else {
			if (tmp_len + strlen(decoded_name) >= 511) {
				return;
			}
			temp_name = decoded_name + "/"s +
			            static_cast<const char *>(pvalue);
		}
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "E-1478: ENOMEM\n");
	}
	encode_hex_binary(temp_name.c_str(), temp_name.size(), encoded_name, arsizeof(encoded_name));
	snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET parent_fid=%llu, name='%s' "
	        "WHERE folder_id=%llu", LLU(parent_id), encoded_name, LLU(folder_id));
	gx_sql_exec(pidb->psqlite, sql_string);
	mail_engine_update_subfolders_name(pidb, folder_id, temp_name.c_str());
}

static void mail_engine_modify_notification_folder(
	IDB_ITEM *pidb, uint64_t folder_id)
{
	int tmp_len;
	void *pvalue;
	char *pdisplayname;
	uint32_t tmp_proptag;
	char sql_string[1280];
	char decoded_name[512];
	PROPTAG_ARRAY proptags;
	char encoded_name[1024];
	TPROPVAL_ARRAY propvals;
	
	switch (folder_id) {	
	case PRIVATE_FID_IPMSUBTREE:
	case PRIVATE_FID_INBOX:
	case PRIVATE_FID_DRAFT:
	case PRIVATE_FID_SENT_ITEMS:
	case PRIVATE_FID_DELETED_ITEMS:
	case PRIVATE_FID_JUNK:
		return;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT name FROM"
	          " folders WHERE folder_id=%llu", LLU(folder_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW ||
	    !decode_hex_binary(S2A(sqlite3_column_text(pstmt, 0)),
	    decoded_name, sizeof(decoded_name)))
		return;
	pstmt.finalize();
	proptags.count = 1;
	proptags.pproptag = &tmp_proptag;
	tmp_proptag = PR_DISPLAY_NAME;
	if (!exmdb_client::get_folder_properties(
		common_util_get_maildir(), 0,
		rop_util_make_eid_ex(1, folder_id),
		&proptags, &propvals)) {
		return;		
	}
	pvalue = propvals.getval(PR_DISPLAY_NAME);
	if (NULL == pvalue) {
		return;
	}
	pdisplayname = strrchr(decoded_name, '/');
	if (NULL == pdisplayname) {
		pdisplayname = decoded_name;
	} else {
		pdisplayname ++;
	}
	if (strcmp(pdisplayname, static_cast<char *>(pvalue)) == 0)
		return;
	tmp_len = strlen(static_cast<char* >(pvalue));
	if (tmp_len >= 256) {
		return;
	}
	if (pdisplayname == decoded_name) {
		memcpy(decoded_name, pvalue, tmp_len);
	} else {
		if (pdisplayname - decoded_name + tmp_len >= 512) {
			return;
		}
		strcpy(pdisplayname, static_cast<char *>(pvalue));
		tmp_len = strlen(decoded_name);
	}
	encode_hex_binary(decoded_name, tmp_len, encoded_name, 1024);
	snprintf(sql_string, arsizeof(sql_string), "UPDATE folders SET name='%s' "
	        "WHERE folder_id=%llu", encoded_name, LLU(folder_id));
	gx_sql_exec(pidb->psqlite, sql_string);
	mail_engine_update_subfolders_name(pidb, folder_id, decoded_name);
}

static void mail_engine_modify_notification_message(
	IDB_ITEM *pidb, uint64_t folder_id, uint64_t message_id)
{
	int b_read;
	int b_unsent;
	uint64_t mod_time;
	char sql_string[256];
	uint32_t message_flags;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	uint32_t tmp_proptags[3];
	
	proptags.count = 3;
	proptags.pproptag = tmp_proptags;
	tmp_proptags[0] = PR_MESSAGE_FLAGS;
	tmp_proptags[1] = PR_LAST_MODIFICATION_TIME;
	tmp_proptags[2] = PidTagMidString;
	if (!exmdb_client::get_message_properties(
		common_util_get_maildir(), NULL, 0,
		rop_util_make_eid_ex(1, message_id),
		&proptags, &propvals)) {
		return;	
	}
	auto pvalue = propvals.getval(PR_MESSAGE_FLAGS);
	if (NULL == pvalue) {
		message_flags = 0;
	} else {
		message_flags= *(uint32_t*)pvalue;
	}
	pvalue = propvals.getval(PidTagMidString);
	if (NULL != pvalue) {
 UPDATE_MESSAGE_FLAGS:
		b_unsent = !!(message_flags & MSGFLAG_UNSENT);
		b_read   = !!(message_flags & MSGFLAG_READ);
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET read=%d, unsent=%d"
		        " WHERE message_id=%llu", b_read, b_unsent, LLU(message_id));
		gx_sql_exec(pidb->psqlite, sql_string);
		return;
	}
	pvalue = propvals.getval(PR_LAST_MODIFICATION_TIME);
	if (NULL == pvalue) {
		mod_time = 0;
	} else {
		mod_time = *(uint64_t*)pvalue;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT mod_time FROM"
	          " messages WHERE message_id=%llu", LLU(message_id));
	auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return;
	if (gx_sql_col_uint64(pstmt, 0) == mod_time) {
		pstmt.finalize();
		goto UPDATE_MESSAGE_FLAGS;
	}
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "DELETE FROM messages"
	        " WHERE message_id=%llu", LLU(message_id));
	if (gx_sql_exec(pidb->psqlite, sql_string) != SQLITE_OK)
		return;	
	return mail_engine_add_notification_message(
					pidb, folder_id, message_id);
}

static void mail_engine_notification_proc(const char *dir,
	BOOL b_table, uint32_t notify_id, const DB_NOTIFY *pdb_notify)
{
	uint64_t folder_id;
	uint64_t parent_id;
	uint64_t message_id;
	char temp_buff[1280];
	char sql_string[1024];
	
	if (TRUE == b_table) {
		return;
	}
	auto pidb = mail_engine_peek_idb(dir);
	if (pidb == nullptr)
		return;
	if (pidb->sub_id != notify_id) {
		return;
	}
	switch (pdb_notify->type) {
	case DB_NOTIFY_TYPE_NEW_MAIL: {
		auto n = static_cast<const DB_NOTIFY_NEW_MAIL *>(pdb_notify->pdata);
		folder_id = n->folder_id;
		message_id = n->message_id;
		mail_engine_add_notification_message(pidb.get(), folder_id, message_id);
		snprintf(sql_string, arsizeof(sql_string), "SELECT name FROM"
		          " folders WHERE folder_id=%llu", LLU(folder_id));
		auto pstmt = gx_sql_prep(pidb->psqlite, sql_string);
		if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
			break;
		snprintf(temp_buff, 1280, "FOLDER-TOUCH %s %s",
		         pidb->username.c_str(), S2A(sqlite3_column_text(pstmt, 0)));
		pstmt.finalize();
		system_services_broadcast_event(temp_buff);
		break;
	}
	case DB_NOTIFY_TYPE_FOLDER_CREATED: {
		auto n = static_cast<const DB_NOTIFY_FOLDER_CREATED *>(pdb_notify->pdata);
		folder_id = n->folder_id;
		parent_id = n->parent_id;
		mail_engine_add_notification_folder(pidb.get(), parent_id, folder_id);
		break;
	}
	case DB_NOTIFY_TYPE_MESSAGE_CREATED: {
		auto n = static_cast<const DB_NOTIFY_MESSAGE_CREATED *>(pdb_notify->pdata);
		folder_id = n->folder_id;
		message_id = n->message_id;
		mail_engine_add_notification_message(pidb.get(), folder_id, message_id);
		break;
	}
	case DB_NOTIFY_TYPE_FOLDER_DELETED: {
		auto n = static_cast<const DB_NOTIFY_FOLDER_DELETED *>(pdb_notify->pdata);
		folder_id = n->folder_id;
		mail_engine_delete_notification_folder(pidb.get(), folder_id);
		break;
	}
	case DB_NOTIFY_TYPE_MESSAGE_DELETED: {
		auto n = static_cast<const DB_NOTIFY_MESSAGE_DELETED *>(pdb_notify->pdata);
		folder_id = n->folder_id;
		message_id = n->message_id;
		mail_engine_delete_notification_message(pidb.get(), folder_id, message_id);
		break;
	}
	case DB_NOTIFY_TYPE_FOLDER_MODIFIED: {
		auto n = static_cast<const DB_NOTIFY_FOLDER_MODIFIED *>(pdb_notify->pdata);
		folder_id = n->folder_id;
		mail_engine_modify_notification_folder(pidb.get(), folder_id);
		break;
	}
	case DB_NOTIFY_TYPE_MESSAGE_MODIFIED: {
		auto n = static_cast<const DB_NOTIFY_MESSAGE_MODIFIED *>(pdb_notify->pdata);
		message_id = n->message_id;
		folder_id = n->folder_id;
		mail_engine_modify_notification_message(pidb.get(), folder_id, message_id);
		break;
	}
	case DB_NOTIFY_TYPE_FOLDER_MOVED: {
		auto n = static_cast<const DB_NOTIFY_FOLDER_MVCP *>(pdb_notify->pdata);
		folder_id = n->folder_id;
		parent_id = n->parent_id;
		mail_engine_move_notification_folder(pidb.get(), parent_id, folder_id);
		break;
	}
	case DB_NOTIFY_TYPE_MESSAGE_MOVED: {
		auto n = static_cast<const DB_NOTIFY_MESSAGE_MVCP *>(pdb_notify->pdata);
		folder_id = n->old_folder_id;
		message_id = n->old_message_id;
		mail_engine_delete_notification_message(pidb.get(), folder_id, message_id);
		folder_id = n->folder_id;
		message_id = n->message_id;
		mail_engine_add_notification_message(pidb.get(), folder_id, message_id);
		break;
	}
	case DB_NOTIFY_TYPE_FOLDER_COPIED: {
		auto n = static_cast<const DB_NOTIFY_FOLDER_MVCP *>(pdb_notify->pdata);
		folder_id = n->folder_id;
		parent_id = n->parent_id;
		if (mail_engine_add_notification_folder(pidb.get(), parent_id, folder_id))
			mail_engine_sync_contents(pidb.get(), folder_id);
		break;
	}
	case DB_NOTIFY_TYPE_MESSAGE_COPIED: {
		auto n = static_cast<const DB_NOTIFY_MESSAGE_MVCP *>(pdb_notify->pdata);
		folder_id = n->folder_id;
		message_id = n->message_id;
		mail_engine_add_notification_message(pidb.get(), folder_id, message_id);
		break;
	}
	}
}

void mail_engine_init(const char *default_charset,
	const char *default_timezone, const char *org_name,
	size_t table_size, BOOL b_async, BOOL b_wal,
	uint64_t mmap_size, int cache_interval, int mime_num)
{
	g_sequence_id = 0;
	gx_strlcpy(g_default_charset, default_charset, GX_ARRAY_SIZE(g_default_charset));
	gx_strlcpy(g_default_timezone, default_timezone, GX_ARRAY_SIZE(g_default_timezone));
	gx_strlcpy(g_org_name, org_name, GX_ARRAY_SIZE(g_org_name));
	g_async = b_async;
	g_wal = b_wal;
	g_mmap_size = mmap_size;
	g_table_size = table_size;
	g_mime_num = mime_num;
	g_cache_interval = cache_interval;
}

int mail_engine_run()
{
	if (SQLITE_OK != sqlite3_config(SQLITE_CONFIG_MULTITHREAD)) {
		printf("[mail_engine]: warning! fail to change "
			"to multiple thread mode for sqlite engine\n");
	}
	if (SQLITE_OK != sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0)) {
		printf("[mail_engine]: warning! fail to close"
			" memory statistic for sqlite engine\n");
	}
	if (FALSE == oxcmail_init_library(g_org_name,
		system_services_get_user_ids, system_services_get_username_from_id,
		system_services_ltag_to_lcid, system_services_lcid_to_ltag,
		system_services_charset_to_cpid, system_services_cpid_to_charset,
		system_services_mime_to_extension, system_services_extension_to_mime)) {
		printf("[mail_engine]: Failed to init oxcmail library\n");
		return -1;
	}
	g_mime_pool = MIME_POOL::create(g_mime_num, FILENUM_PER_MIME);
	if (NULL == g_mime_pool) {
		printf("[mail_engine]: Failed to init MIME pool\n");
		return -3;
	}
	g_alloc_mjson = mjson_allocator_init(g_table_size * 10);
	if (NULL == g_alloc_mjson) {
		printf("[mail_engine]: Failed to init buffer pool for mjson\n");
		return -4;
	}
	g_notify_stop = false;
	auto ret = pthread_create(&g_scan_tid, nullptr, midbme_scanwork, nullptr);
	if (ret != 0) {
		lib_buffer_free(g_alloc_mjson);
		printf("[mail_engine]: failed to create scan thread: %s\n", strerror(ret));
		return -5;
	}
	pthread_setname_np(g_scan_tid, "mail_engine");
	cmd_parser_register_command("M-LIST", mail_engine_mlist);
	cmd_parser_register_command("M-UIDL", mail_engine_muidl);
	cmd_parser_register_command("M-INST", mail_engine_minst);
	cmd_parser_register_command("M-DELE", mail_engine_mdele);
	cmd_parser_register_command("M-COPY", mail_engine_mcopy);
	cmd_parser_register_command("M-MAKF", mail_engine_mmakf);
	cmd_parser_register_command("M-REMF", mail_engine_mremf);
	cmd_parser_register_command("M-RENF", mail_engine_mrenf);
	cmd_parser_register_command("M-ENUM", mail_engine_menum);
	cmd_parser_register_command("M-CKFL", mail_engine_mckfl);
	cmd_parser_register_command("M-PING", mail_engine_mping);
	cmd_parser_register_command("P-OFST", mail_engine_pofst);
	cmd_parser_register_command("P-UNID", mail_engine_punid);
	cmd_parser_register_command("P-FDDT", mail_engine_pfddt);
	cmd_parser_register_command("P-SUBF", mail_engine_psubf);
	cmd_parser_register_command("P-UNSF", mail_engine_punsf);
	cmd_parser_register_command("P-SUBL", mail_engine_psubl);
	cmd_parser_register_command("P-SIML", mail_engine_psiml);
	cmd_parser_register_command("P-SIMU", mail_engine_psimu);
	cmd_parser_register_command("P-DELL", mail_engine_pdell);
	cmd_parser_register_command("P-DTLU", mail_engine_pdtlu);
	cmd_parser_register_command("P-SFLG", mail_engine_psflg);
	cmd_parser_register_command("P-RFLG", mail_engine_prflg);
	cmd_parser_register_command("P-GFLG", mail_engine_pgflg);
	cmd_parser_register_command("P-SRHL", mail_engine_psrhl);
	cmd_parser_register_command("P-SRHU", mail_engine_psrhu);
	exmdb_client_register_proc(reinterpret_cast<void *>(mail_engine_notification_proc));
	return 0;
}

void mail_engine_stop()
{
	g_notify_stop = true;
	pthread_kill(g_scan_tid, SIGALRM);
	pthread_join(g_scan_tid, NULL);
	g_hash_table.clear();
	g_mime_pool.reset();
	lib_buffer_free(g_alloc_mjson);
}

int mail_engine_get_param(int param)
{
	switch (param) {
	case MIDB_TABLE_SIZE:
	case MIDB_TABLE_USED:
		return g_hash_table.size();
	default:
		return -1;
	}
}
