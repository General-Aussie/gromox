#pragma once
#include <cstdint>
#include <deque>
#include <gromox/common_types.hpp>
#include <gromox/contexts_pool.hpp>
#include <gromox/generic_connection.hpp>
#include <gromox/msg_unit.hpp>
#include <gromox/single_list.hpp>
#include <gromox/stream.hpp>
#include <sys/time.h>
#include <openssl/ssl.h>
#define MAX_LINE_LENGTH    64*1024

/* enumeration of pop3_parser */
enum{
    MAX_AUTH_TIMES,
	BLOCK_AUTH_FAIL,
	POP3_SUPPORT_STLS,
	POP3_FORCE_STLS
};

enum {
	POP3_RETRIEVE_TERM,
	POP3_RETRIEVE_OK,
	POP3_RETRIEVE_ERROR
};

struct POP3_CONTEXT final : public SCHEDULE_CONTEXT {
	POP3_CONTEXT();
	~POP3_CONTEXT();
	NOMOVE(POP3_CONTEXT);

	GENERIC_CONNECTION connection;
	char read_buffer[1024]{};
	size_t read_offset{};
	char *write_buff = nullptr;
	size_t write_length = 0, write_offset = 0;
	BOOL data_stat = false, list_stat = false;
	int until_line = 0x7FFFFFFF, cur_line = -1, message_fd = -1;
	STREAM stream; /* stream accepted from pop3 client */
	int total_mail = 0;
	uint64_t total_size = 0;

	/*
	 * @list will hold indices/iterators/pointers to @array elements, so
	 * these elements must not change their memory location when @array is
	 * modified (specifically: only append). We also want O(1) random
	 * access in @array. Therefore, deque is used.
	 */
	std::deque<gromox::MSG_UNIT> array; /* mailbox message list */
	SINGLE_LIST list{}; /* deleted message list */
	BOOL is_login = false; /* if user is logged in */
	BOOL is_stls = false; /* if last command is STLS */
	int auth_times = 0;
	char username[UADDR_SIZE]{};
	char maildir[256]{};
};

void pop3_parser_init(int context_num, size_t retrieving_size, int timeout,
	int max_auth_times, int block_auth_fail, BOOL support_stls, BOOL force_stls,
	const char *certificate_path, const char *cb_passwd, const char *key_path,
	const char *cdn_path);
extern int pop3_parser_run();
int pop3_parser_process(POP3_CONTEXT *pcontext);
extern void pop3_parser_stop();
extern int pop3_parser_get_context_socket(SCHEDULE_CONTEXT *);
extern struct timeval pop3_parser_get_context_timestamp(SCHEDULE_CONTEXT *);
int pop3_parser_get_param(int param);
int pop3_parser_set_param(int param, int value);
extern SCHEDULE_CONTEXT **pop3_parser_get_contexts_list();
int pop3_parser_threads_event_proc(int action);
int pop3_parser_retrieve(POP3_CONTEXT *pcontext);
extern void pop3_parser_log_info(POP3_CONTEXT *pcontext, int level, const char *format, ...);
extern char *pop3_parser_cdn_path();

extern unsigned int g_popcmd_debug;
