// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/common_types.hpp>
#include <gromox/double_list.hpp>
#include "common_util.h"
#include <gromox/console_server.hpp>
#include "cmd_parser.h"
#include <poll.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <csignal>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#define MAX_ARGS			(32*1024)

#define CONN_BUFFLEN        (257*1024)

namespace {
struct COMMAND_ENTRY {
	char cmd[64];
	MIDB_CMD_HANDLER cmd_handler;
};
}

static int g_cmd_num;
static size_t g_threads_num;
static std::atomic<bool> g_notify_stop{false};
static int g_timeout_interval;
static pthread_t *g_thread_ids;
static std::mutex g_connection_lock, g_cond_mutex;
static std::condition_variable g_waken_cond;
static DOUBLE_LIST g_connection_list;
static DOUBLE_LIST g_connection_list1;
static COMMAND_ENTRY g_cmd_entry[128];

static void *midcp_thrwork(void *);
static int cmd_parser_generate_args(char* cmd_line, int cmd_len, char** argv);

static int cmd_parser_ping(int argc, char **argv, int sockd);

void cmd_parser_init(size_t threads_num, int timeout)
{
	g_cmd_num = 0;
	g_threads_num = threads_num;
	g_timeout_interval = timeout;
	double_list_init(&g_connection_list);
	double_list_init(&g_connection_list1);
}


void cmd_parser_free()
{

	double_list_free(&g_connection_list);
	double_list_free(&g_connection_list1);
}

CONNECTION* cmd_parser_get_connection()
{
	std::unique_lock chold(g_connection_lock);
	if (double_list_get_nodes_num(&g_connection_list) + 1 +
		double_list_get_nodes_num(&g_connection_list1) >= g_threads_num) {
		return NULL;
	}
	chold.unlock();
	auto pconnection = me_alloc<CONNECTION>();
	pconnection->node.pdata = pconnection;

	return pconnection;
}

void cmd_parser_put_connection(CONNECTION *pconnection)
{	
	std::unique_lock chold(g_connection_lock);
	double_list_append_as_tail(&g_connection_list1, &pconnection->node);
	chold.unlock();
	g_waken_cond.notify_one();
}


int cmd_parser_run()
{
	size_t i;
	pthread_attr_t thr_attr;


	cmd_parser_register_command("PING", cmd_parser_ping);

	g_thread_ids = me_alloc<pthread_t>(g_threads_num);
	pthread_attr_init(&thr_attr);
	g_notify_stop = false;

	for (i=0; i<g_threads_num; i++) {
		int ret = pthread_create(&g_thread_ids[i], &thr_attr,
		          midcp_thrwork, nullptr);
		if (ret != 0) {
			printf("[cmd_parser]: failed to create pool thread: %s\n", strerror(ret));
			goto FAILURE_EXIT;
		}
		char buf[32];
		snprintf(buf, sizeof(buf), "cmd_parser/%zu", i);
		pthread_setname_np(g_thread_ids[i], buf);
	}

	pthread_attr_destroy(&thr_attr);

	return 0;

 FAILURE_EXIT:
	while (i > 0)
		pthread_cancel(g_thread_ids[--i]);
	pthread_attr_destroy(&thr_attr);
	return -1;
}



int cmd_parser_stop()
{
	DOUBLE_LIST_NODE *pnode;
	CONNECTION *pconnection;

	g_notify_stop = true;
	g_waken_cond.notify_all();
	std::unique_lock chold(g_connection_lock);
	for (pnode=double_list_get_head(&g_connection_list); NULL!=pnode;
		pnode=double_list_get_after(&g_connection_list, pnode)) {
		pconnection = (CONNECTION*)pnode->pdata;
		if (TRUE == pconnection->is_selecting) {
			pthread_kill(pconnection->thr_id, SIGALRM);
		} else {
			close(pconnection->sockd);
			pconnection->sockd = -1;
		}	
	}
	chold.unlock();
	for (size_t i = 0; i < g_threads_num; ++i)
		pthread_join(g_thread_ids[i], NULL);
	while ((pnode = double_list_pop_front(&g_connection_list)) != nullptr) {
		pconnection = (CONNECTION*)pnode->pdata;
		if (-1 != pconnection->sockd) {
			close(pconnection->sockd);
		}
		free(pconnection);
	}

	while ((pnode = double_list_pop_front(&g_connection_list1)) != nullptr) {
		pconnection = (CONNECTION*)pnode->pdata;
		close(pconnection->sockd);
		free(pconnection);
	}
	return 0;
}


void cmd_parser_register_command(const char *command, MIDB_CMD_HANDLER handler)
{
	HX_strlcpy(g_cmd_entry[g_cmd_num].cmd, command, GX_ARRAY_SIZE(g_cmd_entry[g_cmd_num].cmd));
	g_cmd_entry[g_cmd_num].cmd_handler = handler;
	g_cmd_num ++;
}

static void *midcp_thrwork(void *param)
{
	int i, j;
	int argc;
	int result;
	int offset;
	int tv_msec;
	int temp_len;
	int read_len;
	char *argv[MAX_ARGS];
	struct pollfd pfd_read;
	char temp_response[128];
	CONNECTION *pconnection;
	DOUBLE_LIST_NODE *pnode;
	char buffer[CONN_BUFFLEN];

 NEXT_LOOP:
	if (g_notify_stop)
		return nullptr;
	std::unique_lock cm_hold(g_cond_mutex);
	g_waken_cond.wait(cm_hold);
	cm_hold.unlock();
	if (g_notify_stop)
		return nullptr;
	std::unique_lock co_hold(g_connection_lock);
	pnode = double_list_pop_front(&g_connection_list1);
	if (NULL != pnode) {
		double_list_append_as_tail(&g_connection_list, pnode);
	}
	co_hold.unlock();
	if (NULL == pnode) {
		goto NEXT_LOOP;
	}
	pconnection = (CONNECTION*)pnode->pdata;

	offset = 0;

    while (!g_notify_stop) {
		tv_msec = g_timeout_interval * 1000;
		pfd_read.fd = pconnection->sockd;
		pfd_read.events = POLLIN|POLLPRI;
		pconnection->is_selecting = TRUE;
		pconnection->thr_id = pthread_self();
		if (1 != poll(&pfd_read, 1, tv_msec)) {
			pconnection->is_selecting = FALSE;
			std::unique_lock co_hold(g_connection_lock);
			double_list_remove(&g_connection_list, &pconnection->node);
			co_hold.unlock();
			close(pconnection->sockd);
			free(pconnection);
			goto NEXT_LOOP;
		}
		pconnection->is_selecting = FALSE;
		read_len = read(pconnection->sockd, buffer + offset,
					CONN_BUFFLEN - offset);
		if (read_len <= 0) {
			std::unique_lock co_hold(g_connection_lock);
			double_list_remove(&g_connection_list, &pconnection->node);
			co_hold.unlock();
			close(pconnection->sockd);
			free(pconnection);
			goto NEXT_LOOP;
		}
		offset += read_len;
		for (i=0; i<offset-1; i++) {
			if ('\r' == buffer[i] && '\n' == buffer[i + 1]) {
				if (4 == i && 0 == strncasecmp(buffer, "QUIT", 4)) {
					write(pconnection->sockd, "BYE\r\n", 5);
					std::unique_lock co_hold(g_connection_lock);
					double_list_remove(&g_connection_list, &pconnection->node);
					co_hold.unlock();
					close(pconnection->sockd);
					free(pconnection);
					goto NEXT_LOOP;
				}

				argc = cmd_parser_generate_args(buffer, i, argv);
				if(argc < 2) {
					write(pconnection->sockd, "FALSE 1\r\n", 9);
					offset -= i + 2;
					memmove(buffer, buffer + i + 2, offset);
					break;	
				}
				
				/* compare build-in command */
				for (j=0; j<g_cmd_num; j++) {
					if (!g_notify_stop && strcasecmp(g_cmd_entry[j].cmd, argv[0]) == 0) {
						if (FALSE == common_util_build_environment(argv[1])) {
							write(pconnection->sockd, "FALSE 0\r\n", 9);
							continue;
						}
						result = g_cmd_entry[j].cmd_handler(
							argc, argv, pconnection->sockd);
						common_util_free_environment();
						if (0 != result) {
							temp_len = sprintf(temp_response,
										"FALSE %d\r\n", result);
							write(pconnection->sockd, temp_response, temp_len);
						}
						break;
					}
				}
				
				if (!g_notify_stop && j == g_cmd_num)
					write(pconnection->sockd, "FALSE 0\r\n", 9);
				offset -= i + 2;
				memmove(buffer, buffer + i + 2, offset);
				break;	
			}
		}

		if (CONN_BUFFLEN == offset) {
			std::unique_lock co_hold(g_connection_lock);
			double_list_remove(&g_connection_list, &pconnection->node);
			co_hold.unlock();
			close(pconnection->sockd);
			free(pconnection);
			goto NEXT_LOOP;
		}
	}
	return nullptr;
}

static int cmd_parser_ping(int argc, char **argv, int sockd)
{
	write(sockd, "TRUE\r\n", 6);
	return 0;
}

static int cmd_parser_generate_args(char* cmd_line, int cmd_len, char** argv)
{
	int argc;                    /* number of args */
	char *ptr;                   /* ptr that traverses command line  */
	char *last_space;
	
	cmd_line[cmd_len] = ' ';
	cmd_line[cmd_len + 1] = '\0';
	ptr = cmd_line;
	/* Build the argv list */
	argc = 0;
	last_space = cmd_line;
	while (*ptr != '\0') {
		if ('{' == *ptr) {
			if ('}' != cmd_line[cmd_len - 1]) {
				return 0;
			}
			argv[argc] = ptr;
			cmd_line[cmd_len] = '\0';
			argc ++;
			break;
		}

		if (' ' == *ptr) {
			/* ignore leading spaces */
			if (ptr == last_space) {
				last_space ++;
			} else {
				argv[argc] = last_space;
				*ptr = '\0';
				last_space = ptr + 1;
				argc ++;
			}
		}
		ptr ++;
	}
	
	argv[argc] = NULL;
	return argc;
}	

