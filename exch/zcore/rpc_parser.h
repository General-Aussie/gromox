#pragma once
#include <gromox/common_types.hpp>

void rpc_parser_init(int thread_num);
extern int rpc_parser_run();
extern void rpc_parser_stop();
BOOL rpc_parser_activate_connection(int clifd);

extern unsigned int g_zrpc_debug;
