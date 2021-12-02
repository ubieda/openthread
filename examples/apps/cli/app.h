#ifndef _APP_H_
#define _APP_H_

#include <openthread/instance.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_Init(otInstance *instance);
void app_ProcessEvents(otInstance *instance);

#ifdef __cplusplus
}
#endif

#endif /* _APP_H_ */
