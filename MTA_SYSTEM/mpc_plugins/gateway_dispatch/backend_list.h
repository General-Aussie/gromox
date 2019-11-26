#ifndef _H_BACKEND_LIST_
#define _H_BACKEND_LIST_
#include "common_types.h"

enum {
	BACKEND_LIST_SCAN_INTERVAL
};

typedef void (*BACKEND_LIST_ENUM_FUNC)(const char *ip, int port);

void backend_list_init(const char *list_path, int scan_interval);
extern int backend_list_run(void);
BOOL backend_list_get_unit(char *ip, int *port);

void backend_list_invalid_unit(const char *ip, int port);
extern BOOL backend_list_refresh(void);
extern void backend_list_stop(void);
extern void backend_list_free(void);
void backend_list_enum_invalid(BACKEND_LIST_ENUM_FUNC enum_func);

int backend_list_get_param(int param);

void backend_list_set_param(int param, int value);

#endif /* _H_BACKEND_LIST_ */
