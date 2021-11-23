#include <openthread/thread.h>
#include <openthread/dataset.h>

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

int app_InitNetworkDefaultCfg(otInstance *instance)
{
    otError err = 0;

#if !OT_RCP
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
