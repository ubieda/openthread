/*
 *  Copyright (c) 2020, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file includes implementation for SRP server.
 */

#include "srp_server.hpp"

#if OPENTHREAD_CONFIG_SRP_SERVER_ENABLE

#include "common/as_core_type.hpp"
#include "common/const_cast.hpp"
#include "common/instance.hpp"
#include "common/locator_getters.hpp"
#include "common/logging.hpp"
#include "common/new.hpp"
#include "common/random.hpp"
#include "net/dns_types.hpp"
#include "thread/thread_netif.hpp"

namespace ot {
namespace Srp {

static const char kDefaultDomain[]       = "default.service.arpa.";
static const char kServiceSubTypeLabel[] = "._sub.";

static Dns::UpdateHeader::Response ErrorToDnsResponseCode(Error aError)
{
    Dns::UpdateHeader::Response responseCode;

    switch (aError)
    {
    case kErrorNone:
        responseCode = Dns::UpdateHeader::kResponseSuccess;
        break;
    case kErrorNoBufs:
        responseCode = Dns::UpdateHeader::kResponseServerFailure;
        break;
    case kErrorParse:
        responseCode = Dns::UpdateHeader::kResponseFormatError;
        break;
    case kErrorDuplicated:
        responseCode = Dns::UpdateHeader::kResponseNameExists;
        break;
    default:
        responseCode = Dns::UpdateHeader::kResponseRefused;
        break;
    }

    return responseCode;
}

//---------------------------------------------------------------------------------------------------------------------
// Server

Server::Server(Instance &aInstance)
    : InstanceLocator(aInstance)
    , mSocket(aInstance)
    , mServiceUpdateHandler(nullptr)
    , mServiceUpdateHandlerContext(nullptr)
    , mLeaseTimer(aInstance, HandleLeaseTimer)
    , mOutstandingUpdatesTimer(aInstance, HandleOutstandingUpdatesTimer)
    , mServiceUpdateId(Random::NonCrypto::GetUint32())
    , mPort(kUdpPortMin)
    , mState(kStateDisabled)
    , mAddressMode(kDefaultAddressMode)
    , mAnycastSequenceNumber(0)
    , mHasRegisteredAnyService(false)
{
    IgnoreError(SetDomain(kDefaultDomain));
}

void Server::SetServiceHandler(otSrpServerServiceUpdateHandler aServiceHandler, void *aServiceHandlerContext)
{
    mServiceUpdateHandler        = aServiceHandler;
    mServiceUpdateHandlerContext = aServiceHandlerContext;
}

Error Server::SetAddressMode(AddressMode aMode)
{
    Error error = kErrorNone;

    VerifyOrExit(mState == kStateDisabled, error = kErrorInvalidState);
    VerifyOrExit(mAddressMode != aMode);
    otLogInfoSrp("[server] Address Mode: %s -> %s", AddressModeToString(mAddressMode), AddressModeToString(aMode));
    mAddressMode = aMode;

exit:
    return error;
}

Error Server::SetAnycastModeSequenceNumber(uint8_t aSequenceNumber)
{
    Error error = kErrorNone;

    VerifyOrExit(mState == kStateDisabled, error = kErrorInvalidState);
    mAnycastSequenceNumber = aSequenceNumber;

    otLogInfoSrp("[server] Set Anycast Address Mode Seq Number to %d", aSequenceNumber);

exit:
    return error;
}

void Server::SetEnabled(bool aEnabled)
{
    if (aEnabled)
    {
        VerifyOrExit(mState == kStateDisabled);
        mState = kStateStopped;

        // Request publishing of "DNS/SRP Address Service" entry in the
        // Thread Network Data based of `mAddressMode`. Then wait for
        // callback `HandleNetDataPublisherEntryChange()` from the
        // `Publisher` to start the SRP server.

        switch (mAddressMode)
        {
        case kAddressModeUnicast:
            SelectPort();
            Get<NetworkData::Publisher>().PublishDnsSrpServiceUnicast(mPort);
            break;

        case kAddressModeAnycast:
            mPort = kAnycastAddressModePort;
            Get<NetworkData::Publisher>().PublishDnsSrpServiceAnycast(mAnycastSequenceNumber);
            break;
        }
    }
    else
    {
        VerifyOrExit(mState != kStateDisabled);
        Get<NetworkData::Publisher>().UnpublishDnsSrpService();
        Stop();
        mState = kStateDisabled;
    }

exit:
    return;
}

Server::LeaseConfig::LeaseConfig(void)
{
    mMinLease    = kDefaultMinLease;
    mMaxLease    = kDefaultMaxLease;
    mMinKeyLease = kDefaultMinKeyLease;
    mMaxKeyLease = kDefaultMaxKeyLease;
}

bool Server::LeaseConfig::IsValid(void) const
{
    bool valid = false;

    // TODO: Support longer LEASE.
    // We use milliseconds timer for LEASE & KEY-LEASE, this is to avoid overflow.
    VerifyOrExit(mMaxKeyLease <= Time::MsecToSec(TimerMilli::kMaxDelay));
    VerifyOrExit(mMinLease <= mMaxLease);
    VerifyOrExit(mMinKeyLease <= mMaxKeyLease);
    VerifyOrExit(mMinLease <= mMinKeyLease);
    VerifyOrExit(mMaxLease <= mMaxKeyLease);

    valid = true;

exit:
    return valid;
}

uint32_t Server::LeaseConfig::GrantLease(uint32_t aLease) const
{
    OT_ASSERT(mMinLease <= mMaxLease);

    return (aLease == 0) ? 0 : OT_MAX(mMinLease, OT_MIN(mMaxLease, aLease));
}

uint32_t Server::LeaseConfig::GrantKeyLease(uint32_t aKeyLease) const
{
    OT_ASSERT(mMinKeyLease <= mMaxKeyLease);

    return (aKeyLease == 0) ? 0 : OT_MAX(mMinKeyLease, OT_MIN(mMaxKeyLease, aKeyLease));
}

Error Server::SetLeaseConfig(const LeaseConfig &aLeaseConfig)
{
    Error error = kErrorNone;

    VerifyOrExit(aLeaseConfig.IsValid(), error = kErrorInvalidArgs);
    mLeaseConfig = aLeaseConfig;

exit:
    return error;
}

Error Server::SetDomain(const char *aDomain)
{
    Error    error = kErrorNone;
    uint16_t length;

    VerifyOrExit(mState == kStateDisabled, error = kErrorInvalidState);

    length = StringLength(aDomain, Dns::Name::kMaxNameSize);
    VerifyOrExit((length > 0) && (length < Dns::Name::kMaxNameSize), error = kErrorInvalidArgs);

    if (aDomain[length - 1] == '.')
    {
        error = mDomain.Set(aDomain);
    }
    else
    {
        // Need to append dot at the end

        char buf[Dns::Name::kMaxNameSize];

        VerifyOrExit(length < Dns::Name::kMaxNameSize - 1, error = kErrorInvalidArgs);

        memcpy(buf, aDomain, length);
        buf[length]     = '.';
        buf[length + 1] = '\0';

        error = mDomain.Set(buf);
    }

exit:
    return error;
}

const Server::Host *Server::GetNextHost(const Server::Host *aHost)
{
    return (aHost == nullptr) ? mHosts.GetHead() : aHost->GetNext();
}

// This method adds a SRP service host and takes ownership of it.
// The caller MUST make sure that there is no existing host with the same hostname.
void Server::AddHost(Host &aHost)
{
    OT_ASSERT(mHosts.FindMatching(aHost.GetFullName()) == nullptr);
    IgnoreError(mHosts.Add(aHost));
}

void Server::RemoveHost(Host *aHost, RetainName aRetainName, NotifyMode aNotifyServiceHandler)
{
    VerifyOrExit(aHost != nullptr);

    aHost->mLease = 0;
    aHost->ClearResources();

    if (aRetainName)
    {
        otLogInfoSrp("[server] remove host '%s' (but retain its name)", aHost->GetFullName());
    }
    else
    {
        aHost->mKeyLease = 0;
        IgnoreError(mHosts.Remove(*aHost));
        otLogInfoSrp("[server] fully remove host '%s'", aHost->GetFullName());
    }

    if (aNotifyServiceHandler && mServiceUpdateHandler != nullptr)
    {
        uint32_t updateId = AllocateId();

        otLogInfoSrp("[server] SRP update handler is notified (updatedId = %u)", updateId);
        mServiceUpdateHandler(updateId, aHost, kDefaultEventsHandlerTimeout, mServiceUpdateHandlerContext);
        // We don't wait for the reply from the service update handler,
        // but always remove the host (and its services) regardless of
        // host/service update result. Because removing a host should fail
        // only when there is system failure of the platform mDNS implementation
        // and in which case the host is not expected to be still registered.
    }

    if (!aRetainName)
    {
        aHost->Free();
    }

exit:
    return;
}

bool Server::HasNameConflictsWith(Host &aHost) const
{
    bool        hasConflicts = false;
    const Host *existingHost = mHosts.FindMatching(aHost.GetFullName());

    if (existingHost != nullptr && *aHost.GetKey() != *existingHost->GetKey())
    {
        ExitNow(hasConflicts = true);
    }

    for (const Service::Description &desc : aHost.mServiceDescriptions)
    {
        // Check on all hosts for a matching service description with
        // the same instance name and if found, verify that it has the
        // same key.

        for (const Host &host : mHosts)
        {
            if (host.FindServiceDescription(desc.GetInstanceName()) != nullptr)
            {
                VerifyOrExit(*aHost.GetKey() == *host.GetKey(), hasConflicts = true);
            }
        }
    }

exit:
    return hasConflicts;
}

void Server::HandleServiceUpdateResult(ServiceUpdateId aId, Error aError)
{
    UpdateMetadata *update = mOutstandingUpdates.FindMatching(aId);

    if (update != nullptr)
    {
        HandleServiceUpdateResult(update, aError);
    }
    else
    {
        otLogInfoSrp("[server] delayed SRP host update result, the SRP update has been committed (updateId = %u)", aId);
    }
}

void Server::HandleServiceUpdateResult(UpdateMetadata *aUpdate, Error aError)
{
    otLogInfoSrp("[server] handler result of SRP update (id = %u) is received: %s", aUpdate->GetId(),
                 otThreadErrorToString(aError));

    IgnoreError(mOutstandingUpdates.Remove(*aUpdate));
    CommitSrpUpdate(aError, *aUpdate);
    aUpdate->Free();

    if (mOutstandingUpdates.IsEmpty())
    {
        mOutstandingUpdatesTimer.Stop();
    }
    else
    {
        mOutstandingUpdatesTimer.FireAt(mOutstandingUpdates.GetTail()->GetExpireTime());
    }
}

void Server::CommitSrpUpdate(Error aError, Host &aHost, const MessageMetadata &aMessageMetadata)
{
    CommitSrpUpdate(aError, aHost, aMessageMetadata.mDnsHeader, aMessageMetadata.mMessageInfo,
                    aMessageMetadata.mLeaseConfig);
}

void Server::CommitSrpUpdate(Error aError, UpdateMetadata &aUpdateMetadata)
{
    CommitSrpUpdate(aError, aUpdateMetadata.GetHost(), aUpdateMetadata.GetDnsHeader(),
                    aUpdateMetadata.IsDirectRxFromClient() ? &aUpdateMetadata.GetMessageInfo() : nullptr,
                    aUpdateMetadata.GetLeaseConfig());
}

void Server::CommitSrpUpdate(Error                    aError,
                             Host &                   aHost,
                             const Dns::UpdateHeader &aDnsHeader,
                             const Ip6::MessageInfo * aMessageInfo,
                             const LeaseConfig &      aLeaseConfig)
{
    Host *   existingHost;
    uint32_t hostLease;
    uint32_t hostKeyLease;
    uint32_t grantedLease;
    uint32_t grantedKeyLease;
    bool     shouldFreeHost = true;

    SuccessOrExit(aError);

    hostLease       = aHost.GetLease();
    hostKeyLease    = aHost.GetKeyLease();
    grantedLease    = aLeaseConfig.GrantLease(hostLease);
    grantedKeyLease = aLeaseConfig.GrantKeyLease(hostKeyLease);

    aHost.SetLease(grantedLease);
    aHost.SetKeyLease(grantedKeyLease);

    for (Service::Description &desc : aHost.mServiceDescriptions)
    {
        desc.mLease    = grantedLease;
        desc.mKeyLease = grantedKeyLease;
    }

    existingHost = mHosts.FindMatching(aHost.GetFullName());

    if (aHost.GetLease() == 0)
    {
        if (aHost.GetKeyLease() == 0)
        {
            otLogInfoSrp("[server] remove key of host %s", aHost.GetFullName());
            RemoveHost(existingHost, kDeleteName, kDoNotNotifyServiceHandler);
        }
        else if (existingHost != nullptr)
        {
            existingHost->SetKeyLease(aHost.GetKeyLease());
            RemoveHost(existingHost, kRetainName, kDoNotNotifyServiceHandler);

            for (Service &service : existingHost->mServices)
            {
                existingHost->RemoveService(&service, kRetainName, kDoNotNotifyServiceHandler);
            }
        }
    }
    else if (existingHost != nullptr)
    {
        SuccessOrExit(aError = existingHost->MergeServicesAndResourcesFrom(aHost));
    }
    else
    {
        otLogInfoSrp("[server] add new host %s", aHost.GetFullName());

        for (Service &service : aHost.GetServices())
        {
            service.mIsCommitted = true;
            service.Log(Service::kAddNew);
        }

        AddHost(aHost);
        shouldFreeHost = false;

#if OPENTHREAD_CONFIG_SRP_SERVER_PORT_SWITCH_ENABLE
        if (!mHasRegisteredAnyService && (mAddressMode == kAddressModeUnicast))
        {
            Settings::SrpServerInfo info;

            mHasRegisteredAnyService = true;
            info.SetPort(GetSocket().mSockName.mPort);
            IgnoreError(Get<Settings>().Save(info));
        }
#endif
    }

    // Re-schedule the lease timer.
    HandleLeaseTimer();

exit:
    if (aMessageInfo != nullptr)
    {
        if (aError == kErrorNone && !(grantedLease == hostLease && grantedKeyLease == hostKeyLease))
        {
            SendResponse(aDnsHeader, grantedLease, grantedKeyLease, *aMessageInfo);
        }
        else
        {
            SendResponse(aDnsHeader, ErrorToDnsResponseCode(aError), *aMessageInfo);
        }
    }

    if (shouldFreeHost)
    {
        aHost.Free();
    }
}

void Server::SelectPort(void)
{
    mPort = kUdpPortMin;

#if OPENTHREAD_CONFIG_SRP_SERVER_PORT_SWITCH_ENABLE
    {
        Settings::SrpServerInfo info;

        if (Get<Settings>().Read(info) == kErrorNone)
        {
            mPort = info.GetPort() + 1;
            if (mPort < kUdpPortMin || mPort > kUdpPortMax)
            {
                mPort = kUdpPortMin;
            }
        }
    }
#endif

    otLogInfoSrp("[server] selected port %u", mPort);
}

void Server::Start(void)
{
    VerifyOrExit(mState == kStateStopped);

    mState = kStateRunning;
    PrepareSocket();
    otLogInfoSrp("[server] start listening on port %u", mPort);

exit:
    return;
}

void Server::PrepareSocket(void)
{
    Error error = kErrorNone;

#if OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE
    Ip6::Udp::Socket &dnsSocket = Get<Dns::ServiceDiscovery::Server>().mSocket;

    if (dnsSocket.GetSockName().GetPort() == mPort)
    {
        // If the DNS-SD socket matches our port number, we use the
        // same socket so we close our own socket (in case it was
        // open). `GetSocket()` will now return the DNS-SD socket.

        IgnoreError(mSocket.Close());
        ExitNow();
    }
#endif

    VerifyOrExit(!mSocket.IsOpen());
    SuccessOrExit(error = mSocket.Open(HandleUdpReceive, this));
    error = mSocket.Bind(mPort, OT_NETIF_THREAD);

exit:
    if (error != kErrorNone)
    {
        otLogCritSrp("[server] failed to prepare socket: %s", ErrorToString(error));
        Stop();
    }
}

Ip6::Udp::Socket &Server::GetSocket(void)
{
    Ip6::Udp::Socket *socket = &mSocket;

#if OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE
    Ip6::Udp::Socket &dnsSocket = Get<Dns::ServiceDiscovery::Server>().mSocket;

    if (dnsSocket.GetSockName().GetPort() == mPort)
    {
        socket = &dnsSocket;
    }
#endif

    return *socket;
}

#if OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE

void Server::HandleDnssdServerStateChange(void)
{
    // This is called from` Dns::ServiceDiscovery::Server` to notify
    // that it has started or stopped. We check whether we need to
    // share the socket.

    if (mState == kStateRunning)
    {
        PrepareSocket();
    }
}

Error Server::HandleDnssdServerUdpReceive(Message &aMessage, const Ip6::MessageInfo &aMessageInfo)
{
    // This is called from` Dns::ServiceDiscovery::Server` when a UDP
    // message is received on its socket. We check whether we are
    // sharing socket and if so we process the received message. We
    // return `kErrorNone` to indicate that message was successfully
    // processed by `Srp::Server`, otherwise `kErrorDrop` is returned.

    Error error = kErrorDrop;

    VerifyOrExit((mState == kStateRunning) && !mSocket.IsOpen());

    error = ProcessMessage(aMessage, aMessageInfo);

exit:
    return error;
}

#endif // OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE

void Server::Stop(void)
{
    VerifyOrExit(mState == kStateRunning);

    mState = kStateStopped;

    while (!mHosts.IsEmpty())
    {
        RemoveHost(mHosts.GetHead(), kDeleteName, kNotifyServiceHandler);
    }

    // TODO: We should cancel any outstanding service updates, but current
    // OTBR mDNS publisher cannot properly handle it.
    while (!mOutstandingUpdates.IsEmpty())
    {
        mOutstandingUpdates.Pop()->Free();
    }

    mLeaseTimer.Stop();
    mOutstandingUpdatesTimer.Stop();

    otLogInfoSrp("[server] stop listening on %u", mPort);
    IgnoreError(mSocket.Close());
    mHasRegisteredAnyService = false;

exit:
    return;
}

void Server::HandleNetDataPublisherEvent(NetworkData::Publisher::Event aEvent)
{
    switch (aEvent)
    {
    case NetworkData::Publisher::kEventEntryAdded:
        Start();
        break;

    case NetworkData::Publisher::kEventEntryRemoved:
        Stop();
        break;
    }
}

const Server::UpdateMetadata *Server::FindOutstandingUpdate(const MessageMetadata &aMessageMetadata) const
{
    const UpdateMetadata *ret = nullptr;

    VerifyOrExit(aMessageMetadata.IsDirectRxFromClient());

    for (const UpdateMetadata &update : mOutstandingUpdates)
    {
        if (aMessageMetadata.mDnsHeader.GetMessageId() == update.GetDnsHeader().GetMessageId() &&
            aMessageMetadata.mMessageInfo->GetPeerAddr() == update.GetMessageInfo().GetPeerAddr() &&
            aMessageMetadata.mMessageInfo->GetPeerPort() == update.GetMessageInfo().GetPeerPort())
        {
            ExitNow(ret = &update);
        }
    }

exit:
    return ret;
}

void Server::ProcessDnsUpdate(Message &aMessage, MessageMetadata &aMetadata)
{
    Error error = kErrorNone;
    Host *host  = nullptr;

    otLogInfoSrp("[server] Received DNS update from %s",
                 aMetadata.IsDirectRxFromClient() ? aMetadata.mMessageInfo->GetPeerAddr().ToString().AsCString()
                                                  : "an SRPL Partner");

    SuccessOrExit(error = ProcessZoneSection(aMessage, aMetadata));

    if (FindOutstandingUpdate(aMetadata) != nullptr)
    {
        otLogInfoSrp("[server] Drop duplicated SRP update request: MessageId=%hu", aMetadata.mDnsHeader.GetMessageId());

        // Silently drop duplicate requests.
        // This could rarely happen, because the outstanding SRP update timer should
        // be shorter than the SRP update retransmission timer.
        ExitNow(error = kErrorNone);
    }

    // Per 2.3.2 of SRP draft 6, no prerequisites should be included in a SRP update.
    VerifyOrExit(aMetadata.mDnsHeader.GetPrerequisiteRecordCount() == 0, error = kErrorFailed);

    host = Host::New(GetInstance(), aMetadata.mRxTime);
    VerifyOrExit(host != nullptr, error = kErrorNoBufs);
    SuccessOrExit(error = ProcessUpdateSection(*host, aMessage, aMetadata));

    // Parse lease time and validate signature.
    SuccessOrExit(error = ProcessAdditionalSection(host, aMessage, aMetadata));

    HandleUpdate(*host, aMetadata);

exit:
    if (error != kErrorNone)
    {
        if (host != nullptr)
        {
            host->Free();
        }

        if (aMetadata.IsDirectRxFromClient())
        {
            SendResponse(aMetadata.mDnsHeader, ErrorToDnsResponseCode(error), *aMetadata.mMessageInfo);
        }
    }
}

Error Server::ProcessZoneSection(const Message &aMessage, MessageMetadata &aMetadata) const
{
    Error    error = kErrorNone;
    char     name[Dns::Name::kMaxNameSize];
    uint16_t offset = aMetadata.mOffset;

    VerifyOrExit(aMetadata.mDnsHeader.GetZoneRecordCount() == 1, error = kErrorParse);

    SuccessOrExit(error = Dns::Name::ReadName(aMessage, offset, name, sizeof(name)));
    // TODO: return `Dns::kResponseNotAuth` for not authorized zone names.
    VerifyOrExit(strcmp(name, GetDomain()) == 0, error = kErrorSecurity);
    SuccessOrExit(error = aMessage.Read(offset, aMetadata.mDnsZone));
    offset += sizeof(Dns::Zone);

    VerifyOrExit(aMetadata.mDnsZone.GetType() == Dns::ResourceRecord::kTypeSoa, error = kErrorParse);
    aMetadata.mOffset = offset;

exit:
    return error;
}

Error Server::ProcessUpdateSection(Host &aHost, const Message &aMessage, MessageMetadata &aMetadata) const
{
    Error error = kErrorNone;

    // Process Service Discovery, Host and Service Description Instructions with
    // 3 times iterations over all DNS update RRs. The order of those processes matters.

    // 0. Enumerate over all Service Discovery Instructions before processing any other records.
    // So that we will know whether a name is a hostname or service instance name when processing
    // a "Delete All RRsets from a name" record.
    SuccessOrExit(error = ProcessServiceDiscoveryInstructions(aHost, aMessage, aMetadata));

    // 1. Enumerate over all RRs to build the Host Description Instruction.
    SuccessOrExit(error = ProcessHostDescriptionInstruction(aHost, aMessage, aMetadata));

    // 2. Enumerate over all RRs to build the Service Description Instructions.
    SuccessOrExit(error = ProcessServiceDescriptionInstructions(aHost, aMessage, aMetadata));

    // 3. Verify that there are no name conflicts.
    VerifyOrExit(!HasNameConflictsWith(aHost), error = kErrorDuplicated);

exit:
    return error;
}

Error Server::ProcessHostDescriptionInstruction(Host &                 aHost,
                                                const Message &        aMessage,
                                                const MessageMetadata &aMetadata) const
{
    Error    error;
    uint16_t offset = aMetadata.mOffset;

    OT_ASSERT(aHost.GetFullName() == nullptr);

    for (uint16_t numRecords = aMetadata.mDnsHeader.GetUpdateRecordCount(); numRecords > 0; numRecords--)
    {
        char                name[Dns::Name::kMaxNameSize];
        Dns::ResourceRecord record;

        SuccessOrExit(error = Dns::Name::ReadName(aMessage, offset, name, sizeof(name)));

        SuccessOrExit(error = aMessage.Read(offset, record));

        if (record.GetClass() == Dns::ResourceRecord::kClassAny)
        {
            // Delete All RRsets from a name.
            VerifyOrExit(IsValidDeleteAllRecord(record), error = kErrorFailed);

            // A "Delete All RRsets from a name" RR can only apply to a Service or Host Description.

            if (aHost.FindServiceDescription(name) == nullptr)
            {
                // If host name is already set to a different name, `SetFullName()`
                // will return `kErrorFailed`.
                SuccessOrExit(error = aHost.SetFullName(name));
                aHost.ClearResources();
            }
        }
        else if (record.GetType() == Dns::ResourceRecord::kTypeAaaa)
        {
            Dns::AaaaRecord aaaaRecord;

            VerifyOrExit(record.GetClass() == aMetadata.mDnsZone.GetClass(), error = kErrorFailed);

            SuccessOrExit(error = aHost.SetFullName(name));

            SuccessOrExit(error = aMessage.Read(offset, aaaaRecord));
            VerifyOrExit(aaaaRecord.IsValid(), error = kErrorParse);

            // Tolerate kErrorDrop for AAAA Resources.
            VerifyOrExit(aHost.AddIp6Address(aaaaRecord.GetAddress()) != kErrorNoBufs, error = kErrorNoBufs);
        }
        else if (record.GetType() == Dns::ResourceRecord::kTypeKey)
        {
            // We currently support only ECDSA P-256.
            Dns::Ecdsa256KeyRecord key;

            VerifyOrExit(record.GetClass() == aMetadata.mDnsZone.GetClass(), error = kErrorFailed);
            SuccessOrExit(error = aMessage.Read(offset, key));
            VerifyOrExit(key.IsValid(), error = kErrorParse);

            VerifyOrExit(aHost.GetKey() == nullptr || *aHost.GetKey() == key, error = kErrorSecurity);
            aHost.SetKey(key);
        }

        offset += record.GetSize();
    }

    // Verify that we have a complete Host Description Instruction.

    VerifyOrExit(aHost.GetFullName() != nullptr, error = kErrorFailed);
    VerifyOrExit(aHost.GetKey() != nullptr, error = kErrorFailed);

    // We check the number of host addresses after processing of the
    // Lease Option in the Addition Section and determining whether
    // the host is being removed or registered.

exit:
    return error;
}

Error Server::ProcessServiceDiscoveryInstructions(Host &                 aHost,
                                                  const Message &        aMessage,
                                                  const MessageMetadata &aMetadata) const
{
    Error    error  = kErrorNone;
    uint16_t offset = aMetadata.mOffset;

    for (uint16_t numRecords = aMetadata.mDnsHeader.GetUpdateRecordCount(); numRecords > 0; numRecords--)
    {
        char           serviceName[Dns::Name::kMaxNameSize];
        char           instanceName[Dns::Name::kMaxNameSize];
        Dns::PtrRecord ptrRecord;
        const char *   subServiceName;
        Service *      service;
        bool           isSubType;

        SuccessOrExit(error = Dns::Name::ReadName(aMessage, offset, serviceName, sizeof(serviceName)));
        VerifyOrExit(Dns::Name::IsSubDomainOf(serviceName, GetDomain()), error = kErrorSecurity);

        error = Dns::ResourceRecord::ReadRecord(aMessage, offset, ptrRecord);

        if (error == kErrorNotFound)
        {
            // `ReadRecord()` updates `aOffset` to skip over a
            // non-matching record.
            error = kErrorNone;
            continue;
        }

        SuccessOrExit(error);

        SuccessOrExit(error = Dns::Name::ReadName(aMessage, offset, instanceName, sizeof(instanceName)));

        VerifyOrExit(ptrRecord.GetClass() == Dns::ResourceRecord::kClassNone ||
                         ptrRecord.GetClass() == aMetadata.mDnsZone.GetClass(),
                     error = kErrorFailed);

        // Check if the `serviceName` is a subtype with the name
        // format: "<sub-label>._sub.<service-labels>.<domain>."

        subServiceName = StringFind(serviceName, kServiceSubTypeLabel);
        isSubType      = (subServiceName != nullptr);

        if (isSubType)
        {
            // Skip over the "._sub." label to get to the base
            // service name.
            subServiceName += sizeof(kServiceSubTypeLabel) - 1;
        }

        // Verify that instance name and service name are related.

        VerifyOrExit(StringEndsWith(instanceName, isSubType ? subServiceName : serviceName), error = kErrorFailed);

        // Ensure the same service does not exist already.
        VerifyOrExit(aHost.FindService(serviceName, instanceName) == nullptr, error = kErrorFailed);

        service = aHost.AddNewService(serviceName, instanceName, isSubType, aMetadata.mRxTime);
        VerifyOrExit(service != nullptr, error = kErrorNoBufs);

        // This RR is a "Delete an RR from an RRset" update when the CLASS is NONE.
        service->mIsDeleted = (ptrRecord.GetClass() == Dns::ResourceRecord::kClassNone);
    }

exit:
    return error;
}

Error Server::ProcessServiceDescriptionInstructions(Host &           aHost,
                                                    const Message &  aMessage,
                                                    MessageMetadata &aMetadata) const
{
    Error    error  = kErrorNone;
    uint16_t offset = aMetadata.mOffset;

    for (uint16_t numRecords = aMetadata.mDnsHeader.GetUpdateRecordCount(); numRecords > 0; numRecords--)
    {
        Service::Description *desc;
        char                  name[Dns::Name::kMaxNameSize];
        Dns::ResourceRecord   record;

        SuccessOrExit(error = Dns::Name::ReadName(aMessage, offset, name, sizeof(name)));
        SuccessOrExit(error = aMessage.Read(offset, record));

        if (record.GetClass() == Dns::ResourceRecord::kClassAny)
        {
            // Delete All RRsets from a name.
            VerifyOrExit(IsValidDeleteAllRecord(record), error = kErrorFailed);

            desc = aHost.FindServiceDescription(name);

            if (desc != nullptr)
            {
                desc->ClearResources();
                desc->mUpdateTime = aMetadata.mRxTime;
            }

            offset += record.GetSize();
            continue;
        }

        if (record.GetType() == Dns::ResourceRecord::kTypeSrv)
        {
            Dns::SrvRecord srvRecord;
            char           hostName[Dns::Name::kMaxNameSize];
            uint16_t       hostNameLength = sizeof(hostName);

            VerifyOrExit(record.GetClass() == aMetadata.mDnsZone.GetClass(), error = kErrorFailed);
            SuccessOrExit(error = aMessage.Read(offset, srvRecord));
            offset += sizeof(srvRecord);

            SuccessOrExit(error = Dns::Name::ReadName(aMessage, offset, hostName, hostNameLength));
            VerifyOrExit(Dns::Name::IsSubDomainOf(name, GetDomain()), error = kErrorSecurity);
            VerifyOrExit(aHost.Matches(hostName), error = kErrorFailed);

            desc = aHost.FindServiceDescription(name);
            VerifyOrExit(desc != nullptr, error = kErrorFailed);

            // Make sure that this is the first SRV RR for this service description
            VerifyOrExit(desc->mPort == 0, error = kErrorFailed);
            desc->mPriority   = srvRecord.GetPriority();
            desc->mWeight     = srvRecord.GetWeight();
            desc->mPort       = srvRecord.GetPort();
            desc->mUpdateTime = aMetadata.mRxTime;
        }
        else if (record.GetType() == Dns::ResourceRecord::kTypeTxt)
        {
            VerifyOrExit(record.GetClass() == aMetadata.mDnsZone.GetClass(), error = kErrorFailed);

            desc = aHost.FindServiceDescription(name);
            VerifyOrExit(desc != nullptr, error = kErrorFailed);

            offset += sizeof(record);
            SuccessOrExit(error = desc->SetTxtDataFromMessage(aMessage, offset, record.GetLength()));
            offset += record.GetLength();
        }
        else
        {
            offset += record.GetSize();
        }
    }

    // Verify that all service descriptions on `aHost` are updated. Note
    // that `mUpdateTime` on a new `Service::Description` is set to
    // `GetNow().GetDistantPast()`.

    for (Service::Description &desc : aHost.mServiceDescriptions)
    {
        VerifyOrExit(desc.mUpdateTime == aMetadata.mRxTime, error = kErrorFailed);

        // Check that either both `mPort` and `mTxtData` are set
        // (i.e., we saw both SRV and TXT record) or both are default
        // (cleared) value (i.e., we saw neither of them).

        VerifyOrExit((desc.mPort == 0) == desc.mTxtData.IsNull(), error = kErrorFailed);
    }

    aMetadata.mOffset = offset;

exit:
    return error;
}

bool Server::IsValidDeleteAllRecord(const Dns::ResourceRecord &aRecord)
{
    return aRecord.GetClass() == Dns::ResourceRecord::kClassAny && aRecord.GetType() == Dns::ResourceRecord::kTypeAny &&
           aRecord.GetTtl() == 0 && aRecord.GetLength() == 0;
}

Error Server::ProcessAdditionalSection(Host *aHost, const Message &aMessage, MessageMetadata &aMetadata) const
{
    Error            error = kErrorNone;
    Dns::OptRecord   optRecord;
    Dns::LeaseOption leaseOption;
    Dns::SigRecord   sigRecord;
    char             name[2]; // The root domain name (".") is expected.
    uint16_t         offset = aMetadata.mOffset;
    uint16_t         sigOffset;
    uint16_t         sigRdataOffset;
    char             signerName[Dns::Name::kMaxNameSize];
    uint16_t         signatureLength;

    VerifyOrExit(aMetadata.mDnsHeader.GetAdditionalRecordCount() == 2, error = kErrorFailed);

    // EDNS(0) Update Lease Option.

    SuccessOrExit(error = Dns::Name::ReadName(aMessage, offset, name, sizeof(name)));
    SuccessOrExit(error = aMessage.Read(offset, optRecord));
    SuccessOrExit(error = aMessage.Read(offset + sizeof(optRecord), leaseOption));
    VerifyOrExit(leaseOption.IsValid(), error = kErrorFailed);
    VerifyOrExit(optRecord.GetSize() == sizeof(optRecord) + sizeof(leaseOption), error = kErrorParse);

    offset += optRecord.GetSize();

    aHost->SetLease(leaseOption.GetLeaseInterval());
    aHost->SetKeyLease(leaseOption.GetKeyLeaseInterval());

    if (aHost->GetLease() > 0)
    {
        uint8_t hostAddressesNum;

        aHost->GetAddresses(hostAddressesNum);

        // There MUST be at least one valid address if we have nonzero lease.
        VerifyOrExit(hostAddressesNum > 0, error = kErrorFailed);
    }

    // SIG(0).

    sigOffset = offset;
    SuccessOrExit(error = Dns::Name::ReadName(aMessage, offset, name, sizeof(name)));
    SuccessOrExit(error = aMessage.Read(offset, sigRecord));
    VerifyOrExit(sigRecord.IsValid(), error = kErrorParse);

    sigRdataOffset = offset + sizeof(Dns::ResourceRecord);
    offset += sizeof(sigRecord);

    // TODO: Verify that the signature doesn't expire. This is not
    // implemented because the end device may not be able to get
    // the synchronized date/time.

    SuccessOrExit(error = Dns::Name::ReadName(aMessage, offset, signerName, sizeof(signerName)));

    signatureLength = sigRecord.GetLength() - (offset - sigRdataOffset);
    offset += signatureLength;

    // Verify the signature. Currently supports only ECDSA.

    VerifyOrExit(sigRecord.GetAlgorithm() == Dns::KeyRecord::kAlgorithmEcdsaP256Sha256, error = kErrorFailed);
    VerifyOrExit(sigRecord.GetTypeCovered() == 0, error = kErrorFailed);
    VerifyOrExit(signatureLength == Crypto::Ecdsa::P256::Signature::kSize, error = kErrorParse);

    SuccessOrExit(error = VerifySignature(*aHost->GetKey(), aMessage, aMetadata.mDnsHeader, sigOffset, sigRdataOffset,
                                          sigRecord.GetLength(), signerName));

    aMetadata.mOffset = offset;

exit:
    return error;
}

Error Server::VerifySignature(const Dns::Ecdsa256KeyRecord &aKey,
                              const Message &               aMessage,
                              Dns::UpdateHeader             aDnsHeader,
                              uint16_t                      aSigOffset,
                              uint16_t                      aSigRdataOffset,
                              uint16_t                      aSigRdataLength,
                              const char *                  aSignerName) const
{
    Error                          error;
    uint16_t                       offset = aMessage.GetOffset();
    uint16_t                       signatureOffset;
    Crypto::Sha256                 sha256;
    Crypto::Sha256::Hash           hash;
    Crypto::Ecdsa::P256::Signature signature;
    Message *                      signerNameMessage = nullptr;

    VerifyOrExit(aSigRdataLength >= Crypto::Ecdsa::P256::Signature::kSize, error = kErrorInvalidArgs);

    sha256.Start();

    // SIG RDATA less signature.
    sha256.Update(aMessage, aSigRdataOffset, sizeof(Dns::SigRecord) - sizeof(Dns::ResourceRecord));

    // The uncompressed (canonical) form of the signer name should be used for signature
    // verification. See https://tools.ietf.org/html/rfc2931#section-3.1 for details.
    signerNameMessage = Get<Ip6::Udp>().NewMessage(0);
    VerifyOrExit(signerNameMessage != nullptr, error = kErrorNoBufs);
    SuccessOrExit(error = Dns::Name::AppendName(aSignerName, *signerNameMessage));
    sha256.Update(*signerNameMessage, signerNameMessage->GetOffset(), signerNameMessage->GetLength());

    // We need the DNS header before appending the SIG RR.
    aDnsHeader.SetAdditionalRecordCount(aDnsHeader.GetAdditionalRecordCount() - 1);
    sha256.Update(aDnsHeader);
    sha256.Update(aMessage, offset + sizeof(aDnsHeader), aSigOffset - offset - sizeof(aDnsHeader));

    sha256.Finish(hash);

    signatureOffset = aSigRdataOffset + aSigRdataLength - Crypto::Ecdsa::P256::Signature::kSize;
    SuccessOrExit(error = aMessage.Read(signatureOffset, signature));

    error = aKey.GetKey().Verify(hash, signature);

exit:
    FreeMessage(signerNameMessage);
    return error;
}

void Server::HandleUpdate(Host &aHost, const MessageMetadata &aMetadata)
{
    Error error = kErrorNone;
    Host *existingHost;

    // Check whether the SRP update wants to remove `aHost`.

    VerifyOrExit(aHost.GetLease() == 0);

    aHost.ClearResources();

    existingHost = mHosts.FindMatching(aHost.GetFullName());
    VerifyOrExit(existingHost != nullptr);

    // The client may not include all services it has registered before
    // when removing a host. We copy and append any missing services to
    // `aHost` from the `existingHost` and mark them as deleted.

    for (Service &service : existingHost->mServices)
    {
        if (service.mIsDeleted)
        {
            continue;
        }

        if (aHost.FindService(service.GetServiceName(), service.GetInstanceName()) == nullptr)
        {
            Service *newService = aHost.AddNewService(service.GetServiceName(), service.GetInstanceName(),
                                                      service.IsSubType(), aMetadata.mRxTime);

            VerifyOrExit(newService != nullptr, error = kErrorNoBufs);
            newService->mDescription->mUpdateTime = aMetadata.mRxTime;
            newService->mIsDeleted                = true;
        }
    }

exit:
    if ((error == kErrorNone) && (mServiceUpdateHandler != nullptr))
    {
        UpdateMetadata *update = UpdateMetadata::New(GetInstance(), aHost, aMetadata);

        mOutstandingUpdates.Push(*update);
        mOutstandingUpdatesTimer.FireAtIfEarlier(update->GetExpireTime());

        otLogInfoSrp("[server] SRP update handler is notified (updatedId = %u)", update->GetId());
        mServiceUpdateHandler(update->GetId(), &aHost, kDefaultEventsHandlerTimeout, mServiceUpdateHandlerContext);
    }
    else
    {
        CommitSrpUpdate(error, aHost, aMetadata);
    }
}

void Server::SendResponse(const Dns::UpdateHeader &   aHeader,
                          Dns::UpdateHeader::Response aResponseCode,
                          const Ip6::MessageInfo &    aMessageInfo)
{
    Error             error;
    Message *         response = nullptr;
    Dns::UpdateHeader header;

    response = GetSocket().NewMessage(0);
    VerifyOrExit(response != nullptr, error = kErrorNoBufs);

    header.SetMessageId(aHeader.GetMessageId());
    header.SetType(Dns::UpdateHeader::kTypeResponse);
    header.SetQueryType(aHeader.GetQueryType());
    header.SetResponseCode(aResponseCode);
    SuccessOrExit(error = response->Append(header));

    SuccessOrExit(error = GetSocket().SendTo(*response, aMessageInfo));

    if (aResponseCode != Dns::UpdateHeader::kResponseSuccess)
    {
        otLogInfoSrp("[server] send fail response: %d", aResponseCode);
    }
    else
    {
        otLogInfoSrp("[server] send success response");
    }

exit:
    if (error != kErrorNone)
    {
        otLogWarnSrp("[server] failed to send response: %s", ErrorToString(error));
        FreeMessage(response);
    }
}

void Server::SendResponse(const Dns::UpdateHeader &aHeader,
                          uint32_t                 aLease,
                          uint32_t                 aKeyLease,
                          const Ip6::MessageInfo & aMessageInfo)
{
    Error             error;
    Message *         response = nullptr;
    Dns::UpdateHeader header;
    Dns::OptRecord    optRecord;
    Dns::LeaseOption  leaseOption;

    response = GetSocket().NewMessage(0);
    VerifyOrExit(response != nullptr, error = kErrorNoBufs);

    header.SetMessageId(aHeader.GetMessageId());
    header.SetType(Dns::UpdateHeader::kTypeResponse);
    header.SetQueryType(aHeader.GetQueryType());
    header.SetResponseCode(Dns::UpdateHeader::kResponseSuccess);
    header.SetAdditionalRecordCount(1);
    SuccessOrExit(error = response->Append(header));

    // Append the root domain (".").
    SuccessOrExit(error = Dns::Name::AppendTerminator(*response));

    optRecord.Init();
    optRecord.SetUdpPayloadSize(kUdpPayloadSize);
    optRecord.SetDnsSecurityFlag();
    optRecord.SetLength(sizeof(Dns::LeaseOption));
    SuccessOrExit(error = response->Append(optRecord));

    leaseOption.Init();
    leaseOption.SetLeaseInterval(aLease);
    leaseOption.SetKeyLeaseInterval(aKeyLease);
    SuccessOrExit(error = response->Append(leaseOption));

    SuccessOrExit(error = GetSocket().SendTo(*response, aMessageInfo));

    otLogInfoSrp("[server] send response with granted lease: %u and key lease: %u", aLease, aKeyLease);

exit:
    if (error != kErrorNone)
    {
        otLogWarnSrp("[server] failed to send response: %s", ErrorToString(error));
        FreeMessage(response);
    }
}

void Server::HandleUdpReceive(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    static_cast<Server *>(aContext)->HandleUdpReceive(AsCoreType(aMessage), AsCoreType(aMessageInfo));
}

void Server::HandleUdpReceive(Message &aMessage, const Ip6::MessageInfo &aMessageInfo)
{
    Error error = ProcessMessage(aMessage, aMessageInfo);

    if (error != kErrorNone)
    {
        otLogInfoSrp("[server] failed to handle DNS message: %s", ErrorToString(error));
    }
}

Error Server::ProcessMessage(Message &aMessage, const Ip6::MessageInfo &aMessageInfo)
{
    return ProcessMessage(aMessage, TimerMilli::GetNow(), mLeaseConfig, &aMessageInfo);
}

Error Server::ProcessMessage(Message &               aMessage,
                             TimeMilli               aRxTime,
                             const LeaseConfig &     aLeaseConfig,
                             const Ip6::MessageInfo *aMessageInfo)
{
    Error           error;
    MessageMetadata metadata;

    metadata.mOffset      = aMessage.GetOffset();
    metadata.mRxTime      = aRxTime;
    metadata.mLeaseConfig = aLeaseConfig;
    metadata.mMessageInfo = aMessageInfo;

    SuccessOrExit(error = aMessage.Read(metadata.mOffset, metadata.mDnsHeader));
    metadata.mOffset += sizeof(Dns::UpdateHeader);

    VerifyOrExit(metadata.mDnsHeader.GetType() == Dns::UpdateHeader::Type::kTypeQuery, error = kErrorDrop);
    VerifyOrExit(metadata.mDnsHeader.GetQueryType() == Dns::UpdateHeader::kQueryTypeUpdate, error = kErrorDrop);

    ProcessDnsUpdate(aMessage, metadata);

exit:
    return error;
}

void Server::HandleLeaseTimer(Timer &aTimer)
{
    aTimer.Get<Server>().HandleLeaseTimer();
}

void Server::HandleLeaseTimer(void)
{
    TimeMilli now                = TimerMilli::GetNow();
    TimeMilli earliestExpireTime = now.GetDistantFuture();
    Host *    nextHost;

    for (Host *host = mHosts.GetHead(); host != nullptr; host = nextHost)
    {
        nextHost = host->GetNext();

        if (host->GetKeyExpireTime() <= now)
        {
            otLogInfoSrp("[server] KEY LEASE of host %s expired", host->GetFullName());

            // Removes the whole host and all services if the KEY RR expired.
            RemoveHost(host, kDeleteName, kNotifyServiceHandler);
        }
        else if (host->IsDeleted())
        {
            // The host has been deleted, but the hostname & service instance names retain.

            Service *next;

            earliestExpireTime = OT_MIN(earliestExpireTime, host->GetKeyExpireTime());

            // Check if any service instance name expired.
            for (Service *service = host->mServices.GetHead(); service != nullptr; service = next)
            {
                next = service->GetNext();

                OT_ASSERT(service->mIsDeleted);

                if (service->GetKeyExpireTime() <= now)
                {
                    service->Log(Service::kKeyLeaseExpired);
                    host->RemoveService(service, kDeleteName, kNotifyServiceHandler);
                }
                else
                {
                    earliestExpireTime = OT_MIN(earliestExpireTime, service->GetKeyExpireTime());
                }
            }
        }
        else if (host->GetExpireTime() <= now)
        {
            otLogInfoSrp("[server] LEASE of host %s expired", host->GetFullName());

            // If the host expired, delete all resources of this host and its services.
            for (Service &service : host->mServices)
            {
                // Don't need to notify the service handler as `RemoveHost` at below will do.
                host->RemoveService(&service, kRetainName, kDoNotNotifyServiceHandler);
            }

            RemoveHost(host, kRetainName, kNotifyServiceHandler);

            earliestExpireTime = OT_MIN(earliestExpireTime, host->GetKeyExpireTime());
        }
        else
        {
            // The host doesn't expire, check if any service expired or is explicitly removed.

            Service *next;

            OT_ASSERT(!host->IsDeleted());

            earliestExpireTime = OT_MIN(earliestExpireTime, host->GetExpireTime());

            for (Service *service = host->mServices.GetHead(); service != nullptr; service = next)
            {
                next = service->GetNext();

                if (service->GetKeyExpireTime() <= now)
                {
                    service->Log(Service::kKeyLeaseExpired);
                    host->RemoveService(service, kDeleteName, kNotifyServiceHandler);
                }
                else if (service->mIsDeleted)
                {
                    // The service has been deleted but the name retains.
                    earliestExpireTime = OT_MIN(earliestExpireTime, service->GetKeyExpireTime());
                }
                else if (service->GetExpireTime() <= now)
                {
                    service->Log(Service::kLeaseExpired);

                    // The service is expired, delete it.
                    host->RemoveService(service, kRetainName, kNotifyServiceHandler);
                    earliestExpireTime = OT_MIN(earliestExpireTime, service->GetKeyExpireTime());
                }
                else
                {
                    earliestExpireTime = OT_MIN(earliestExpireTime, service->GetExpireTime());
                }
            }
        }
    }

    if (earliestExpireTime != now.GetDistantFuture())
    {
        OT_ASSERT(earliestExpireTime >= now);
        if (!mLeaseTimer.IsRunning() || earliestExpireTime <= mLeaseTimer.GetFireTime())
        {
            otLogInfoSrp("[server] lease timer is scheduled for %u seconds", Time::MsecToSec(earliestExpireTime - now));
            mLeaseTimer.StartAt(earliestExpireTime, 0);
        }
    }
    else
    {
        otLogInfoSrp("[server] lease timer is stopped");
        mLeaseTimer.Stop();
    }
}

void Server::HandleOutstandingUpdatesTimer(Timer &aTimer)
{
    aTimer.Get<Server>().HandleOutstandingUpdatesTimer();
}

void Server::HandleOutstandingUpdatesTimer(void)
{
    while (!mOutstandingUpdates.IsEmpty() && mOutstandingUpdates.GetTail()->GetExpireTime() <= TimerMilli::GetNow())
    {
        otLogInfoSrp("[server] outstanding service update timeout (updateId = %u)",
                     mOutstandingUpdates.GetTail()->GetId());
        HandleServiceUpdateResult(mOutstandingUpdates.GetTail(), kErrorResponseTimeout);
    }
}

const char *Server::AddressModeToString(AddressMode aMode)
{
    static const char *const kAddressModeStrings[] = {
        "unicast", // (0) kAddressModeUnicast
        "anycast", // (1) kAddressModeAnycast
    };

    static_assert(kAddressModeUnicast == 0, "kAddressModeUnicast value is incorrect");
    static_assert(kAddressModeAnycast == 1, "kAddressModeAnycast value is incorrect");

    return kAddressModeStrings[aMode];
}

//---------------------------------------------------------------------------------------------------------------------
// Server::Service

Error Server::Service::Init(const char *aServiceName, Description &aDescription, bool aIsSubType, TimeMilli aUpdateTime)
{
    mDescription = &aDescription;
    mNext        = nullptr;
    mUpdateTime  = aUpdateTime;
    mIsDeleted   = false;
    mIsSubType   = aIsSubType;
    mIsCommitted = false;

    return mServiceName.Set(aServiceName);
}

Error Server::Service::GetServiceSubTypeLabel(char *aLabel, uint8_t aMaxSize) const
{
    Error       error       = kErrorNone;
    const char *serviceName = GetServiceName();
    const char *subServiceName;
    uint8_t     labelLength;

    memset(aLabel, 0, aMaxSize);

    VerifyOrExit(IsSubType(), error = kErrorInvalidArgs);

    subServiceName = StringFind(serviceName, kServiceSubTypeLabel);
    OT_ASSERT(subServiceName != nullptr);

    if (subServiceName - serviceName < aMaxSize)
    {
        labelLength = static_cast<uint8_t>(subServiceName - serviceName);
    }
    else
    {
        labelLength = aMaxSize - 1;
        error       = kErrorNoBufs;
    }

    memcpy(aLabel, serviceName, labelLength);

exit:
    return error;
}

TimeMilli Server::Service::GetExpireTime(void) const
{
    OT_ASSERT(!mIsDeleted);
    OT_ASSERT(!GetHost().IsDeleted());

    return mUpdateTime + Time::SecToMsec(mDescription->mLease);
}

TimeMilli Server::Service::GetKeyExpireTime(void) const
{
    return mUpdateTime + Time::SecToMsec(mDescription->mKeyLease);
}

bool Server::Service::MatchesFlags(Flags aFlags) const
{
    bool matches = false;

    if (IsSubType())
    {
        VerifyOrExit(aFlags & kFlagSubType);
    }
    else
    {
        VerifyOrExit(aFlags & kFlagBaseType);
    }

    if (IsDeleted())
    {
        VerifyOrExit(aFlags & kFlagDeleted);
    }
    else
    {
        VerifyOrExit(aFlags & kFlagActive);
    }

    matches = true;

exit:
    return matches;
}

#if (OPENTHREAD_CONFIG_LOG_LEVEL >= OT_LOG_LEVEL_INFO) && OPENTHREAD_CONFIG_LOG_SRP
void Server::Service::Log(Action aAction) const
{
    static const char *const kActionStrings[] = {
        "add new",                   // (0) kAddNew
        "update existing",           // (1) kUpdateExisting
        "remove but retain name of", // (2) kRemoveButRetainName
        "full remove",               // (3) kFullyRemove
        "LEASE expired for ",        // (4) kLeaseExpired
        "KEY LEASE expired for",     // (5) kKeyLeaseExpired
    };

    char subLabel[Dns::Name::kMaxLabelSize];

    static_assert(0 == kAddNew, "kAddNew value is incorrect");
    static_assert(1 == kUpdateExisting, "kUpdateExisting value is incorrect");
    static_assert(2 == kRemoveButRetainName, "kRemoveButRetainName value is incorrect");
    static_assert(3 == kFullyRemove, "kFullyRemove value is incorrect");
    static_assert(4 == kLeaseExpired, "kLeaseExpired value is incorrect");
    static_assert(5 == kKeyLeaseExpired, "kKeyLeaseExpired value is incorrect");

    // We only log if the `Service` is marked as committed. This
    // ensures that temporary `Service` entries associated with a
    // newly received SRP update message are not logged (e.g., when
    // associated `Host` is being freed).

    if (mIsCommitted)
    {
        IgnoreError(GetServiceSubTypeLabel(subLabel, sizeof(subLabel)));

        otLogInfoSrp("[server] %s service '%s'%s%s", kActionStrings[aAction], GetInstanceName(),
                     IsSubType() ? " subtype:" : "", subLabel);
    }
}
#else
void Server::Service::Log(Action) const
{
}
#endif // #if (OPENTHREAD_CONFIG_LOG_LEVEL >= OT_LOG_LEVEL_INFO) && OPENTHREAD_CONFIG_LOG_SRP

//---------------------------------------------------------------------------------------------------------------------
// Server::Service::Description

Error Server::Service::Description::Init(const char *aInstanceName, Host &aHost)
{
    mNext       = nullptr;
    mHost       = &aHost;
    mPriority   = 0;
    mWeight     = 0;
    mPort       = 0;
    mLease      = 0;
    mKeyLease   = 0;
    mUpdateTime = TimerMilli::GetNow().GetDistantPast();
    mTxtData.Free();

    return mInstanceName.Set(aInstanceName);
}

void Server::Service::Description::ClearResources(void)
{
    mPort = 0;
    mTxtData.Free();
}

void Server::Service::Description::TakeResourcesFrom(Description &aDescription)
{
    mTxtData.SetFrom(static_cast<Heap::Data &&>(aDescription.mTxtData));

    mPriority = aDescription.mPriority;
    mWeight   = aDescription.mWeight;
    mPort     = aDescription.mPort;

    mLease      = aDescription.mLease;
    mKeyLease   = aDescription.mKeyLease;
    mUpdateTime = TimerMilli::GetNow();
}

Error Server::Service::Description::SetTxtDataFromMessage(const Message &aMessage, uint16_t aOffset, uint16_t aLength)
{
    Error error;

    SuccessOrExit(error = mTxtData.SetFrom(aMessage, aOffset, aLength));
    VerifyOrExit(Dns::TxtRecord::VerifyTxtData(mTxtData.GetBytes(), mTxtData.GetLength(), /* aAllowEmpty */ false),
                 error = kErrorParse);

exit:
    if (error != kErrorNone)
    {
        mTxtData.Free();
    }

    return error;
}

//---------------------------------------------------------------------------------------------------------------------
// Server::Host

Error Server::Host::Init(Instance &aInstance, TimeMilli aUpdateTime)
{
    InstanceLocatorInit::Init(aInstance);
    mNext = nullptr;
    mFullName.Free();
    mAddresses.Clear();
    mKey.Clear();
    mLease      = 0;
    mKeyLease   = 0;
    mUpdateTime = aUpdateTime;
    mServices.Clear();
    mServiceDescriptions.Clear();

    return kErrorNone;
}

Server::Host::~Host(void)
{
    FreeAllServices();
}

Error Server::Host::SetFullName(const char *aFullName)
{
    // `mFullName` becomes immutable after it is set, so if it is
    // already set, we only accept a `aFullName` that matches the
    // current name.

    Error error;

    if (mFullName.IsNull())
    {
        error = mFullName.Set(aFullName);
    }
    else
    {
        error = Matches(aFullName) ? kErrorNone : kErrorFailed;
    }

    return error;
}

void Server::Host::SetKey(Dns::Ecdsa256KeyRecord &aKey)
{
    OT_ASSERT(aKey.IsValid());

    mKey = aKey;
}

TimeMilli Server::Host::GetExpireTime(void) const
{
    OT_ASSERT(!IsDeleted());

    return mUpdateTime + Time::SecToMsec(mLease);
}

TimeMilli Server::Host::GetKeyExpireTime(void) const
{
    return mUpdateTime + Time::SecToMsec(mKeyLease);
}

const Server::Service *Server::Host::FindNextService(const Service *aPrevService,
                                                     Service::Flags aFlags,
                                                     const char *   aServiceName,
                                                     const char *   aInstanceName) const
{
    const Service *service = (aPrevService == nullptr) ? GetServices().GetHead() : aPrevService->GetNext();

    for (; service != nullptr; service = service->GetNext())
    {
        if (!service->MatchesFlags(aFlags))
        {
            continue;
        }

        if ((aServiceName != nullptr) && !service->MatchesServiceName(aServiceName))
        {
            continue;
        }

        if ((aInstanceName != nullptr) && !service->MatchesInstanceName(aInstanceName))
        {
            continue;
        }

        break;
    }

    return service;
}

Server::Service *Server::Host::AddNewService(const char *aServiceName,
                                             const char *aInstanceName,
                                             bool        aIsSubType,
                                             TimeMilli   aUpdateTime)
{
    Service *             service = nullptr;
    Service::Description *desc;

    desc = FindServiceDescription(aInstanceName);

    if (desc == nullptr)
    {
        desc = Service::Description::New(aInstanceName, *this);
        VerifyOrExit(desc != nullptr);
        mServiceDescriptions.Push(*desc);
    }

    service = Service::New(aServiceName, *desc, aIsSubType, aUpdateTime);
    VerifyOrExit(service != nullptr);

    mServices.Push(*service);

exit:
    return service;
}

void Server::Host::RemoveService(Service *aService, RetainName aRetainName, NotifyMode aNotifyServiceHandler)
{
    Server &server = Get<Server>();

    VerifyOrExit(aService != nullptr);

    aService->mIsDeleted = true;

    aService->Log(aRetainName ? Service::kRemoveButRetainName : Service::kFullyRemove);

    if (aNotifyServiceHandler && server.mServiceUpdateHandler != nullptr)
    {
        uint32_t updateId = server.AllocateId();

        otLogInfoSrp("[server] SRP update handler is notified (updatedId = %u)", updateId);
        server.mServiceUpdateHandler(updateId, this, kDefaultEventsHandlerTimeout, server.mServiceUpdateHandlerContext);
        // We don't wait for the reply from the service update handler,
        // but always remove the service regardless of service update result.
        // Because removing a service should fail only when there is system
        // failure of the platform mDNS implementation and in which case the
        // service is not expected to be still registered.
    }

    if (!aRetainName)
    {
        IgnoreError(mServices.Remove(*aService));
        aService->Free();
        FreeUnusedServiceDescriptions();
    }

exit:
    return;
}

void Server::Host::FreeAllServices(void)
{
    while (!mServices.IsEmpty())
    {
        RemoveService(mServices.GetHead(), kDeleteName, kDoNotNotifyServiceHandler);
    }
}

void Server::Host::FreeUnusedServiceDescriptions(void)
{
    Service::Description *desc;
    Service::Description *prev;
    Service::Description *next;

    for (prev = nullptr, desc = mServiceDescriptions.GetHead(); desc != nullptr; desc = next)
    {
        next = desc->GetNext();

        if (FindNextService(/* aPrevService */ nullptr, kFlagsAnyService, /* aServiceName */ nullptr,
                            desc->GetInstanceName()) == nullptr)
        {
            mServiceDescriptions.PopAfter(prev);
            desc->Free();

            // When the `desc` is removed from the list
            // we keep the `prev` pointer same as before.
        }
        else
        {
            prev = desc;
        }
    }
}

void Server::Host::ClearResources(void)
{
    mAddresses.Clear();
}

Error Server::Host::MergeServicesAndResourcesFrom(Host &aHost)
{
    // This method merges services, service descriptions, and other
    // resources from another `aHost` into current host. It can
    // possibly take ownership of some items from `aHost`.

    Error error = kErrorNone;

    otLogInfoSrp("[server] update host %s", GetFullName());

    mAddresses  = aHost.mAddresses;
    mKey        = aHost.mKey;
    mLease      = aHost.mLease;
    mKeyLease   = aHost.mKeyLease;
    mUpdateTime = TimerMilli::GetNow();

    for (Service &service : aHost.mServices)
    {
        Service *existingService = FindService(service.GetServiceName(), service.GetInstanceName());
        Service *newService;

        if (service.mIsDeleted)
        {
            // `RemoveService()` does nothing if `exitsingService` is `nullptr`.
            RemoveService(existingService, kRetainName, kDoNotNotifyServiceHandler);
            continue;
        }

        // Add/Merge `service` into the existing service or a allocate a new one

        newService = (existingService != nullptr) ? existingService
                                                  : AddNewService(service.GetServiceName(), service.GetInstanceName(),
                                                                  service.IsSubType(), service.GetUpdateTime());

        VerifyOrExit(newService != nullptr, error = kErrorNoBufs);

        newService->mIsDeleted   = false;
        newService->mIsCommitted = true;
        newService->mUpdateTime  = TimerMilli::GetNow();

        if (!service.mIsSubType)
        {
            // (1) Service description is shared across a base type and all its subtypes.
            // (2) `TakeResourcesFrom()` releases resources pinned to its argument.
            // Therefore, make sure the function is called only for the base type.
            newService->mDescription->TakeResourcesFrom(*service.mDescription);
        }

        newService->Log((existingService != nullptr) ? Service::kUpdateExisting : Service::kAddNew);
    }

exit:
    return error;
}

const Server::Service::Description *Server::Host::FindServiceDescription(const char *aInstanceName) const
{
    return mServiceDescriptions.FindMatching(aInstanceName);
}

Server::Service::Description *Server::Host::FindServiceDescription(const char *aInstanceName)
{
    return AsNonConst(AsConst(this)->FindServiceDescription(aInstanceName));
}

const Server::Service *Server::Host::FindService(const char *aServiceName, const char *aInstanceName) const
{
    return FindNextService(/* aPrevService */ nullptr, kFlagsAnyService, aServiceName, aInstanceName);
}

Server::Service *Server::Host::FindService(const char *aServiceName, const char *aInstanceName)
{
    return AsNonConst(AsConst(this)->FindService(aServiceName, aInstanceName));
}

Error Server::Host::AddIp6Address(const Ip6::Address &aIp6Address)
{
    Error error = kErrorNone;

    if (aIp6Address.IsMulticast() || aIp6Address.IsUnspecified() || aIp6Address.IsLoopback())
    {
        // We don't like those address because they cannot be used
        // for communication with exterior devices.
        ExitNow(error = kErrorDrop);
    }

    // Drop duplicate addresses.
    VerifyOrExit(!mAddresses.Contains(aIp6Address), error = kErrorDrop);

    error = mAddresses.PushBack(aIp6Address);

    if (error == kErrorNoBufs)
    {
        otLogWarnSrp("[server] too many addresses for host %s", GetFullName());
    }

exit:
    return error;
}

//---------------------------------------------------------------------------------------------------------------------
// Server::UpdateMetadata

Error Server::UpdateMetadata::Init(Instance &aInstance, Host &aHost, const MessageMetadata &aMessageMetadata)
{
    InstanceLocatorInit::Init(aInstance);
    mNext                 = nullptr;
    mExpireTime           = TimerMilli::GetNow() + kDefaultEventsHandlerTimeout;
    mDnsHeader            = aMessageMetadata.mDnsHeader;
    mId                   = Get<Server>().AllocateId();
    mLeaseConfig          = aMessageMetadata.mLeaseConfig;
    mHost                 = &aHost;
    mIsDirectRxFromClient = aMessageMetadata.IsDirectRxFromClient();

    if (aMessageMetadata.mMessageInfo != nullptr)
    {
        mMessageInfo = *aMessageMetadata.mMessageInfo;
    }

    return kErrorNone;
}

} // namespace Srp
} // namespace ot

#endif // OPENTHREAD_CONFIG_SRP_SERVER_ENABLE
