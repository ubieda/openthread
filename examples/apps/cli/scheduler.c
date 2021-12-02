#include "scheduler.h"
#include <openthread/platform/alarm-milli.h>

struct schedulerState {
    bool active;
    scheduler_Handler handler;
    uint32_t periodMs;
    uint32_t lastTriggerMs;
};

struct schedulerState aScheduler;

void scheduler_Init(scheduler_Handler handler, uint32_t periodMs)
{
    if (!handler) {
        return;
    }

    aScheduler.handler = handler;
    aScheduler.periodMs = periodMs;
    aScheduler.lastTriggerMs = otPlatAlarmMilliGetNow();
    aScheduler.active = true;
}

void scheduler_ProcessEvent(void)
{
    if (aScheduler.active) {
        uint32_t timeNow = otPlatAlarmMilliGetNow();
        if (timeNow - aScheduler.lastTriggerMs >= aScheduler.periodMs) {
            aScheduler.lastTriggerMs = timeNow;
            aScheduler.handler();
        }
    }
}