#include <openthread/thread.h>
#include <openthread/dataset.h>
#include <openthread/instance.h>
#include <openthread/thread_ftd.h>
#include <openthread/ping_sender.h>
#include <openthread/platform/alarm-milli.h>

#include "openthread-system.h"
#include "scheduler.h"
#include "app.h"

const otOperationalDataset dataset = {
    .mNetworkKey.m8 = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},
    .mActiveTimestamp = 1,
    .mPanId = 0xDEAD,
    .mExtendedPanId.m8 = {1,2,3,4,5,6,7,8},
    .mNetworkName.m8 = "ot-sample-nw",
    .mComponents = {
        .mIsNetworkKeyPresent = 1,
        .mIsActiveTimestampPresent = 1,
        .mIsPanIdPresent = 1,
        .mIsExtendedPanIdPresent = 1,
        .mIsNetworkNamePresent = 1,
    },
};

static bool mInterfererEnabled = false;

static otInstance *mInstance;

/** 
 * Scheduler Handler, here act all the periodic actions.
*/
static void handlerScheduler(void)
{
    static uint8_t tick = 0;
    bool toggleLed = false;

    if (mInterfererEnabled) {
        otSysLedToggle(4);

    } else if (tick % 10 == 0) {
        otSysLedToggle(4);
        tick = 0;
    }

    tick++;
}

/**
 * Function to handle button push event
 */
static void handleButtonInterrupt(otInstance *aInstance)
{
    mInterfererEnabled ^= 0x01;
}

static void handleNetifStateChanged(uint32_t aFlags, void *aContext)
{
   if ((aFlags & OT_CHANGED_THREAD_ROLE) != 0)
   {
       otDeviceRole changedRole = otThreadGetDeviceRole(aContext);

       switch (changedRole)
       {
       case OT_DEVICE_ROLE_LEADER:
           otSysLedSet(1, true);
           otSysLedSet(2, false);
           otSysLedSet(3, false);
           break;

       case OT_DEVICE_ROLE_ROUTER:
           otSysLedSet(1, false);
           otSysLedSet(2, true);
           otSysLedSet(3, false);
           break;

       case OT_DEVICE_ROLE_CHILD:
           otSysLedSet(1, false);
           otSysLedSet(2, false);
           otSysLedSet(3, true);
           break;

       case OT_DEVICE_ROLE_DETACHED:
       case OT_DEVICE_ROLE_DISABLED:
           otSysLedSet(1, false);
           otSysLedSet(2, false);
           otSysLedSet(3, false);
           break;
        }
    }
}

static int app_InitNetworkDefaultCfg(otInstance *instance)
{
    otError err = 0;

#if !OT_RCP

    /* Register Thread state change handler */
    err = otSetStateChangedCallback(instance, handleNetifStateChanged, instance);
    if (err) {
        return err;
    }

    /* dataset networkkey */
    err = otDatasetSetActive(instance, &dataset);
    if (err) {
        return err;
    }

    /* ifconfig up */
    if(otIp6SetEnabled(instance, true)) {
        return -1;
    }

    /* thread start */
    err = otThreadSetEnabled(instance, true);
    if (err) {
        return err;
    }
#endif

    return (int)err;
}

int app_Init(otInstance *instance)
{
    otError err = 0;

    otSysLedInit();
    otSysButtonInit(handleButtonInterrupt);

    err = app_InitNetworkDefaultCfg(instance);
    if (err) {
        return err;
    }

    scheduler_Init(handlerScheduler, 100);

    mInstance = instance;

    return err;
}

void app_ProcessEvents(otInstance *instance)
{
    otSysButtonProcess(instance);
    scheduler_ProcessEvent();
}