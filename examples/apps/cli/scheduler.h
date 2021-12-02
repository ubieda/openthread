#ifndef _APP_SCHEDULER_H_
#define _APP_SCHEDULER_H_

#include <stdint.h>

typedef void (*scheduler_Handler)(void);
void scheduler_Init(scheduler_Handler handler, uint32_t periodMs);
void scheduler_ProcessEvent(void);

#endif /* _APP_SCHEDULER_H_ */