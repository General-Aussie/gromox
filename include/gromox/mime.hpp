#pragma once
#include <gromox/stream.hpp>
#include <gromox/mem_file.hpp>
#include <gromox/simple_tree.hpp>
#include <openssl/ssl.h>
#define VALUE_LEN	256

enum{
	NONE_MIME = 0,
	SINGLE_MIME,
	MULTIPLE_MIME
};

enum{
	MIME_ENCODING_NONE = 0,
	MIME_ENCODING_BASE64,
	MIME_ENCODING_QP,
	MIME_ENCODING_UUENCODE,
	MIME_ENCODING_UNKNOWN
};

using MIME_FIELD_ENUM = BOOL (*)(const char *, char *, void *);

struct MAIL;
struct GX_EXPORT MIME {
	BOOL retrieve(MIME *parent, char *in_buf, size_t len);
	void clear();
	BOOL write_content(const char *content, size_t len, int encoding_type);
	BOOL write_mail(MAIL *);
	BOOL read_head(char *out_buf, size_t *len);
	BOOL read_content(char *out_buf, size_t *len);
	inline size_t get_children_num() const { return simple_tree_node_get_children_num(&node); }

	SIMPLE_TREE_NODE node;
	int			mime_type;
	char 		content_type[VALUE_LEN];
	char		boundary_string[VALUE_LEN];
	int			boundary_len;
	MEM_FILE	f_type_params;
	MEM_FILE	f_other_fields;
	BOOL		head_touched;
	BOOL		content_touched;
	char		*head_begin;
	size_t		head_length;
	char		*content_begin;
	size_t		content_length;
	char		*first_boundary;
	char		*last_boundary;
};

void mime_init(MIME *pmime, LIB_BUFFER *palloc);
void mime_free(MIME *pmime);
BOOL mime_set_content_type(MIME *pmime, const char *content_type);
BOOL mime_enum_field(MIME *pmime, MIME_FIELD_ENUM enum_func, void *pparam);
BOOL mime_get_field(MIME *pmime, const char *tag, char *value, int length);
int mime_get_field_num(MIME *pmime, const char *tag);
BOOL mime_search_field(MIME *pmime, const char *tag, int order, char *value,
	int length);
BOOL mime_set_field(MIME *pmime, const char *tag, const char *value);
BOOL mime_append_field(MIME *pmime, const char *tag, const char *value);
BOOL mime_remove_field(MIME *pmime, const char *tag);
BOOL mime_get_content_param(MIME *pmime, const char *tag, char *value, 
	int length);
BOOL mime_set_content_param(MIME *pmime, const char *tag, const char *value);
extern GX_EXPORT ssize_t mime_get_mimes_digest(MIME *, const char *, size_t *, size_t *, char *, size_t);
extern GX_EXPORT ssize_t mime_get_structure_digest(MIME *, const char *, size_t *, size_t *, char *, size_t);
BOOL mime_serialize(MIME *pmime, STREAM *pstream);
BOOL mime_to_file(MIME *pmime, int fd);
BOOL mime_to_ssl(MIME *pmime, SSL *ssl);
BOOL mime_check_dot(MIME *pmime);
extern ssize_t mime_get_length(MIME *);
BOOL mime_get_filename(MIME *pmime, char *file_name);
void mime_copy(MIME *pmime_src, MIME *pmime_dst);
MIME* mime_get_child(MIME *pmime);
MIME* mime_get_parent(MIME *pmime);
extern MIME *mime_get_sibling(MIME *);
