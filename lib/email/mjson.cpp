// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <memory>
#include <string>
#include <utility>
#include <libHX/ctype_helper.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/mail.hpp>
#include <gromox/mime_pool.hpp>
#include <gromox/util.hpp>
#include <gromox/mjson.hpp>
#include <gromox/mail_func.hpp>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#define MAX_RFC822_DEPTH	5

#define MAX_DIGLEN			256*1024

#define DEF_MODE			S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH

using namespace gromox;

enum {
	RETRIEVE_NONE,
	RETRIEVE_TAG_FINDING,
	RETRIEVE_TAG_FOUND,
	RETRIEVE_TAG_END,
	RETRIEVE_VALUE_FINDING,
	RETRIEVE_VALUE_FOUND,
	RETRIEVE_VALUE_END,
	RETRIEVE_END
};

enum {
	RETRIEVE_TOKEN_QUOTA,
	RETRIEVE_TOKEN_SQUARE,
	RETRIEVE_TOKEN_BRACKET,
	RETRIEVE_TOKEN_DIGIT
};

enum {
	PARSE_STAT_NONE,
	PARSE_STAT_PROCESSING,
	PARSE_STAT_PROCESSED,
	PARSE_STAT_FINDITEM,
	PARSE_STAT_END
};

enum {
	TYPE_STRUCTURE,
	TYPE_MIMES
};

namespace {

struct ENUM_PARAM {
	const char *id;
	MJSON_MIME *pmime;
};

struct BUILD_PARAM {
	const char *filename = nullptr, *msg_path = nullptr;
	const char *storage_path = nullptr;
	int depth = 0;
	std::shared_ptr<MIME_POOL> ppool;
	BOOL build_result = false;
};

}

static void mjson_enum_delete(SIMPLE_TREE_NODE *pnode);

static void mjson_enum_id(SIMPLE_TREE_NODE *pnode, void *param);

static void mjson_enum_none(SIMPLE_TREE_NODE *pnode, void *param);
static BOOL mjson_record_value(MJSON *pjson, char *tag, char *value, size_t length);
static BOOL mjson_parse_array(MJSON *pjson, char *value, int length, int type);

static BOOL mjson_record_node(MJSON *pjson, char *value, int length, int type);

static int mjson_fetch_mime_structure(MJSON_MIME *pmime,
	const char *storage_path, const char *msg_filename, const char* charset,
	const char *email_charset, BOOL b_ext, char *buff, int length);

static int mjson_convert_address(char *address, const char *charset,
	const char *email_charset, char *buff, int length);

static BOOL mjson_check_ascii_printable(const char *astring);

static void mjson_convert_quoted_printable(const char *astring,
	char *out_stirng);

static void mjson_emum_rfc822(MJSON_MIME *pmime, BOOL *pb_found);

static void mjson_enum_build(MJSON_MIME *pmime, BUILD_PARAM *pbuild);

static int mjson_rfc822_fetch_internal(MJSON *pjson, const char *storage_path,
	const char *charset, BOOL b_ext, char *buff, int length);

LIB_BUFFER *mjson_allocator_init(size_t max_size)
{
	return lib_buffer_init(sizeof(MJSON_MIME), max_size, TRUE);
}

void mjson_allocator_free(LIB_BUFFER *pallocator)
{
	lib_buffer_free(pallocator);
}

/*
 *	@param
 *		pjson [in]			indicate the mjson object
 *		ppool [in]		    indicate the allocator for mime object
 */
MJSON::MJSON(LIB_BUFFER *p) : ppool(p)
{
#ifdef _DEBUG_UMTA
	if (p == nullptr)
		throw std::invalid_argument("[mail]: NULL pointer in mjson_init");
#endif
	simple_tree_init(&tree);
}

/*
 *	clear the mjson mime nodes from the tree and
 *  the head information of mail
 *	@param
 *		pjson [in]			indicate the mail object
 */
void MJSON::clear()
{
	auto pjson = this;
	auto pnode = simple_tree_get_root(&pjson->tree);
	if (NULL != pnode) {
		simple_tree_destroy_node(&pjson->tree, pnode, mjson_enum_delete);
	}
	
	if (-1 != pjson->message_fd) {
		close(pjson->message_fd);
		pjson->message_fd = -1;
	}
	pjson->uid = 0;
	pjson->path[0] = '\0';
	pjson->filename[0] = '\0';
	pjson->msgid[0] = '\0';
	pjson->from[0] = '\0';
	pjson->sender[0] = '\0';
	pjson->reply[0] = '\0';
	pjson->to[0] = '\0';
	pjson->cc[0] = '\0';
	pjson->inreply[0] = '\0';
	pjson->subject[0] = '\0';
	pjson->received[0] = '\0';
	pjson->date[0] = '\0';
	pjson->ref[0] = '\0';
	pjson->read = 0;
	pjson->replied = 0;
	pjson->forwarded = 0;
	pjson->unsent = 0;
	pjson->flag = 0;
	pjson->priority = 0;
	pjson->notification[0] = '\0';
	pjson->size = 0;
}

static void mjson_enum_delete(SIMPLE_TREE_NODE *pnode)
{
#ifdef _DEBUG_UMTA
	if (NULL == pnode) {
		debug_info("[mail]: NULL pointer in mjson_enum_delete");
		return;
	}
#endif
	lib_buffer_put(((MJSON_MIME*)pnode->pdata)->ppool, pnode->pdata);
}

MJSON::~MJSON()
{
	clear();
	simple_tree_free(&tree);
}

/*
 *	retrieve mjson object from mail digest string
 *	@param
 *		pjson [in]			indicate the mjson object
 *		digest_buff[in]		mail digest string buffer
 *		length				string buffer length
 *		path [in]			mail file path, can be NULL.
 *                          if you want to build rfc822
 *                          or seek file discritor in
 *                          message, path cannot be NULL.
 */
BOOL MJSON::retrieve(char *digest_buff,
	int length, const char *path)
{
	auto pjson = this;
	int bcount = 0, scount = 0;
	BOOL b_none, b_quota = false;
	int i, rstat;
	int last_pos;
	int token_type;
	char temp_tag[128];
	SIMPLE_TREE_NODE *pnode;
	
#ifdef _DEBUG_UMTA
	if (digest_buff == nullptr) {
		debug_info("[mail]: NULL pointer in mjson_retrieve");
		return FALSE;
	}
#endif
	
	last_pos = 0;
	clear();
	rstat = RETRIEVE_NONE;
    for (i=0; i<length; i++) {
		switch (rstat) {
		case RETRIEVE_NONE:
			/* get the first "{" in the buffer */
			if ('{' == digest_buff[i]) {
				rstat = RETRIEVE_TAG_FINDING;
			} else if (' ' != digest_buff[i] && '\t' != digest_buff[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_TAG_FINDING:
			if ('"' == digest_buff[i]) {
				rstat = RETRIEVE_TAG_FOUND;
				last_pos = i + 1;
			} else if ('}' == digest_buff[i]) {
				rstat = RETRIEVE_END;
			} else if (' ' != digest_buff[i] && '\t' != digest_buff[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_TAG_FOUND:
			if ('"' == digest_buff[i] && '\\' != digest_buff[i - 1]) {
				if (i < last_pos || i - last_pos > 127) {
					return FALSE;
				}
				rstat = RETRIEVE_TAG_END;
				memcpy(temp_tag, digest_buff + last_pos, i - last_pos);
				temp_tag[i - last_pos] = '\0';
			}
			break;
		case RETRIEVE_TAG_END:
			if (':' == digest_buff[i]) {
				rstat = RETRIEVE_VALUE_FINDING;
			} else if (' ' != digest_buff[i] && '\t' != digest_buff[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_VALUE_FINDING:
			if ('"' == digest_buff[i]) {
				rstat = RETRIEVE_VALUE_FOUND;
				last_pos = i + 1;
				token_type = RETRIEVE_TOKEN_QUOTA;
			} else if ('[' == digest_buff[i]) {
				rstat = RETRIEVE_VALUE_FOUND;
				last_pos = i ;
				b_quota = FALSE;
				scount = 0;
				bcount = 0;
				token_type = RETRIEVE_TOKEN_SQUARE;
			} else if ('{' == digest_buff[i]) {
				rstat = RETRIEVE_VALUE_FOUND;
				last_pos = i;
				b_quota = FALSE;
				scount = 0;
				bcount = 0;
				token_type = RETRIEVE_TOKEN_BRACKET;
			} else if (HX_isdigit(digest_buff[i])) {
				rstat = RETRIEVE_VALUE_FOUND;
				last_pos = i;
				token_type = RETRIEVE_TOKEN_DIGIT;
			} else if (' ' != digest_buff[i] && '\t' != digest_buff[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_VALUE_FOUND:
			switch (token_type) {
			case RETRIEVE_TOKEN_QUOTA:
				if ('"' == digest_buff[i] && '\\' != digest_buff[i - 1]) {
					if (i < last_pos) {
						return FALSE;
					}
					if (FALSE == mjson_record_value(pjson, temp_tag,
						digest_buff + last_pos, i - last_pos)) {
						return FALSE;
					}
					rstat = RETRIEVE_VALUE_END;
				}
				break;
			case RETRIEVE_TOKEN_SQUARE:
				if (FALSE == b_quota && 0 == scount &&
					0 == bcount && ']' == digest_buff[i]) {
					if (i + 1 < last_pos) {
						return FALSE;
					}
					if (FALSE == mjson_record_value(pjson, temp_tag,
						digest_buff + last_pos, i + 1 - last_pos)) {
						return FALSE;
					}
					rstat = RETRIEVE_VALUE_END;
				} 
				if ('"' == digest_buff[i] && '\\' != digest_buff[i - 1]) {
					b_quota = b_quota?FALSE:TRUE;
				}
				if (FALSE == b_quota) {
					if ('{' == digest_buff[i]) {
						bcount ++;
					} else if ('}' == digest_buff[i]) {
						bcount --;
					} else if ('[' == digest_buff[i]) {
						scount ++;
					} else if (']' == digest_buff[i]) {
						scount --;
					}
				}
				break;
			case RETRIEVE_TOKEN_BRACKET:
				if (FALSE == b_quota && 0 == bcount &&
					0 == scount && '}' == digest_buff[i]) {
					if (i + 1 < last_pos) {
						return FALSE;
					}
					if (FALSE == mjson_record_value(pjson, temp_tag,
						digest_buff + last_pos, i + 1 - last_pos)) {
						return FALSE;
					}
					rstat = RETRIEVE_VALUE_END;
				}
				if ('"' == digest_buff[i] && '\\' != digest_buff[i - 1]) {
					b_quota = b_quota?FALSE:TRUE;
				}
				if (FALSE == b_quota) {
					if ('{' == digest_buff[i]) {
						bcount ++;
					} else if ('}' == digest_buff[i]) {
						bcount --;
					} else if ('[' == digest_buff[i]) {
						scount ++;
					} else if (']' == digest_buff[i]) {
						scount --;
					}
				}
				break;
			case RETRIEVE_TOKEN_DIGIT:
				if (',' == digest_buff[i]) {
					if (i < last_pos) {
						return FALSE;
					}
					if (FALSE == mjson_record_value(pjson, temp_tag,
						digest_buff + last_pos, i - last_pos)) {
						return FALSE;
					}
					rstat = RETRIEVE_TAG_FINDING;
				} else if ('}' == digest_buff[i]) {
					if (i < last_pos) {
						return FALSE;
					}
					if (FALSE == mjson_record_value(pjson, temp_tag,
						digest_buff + last_pos, i - last_pos)) {
						return FALSE;
					}
					rstat = RETRIEVE_END;
				} else if (' ' == digest_buff[i] || '\t' == digest_buff[i]) {
					if (i < last_pos) {
						return FALSE;
					}
					if (FALSE == mjson_record_value(pjson, temp_tag,
						digest_buff + last_pos, i - last_pos)) {
						return FALSE;
					}
					rstat = RETRIEVE_VALUE_END;
				} else if (!HX_isdigit(digest_buff[i])) {
					return FALSE;
				}
				break;
			}
			break;
		case RETRIEVE_VALUE_END:
			if (',' == digest_buff[i]) {
				rstat = RETRIEVE_TAG_FINDING;
			} else if ('}' == digest_buff[i]) {
				rstat = RETRIEVE_END;
			} else if (' ' != digest_buff[i] && '\t' != digest_buff[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_END:
			if (' ' != digest_buff[i] && '\t' != digest_buff[i] &&
				'\0' != digest_buff[i]) {
				return FALSE;
			}
			break;
		}
	}
	
	if (RETRIEVE_END != rstat) {
		return FALSE;
	}
	
	pnode = simple_tree_get_root(&pjson->tree);
	if (NULL == pnode) {
		return FALSE;
	}
	/* check for NONE mime in tree */
	b_none = FALSE;
	simple_tree_enum_from_node(pnode, mjson_enum_none, &b_none);
	if (TRUE == b_none) {
		return FALSE;
	}
	
	if (NULL != path) {
		strcpy(pjson->path, path);
	}
	return TRUE;
}

static void mjson_enum_none(SIMPLE_TREE_NODE *pnode, void *param)
{
	
#ifdef _DEBUG_UMTA
    if (NULL == pnode || NULL == param) {
		debug_info("[mail]: NULL pointer in mjson_enum_none");
        return;
    }
#endif
	if (MJSON_MIME_NONE == ((MJSON_MIME*)pnode->pdata)->mime_type) {
		*(BOOL*)param = TRUE;
	}
}

void MJSON::enum_mime(MJSON_MIME_ENUM enum_func, void *param)
{
	auto pjson = this;
#ifdef _DEBUG_UMTA
	if (enum_func == nullptr) {
        debug_info("[mail]: NULL pointer in mjson_enum_mime");
        return;
    }
#endif
    simple_tree_enum_from_node(simple_tree_get_root(&pjson->tree),
        (SIMPLE_TREE_ENUM)enum_func, param);
}

size_t MJSON_MIME::get_length(unsigned int param) const
{
	auto pmime = this;
	switch (param) {
	case MJSON_MIME_HEAD:
		return (pmime->begin - pmime->head);
	case MJSON_MIME_CONTENT:
		return pmime->length;
	case MJSON_MIME_ENTIRE:
		return (pmime->begin + pmime->length - pmime->head);
	default:
		return 0;
	}
}

size_t MJSON_MIME::get_offset(unsigned int param) const
{
	auto pmime = this;
	switch (param) {
	case MJSON_MIME_HEAD:
		return pmime->head;
	case MJSON_MIME_CONTENT:
		return pmime->begin;
	default:
		return 0;
	}
}

/*
 *	get file descrition of mail file and seek pointer to location
 *	@param
 *		pjson [in]			indicate the mjson object
 *		id [in]				id string of mime
 *		whence				MJSON_MIME_HEAD
 *							MJSON_MIME_CONTENT
 */
int MJSON::seek_fd(const char *id, int whence)
{
	auto pjson = this;
	if ('\0' == pjson->path[0]) {
		return -1;
	}

	if (MJSON_MIME_HEAD != whence && MJSON_MIME_CONTENT != whence) {
		return -1;
	}
	auto pmime = pjson->get_mime(id);
	if (NULL == pmime) {
		return -1;
	}
	
	if (-1 == pjson->message_fd) {
		try {
			auto temp_path = std::string(pjson->path) + "/" + pjson->filename;
			pjson->message_fd = open(temp_path.c_str(), O_RDONLY);
		} catch (const std::bad_alloc &) {
			fprintf(stderr, "E-1476: ENOMEM\n");
		}
		if (-1 == pjson->message_fd) {
			return -1;
		}
	}
	
	switch (whence) {
	case MJSON_MIME_HEAD:
		lseek(pjson->message_fd, pmime->head, SEEK_SET);
		break;
	case MJSON_MIME_CONTENT:
		lseek(pjson->message_fd, pmime->begin, SEEK_SET);
		break;
	}
	return pjson->message_fd;
}

/*
 *	get mime from mjson object
 *	@param
 *		pmime [in]			indicate the mime object
 */
MJSON_MIME *MJSON::get_mime(const char *id)
{
	ENUM_PARAM enum_param = {id};
	simple_tree_enum_from_node(simple_tree_get_root(&tree), mjson_enum_id, &enum_param);
	return enum_param.pmime;
}

static void mjson_enum_id(SIMPLE_TREE_NODE *pnode, void *param)
{
	MJSON_MIME *pmime;
	ENUM_PARAM *penum_param;

#ifdef _DEBUG_UMTA
    if (NULL == pnode || NULL == param) {
        debug_info("[mail]: NULL pointer in mjson_enum_id");
        return;
    }
#endif
	
	penum_param = (ENUM_PARAM*)param;
	if (NULL != penum_param->pmime) {
		return;
	}
	
	pmime = (MJSON_MIME*)pnode->pdata;
	
	if (0 == strcmp(pmime->id, penum_param->id)) {
		penum_param->pmime = pmime;
	}
}

static BOOL mjson_record_value(MJSON *pjson, char *tag,
    char *value, size_t length)
{
	size_t temp_len;
	char temp_buff[32];
	
	if (0 == strcasecmp(tag, "file")) {
		if ('\0' == pjson->filename[0] && length < 128) {
			memcpy(pjson->filename, value, length);
			pjson->filename[length] = '\0';
		}
	} else if (0 == strcasecmp(tag, "uid")) {
		if (0 == pjson->uid && length < 16) {
			memcpy(temp_buff, value, length);
			temp_buff[length] = '\0';
			pjson->uid = strtol(temp_buff, nullptr, 0);
		}
	} else if (0 == strcasecmp(tag, "msgid")) {
		if ('\0' == pjson->msgid[0] && length <= sizeof(pjson->msgid)) {
			if (0 != decode64(value, length, pjson->msgid, &temp_len)) {
				pjson->msgid[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "from")) {
		if ('\0' == pjson->from[0] && length <= sizeof(pjson->from)) {
			if (0 != decode64(value, length, pjson->from, &temp_len)) {
				pjson->from[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "charset")) {
		if ('\0' == pjson->charset[0] && length < 32) {
			memcpy(pjson->charset, value, length);
			pjson->charset[length] = '\0';
		}
	} else if (0 == strcasecmp(tag, "sender")) {
		if ('\0' == pjson->sender[0] && length <= sizeof(pjson->sender)) {
			if (0 != decode64(value, length, pjson->sender, &temp_len)) {
				pjson->sender[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "reply")) {
		if ('\0' == pjson->reply[0] && length <= sizeof(pjson->reply)) {
			if (0 != decode64(value, length, pjson->reply, &temp_len)) {
				pjson->reply[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "to")) {
		if ('\0' == pjson->to[0] && length <= sizeof(pjson->to)) {
			if (0 != decode64(value, length, pjson->to, &temp_len)) {
				pjson->to[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "cc")) {
		if ('\0' == pjson->cc[0] && length <= sizeof(pjson->cc)) {
			if (0 != decode64(value, length, pjson->cc, &temp_len)) {
				pjson->cc[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "inreply")) {
		if ('\0' == pjson->inreply[0] && length <= sizeof(pjson->inreply)) {
			if (0 != decode64(value, length, pjson->inreply, &temp_len)) {
				pjson->inreply[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "subject")) {
		if ('\0' == pjson->subject[0] && length <= sizeof(pjson->subject)) {
			if (0 != decode64(value, length, pjson->subject, &temp_len)) {
				pjson->subject[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "received")) {
		if ('\0' == pjson->received[0] && length <= sizeof(pjson->received)) {
			if (0 != decode64(value, length, pjson->received, &temp_len)) {
				pjson->received[0] = '\0';
			} else {
				HX_strltrim(pjson->received);
			}
		}
	} else if (0 == strcasecmp(tag, "date")) {
		if ('\0' == pjson->date[0] && length <= sizeof(pjson->date)) {
			if (0 != decode64(value, length, pjson->date, &temp_len)) {
				pjson->date[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "notification")) {
		if ('\0' == pjson->notification[0] && length <= sizeof(pjson->notification)) {
			if (0 != decode64(value, length, pjson->notification, &temp_len)) {
				pjson->notification[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "read")) {
		if (0 == pjson->read && 1 == length) {
			if ('1' == value[0]) {
				pjson->read = 1;
			}
		}
	} else if (0 == strcasecmp(tag, "replied")) {
		if (0 == pjson->replied && 1 == length) {
			if ('1' == value[0]) {
				pjson->replied = 1;
			}
		}
	} else if (0 == strcasecmp(tag, "unsent")) {
		if (0 == pjson->unsent && 1 == length) {
			if ('1' == value[0]) {
				pjson->unsent = 1;
			}
		}
	} else if (0 == strcasecmp(tag, "forwarded")) {
		if (0 == pjson->forwarded && 1 == length) {
			if ('1' == value[0]) {
				pjson->forwarded = 1;
			}
		}
	} else if (0 == strcasecmp(tag, "flag")) {
		if (0 == pjson->flag && 1 == length) {
			if ('1' == value[0]) {
				pjson->flag = 1;
			}
		}
	} else if (0 == strcasecmp(tag, "priority")) {
		if (0 == pjson->priority && 1 == length) {
			if ('0' <= value[0] && value[0] <= '9') {
				pjson->priority = value[0] - '0';
			}
		}
	} else if (0 == strcasecmp(tag, "ref")) {
		if ('\0' == pjson->ref[0] && length <= sizeof(pjson->ref)) {
			if (0 != decode64(value, length, pjson->ref, &temp_len)) {
				pjson->ref[0] = '\0';
			}
		}
	} else if (0 == strcasecmp(tag, "structure")) {
		if (FALSE == mjson_parse_array(pjson, value, length, TYPE_STRUCTURE)) {
			return FALSE;
		}
	} else if (0 == strcasecmp(tag, "mimes")) {
		if (FALSE == mjson_parse_array(pjson, value, length, TYPE_MIMES)) {
			return FALSE;
		}
	} else if (0 == strcasecmp(tag, "size")) {
		if (0 == pjson->size && length <= 16) {
			memcpy(temp_buff, value, length);
			temp_buff[length] = '\0';
			pjson->size = strtoull(temp_buff, nullptr, 0);
		}
	}
	return TRUE;
}

static BOOL mjson_parse_array(MJSON *pjson, char *value, int length, int type)
{
	int rstat;
	int i, bcount, last_pos = 0;
	BOOL b_quota;
	
	rstat = PARSE_STAT_NONE;
	for (i=0; i<length; i++) {
		switch (rstat) {
		case PARSE_STAT_NONE:
			if ('[' == value[i]) {
				rstat = PARSE_STAT_FINDITEM;
			} else if (' ' != value[i] && '\t' != value[i]) {
				return FALSE;
			}
			break;
		case PARSE_STAT_FINDITEM:
			if ('{' == value[i]) {
				bcount = 1;
				last_pos = i + 1;
				b_quota = FALSE;
				rstat = PARSE_STAT_PROCESSING;
			} else if (']' == value[i]) {
				/* empty array like [] */
				rstat = PARSE_STAT_END;
			}  else if (' ' != value[i] && '\t' != value[i]) {
				return FALSE;
			}
			break;
		case PARSE_STAT_PROCESSING:
			if ('"' == value[i] && '\\' != value[i - 1]) {
				b_quota = b_quota?FALSE:TRUE;
			} else {
				if (FALSE == b_quota) {
					if ('{' == value[i]) {
						bcount ++;
					} else if ('}' == value[i]) {
						bcount --;
						if (0 == bcount) {
							if (i < last_pos) {
								return FALSE;
							}
							if (FALSE == mjson_record_node(pjson, 
								value + last_pos, i - last_pos, type)) {
								return FALSE;
							}
							rstat = PARSE_STAT_PROCESSED;
						}
					}
				}
			}
			break;
		case PARSE_STAT_PROCESSED:
			if (',' == value[i]) {
				rstat = PARSE_STAT_FINDITEM;
			} else if (']' == value[i]) {
				rstat = PARSE_STAT_END;
			} else if (' ' != value[i] && '\t' != value[i]) {
				return FALSE;
			}
			break;
		case PARSE_STAT_END:
			if (' ' != value[i] && '\t' != value[i]) {
				return FALSE;
			}
			break;
		}
	}
	
	return rstat == PARSE_STAT_END ? TRUE : false;
}

static BOOL mjson_record_node(MJSON *pjson, char *value, int length, int type)
{
	int rstat, j, last_pos = 0;
	size_t temp_len;
	BOOL b_digit;
	char temp_tag[128];
	char temp_buff[64];
	MJSON_MIME temp_mime;
	SIMPLE_TREE_NODE *pnode;
	
	memset(&temp_mime, 0, sizeof(temp_mime));
	temp_mime.ppool = pjson->ppool;
	if (TYPE_STRUCTURE == type) {
		temp_mime.mime_type = MJSON_MIME_MULTIPLE;
	} else {
		temp_mime.mime_type = MJSON_MIME_SINGLE;
	}
	rstat = RETRIEVE_TAG_FINDING;
	for (int i = 0; i < length; ++i) {
		switch (rstat) {
		case RETRIEVE_TAG_FINDING:
			if ('"' == value[i]) {
				rstat = RETRIEVE_TAG_FOUND;
				last_pos = i + 1;
			} else if (' ' != value[i] && '\t' != value[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_TAG_FOUND:
			if ('"' == value[i] && '\\' != value[i - 1]) {
				if (i < last_pos || i - last_pos > 127) {
					return FALSE;
				}
				rstat = RETRIEVE_TAG_END;
				memcpy(temp_tag, value + last_pos, i - last_pos);
				temp_tag[i - last_pos] = '\0';
			}
			break;
		case RETRIEVE_TAG_END:
			if (':' == value[i]) {
				rstat = RETRIEVE_VALUE_FINDING;
			} else if (' ' != value[i] && '\t' != value[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_VALUE_FINDING:
			if ('"' == value[i]) {
				rstat = RETRIEVE_VALUE_FOUND;
				b_digit = FALSE;
				last_pos = i + 1;
			} else if (HX_isdigit(value[i])) {
				rstat = RETRIEVE_VALUE_FOUND;
				b_digit = TRUE;
				last_pos = i;
			} else if (' ' != value[i] && '\t' != value[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_VALUE_FOUND:
			if (( FALSE == b_digit && 
				('"' == value[i] && '\\' != value[i - 1])) ||
				(TRUE == b_digit && 
				(' ' == value[i] || '\t' == value[i] || 
				',' == value[i] || i == length - 1))) {
				if (FALSE == b_digit) {
					temp_len = i - last_pos;
				} else {
					temp_len = i + 1 - last_pos;
				}
				if (0 == strcasecmp(temp_tag, "id") && temp_len < 64) {
					memcpy(temp_mime.id, value + last_pos, temp_len);
					temp_mime.id[temp_len] = '\0';
				} else if (0 == strcasecmp(temp_tag, "ctype") && temp_len < 256) {
					memcpy(temp_mime.ctype, value + last_pos, temp_len);
					temp_mime.ctype[temp_len] = '\0';
				} else if (0 == strcasecmp(temp_tag, "encoding") && temp_len < 32) {
					memcpy(temp_mime.encoding, value + last_pos, temp_len);
					temp_mime.encoding[temp_len] = '\0';
				} else if(0 == strcasecmp(temp_tag, "charset") && temp_len < 32) {
					memcpy(temp_mime.charset, value + last_pos, temp_len);
					temp_mime.charset[temp_len] = '\0';
				} else if (0 == strcasecmp(temp_tag, "filename") &&
					temp_len < sizeof(temp_mime.filename)) {
					if (0 != decode64(value + last_pos, temp_len, 
						temp_mime.filename, &temp_len)) {
						temp_mime.filename[0] = '\0';
					}
				} else if (0 == strcasecmp(temp_tag, "cid") &&
					temp_len < sizeof(temp_mime.cid)) {
					if (0 != decode64(value + last_pos, temp_len,
						temp_mime.cid, &temp_len)) {
						temp_mime.cid[0] = '\0';
					}
				} else if (0 == strcasecmp(temp_tag, "cntl") &&
					temp_len < sizeof(temp_mime.cntl)) {
					if (0 != decode64(value + last_pos, temp_len,
						temp_mime.cntl, &temp_len)) {
						temp_mime.cntl[0] = '\0';
					}
				} else if (0 == strcasecmp(temp_tag, "cntdspn") &&
					temp_len < sizeof(temp_mime.cntdspn)) {
					memcpy(temp_mime.cntdspn, value + last_pos, temp_len);
					temp_mime.cntdspn[temp_len] = '\0';
				} else if (0 == strcasecmp(temp_tag, "head") && temp_len < 16) {
					memcpy(temp_buff, value + last_pos, temp_len);
					temp_buff[temp_len] = '\0';
					temp_mime.head = strtoull(temp_buff, nullptr, 0);
				} else if (0 == strcasecmp(temp_tag, "begin")) {
					memcpy(temp_buff, value + last_pos, temp_len);
					temp_buff[temp_len] = '\0';
					temp_mime.begin = strtoull(temp_buff, nullptr, 0);
				} else if (0 == strcasecmp(temp_tag, "length")) {
					memcpy(temp_buff, value + last_pos, temp_len);
					temp_buff[temp_len] = '\0';
					temp_mime.length = strtoull(temp_buff, nullptr, 0);
				}
				if (TRUE == b_digit && ',' == value[i]) {
					rstat = RETRIEVE_TAG_FINDING;
				} else {
					rstat = RETRIEVE_VALUE_END;
				}
			}
			break;
		case RETRIEVE_VALUE_END:
			if (',' == value[i]) {
				rstat = RETRIEVE_TAG_FINDING;
			}  else if (' ' != value[i] && '\t' != value[i]) {
				return FALSE;
			}
			break;
		}
	}
	
	
	if ('\0' == temp_mime.ctype[0]) {
		strcpy(temp_mime.ctype, "application/octet-stream");
	}
	
	temp_len = strlen(temp_mime.filename);

	/* for some MUA sunch as Foxmail, use application/octet-stream
	   as the Content-Type, so make the revision for these mimes
	*/
    if (temp_len > 4 && 0 == strncasecmp(temp_mime.filename + temp_len - 4,
		".eml", 4) && 0 != strcasecmp(temp_mime.ctype, "message/rfc822")) {
        strcpy(temp_mime.ctype, "message/rfc822");
    }
	
	pnode = simple_tree_get_root(&pjson->tree);
	if (NULL == pnode) {
		auto pmime = lib_buffer_get<MJSON_MIME>(pjson->ppool);
		pmime->node.pdata = pmime;
		pmime->ppool = pjson->ppool;
		pmime->mime_type = MJSON_MIME_NONE;
		simple_tree_set_root(&pjson->tree, &pmime->node);
	}
	pnode = simple_tree_get_root(&pjson->tree);
	if (NULL == pnode) {
		return FALSE;
	}
	auto pmime = static_cast<MJSON_MIME *>(pnode->pdata);
	
	if ('\0' == temp_mime.id[0]) {
		if (MJSON_MIME_NONE != pmime->mime_type) {
			return FALSE;
		}
		memcpy(reinterpret_cast<char *>(pmime) + sizeof(SIMPLE_TREE_NODE), 
		       reinterpret_cast<char *>(&temp_mime) + sizeof(SIMPLE_TREE_NODE),
			sizeof(MJSON_MIME) - sizeof(SIMPLE_TREE_NODE));
		return TRUE;
	} else {
		temp_len = strlen(temp_mime.id);
		memcpy(temp_buff, temp_mime.id, temp_len + 1);
		last_pos = 0;
		for (size_t i = 0; i <= temp_len; ++i) {
			if ('.' == temp_buff[i] || '\0' == temp_buff[i]) {
				temp_buff[i] = '\0';
				int offset = strtol(temp_buff + last_pos, nullptr, 0);
				pnode = simple_tree_node_get_child(&pmime->node);
				if (NULL == pnode) {
					pnode = &pmime->node;
					pmime = lib_buffer_get<MJSON_MIME>(pjson->ppool);
					pmime->node.pdata = pmime;
					pmime->ppool = pjson->ppool;
					pmime->mime_type = MJSON_MIME_NONE;
					if (FALSE == simple_tree_add_child(&pjson->tree,
						pnode, &pmime->node, SIMPLE_TREE_ADD_LAST)) {
						lib_buffer_put(pjson->ppool, pmime);
						return FALSE;
					}
				} else {
					pmime = (MJSON_MIME*)pnode->pdata;
				}
				
				for (j=1; j<offset; j++) {
					pnode = simple_tree_node_get_sibling(&pmime->node);
					if (NULL == pnode) {
						pnode = &pmime->node;
						pmime = lib_buffer_get<MJSON_MIME>(pjson->ppool);
						pmime->node.pdata = pmime;
						pmime->ppool = pjson->ppool;
						pmime->mime_type = MJSON_MIME_NONE;
						if (!simple_tree_insert_sibling(&pjson->tree,
							pnode, &pmime->node, SIMPLE_TREE_INSERT_AFTER)) {
							lib_buffer_put(pjson->ppool, pmime);
							return FALSE;
						}
					} else {
						pmime = (MJSON_MIME*)pnode->pdata;
					}
				}
				last_pos = i + 1;
			}
		}
		
		if (MJSON_MIME_NONE != pmime->mime_type) {
			return FALSE;
		}
		memcpy(reinterpret_cast<char *>(pmime) + sizeof(SIMPLE_TREE_NODE), 
		       reinterpret_cast<char *>(&temp_mime) + sizeof(SIMPLE_TREE_NODE),
			sizeof(MJSON_MIME) - sizeof(SIMPLE_TREE_NODE));
		return TRUE;
	}	
} 

int MJSON::fetch_structure(const char *charset, BOOL b_ext, char *buff, int length)
{
	auto pjson = this;
	int ret_len;
	MJSON_MIME *pmime;
	SIMPLE_TREE_NODE *pnode;

#ifdef _DEBUG_UMTA
	if (buff == nullptr) {
		debug_info("[mail]: NULL pointer in mjson_fetch_structure");
		return -1;
	}
#endif


	pnode = simple_tree_get_root(&pjson->tree);
	if (NULL == pnode) {
		return -1;
	}
	
	pmime = (MJSON_MIME*)pnode->pdata;
	ret_len = mjson_fetch_mime_structure(pmime, NULL, NULL, charset,
				pjson->charset, b_ext, buff, length);
	if (ret_len == -1)
		return -1;
	buff[ret_len] = '\0';
	return ret_len;
}

static int mjson_fetch_mime_structure(MJSON_MIME *pmime,
	const char *storage_path, const char *msg_filename, const char *charset,
	const char *email_charset, BOOL b_ext, char *buff, int length)
{
	int offset;
	int ret_len;
	BOOL b_space;
	size_t ecode_len;
	char ctype[256];
	char *psubtype;
	char temp_buff[2048];
	SIMPLE_TREE_NODE *pnode;

#ifdef _DEBUG_UMTA
	if (NULL == pmime || NULL == buff) {
		debug_info("[mail]: NULL pointer in mjson_fetch_mime_structure");
		return -1;
	}
#endif
	
	offset = 0;
	
 FETCH_STRUCTURE_LOOP:
	gx_strlcpy(ctype, pmime->ctype, GX_ARRAY_SIZE(ctype));
	HX_strupper(ctype);
	psubtype = strchr(ctype, '/');
	if (NULL == psubtype) {
		psubtype = deconst("NIL");
	} else {
		*psubtype = '\0';
		psubtype ++;
	}
	
	if (MJSON_MIME_SINGLE == pmime->mime_type) {
		offset += gx_snprintf(buff + offset, length - offset,
					"(\"%s\" \"%s\"", ctype, psubtype);
		if ('\0' != pmime->charset[0] || '\0' != pmime->filename[0]) {
			buff[offset] = ' ';
			offset ++;
			buff[offset] = '(';
			offset ++;
			
			b_space = FALSE;
			if ('\0' != pmime->charset[0]) {
				offset += gx_snprintf(buff + offset, length - offset,
							"\"CHARSET\" \"%s\"", pmime->charset);
				b_space = TRUE;
			} else {
				if (0 == strcasecmp(ctype, "text") &&
					'\0' != email_charset[0]) {
					offset += gx_snprintf(buff + offset, length - offset,
							"\"CHARSET\" \"%s\"", email_charset);
					b_space = TRUE;
				}
			}
			
			if ( '\0' != pmime->filename[0]) {
				if (TRUE == b_space) {
					buff[offset] = ' ';
					offset ++;
				}
				if (TRUE == mjson_check_ascii_printable(pmime->filename)) {
					mjson_convert_quoted_printable(pmime->filename, temp_buff);
					offset += gx_snprintf(buff + offset, length - offset,
								"\"NAME\" \"%s\"", temp_buff);
				} else {
					offset += gx_snprintf(buff + offset, length - offset,
								"\"NAME\" \"=?%s?b?",
								('\0' != email_charset[0])?email_charset:charset);
					if (0 != encode64(pmime->filename, strlen(pmime->filename),
						buff + offset, length - offset, &ecode_len)) {
						return -1;
					}
					offset += ecode_len;
					memcpy(buff + offset, "?=\"", 3);
					offset += 3;
				}
			}
			
			buff[offset] = ')';
			offset ++;
		} else {
			memcpy(buff + offset, " NIL", 4);
			offset += 4;
		}
		
		if ('\0' != pmime->cid[0] &&
			TRUE == mjson_check_ascii_printable(pmime->cid)) {
			mjson_convert_quoted_printable(pmime->cid, temp_buff);
			offset += gx_snprintf(buff + offset, length - offset,
						" \"%s\"", temp_buff);
		} else {
			memcpy(buff + offset, " NIL", 4);
			offset += 4;
		}
		
		/* body description */
		memcpy(buff + offset, " NIL", 4);
		offset += 4;
		
		if ('\0' != pmime->encoding[0]) {
			if (NULL != storage_path && NULL != msg_filename &&
				0 == strcasecmp(pmime->ctype, "MESSAGE/RFC822")) {
				/* revision for APPLE device */
				if (0 == strcasecmp(pmime->encoding, "base64") ||
					0 == strcasecmp(pmime->encoding, "quoted-printable")) {
					offset += gx_snprintf(buff + offset, length - offset,
								" \"7bit\"");
				} else {
					offset += gx_snprintf(buff + offset, length - offset,
								" \"%s\"", pmime->encoding);
				}
			} else {
				offset += gx_snprintf(buff + offset, length - offset,
							" \"%s\"", pmime->encoding);
			}
		} else {
			memcpy(buff + offset, " NIL", 4);
			offset += 4;
		}
		
		if (NULL != storage_path && NULL != msg_filename &&
			0 == strcasecmp(pmime->ctype, "MESSAGE/RFC822") &&
			(0 == strcasecmp(pmime->encoding, "base64") ||
			0 == strcasecmp(pmime->encoding, "quoted-printable"))) {
			char temp_path[256];
			struct stat node_stat;
			
			if ('\0' == msg_filename[0]) {
				snprintf(temp_path, 256, "%s/%s", storage_path,
					pmime->id);
			} else {
				snprintf(temp_path, 256, "%s/%s.%s", storage_path,
					msg_filename, pmime->id);
			}
			
			if (0 == stat(temp_path, &node_stat)) {
				offset += gx_snprintf(buff + offset, length - offset,
				          " %llu", static_cast<unsigned long long>(node_stat.st_size));
			} else {
				memcpy(buff + offset, " NIL", 4);
				offset += 4;
			}
		} else {
			offset += gx_snprintf(buff + offset, length - offset,
			          " %zu", pmime->length);
		}
					
		if (0 == strcasecmp(ctype, "TEXT")) {
			/* body lines */
			memcpy(buff + offset, " NIL", 4);
			offset += 4;
		}
		
		if (NULL != storage_path && NULL != msg_filename &&
			0 == strcasecmp(pmime->ctype, "MESSAGE/RFC822")) {
			int fd;
			int envl_len;
			int body_len;
			char temp_path[256];
			struct stat node_stat;
			char *digest_buff;
			
			if ('\0' == msg_filename[0]) {
				snprintf(temp_path, 256, "%s/%s.dgt", storage_path,
					pmime->id);
			} else {
				snprintf(temp_path, 256, "%s/%s.%s.dgt", storage_path,
					msg_filename, pmime->id);
			}
			if (0 != stat(temp_path, &node_stat) ||
				0 == S_ISREG(node_stat.st_mode) ||
				node_stat.st_size > MAX_DIGLEN) {
				goto RFC822_FAILURE;
			}
			digest_buff = static_cast<char *>(malloc(MAX_DIGLEN));
			if (NULL == digest_buff) {
				goto RFC822_FAILURE;
			}
			fd = open(temp_path, O_RDONLY);
			if (-1 == fd) {
				free(digest_buff);
				goto RFC822_FAILURE;
			}
			
			if (::read(fd, digest_buff, node_stat.st_size) != node_stat.st_size) {
				free(digest_buff);
				close(fd);
				goto RFC822_FAILURE;
			}
			
			close(fd);
			MJSON temp_mjson(pmime->ppool);
			if (!temp_mjson.retrieve(digest_buff, node_stat.st_size, storage_path)) {
				free(digest_buff);
				goto RFC822_FAILURE;
			}
			free(digest_buff);
			
			buff[offset] = ' ';
			envl_len = temp_mjson.fetch_envelope(charset,
						buff + offset + 1, length - offset - 1);
			if (-1 == envl_len) {
				goto RFC822_FAILURE;
			}
			
			buff[offset + 1 + envl_len] = ' ';
			
			body_len = mjson_rfc822_fetch_internal(&temp_mjson, storage_path,
						charset, b_ext, buff + offset + envl_len + 2,
						length - offset - envl_len - 2);
			if (-1 == body_len) {
				goto RFC822_FAILURE;
			}
			offset += envl_len + body_len + 2;
			goto RFC822_SUCCESS;
		}
		
 RFC822_FAILURE:
		if (TRUE == b_ext) {

			/* body MD5 */
			
			
			memcpy(buff + offset, " NIL", 4);
			offset += 4;
			
		
			if ('\0' != pmime->cntdspn[0]) {
				offset += gx_snprintf(buff + offset, length - offset,
							" (\"%s\" NIL)", pmime->cntdspn);
			} else {
				memcpy(buff + offset, " NIL", 4);
				offset += 4;
			}
			
			/* body language */
			
			memcpy(buff + offset, " NIL", 4);
			offset += 4;
			
			if ('\0' != pmime->cntl[0] &&
				TRUE == mjson_check_ascii_printable(pmime->cntl)) {
				mjson_convert_quoted_printable(pmime->cntl, temp_buff);
				offset += gx_snprintf(buff + offset, length - offset,
							" \"%s\"", temp_buff);
			} else {
				memcpy(buff + offset, " NIL", 4);
				offset += 4;
			}
		}
		
 RFC822_SUCCESS:
		buff[offset] = ')';
		offset ++;
	} else if (MJSON_MIME_MULTIPLE == pmime->mime_type) {
		buff[offset] = '(';
		offset ++;
		pnode = simple_tree_node_get_child(&pmime->node);
		if (NULL == pnode) {
			return -1;
		}
		ret_len = mjson_fetch_mime_structure((MJSON_MIME*)pnode->pdata,
					storage_path, msg_filename, charset, email_charset,
					b_ext, buff + offset, length - offset);
		if (-1 == ret_len) {
			return -1;
		}
		offset += ret_len;
		offset += gx_snprintf(buff + offset, length - offset,
					" \"%s\"", psubtype);
		if (TRUE == b_ext) {
			memcpy(buff + offset, " NIL NIL NIL", 12);
			offset += 12;
		}
		buff[offset] = ')';
		offset ++;
	} else {
		return -1;
	}
	
	pnode = simple_tree_node_get_sibling(&pmime->node);
	if (NULL != pnode) {
		pmime = (MJSON_MIME*)pnode->pdata;
		goto FETCH_STRUCTURE_LOOP;
	}
	
	if (offset >= length) {
		return -1;
	}
	
	return offset;
}

static int mjson_convert_address(char *address, const char *charset,
	const char *email_charset, char *buff, int length)
{
	int offset;
	size_t temp_len;
	size_t ecode_len;
	EMAIL_ADDR email_addr;
	char temp_buff[2048];
	char temp_address[1024];
	ENCODE_STRING encode_string;
	
	offset = 0;
	temp_len = strlen(address);
	if (0 == strncmp(address, "=?", 2) &&
		0 == strncmp(address + temp_len - 2, "?=", 2)) {
		parse_mime_encode_string(address, temp_len, &encode_string);
		if (0 == strcasecmp(encode_string.encoding, "base64") &&
			0 != strcasecmp(encode_string.charset, "default")) {
			if (0 == decode64(encode_string.title, strlen(encode_string.title),
				temp_address, &temp_len)) {
				email_charset = encode_string.charset;
			} else {
				gx_strlcpy(temp_address, address, GX_ARRAY_SIZE(temp_address));
			}
		} else if (0 == strcasecmp(encode_string.encoding,
			"quoted-printable") && 0 != strcasecmp(
			encode_string.charset, "default")) {
			qp_decode(temp_address, encode_string.title,
				strlen(encode_string.title));
			email_charset = encode_string.charset;
		} else {
			gx_strlcpy(temp_address, address, GX_ARRAY_SIZE(temp_address));
		}
	} else {
		gx_strlcpy(temp_address, address, GX_ARRAY_SIZE(temp_address));
	}
	
	parse_mime_addr(&email_addr, temp_address);
	if ('\0' != email_addr.display_name[0]) {
		if (TRUE == mjson_check_ascii_printable(email_addr.display_name)) {
			mjson_convert_quoted_printable(email_addr.display_name, temp_buff);
			offset += gx_snprintf(buff + offset, length - offset,
						"(\"%s\"", temp_buff);
		} else {
			offset += gx_snprintf(buff + offset, length - offset, "(\"=?%s?b?",
						('\0' != email_charset[0])?email_charset:charset);
			if (0 != encode64(email_addr.display_name,
				strlen(email_addr.display_name), buff + offset,
				length - offset, &ecode_len)) {
				return -1;
			}
			offset += ecode_len;
			memcpy(buff + offset, "?=\"", 3);
			offset += 3;
		}
	} else {
		memcpy(buff + offset, "(NIL", 4);
		offset += 4;
	}
	
	/* at-domain-list */
	memcpy(buff + offset, " NIL", 4);
	offset += 4;
	
	if ('\0' != email_addr.local_part[0] &&
		TRUE == mjson_check_ascii_printable(email_addr.local_part)) {
		mjson_convert_quoted_printable(email_addr.local_part, temp_buff);
		offset += gx_snprintf(buff + offset, length - offset,
					" \"%s\"", temp_buff);
	} else {
		memcpy(buff + offset, " NIL", 4);
		offset += 4;
	}

	if ('\0' != email_addr.domain[0] &&
		TRUE == mjson_check_ascii_printable(email_addr.domain)) {
		mjson_convert_quoted_printable(email_addr.domain, temp_buff);
		offset += gx_snprintf(buff + offset, length - offset,
					" \"%s\")", temp_buff);
	} else {
		memcpy(buff + offset, " NIL)", 5);
		offset += 5;
	}

	if (offset >= length) {
		return -1;
	}
	
	return offset;
}

int MJSON::fetch_envelope(const char *charset, char *buff, int length)
{
	auto pjson = this;
	int offset;
	int i, len;
	int ret_len;
	int tmp_len;
	int last_pos;
	size_t ecode_len;
	BOOL b_quoted;
	BOOL b_bracket;
	char temp_buff[2048];


#ifdef _DEBUG_UMTA
	if (NULL == pjson || NULL == buff) {
		debug_info("[mail]: NULL pointer in mjson_fetch_envelope");
		return -1;
	}
#endif
	
	
	buff[0] = '(';
	offset = 1;
	
	if ('\0' != pjson->date[0] &&
		TRUE == mjson_check_ascii_printable(pjson->date)) {
		mjson_convert_quoted_printable(pjson->date, temp_buff);
		offset += gx_snprintf(buff + offset, length - offset,
					"\"%s\"", temp_buff);
	} else {
		memcpy(buff + offset, "NIL", 3);
		offset += 3;
	}
	
	if ('\0' != pjson->subject[0]) {
		if (TRUE == mjson_check_ascii_printable(pjson->subject)) {
			mjson_convert_quoted_printable(pjson->subject, temp_buff);
			offset += gx_snprintf(buff + offset, length - offset,
						" \"%s\"", temp_buff);
		} else {
			offset += gx_snprintf(buff + offset, length - offset, " \"=?%s?b?",
						('\0' != pjson->charset[0])?pjson->charset:charset);
			if (0 != encode64(pjson->subject, strlen(pjson->subject),
				buff + offset, length - offset, &ecode_len)) {
				return -1;
			}
			offset += ecode_len;
			memcpy(buff + offset, "?=\"", 3);
			offset += 3;
		}
	} else {
		memcpy(buff + offset, " NIL", 4);
		offset += 4;
	}
	
	buff[offset] = ' ';
	offset ++;
	buff[offset] = '(';
	offset ++;
	ret_len = mjson_convert_address(pjson->from, charset, pjson->charset,
				buff + offset, length - offset);
	if (-1 == ret_len) {
		return -1;
	}
	offset += ret_len;
	buff[offset] = ')';
	offset ++;
	
	buff[offset] = ' ';
	offset ++;
	buff[offset] = '(';
	offset ++;
	if (strlen(pjson->sender) > 0) { 
		ret_len = mjson_convert_address(pjson->sender, charset, pjson->charset,
					buff + offset, length - offset);
	} else {
		ret_len = mjson_convert_address(pjson->from, charset, pjson->charset,
					buff + offset, length - offset);
	}
	if (-1 == ret_len) {
		return -1;
	}
	offset += ret_len;
	buff[offset] = ')';
	offset ++;
	
	buff[offset] = ' ';
	offset ++;
	buff[offset] = '(';
	offset ++;
	if (strlen(pjson->sender) > 0) { 
		ret_len = mjson_convert_address(pjson->reply, charset, pjson->charset,
					buff + offset, length - offset);
	} else {
		ret_len = mjson_convert_address(pjson->from, charset, pjson->charset,
					buff + offset, length - offset);
	}
	if (-1 == ret_len) {
		return -1;
	}
	offset += ret_len;
	buff[offset] = ')';
	offset ++;
	
	len = strlen(pjson->to);
	last_pos = 0;
	b_quoted = FALSE;
	b_bracket = FALSE;
	for (i=0; i<=len; i++) {
		if ('"' == pjson->to[i]) {
			b_quoted = b_quoted?FALSE:TRUE;
		} else if (',' == pjson->to[i] || ';' == pjson->to[i] ||
			'\0' == pjson->to[i]) {
			tmp_len = i - last_pos;
			if (FALSE == b_quoted && tmp_len < 1024 && tmp_len > 0) {
				buff[offset] = ' ';
				offset ++;
				if (FALSE == b_bracket) {
					buff[offset] = '(';
					offset ++;
					b_bracket = TRUE;
				}
				memcpy(temp_buff, pjson->to + last_pos, tmp_len);
				temp_buff[tmp_len] = '\0';
				ret_len = mjson_convert_address(temp_buff, charset,
							pjson->charset, buff + offset, length - offset);
				if (-1 == ret_len) {
					return -1;
				}
				offset += ret_len;
				last_pos = i + 1;
			}
		}
	}
	
	if (FALSE == b_bracket) {
		memcpy(buff + offset, " NIL", 4);
		offset += 4;
	} else {
		buff[offset] = ')';
		offset ++;
	}
	
	
	len = strlen(pjson->cc);
	last_pos = 0;
	b_quoted = FALSE;
	b_bracket = FALSE;
	for (i=0; i<=len; i++) {
		if ('"' == pjson->cc[i]) {
			b_quoted = b_quoted?FALSE:TRUE;
		} else if (',' == pjson->cc[i] || ';' == pjson->cc[i] ||
			'\0' == pjson->cc[i]) {
			tmp_len = i - last_pos;
			if (FALSE == b_quoted && tmp_len < 1024 && tmp_len > 0) {
				buff[offset] = ' ';
				offset ++;
				if (FALSE == b_bracket) {
					buff[offset] = '(';
					offset ++;
					b_bracket = TRUE;
				}
				memcpy(temp_buff, pjson->cc + last_pos, tmp_len);
				temp_buff[tmp_len] = '\0';
				ret_len = mjson_convert_address(temp_buff, charset,
							pjson->charset, buff + offset, length - offset);
				if (-1 == ret_len) {
					return -1;
				}
				offset += ret_len;
				last_pos = i + 1;
			}
		}
	}
	
	if (FALSE == b_bracket) {
		memcpy(buff + offset, " NIL", 4);
		offset += 4;
	} else {
		buff[offset] = ')';
		offset ++;
	}
	
	/* bcc */
	memcpy(buff + offset, " NIL", 4);
	offset += 4;
	
	if ('\0' != pjson->inreply[0] &&
		TRUE == mjson_check_ascii_printable(pjson->inreply)) {
		mjson_convert_quoted_printable(pjson->inreply, temp_buff);
		offset += gx_snprintf(buff + offset, length - offset,
					" \"%s\"", temp_buff);
	} else {
		memcpy(buff + offset, " NIL", 4);
		offset += 4;
	}
	
	if ('\0' != pjson->msgid[0] &&
		TRUE == mjson_check_ascii_printable(pjson->msgid)) {
		mjson_convert_quoted_printable(pjson->msgid, temp_buff);
		offset += gx_snprintf(buff + offset, length - offset,
					" \"%s\"", temp_buff);
	} else {
		memcpy(buff + offset, " NIL", 4);
		offset += 4;
	}
	
	buff[offset] = ')';
	offset ++;
	buff[offset] = '\0';
	
	if (offset >= length) {
		return -1;
	}
	
	return offset;
}

static BOOL mjson_check_ascii_printable(const char *astring)
{
	int i, len;
	
	len = strlen(astring);
	
	for (i=0; i<len; i++) {
		if (astring[i] < 0x20 || astring[i] > 0x7E) {
			return FALSE;
		}
	}
	return TRUE;
}

static void mjson_convert_quoted_printable(const char *astring,
	char *out_string)
{
	int i, j, len;
	
	len = strlen(astring) + 1;
	
	for (i=0,j=0; i<len; i++,j++) {
		if ('"' == astring[i] || '\\' == astring[i]) {
			out_string[j] = '\\';
			j ++;
		}
		out_string[j] = astring[i];
	}

}

static void mjson_emum_rfc822(MJSON_MIME *pmime, BOOL *pb_found)
{
	if (FALSE == *pb_found && 0 == strcasecmp(pmime->ctype,
		"message/rfc822")) {
		*pb_found = TRUE;
	}
}

BOOL MJSON::rfc822_check()
{
	BOOL b_found = false;
	enum_mime(reinterpret_cast<MJSON_MIME_ENUM>(mjson_emum_rfc822), &b_found);
	return b_found;
}

static void mjson_enum_build(MJSON_MIME *pmime, BUILD_PARAM *pbuild)
{
	int fd;
	size_t length1;
	char msg_path[256];
	char dgt_path[256];
	char temp_path[256];
	
	if (FALSE == pbuild->build_result || pbuild->depth > MAX_RFC822_DEPTH ||
		0 != strcasecmp(pmime->ctype, "message/rfc822")) {
		return;
	}
	
	snprintf(temp_path, 256, "%s/%s", pbuild->msg_path, pbuild->filename);
	if (1 == pbuild->depth) {
		snprintf(msg_path, 256, "%s/%s", pbuild->storage_path, pmime->id);
		snprintf(dgt_path, 256, "%s/%s.dgt", pbuild->storage_path, pmime->id);
	} else {
		snprintf(msg_path, 256, "%s/%s.%s", pbuild->storage_path,
			pbuild->filename, pmime->id);
		snprintf(dgt_path, 256, "%s/%s.%s.dgt", pbuild->storage_path,
			pbuild->filename, pmime->id);
	}
		
	
	fd = open(temp_path, O_RDONLY);
	
	if (-1 == fd) {
		pbuild->build_result = FALSE;
		return;
	}
	
	auto length = pmime->get_length(MJSON_MIME_CONTENT);
	std::unique_ptr<char[], stdlib_delete> pbuff(static_cast<char *>(malloc(strange_roundup(length - 1, 64 * 1024))));
	if (NULL == pbuff) {
		close(fd);
		pbuild->build_result = FALSE;
		return;
	}
	
	if (lseek(fd, pmime->get_offset(MJSON_MIME_CONTENT), SEEK_SET) < 0)
		fprintf(stderr, "E-1430: lseek: %s\n", strerror(errno));
	auto rdlen = ::read(fd, pbuff.get(), length);
	if (rdlen < 0 || static_cast<size_t>(rdlen) != length) {
		close(fd);
		pbuild->build_result = FALSE;
		return;
	}
	close(fd);
	
	if (0 == strcasecmp(pmime->encoding, "base64")) {
		std::unique_ptr<char[], stdlib_delete> pbuff1(static_cast<char *>(malloc(strange_roundup(length - 1, 64 * 1024))));
		if (NULL == pbuff1) {
			pbuild->build_result = FALSE;
			return;
		}
		if (decode64_ex(pbuff.get(), length, pbuff1.get(), length, &length1) != 0) {
			pbuild->build_result = FALSE;
			return;
		}
		pbuff = std::move(pbuff1);
		length = length1;
	} else if (0 == strcasecmp(pmime->encoding, "quoted-printable")) {
		std::unique_ptr<char[], stdlib_delete> pbuff1(static_cast<char *>(malloc(strange_roundup(length - 1, 64 * 1024))));
		if (NULL == pbuff1) {
			pbuild->build_result = FALSE;
			return;
		}
		auto qdlen = qp_decode_ex(pbuff1.get(), length, pbuff.get(), length);
		if (qdlen < 0) {
			pbuild->build_result = false;
			return;
		}
		length = qdlen;
		pbuff = std::move(pbuff1);
	}
	
	MJSON temp_mjson(pmime->ppool);
	MAIL imail(pbuild->ppool);
	if (!imail.retrieve(pbuff.get(), length)) {
		pbuild->build_result = FALSE;
		return;
	} else {
		/* for saving stacking size, so use C++
			style of local variable declaration */
		size_t mess_len;
		int digest_len;
		char digest_buff[MAX_DIGLEN];
		
		fd = open(msg_path, O_CREAT|O_TRUNC|O_WRONLY, DEF_MODE);
		if (-1 == fd) {
			pbuild->build_result = FALSE;
			return;
		}
		if (!imail.to_file(fd)) {
			close(fd);
			if (remove(msg_path) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1372: remove %s: %s\n", msg_path, strerror(errno));
			pbuild->build_result = FALSE;
			return;
		}
		close(fd);
		if (1 == pbuild->depth) {
			digest_len = sprintf(digest_buff, "{\"file\":\"%s\",",
							pmime->id);
		} else {
			digest_len = sprintf(digest_buff, "{\"file\":\"%s.%s\",",
							pbuild->filename, pmime->id);
		}
		int result = imail.get_digest(&mess_len, digest_buff + digest_len,
					MAX_DIGLEN - digest_len - 1);
		imail.clear();
		pbuff.reset();
		if (result <= 0) {
			if (remove(msg_path) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1373: remove %s: %s\n", msg_path, strerror(errno));
			pbuild->build_result = FALSE;
			return;
		}
		digest_len = strlen(digest_buff);
		digest_buff[digest_len] = '}';
		digest_len ++;
		digest_buff[digest_len] = '\0';
		
		fd = open(dgt_path, O_CREAT|O_TRUNC|O_WRONLY, DEF_MODE);
		if (-1 == fd) {
			if (remove(msg_path) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1374: remove %s: %s\n", msg_path, strerror(errno));
			pbuild->build_result = FALSE;
			return;
		}
		if (digest_len != write(fd, digest_buff, digest_len)) {
			close(fd);
			if (remove(dgt_path) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1375: remove %s: %s\n", dgt_path, strerror(errno));
			if (remove(msg_path) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1376: remove %s: %s\n", msg_path, strerror(errno));
			pbuild->build_result = FALSE;
			return;
		}
		close(fd);
		
		if (!temp_mjson.retrieve(digest_buff, digest_len, pbuild->storage_path)) {
			if (remove(dgt_path) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1377: remove %s: %s\n", dgt_path, strerror(errno));
			if (remove(msg_path) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1378: remove %s: %s\n", msg_path, strerror(errno));
			pbuild->build_result = FALSE;
			return;
		}
	}
	
	if (pbuild->depth < MAX_RFC822_DEPTH && temp_mjson.rfc822_check()) {
		BUILD_PARAM build_param;
		build_param.filename = temp_mjson.filename;
		build_param.msg_path = temp_mjson.path;
		build_param.storage_path = pbuild->storage_path;
		build_param.depth = pbuild->depth + 1;
		build_param.ppool = pbuild->ppool;
		build_param.build_result = TRUE;
		
		temp_mjson.enum_mime(reinterpret_cast<MJSON_MIME_ENUM>(mjson_enum_build), &build_param);
		if (FALSE == build_param.build_result) {
			if (remove(dgt_path) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1379: remove %s: %s\n", dgt_path, strerror(errno));
			if (remove(msg_path) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1380: remove %s: %s\n", msg_path, strerror(errno));
			pbuild->build_result = FALSE;
		}
	}
	return;
}

BOOL MJSON::rfc822_build(std::shared_ptr<MIME_POOL> ppool, const char *storage_path)
{
	auto pjson = this;
	char temp_path[256];
	
	if (!rfc822_check())
		return FALSE;
	if ('\0' == pjson->path[0]) {
		return FALSE;
	}
	
	snprintf(temp_path, 256, "%s/%s", storage_path, pjson->filename);
	if (mkdir(temp_path, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "E-1433: mkdir %s: %s\n", temp_path, strerror(errno));
		return FALSE;
	}
	BUILD_PARAM build_param;
	build_param.filename = pjson->filename;
	build_param.msg_path = pjson->path;
	build_param.storage_path = temp_path;
	build_param.depth = 1;
	build_param.ppool = ppool;
	build_param.build_result = TRUE;
	pjson->enum_mime(reinterpret_cast<MJSON_MIME_ENUM>(mjson_enum_build), &build_param);
	if (FALSE == build_param.build_result) {
		rmdir(temp_path);
	}
	return build_param.build_result;
}

BOOL MJSON::rfc822_get(MJSON *pjson, const char *storage_path, const char *id,
    char *mjson_id, char *mime_id)
{
	auto pjson_base = this;
	int fd;
	char *pdot;
	char temp_path[256];
	struct stat node_stat;
	char digest_buff[MAX_DIGLEN];

	if (!rfc822_check())
		return FALSE;
	snprintf(temp_path, 256, "%s/%s", storage_path, pjson_base->filename);
	if (0 != stat(temp_path, &node_stat) || 0 == S_ISDIR(node_stat.st_mode)) {
		return FALSE;
	}
	
	snprintf(mjson_id, 64, "%s.", id);
	while (NULL != (pdot = strrchr(mjson_id, '.'))) {
		*pdot = '\0';
		snprintf(temp_path, 256, "%s/%s/%s.dgt", storage_path,
			pjson_base->filename, mjson_id);
		fd = open(temp_path, O_RDONLY);
		if (-1 == fd) {
			if (errno == ENOENT || errno == EISDIR)
				continue;
			return FALSE;
		}
		if (fstat(fd, &node_stat) != 0) {
			close(fd);
			return false;
		}
		if (!S_ISREG(node_stat.st_mode) || node_stat.st_size > MAX_DIGLEN) {
			close(fd);
			return FALSE;
		}
			if (::read(fd, digest_buff, node_stat.st_size) != node_stat.st_size) {
				close(fd);
				return FALSE;
			}
			close(fd);
			pjson->clear();
			snprintf(temp_path, 256, "%s/%s", storage_path,
				pjson_base->filename);
			if (!pjson->retrieve(digest_buff, node_stat.st_size, temp_path)) {
				/* was never implemented */
			}
			strcpy(mime_id, pdot + 1);
			return TRUE;
	}
	return FALSE;
}
	
int MJSON::rfc822_fetch(const char *storage_path,
	const char *charset, BOOL b_ext, char *buff, int length)
{
	auto pjson = this;
	int ret_len;
	MJSON_MIME *pmime;
	char temp_path[256];
	struct stat node_stat;
	SIMPLE_TREE_NODE *pnode;
	

#ifdef _DEBUG_UMTA
	if (storage_path == nullptr || buff == nullptr) {
		debug_info("[mail]: NULL pointer in mjson_rfc822_fetch");
		return -1;
	}
#endif
	if (!rfc822_check())
		return FALSE;
	snprintf(temp_path, 256, "%s/%s", storage_path, pjson->filename);
	if (0 != stat(temp_path, &node_stat) ||
		0 == S_ISDIR(node_stat.st_mode)) {
		return FALSE;
	}


	pnode = simple_tree_get_root(&pjson->tree);
	if (NULL == pnode) {
		return -1;
	}
	
	pmime = (MJSON_MIME*)pnode->pdata;
	ret_len = mjson_fetch_mime_structure(pmime, temp_path, "", charset,
				pjson->charset, b_ext, buff, length);
	if (ret_len == -1)
		return -1;
	buff[ret_len] = '\0';
	return ret_len;
}

static int mjson_rfc822_fetch_internal(MJSON *pjson, const char *storage_path,
	const char *charset, BOOL b_ext, char *buff, int length)
{
	int ret_len;
	MJSON_MIME *pmime;
	SIMPLE_TREE_NODE *pnode;

#ifdef _DEBUG_UMTA
	if (NULL == pjson || NULL == storage_path || NULL == buff) {
		debug_info("[mail]: NULL pointer in mjson_rfc822_fetch_internal");
		return -1;
	}
#endif

	pnode = simple_tree_get_root(&pjson->tree);
	if (NULL == pnode) {
		return -1;
	}
	
	pmime = (MJSON_MIME*)pnode->pdata;
	ret_len = mjson_fetch_mime_structure(pmime, storage_path, pjson->filename,
				charset, pjson->charset, b_ext, buff, length);
	if (ret_len == -1)
		return -1;
	buff[ret_len] = '\0';
	return ret_len;
}

