#ifndef _H_BOUNCE_PRODUCER_
#define _H_BOUNCE_PRODUCER_

#include <time.h>
#include <gromox/hook_common.h>

void bounce_producer_init(const char *path, const char* separator);
extern int bounce_producer_run(void);
extern void bounce_producer_stop(void);
extern void bounce_producer_free(void);
extern BOOL bounce_producer_refresh(void);
void bounce_producer_make(MESSAGE_CONTEXT *pcontext,
	time_t original_time, MAIL *pmail);

#endif
