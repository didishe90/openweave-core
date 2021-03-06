/*
 *
 *    Copyright (c) 2016-2017 Nest Labs, Inc.
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file implements subscription engine for Weave
 *      Data Management (WDM) profile.
 *
 */

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif // __STDC_FORMAT_MACROS

#include <Weave/Profiles/WeaveProfiles.h>
#include <Weave/Profiles/common/CommonProfile.h>
#include <Weave/Profiles/data-management/Current/WdmManagedNamespace.h>
#include <Weave/Profiles/data-management/DataManagement.h>
#include <Weave/Profiles/data-management/NotificationEngine.h>
#include <Weave/Profiles/status-report/StatusReportProfile.h>
#include <Weave/Profiles/time/WeaveTime.h>
#include <Weave/Support/crypto/WeaveCrypto.h>
#include <Weave/Support/WeaveFaultInjection.h>
#include <SystemLayer/SystemStats.h>

#ifndef WEAVE_WDM_ALIGNED_TYPE
#define WEAVE_WDM_ALIGNED_TYPE(address, type) reinterpret_cast<type *> WEAVE_SYSTEM_ALIGN_SIZE((size_t)(address), 4)
#endif

namespace nl {
namespace Weave {
namespace Profiles {
namespace WeaveMakeManagedNamespaceIdentifier(DataManagement, kWeaveManagedNamespaceDesignation_Current) {

SubscriptionEngine::SubscriptionEngine() { }

void SubscriptionEngine::SetEventCallback(void * const aAppState, const EventCallback aEventCallback)
{
    mAppState      = aAppState;
    mEventCallback = aEventCallback;
}

void SubscriptionEngine::DefaultEventHandler(EventID aEvent, const InEventParam & aInParam, OutEventParam & aOutParam)
{
    IgnoreUnusedVariable(aInParam);
    IgnoreUnusedVariable(aOutParam);

    WeaveLogDetail(DataManagement, "%s event: %d", __func__, aEvent);
}

WEAVE_ERROR SubscriptionEngine::Init(nl::Weave::WeaveExchangeManager * const apExchangeMgr, void * const aAppState,
                                     const EventCallback aEventCallback)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;

    mExchangeMgr   = apExchangeMgr;
    mAppState      = aAppState;
    mEventCallback = aEventCallback;
    mLock          = NULL;

    err = mExchangeMgr->RegisterUnsolicitedMessageHandler(nl::Weave::Profiles::kWeaveProfile_WDM, UnsolicitedMessageHandler, this);
    SuccessOrExit(err);

#if WDM_ENABLE_SUBSCRIPTION_CLIENT
    for (size_t i = 0; i < kMaxNumCommandObjs; ++i)
    {
        mCommandObjs[i].Init(NULL);
    }

    for (size_t i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        mClients[i].InitAsFree();
    }

#endif // WDM_ENABLE_SUBSCRIPTION_CLIENT

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER
    err = mNotificationEngine.Init();
    SuccessOrExit(err);

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        mHandlers[i].InitAsFree();
    }

    // erase everything
    DisablePublisher();

#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

    mNumTraitInfosInPool = 0;

exit:
    WeaveLogFunctError(err);

    return err;
}

#if WEAVE_DETAIL_LOGGING
void SubscriptionEngine::LogSubscriptionFreed(void) const
{
    // Report number of clients and handlers that are still allocated
    uint32_t countAllocatedClients  = 0;
    uint32_t countAllocatedHandlers = 0;

#if WDM_ENABLE_SUBSCRIPTION_CLIENT
    for (int i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if (SubscriptionClient::kState_Free != mClients[i].mCurrentState)
        {
            ++countAllocatedClients;
        }
    }
#endif // #if WDM_ENABLE_SUBSCRIPTION_CLIENT

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER
    for (int i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        if (SubscriptionHandler::kState_Free != mHandlers[i].mCurrentState)
        {
            ++countAllocatedHandlers;
        }
    }
#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

    WeaveLogDetail(DataManagement, "Allocated clients: %" PRIu32 ". Allocated handlers: %" PRIu32 ".", countAllocatedClients,
                   countAllocatedHandlers);
}
#endif // #if WEAVE_DETAIL_LOGGING

#if WDM_ENABLE_SUBSCRIPTION_CANCEL
void SubscriptionEngine::OnCancelRequest(nl::Weave::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                         const nl::Weave::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId, uint8_t aMsgType,
                                         PacketBuffer * aPayload)
{
    WEAVE_ERROR err                    = WEAVE_NO_ERROR;
    SubscriptionEngine * const pEngine = reinterpret_cast<SubscriptionEngine *>(aEC->AppState);
    uint64_t SubscriptionId            = 0;
    bool found                         = false;

    {
        nl::Weave::TLV::TLVReader reader;
        SubscribeCancelRequest::Parser request;

        reader.Init(aPayload);

        err = reader.Next();
        SuccessOrExit(err);

        err = request.Init(reader);
        SuccessOrExit(err);

#if WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK
        err = request.CheckSchemaValidity();
        SuccessOrExit(err);
#endif // WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK

        err = request.GetSubscriptionID(&SubscriptionId);
        SuccessOrExit(err);
    }

#if WDM_ENABLE_SUBSCRIPTION_CLIENT
    for (size_t i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if ((SubscriptionClient::kState_SubscriptionEstablished_Idle == pEngine->mClients[i].mCurrentState) ||
            (SubscriptionClient::kState_SubscriptionEstablished_Confirming == pEngine->mClients[i].mCurrentState))
        {
            if (pEngine->mClients[i].mSubscriptionId == SubscriptionId)
            {
                pEngine->mClients[i].CancelRequestHandler(aEC, aPktInfo, aMsgInfo, aPayload);
                found = true;
                break;
            }
        }
    }
#endif // #if WDM_ENABLE_SUBSCRIPTION_CLIENT

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER
    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        if ((pEngine->mHandlers[i].mCurrentState >= SubscriptionHandler::kState_SubscriptionInfoValid_Begin) &&
            (pEngine->mHandlers[i].mCurrentState <= SubscriptionHandler::kState_SubscriptionInfoValid_End))
        {
            // Note that there is no need to compare more than subscription ID, because it must already be unique on publisher side
            if (pEngine->mHandlers[i].mSubscriptionId == SubscriptionId)
            {
                pEngine->mHandlers[i].CancelRequestHandler(aEC, aPktInfo, aMsgInfo, aPayload);
                found = true;
                break;
            }
        }
    }
#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

    if (!found)
    {
        err = SendStatusReport(aEC, nl::Weave::Profiles::kWeaveProfile_WDM, kStatus_InvalidSubscriptionID);
        SuccessOrExit(err);
    }

exit:
    WeaveLogFunctError(err);

    // aPayload guaranteed to be non-NULL
    PacketBuffer::Free(aPayload);

    // aEC guaranteed to be non-NULL
    aEC->Close();
}
#endif // WDM_ENABLE_SUBSCRIPTION_CANCEL

#if WDM_ENABLE_SUBSCRIPTION_CLIENT

uint16_t SubscriptionEngine::GetClientId(const SubscriptionClient * const apClient) const
{
    return static_cast<uint16_t>(apClient - mClients);
}

WEAVE_ERROR SubscriptionEngine::NewClient(SubscriptionClient ** const appClient, Binding * const apBinding, void * const apAppState,
                                          SubscriptionClient::EventCallback const aEventCallback,
                                          const TraitCatalogBase<TraitDataSink> * const apCatalog,
                                          const uint32_t aInactivityTimeoutDuringSubscribingMsec, IWeaveWDMMutex * aUpdateMutex)
{
    WEAVE_ERROR err = WEAVE_ERROR_NO_MEMORY;

#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
    uint32_t maxSize = WDM_MAX_UPDATE_SIZE;
#else
    VerifyOrExit(aUpdateMutex == NULL, err = WEAVE_ERROR_INVALID_ARGUMENT);
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE

    WEAVE_FAULT_INJECT(FaultInjection::kFault_WDM_SubscriptionClientNew, ExitNow());

    *appClient = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if (SubscriptionClient::kState_Free == mClients[i].mCurrentState)
        {
            *appClient = &mClients[i];
            err =
                (*appClient)
                    ->Init(apBinding, apAppState, aEventCallback, apCatalog, aInactivityTimeoutDuringSubscribingMsec, aUpdateMutex);

            if (WEAVE_NO_ERROR != err)
            {
                *appClient = NULL;
                ExitNow();
            }
#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
            mClients[i].SetMaxUpdateSize(maxSize);
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE
            SYSTEM_STATS_INCREMENT(nl::Weave::System::Stats::kWDM_NumSubscriptionClients);
            break;
        }
    }

exit:

    return err;
}

WEAVE_ERROR SubscriptionEngine::NewClient(SubscriptionClient ** const appClient, Binding * const apBinding, void * const apAppState,
                                          SubscriptionClient::EventCallback const aEventCallback,
                                          const TraitCatalogBase<TraitDataSink> * const apCatalog,
                                          const uint32_t aInactivityTimeoutDuringSubscribingMsec)
{
    return NewClient(appClient, apBinding, apAppState, aEventCallback, apCatalog, aInactivityTimeoutDuringSubscribingMsec, NULL);
}

#if WEAVE_CONFIG_PERSIST_SUBSCRIPTION_STATE
WEAVE_ERROR SubscriptionEngine::NewClientFromPersistedState(SubscriptionClient ** const appClient, Binding * const apBinding, void * const apAppState,
                                                            SubscriptionClient::EventCallback const aEventCallback,
                                                            const TraitCatalogBase<TraitDataSink> * const apCatalog,
                                                            const uint32_t aInactivityTimeoutDuringSubscribingMsec, TLVReader & reader)
{
    WEAVE_ERROR err = WEAVE_ERROR_NO_MEMORY;

#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
    uint32_t maxSize = WDM_MAX_UPDATE_SIZE;
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE

    WEAVE_FAULT_INJECT(FaultInjection::kFault_WDM_SubscriptionClientNew, ExitNow());

    *appClient = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if (SubscriptionClient::kState_Free == mClients[i].mCurrentState)
        {
            *appClient = &mClients[i];
            err =
                (*appClient)
                    ->LoadFromPersistedState(apBinding, apAppState, aEventCallback, apCatalog, aInactivityTimeoutDuringSubscribingMsec, NULL, reader);

            if (WEAVE_NO_ERROR != err)
            {
                *appClient = NULL;
                ExitNow();
            }
#if WEAVE_CONFIG_ENABLE_WDM_UPDATE
            mClients[i].SetMaxUpdateSize(maxSize);
#endif // WEAVE_CONFIG_ENABLE_WDM_UPDATE
            SYSTEM_STATS_INCREMENT(nl::Weave::System::Stats::kWDM_NumSubscriptionClients);
            break;
        }
    }

exit:
    if (WEAVE_NO_ERROR != err)
    {
        WeaveLogError(DataManagement, "Load persistent subscription client failed with error: %d", err);
    }
    return err;
}

WEAVE_ERROR SubscriptionEngine::NewSubscriptionHandlerFromPersistedState(Binding * const apBinding, void * const apAppState,
                                                                         SubscriptionHandler::EventCallback const aEventCallback, TLVReader & reader)
{
    WEAVE_ERROR err                    = WEAVE_NO_ERROR;
    SubscriptionHandler * handler      = NULL;

    err = NewSubscriptionHandler(&handler);
    SuccessOrExit(err);

    handler->SetMaxNotificationSize(WDM_MAX_NOTIFICATION_SIZE);
    err = handler->LoadFromPersistedState(apBinding, apAppState, aEventCallback, reader);
    SuccessOrExit(err);

exit:
    WeaveLogFunctError(err);

    return err;
}

WEAVE_ERROR SubscriptionEngine::SaveClient(uint64_t aPeerNodeID, TLVWriter &aWriter)
{
    WEAVE_ERROR err                      = WEAVE_NO_ERROR;
    SubscriptionClient * client          = NULL;

    client = FindEstablishedIdleClient(aPeerNodeID);
    VerifyOrExit(client != NULL, err = WEAVE_ERROR_INCORRECT_STATE);

    err = client->SerializeSubscriptionState(aWriter);
    SuccessOrExit(err);

exit:

    return err;
}

WEAVE_ERROR SubscriptionEngine::SaveSubscriptionHandler(uint64_t aPeerNodeID, TLVWriter &aWriter)
{
    WEAVE_ERROR err                      = WEAVE_NO_ERROR;
    SubscriptionHandler * handler        = NULL;

    handler = FindEstablishedIdleHandler(aPeerNodeID);
    VerifyOrExit(handler != NULL, err = WEAVE_ERROR_INCORRECT_STATE);

    err = handler->SerializeSubscriptionState(aWriter);
    SuccessOrExit(err);

exit:

    return err;
}
#endif // WEAVE_CONFIG_PERSIST_SUBSCRIPTION_STATE

/**
 * Reply to a request with a StatuReport message.
 *
 * @param[in]   aEC         Pointer to the ExchangeContext on which the request was received.
 *                          This function does not take ownership of this object. The ExchangeContext
 *                          must be closed or aborted by the calling function according to the
 *                          WEAVE_ERROR returned.
 * @param[in]   aProfileId  The profile to be put in the StatusReport payload.
 * @param[in]   aStatusCode The status code to be put in the StatusReport payload; must refer to the
 *                          profile passed in aProfileId, but this function does not enforce this
 *                          condition.
 *
 * @return      WEAVE_NO_ERROR in case of success.
 *              WEAVE_NO_MEMORY if no pbufs are available.
 *              Any other WEAVE_ERROR code returned by ExchangeContext::SendMessage
 */
WEAVE_ERROR SubscriptionEngine::SendStatusReport(nl::Weave::ExchangeContext * aEC, uint32_t aProfileId, uint16_t aStatusCode)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;

    err = nl::Weave::WeaveServerBase::SendStatusReport(aEC, aProfileId, aStatusCode, WEAVE_NO_ERROR);
    WeaveLogFunctError(err);

    return err;
}

/**
 * Unsolicited message handler for all WDM messages.
 * This function is a @ref ExchangeContext::MessageReceiveFunct.
 */
void SubscriptionEngine::UnsolicitedMessageHandler(nl::Weave::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                                   const nl::Weave::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId,
                                                   uint8_t aMsgType, PacketBuffer * aPayload)
{
    nl::Weave::ExchangeContext::MessageReceiveFunct func = OnUnknownMsgType;

    // If the message was received over UDP and the peer requested an ACK, arrange for
    // any message sent as a response to also request an ACK.
    if (aMsgInfo->InCon == NULL && GetFlag(aMsgInfo->Flags, kWeaveMessageFlag_PeerRequestedAck))
    {
        aEC->SetAutoRequestAck(true);
    }

    switch (aMsgType)
    {
#if WDM_ENABLE_SUBSCRIPTION_CLIENT
    case kMsgType_NotificationRequest:
        func = OnNotificationRequest;

        WEAVE_FAULT_INJECT(FaultInjection::kFault_WDM_TreatNotifyAsCancel, func = OnCancelRequest);
        break;
#endif // WDM_ENABLE_SUBSCRIPTION_CLIENT

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER

    case kMsgType_SubscribeRequest:
        func = OnSubscribeRequest;
        break;

    case kMsgType_SubscribeConfirmRequest:
        func = OnSubscribeConfirmRequest;
        break;

    case kMsgType_CustomCommandRequest:
    case kMsgType_OneWayCommand:
        func = OnCustomCommand;
        break;

#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

#if WDM_ENABLE_SUBSCRIPTION_CANCEL
    case kMsgType_SubscribeCancelRequest:
        func = OnCancelRequest;
        break;
#endif // WDM_ENABLE_SUBSCRIPTION_CANCEL

#if WDM_ENABLE_SUBSCRIPTIONLESS_NOTIFICATION
    case kMsgType_SubscriptionlessNotification:
        func = OnSubscriptionlessNotification;
        break;
#endif // WDM_ENABLE_SUBSCRIPTIONLESS_NOTIFICATION

#if WDM_ENABLE_PUBLISHER_UPDATE_SERVER_SUPPORT
    case kMsgType_UpdateRequest:
        func = OnUpdateRequest;
        break;

    case kMsgType_PartialUpdateRequest:
        WeaveLogDetail(DataManagement, "PartialUpdateRequest not supported yet for update server");
        break;
#endif // WDM_ENABLE_PUBLISHER_UPDATE_SERVER_SUPPORT

    default:
        break;
    }

    func(aEC, aPktInfo, aMsgInfo, aProfileId, aMsgType, aPayload);
}

/**
 * Unsolicited message handler for unsupported WDM messages.
 * This function is a @ref ExchangeContext::MessageReceiveFunct.
 */
void SubscriptionEngine::OnUnknownMsgType(nl::Weave::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                          const nl::Weave::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId, uint8_t aMsgType,
                                          PacketBuffer * aPayload)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;

    PacketBuffer::Free(aPayload);
    aPayload = NULL;

    WeaveLogDetail(DataManagement, "Msg type %" PRIu8 " not supported", aMsgType);

    err = SendStatusReport(aEC, nl::Weave::Profiles::kWeaveProfile_Common, nl::Weave::Profiles::Common::kStatus_UnsupportedMessage);
    SuccessOrExit(err);

    aEC->Close();
    aEC = NULL;

exit:
    WeaveLogFunctError(err);

    if (NULL != aEC)
    {
        aEC->Abort();
        aEC = NULL;
    }
}

void SubscriptionEngine::OnNotificationRequest(nl::Weave::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                               const nl::Weave::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId, uint8_t aMsgType,
                                               PacketBuffer * aPayload)
{
    WEAVE_ERROR err                    = WEAVE_NO_ERROR;
    SubscriptionEngine * const pEngine = reinterpret_cast<SubscriptionEngine *>(aEC->AppState);
    uint64_t SubscriptionId            = 0;

    {
        nl::Weave::TLV::TLVReader reader;
        NotificationRequest::Parser notify;

        reader.Init(aPayload);

        err = reader.Next();
        SuccessOrExit(err);

        err = notify.Init(reader);
        SuccessOrExit(err);

        // Note that it is okay to bail out, without any response, if the message doesn't even have a subscription ID in it
        err = notify.GetSubscriptionID(&SubscriptionId);
        SuccessOrExit(err);

        WEAVE_FAULT_INJECT(FaultInjection::kFault_WDM_BadSubscriptionId, SubscriptionId += 1);
    }

    for (size_t i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if ((SubscriptionClient::kState_SubscriptionEstablished_Idle == pEngine->mClients[i].mCurrentState) ||
            (SubscriptionClient::kState_SubscriptionEstablished_Confirming == pEngine->mClients[i].mCurrentState))
        {
            if (pEngine->mClients[i].mBinding->IsAuthenticMessageFromPeer(aMsgInfo) &&
                pEngine->mClients[i].mSubscriptionId == SubscriptionId)
            {
                pEngine->mClients[i].NotificationRequestHandler(aEC, aPktInfo, aMsgInfo, aPayload);
                aPayload = NULL;
                aEC      = NULL;
                ExitNow();
            }
        }
    }

    WeaveLogDetail(DataManagement, "%s: couldn't find matching client. Subscription ID: 0x%" PRIX64, __func__, SubscriptionId);

    err = SendStatusReport(aEC, nl::Weave::Profiles::kWeaveProfile_WDM, kStatus_InvalidSubscriptionID);
    SuccessOrExit(err);

exit:
    WeaveLogFunctError(err);

    if (NULL != aPayload)
    {
        PacketBuffer::Free(aPayload);
        aPayload = NULL;
    }

    if (NULL != aEC)
    {
        aEC->Abort();
        aEC = NULL;
    }
}

WEAVE_ERROR SubscriptionEngine::ProcessDataList(nl::Weave::TLV::TLVReader & aReader,
                                                const TraitCatalogBase<TraitDataSink> * aCatalog, bool & aOutIsPartialChange,
                                                TraitDataHandle & aOutTraitDataHandle,
                                                IDataElementAccessControlDelegate & acDelegate)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;

    // TODO: We currently don't support changes that span multiple notifies, nor changes
    // that get aborted and restarted within the same notify. See WEAV-1586 for more details.
    bool isPartialChange = false;
    uint8_t flags;

    VerifyOrExit(aCatalog != NULL, err = WEAVE_ERROR_INVALID_ARGUMENT);

    while (WEAVE_NO_ERROR == (err = aReader.Next()))
    {
        nl::Weave::TLV::TLVReader pathReader;

        {
            DataElement::Parser element;

            err = element.Init(aReader);
            SuccessOrExit(err);

            err = element.GetReaderOnPath(&pathReader);
            SuccessOrExit(err);

            isPartialChange = false;
            err             = element.GetPartialChangeFlag(&isPartialChange);
            VerifyOrExit(err == WEAVE_NO_ERROR || err == WEAVE_END_OF_TLV, );
        }

        TraitPath traitPath;
        TraitDataSink * dataSink;
        TraitDataHandle handle;
        PropertyPathHandle pathHandle;
        SchemaVersionRange versionRange;

        err = aCatalog->AddressToHandle(pathReader, handle, versionRange);

        if (err == WEAVE_ERROR_INVALID_PROFILE_ID)
        {
            // AddressToHandle() can return an error if the sink has been removed from the catalog. In that case,
            // continue to next entry
            err = WEAVE_NO_ERROR;
            continue;
        }

        SuccessOrExit(err);

        if (aCatalog->Locate(handle, &dataSink) != WEAVE_NO_ERROR)
        {
            // Ideally, this code will not be reached as Locate() should find the entry in the catalog.
            // Otherwise, the earlier AddressToHandle() call would have continued.
            // However, keeping this check here for consistency and code safety
            continue;
        }

        err = dataSink->GetSchemaEngine()->MapPathToHandle(pathReader, pathHandle);
#if TDM_DISABLE_STRICT_SCHEMA_COMPLIANCE
        // if we're not in strict compliance mode, we can ignore data elements that refer to paths we can't map due to mismatching
        // schema. The eventual call to StoreDataElement will correctly deal with the presence of a null property path handle that
        // has been returned by the above call. It's necessary to call into StoreDataElement with this null handle to ensure
        // the requisite OnEvent calls are made to the application despite the presence of an unknown tag. It's also necessary to
        // ensure that we update the internal version tracked by the sink.
        if (err == WEAVE_ERROR_TLV_TAG_NOT_FOUND)
        {
            WeaveLogDetail(DataManagement, "Ignoring un-mappable path!");
            err = WEAVE_NO_ERROR;
        }
#endif
        SuccessOrExit(err);

        traitPath.mTraitDataHandle    = handle;
        traitPath.mPropertyPathHandle = pathHandle;

        err = acDelegate.DataElementAccessCheck(traitPath, *aCatalog);

        if (err == WEAVE_ERROR_ACCESS_DENIED)
        {
            WeaveLogDetail(DataManagement, "Ignoring path. Subscriptionless notification not accepted by data sink.");

            continue;
        }
        SuccessOrExit(err);

        pathReader = aReader;
        flags      = 0;

#if WDM_ENABLE_PROTOCOL_CHECKS
        // If we previously had a partial change, the current handle should match the previous one.
        // If they don't, we have a partial change violation.
        if (aOutIsPartialChange && (aOutTraitDataHandle != handle))
        {
            WeaveLogError(DataManagement, "Encountered partial change flag violation (%u, %x, %x)", aOutIsPartialChange,
                          aOutTraitDataHandle, handle);
            err = WEAVE_ERROR_INVALID_DATA_LIST;
            goto exit;
        }
#endif

        if (!aOutIsPartialChange)
        {
            flags = TraitDataSink::kFirstElementInChange;
        }

        if (!isPartialChange)
        {
            flags |= TraitDataSink::kLastElementInChange;
        }

        err = dataSink->StoreDataElement(pathHandle, pathReader, flags, NULL, NULL, handle);
        SuccessOrExit(err);

        aOutIsPartialChange = isPartialChange;

#if WDM_ENABLE_PROTOCOL_CHECKS
        aOutTraitDataHandle = handle;
#endif
    }

    // if we have exhausted this container
    if (WEAVE_END_OF_TLV == err)
    {
        err = WEAVE_NO_ERROR;
    }

exit:
    return err;
}

#if WDM_ENABLE_SUBSCRIPTIONLESS_NOTIFICATION
WEAVE_ERROR SubscriptionEngine::RegisterForSubscriptionlessNotifications(const TraitCatalogBase<TraitDataSink> * const apCatalog)
{
    WEAVE_ERROR err                    = WEAVE_NO_ERROR;
    mSubscriptionlessNotifySinkCatalog = apCatalog;

    return err;
}

void SubscriptionEngine::OnSubscriptionlessNotification(nl::Weave::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                                        const nl::Weave::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId,
                                                        uint8_t aMsgType, PacketBuffer * aPayload)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;
    NotificationRequest::Parser notify;
    nl::Weave::TLV::TLVReader reader;
    bool isDataListPresent = false;
    InEventParam inParam;
    OutEventParam outParam;

    SubscriptionEngine * const pEngine = reinterpret_cast<SubscriptionEngine *>(aEC->AppState);

    // Send an event to the application indicating the receipt of a
    // subscriptionless notification.

    inParam.Clear();
    outParam.Clear();

    inParam.mIncomingSubscriptionlessNotification.processingError            = err;
    inParam.mIncomingSubscriptionlessNotification.mMsgInfo                   = aMsgInfo;
    outParam.mIncomingSubscriptionlessNotification.mShouldContinueProcessing = true;

    if (pEngine->mEventCallback)
    {
        pEngine->mEventCallback(pEngine->mAppState, kEvent_OnIncomingSubscriptionlessNotification, inParam, outParam);
    }

    if (!outParam.mIncomingSubscriptionlessNotification.mShouldContinueProcessing)
    {
        WeaveLogDetail(DataManagement, "Subscriptionless Notification not allowed");
        ExitNow();
    }

    reader.Init(aPayload);

    err = reader.Next();
    SuccessOrExit(err);

    err = notify.Init(reader);
    SuccessOrExit(err);

#if WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK
    // simple schema checking
    err = notify.CheckSchemaValidity();
    SuccessOrExit(err);
#endif // WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK

    {
        DataList::Parser dataList;

        err = notify.GetDataList(&dataList);
        if (WEAVE_NO_ERROR == err)
        {
            isDataListPresent = true;
        }
        else if (WEAVE_END_OF_TLV == err)
        {
            isDataListPresent = false;
            err               = WEAVE_NO_ERROR;
        }
        SuccessOrExit(err);

        // re-initialize the reader to point to individual date element (reuse to save stack depth).
        dataList.GetReader(&reader);
    }

    if (isDataListPresent)
    {
        bool isPartialChange = false;
        TraitDataHandle traitDataHandle;
        SubscriptionlessNotifyDataElementAccessControlDelegate acDelegate(aMsgInfo);
        IDataElementAccessControlDelegate & acDelegateRef = acDelegate;

        err = ProcessDataList(reader, pEngine->mSubscriptionlessNotifySinkCatalog, isPartialChange, traitDataHandle, acDelegateRef);
        SuccessOrExit(err);

        if (isPartialChange)
        {
            // Subscriptionless notification should not contain partial trait
            // data info.
            ExitNow(err = WEAVE_ERROR_WDM_SUBSCRIPTIONLESS_NOTIFY_PARTIAL);
        }
    }

exit:

    if (NULL != aPayload)
    {
        PacketBuffer::Free(aPayload);
        aPayload = NULL;
    }

    if (NULL != aEC)
    {
        aEC->Abort();
        aEC = NULL;
    }

    if (pEngine->mEventCallback)
    {
        inParam.Clear();
        outParam.Clear();

        inParam.mIncomingSubscriptionlessNotification.processingError = err;
        inParam.mIncomingSubscriptionlessNotification.mMsgInfo        = aMsgInfo;
        // Subscriptionless Notification completion event indication.
        pEngine->mEventCallback(pEngine->mAppState, kEvent_SubscriptionlessNotificationProcessingComplete, inParam, outParam);
    }
}

WEAVE_ERROR SubscriptionEngine::SubscriptionlessNotifyDataElementAccessControlDelegate::DataElementAccessCheck(
    const TraitPath & aTraitPath, const TraitCatalogBase<TraitDataSink> & aCatalog)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;
    TraitDataSink * dataSink;
    InEventParam inParam;
    OutEventParam outParam;
    SubscriptionEngine * pEngine = SubscriptionEngine::GetInstance();

    err = aCatalog.Locate(aTraitPath.mTraitDataHandle, &dataSink);
    SuccessOrExit(err);

    inParam.Clear();
    outParam.Clear();

    if (dataSink->AcceptsSubscriptionlessNotifications())
    {
        outParam.mDataElementAccessControlForNotification.mRejectNotification = false;
        outParam.mDataElementAccessControlForNotification.mReason             = WEAVE_NO_ERROR;
    }
    else
    {
        outParam.mDataElementAccessControlForNotification.mRejectNotification = true;
        outParam.mDataElementAccessControlForNotification.mReason             = WEAVE_ERROR_ACCESS_DENIED;
    }

    inParam.mDataElementAccessControlForNotification.mPath    = &aTraitPath;
    inParam.mDataElementAccessControlForNotification.mCatalog = &aCatalog;
    inParam.mDataElementAccessControlForNotification.mMsgInfo = mMsgInfo;

    if (NULL != pEngine->mEventCallback)
    {
        pEngine->mEventCallback(pEngine->mAppState, kEvent_DataElementAccessControlCheck, inParam, outParam);
    }

    // If application rejects it then deny access, else set reason to whatever
    // reason is set by application.
    if (outParam.mDataElementAccessControlForNotification.mRejectNotification == true)
    {
        err = WEAVE_ERROR_ACCESS_DENIED;
    }
    else
    {
        err = outParam.mDataElementAccessControlForNotification.mReason;
    }

exit:

    return err;
}
#endif // WDM_ENABLE_SUBSCRIPTIONLESS_NOTIFICATION

SubscriptionClient * SubscriptionEngine::FindClient(const uint64_t aPeerNodeId, const uint64_t aSubscriptionId)
{
    SubscriptionClient * result = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if ((mClients[i].mCurrentState >= SubscriptionClient::kState_Subscribing_IdAssigned) &&
            (mClients[i].mCurrentState <= SubscriptionClient::kState_SubscriptionEstablished_Confirming))
        {
            if ((aPeerNodeId == mClients[i].mBinding->GetPeerNodeId()) && (mClients[i].mSubscriptionId == aSubscriptionId))
            {
                result = &mClients[i];
                break;
            }
        }
    }

    return result;
}

#if WEAVE_CONFIG_PERSIST_SUBSCRIPTION_STATE
SubscriptionClient * SubscriptionEngine::FindEstablishedIdleClient(const uint64_t aPeerNodeId)
{
    SubscriptionClient * result = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionClients; ++i)
    {
        if (mClients[i].mCurrentState == SubscriptionClient::kState_SubscriptionEstablished_Idle)
        {
            if (aPeerNodeId == mClients[i].mBinding->GetPeerNodeId())
            {
                result = &mClients[i];
                break;
            }
        }
    }

    return result;
}
#endif // WEAVE_CONFIG_PERSIST_SUBSCRIPTION_STATE

bool SubscriptionEngine::UpdateClientLiveness(const uint64_t aPeerNodeId, const uint64_t aSubscriptionId, const bool aKill)
{
    WEAVE_ERROR err              = WEAVE_NO_ERROR;
    bool found                   = false;
    SubscriptionClient * pClient = FindClient(aPeerNodeId, aSubscriptionId);

    if (NULL != pClient)
    {
        found = true;

        if (aKill)
        {
            err = WEAVE_ERROR_TRANSACTION_CANCELED;
        }
        else
        {
            WeaveLogDetail(DataManagement, "Client[%d] [%5.5s] liveness confirmed", GetClientId(pClient), pClient->GetStateStr());

            // emit a subscription activity event
            pClient->IndicateActivity();

            // ignore incorrect state error, otherwise, let it flow through
            err = pClient->RefreshTimer();
            if (WEAVE_ERROR_INCORRECT_STATE == err)
            {
                err = WEAVE_NO_ERROR;

                WeaveLogDetail(DataManagement, "Client[%d] [%5.5s] liveness confirmation failed, ignore", GetClientId(pClient),
                               pClient->GetStateStr());
            }
        }

        if (WEAVE_NO_ERROR != err)
        {
            WeaveLogDetail(DataManagement, "Client[%d] [%5.5s] bound mutual subscription is going away", GetClientId(pClient),
                           pClient->GetStateStr());

            pClient->TerminateSubscription(err, NULL, false);
        }
    }

    return found;
}

#endif // WDM_ENABLE_SUBSCRIPTION_CLIENT

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER

uint16_t SubscriptionEngine::GetHandlerId(const SubscriptionHandler * const apHandler) const
{
    return static_cast<uint16_t>(apHandler - mHandlers);
}

uint16_t SubscriptionEngine::GetCommandObjId(const Command * const apHandle) const
{
    return static_cast<uint16_t>(apHandle - mCommandObjs);
}

bool SubscriptionEngine::UpdateHandlerLiveness(const uint64_t aPeerNodeId, const uint64_t aSubscriptionId, const bool aKill)
{
    WEAVE_ERROR err                = WEAVE_NO_ERROR;
    bool found                     = false;
    SubscriptionHandler * pHandler = FindHandler(aPeerNodeId, aSubscriptionId);
    if (NULL != pHandler)
    {
        found = true;

        if (aKill)
        {
            err = WEAVE_ERROR_TRANSACTION_CANCELED;
        }
        else
        {
            WeaveLogDetail(DataManagement, "Handler[%d] [%5.5s] liveness confirmed", GetHandlerId(pHandler),
                           pHandler->GetStateStr());

            // ignore incorrect state error, otherwise, let it flow through
            err = pHandler->RefreshTimer();
            if (WEAVE_ERROR_INCORRECT_STATE == err)
            {
                err = WEAVE_NO_ERROR;

                WeaveLogDetail(DataManagement, "Handler[%d] [%5.5s] liveness confirmation failed, ignore", GetHandlerId(pHandler),
                               pHandler->GetStateStr());
            }
        }

        if (WEAVE_NO_ERROR != err)
        {
            WeaveLogDetail(DataManagement, "Handler[%d] [%5.5s] bound mutual subscription is going away", GetHandlerId(pHandler),
                           pHandler->GetStateStr());

            pHandler->TerminateSubscription(err, NULL, false);
        }
    }

    return found;
}

SubscriptionHandler * SubscriptionEngine::FindHandler(const uint64_t aPeerNodeId, const uint64_t aSubscriptionId)
{
    SubscriptionHandler * result = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        if ((mHandlers[i].mCurrentState >= SubscriptionHandler::kState_SubscriptionInfoValid_Begin) &&
            (mHandlers[i].mCurrentState <= SubscriptionHandler::kState_SubscriptionInfoValid_End))
        {
            if ((aPeerNodeId == mHandlers[i].mBinding->GetPeerNodeId()) && (aSubscriptionId == mHandlers[i].mSubscriptionId))
            {
                result = &mHandlers[i];
                break;
            }
        }
    }

    return result;
}

#if WEAVE_CONFIG_PERSIST_SUBSCRIPTION_STATE
SubscriptionHandler * SubscriptionEngine::FindEstablishedIdleHandler(const uint64_t aPeerNodeId)
{
    SubscriptionHandler * result = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        if (mHandlers[i].mCurrentState == SubscriptionHandler::kState_SubscriptionEstablished_Idle)
        {
            if (aPeerNodeId == mHandlers[i].GetPeerNodeId())
            {
                result = &mHandlers[i];
                break;
            }
        }
    }

    return result;
}
#endif // WEAVE_CONFIG_PERSIST_SUBSCRIPTION_STATE

WEAVE_ERROR SubscriptionEngine::GetMinEventLogPosition(size_t & outLogPosition) const
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;

    for (size_t subIdx = 0; subIdx < kMaxNumSubscriptionHandlers; ++subIdx)
    {
        const SubscriptionHandler * subHandler = &(mHandlers[subIdx]);
        if (subHandler->mCurrentState == SubscriptionHandler::kState_Free)
        {
            continue;
        }

        if (subHandler->mBytesOffloaded < outLogPosition)
        {
            outLogPosition = subHandler->mBytesOffloaded;
        }
    }

    return err;
}

void SubscriptionEngine::ReclaimTraitInfo(SubscriptionHandler * const aHandlerToBeReclaimed)
{
    SubscriptionHandler::TraitInstanceInfo * const traitInfoList = aHandlerToBeReclaimed->mTraitInstanceList;
    const uint16_t numTraitInstances                             = aHandlerToBeReclaimed->mNumTraitInstances;
    size_t numTraitInstancesToBeAffected;

    aHandlerToBeReclaimed->mTraitInstanceList = NULL;
    aHandlerToBeReclaimed->mNumTraitInstances = 0;

    if (!numTraitInstances)
    {
        WeaveLogDetail(DataManagement, "No trait instances allocated for this subscription");
        ExitNow();
    }

    // make sure everything is still sane
    WeaveLogIfFalse(traitInfoList >= mTraitInfoPool);
    WeaveLogIfFalse(numTraitInstances <= mNumTraitInfosInPool);

    // mPathGroupPool + kMaxNumPathGroups is a pointer which points to the last+1byte of this array
    // traitInfoList is a pointer to the first trait instance to be released
    // the result of subtraction is the number of trait instances from traitInfoList to the end of this array
    numTraitInstancesToBeAffected = (mTraitInfoPool + mNumTraitInfosInPool) - traitInfoList;

    // Shrink the traitInfosInPool by the number of trait instances in this subscription.
    mNumTraitInfosInPool -= numTraitInstances;
    SYSTEM_STATS_DECREMENT_BY_N(nl::Weave::System::Stats::kWDM_NumTraits, numTraitInstances);

    if (numTraitInstances == numTraitInstancesToBeAffected)
    {
        WeaveLogDetail(DataManagement, "Releasing the last block of trait instances");
        ExitNow();
    }

    WeaveLogDetail(DataManagement, "Moving %u trait instances forward",
                   static_cast<unsigned int>(numTraitInstancesToBeAffected - numTraitInstances));

    memmove(traitInfoList, traitInfoList + numTraitInstances,
            sizeof(SubscriptionHandler::TraitInstanceInfo) * (numTraitInstancesToBeAffected - numTraitInstances));

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        SubscriptionHandler * const pHandler = mHandlers + i;

        if ((aHandlerToBeReclaimed != pHandler) && (pHandler->mTraitInstanceList > traitInfoList))
        {
            pHandler->mTraitInstanceList -= numTraitInstances;
        }
    }

exit:
    WeaveLogDetail(DataManagement, "Number of allocated trait instances: %u", mNumTraitInfosInPool);
}

WEAVE_ERROR SubscriptionEngine::EnablePublisher(IWeavePublisherLock * aLock,
                                                TraitCatalogBase<TraitDataSource> * const aPublisherCatalog)
{
    // force abandon all subscription first, so we can have a clean slate
    DisablePublisher();

    mLock = aLock;

    // replace catalog
    mPublisherCatalog = aPublisherCatalog;

    mIsPublisherEnabled = true;

    mNextHandlerToNotify = 0;

    return WEAVE_NO_ERROR;
}

WEAVE_ERROR SubscriptionEngine::Lock()
{
    if (mLock)
    {
        return mLock->Lock();
    }

    return WEAVE_NO_ERROR;
}

WEAVE_ERROR SubscriptionEngine::Unlock()
{
    if (mLock)
    {
        return mLock->Unlock();
    }

    return WEAVE_NO_ERROR;
}

void SubscriptionEngine::DisablePublisher()
{
    mIsPublisherEnabled = false;
    mPublisherCatalog   = NULL;

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        switch (mHandlers[i].mCurrentState)
        {
        case SubscriptionHandler::kState_Free:
        case SubscriptionHandler::kState_Terminated:
            break;
        default:
            mHandlers[i].AbortSubscription();
        }
    }

    // Note that the command objects are not closed when publisher is disabled.
    // This is because the processing flow of commands are not directly linked
    // with subscriptions.
}

WEAVE_ERROR SubscriptionEngine::NewSubscriptionHandler(SubscriptionHandler ** subHandler)
{
    WEAVE_ERROR err = WEAVE_ERROR_NO_MEMORY;

    *subHandler = NULL;

    WEAVE_FAULT_INJECT(FaultInjection::kFault_WDM_SubscriptionHandlerNew, ExitNow());

    for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
    {
        if (SubscriptionHandler::kState_Free == mHandlers[i].mCurrentState)
        {
            WeaveLogIfFalse(0 == mHandlers[i].mRefCount);
            *subHandler = &mHandlers[i];
            err         = WEAVE_NO_ERROR;

            SYSTEM_STATS_INCREMENT(nl::Weave::System::Stats::kWDM_NumSubscriptionHandlers);

            break;
        }
    }

    ExitNow(); // silence warnings about unused labels.
exit:
    return err;
}

void SubscriptionEngine::OnSubscribeRequest(nl::Weave::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                            const nl::Weave::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId, uint8_t aMsgType,
                                            PacketBuffer * aPayload)
{
    WEAVE_ERROR err                    = WEAVE_NO_ERROR;
    SubscriptionEngine * const pEngine = reinterpret_cast<SubscriptionEngine *>(aEC->AppState);
    SubscriptionHandler * handler      = NULL;
    uint32_t reasonProfileId           = nl::Weave::Profiles::kWeaveProfile_Common;
    uint16_t reasonStatusCode          = nl::Weave::Profiles::Common::kStatus_InternalServerProblem;
    InEventParam inParam;
    OutEventParam outParam;
    Binding * binding;
    uint64_t subscriptionId = 0;

    // Note that there is no event callback nor App state assigned to this newly allocated binding
    // We will need to assign a callback handler when binding actually generates useful events
    binding = pEngine->mExchangeMgr->NewBinding();
    if (NULL == binding)
    {
        // log as error as it might be difficult to estimate how many bindings are needed on a system
        WeaveLogError(DataManagement, "%s: Out of Binding", __func__);
        ExitNow(err = WEAVE_ERROR_NO_MEMORY);
    }

    // Configure the binding to communicate back to the sender of the Subscribe request. Later,
    // after the subscription is established, this binding will be used to initiate unsolicited
    // exchanges with the client, e.g. to deliver notifications.
    err = binding->BeginConfiguration().ConfigureFromMessage(aMsgInfo, aPktInfo).PrepareBinding();
    SuccessOrExit(err);

    if (pEngine->mIsPublisherEnabled && (NULL != pEngine->mEventCallback))
    {
        outParam.mIncomingSubscribeRequest.mAutoClosePriorSubscription = true;
        outParam.mIncomingSubscribeRequest.mRejectRequest              = false;
        outParam.mIncomingSubscribeRequest.mpReasonProfileId           = &reasonProfileId;
        outParam.mIncomingSubscribeRequest.mpReasonStatusCode          = &reasonStatusCode;

        inParam.mIncomingSubscribeRequest.mEC      = aEC;
        inParam.mIncomingSubscribeRequest.mPktInfo = aPktInfo;
        inParam.mIncomingSubscribeRequest.mMsgInfo = aMsgInfo;
        inParam.mIncomingSubscribeRequest.mPayload = aPayload;
        inParam.mIncomingSubscribeRequest.mBinding = binding;

        // note the binding is exposed to app layer for configuration here, and again later after
        // the request is fully parsed
        pEngine->mEventCallback(pEngine->mAppState, kEvent_OnIncomingSubscribeRequest, inParam, outParam);

        // Make sure messages sent through this EC are sent with proper re-transmission/timeouts settings
        // This is mainly for rejections, as the EC would be configured again in SubscriptionHandler::AcceptSubscribeRequest

        err = binding->AdjustResponseTimeout(aEC);
        SuccessOrExit(err);
    }
    else
    {
        ExitNow(err = WEAVE_ERROR_NO_MESSAGE_HANDLER);
    }

    if (outParam.mIncomingSubscribeRequest.mRejectRequest)
    {
        // reject this request (without touching existing subscriptions)
        ExitNow(err = WEAVE_ERROR_TRANSACTION_CANCELED);
    }
    else
    {
        if (outParam.mIncomingSubscribeRequest.mAutoClosePriorSubscription)
        {
            // if not rejected, default behavior is to abort any prior communication with this node id
            for (size_t i = 0; i < kMaxNumSubscriptionHandlers; ++i)
            {
                if ((pEngine->mHandlers[i].mCurrentState >= SubscriptionHandler::kState_SubscriptionInfoValid_Begin) &&
                    (pEngine->mHandlers[i].mCurrentState <= SubscriptionHandler::kState_SubscriptionInfoValid_End))
                {
                    uint64_t nodeId = pEngine->mHandlers[i].GetPeerNodeId();

                    if (nodeId == aEC->PeerNodeId)
                    {
                        pEngine->mHandlers[i].TerminateSubscription(err, NULL, false);
                    }
                }
            }
        }

        err = nl::Weave::Platform::Security::GetSecureRandomData((uint8_t *) &subscriptionId, sizeof(subscriptionId));
        SuccessOrExit(err);

        err = pEngine->NewSubscriptionHandler(&handler);
        if (err != WEAVE_NO_ERROR)
        {
            // try to give slightly more detail on the issue for this potentially common problem
            reasonStatusCode = (err == WEAVE_ERROR_NO_MEMORY ? nl::Weave::Profiles::Common::kStatus_OutOfMemory
                                                             : nl::Weave::Profiles::Common::kStatus_InternalServerProblem);

            ExitNow();
        }
        else
        {
            handler->mAppState      = outParam.mIncomingSubscribeRequest.mHandlerAppState;
            handler->mEventCallback = outParam.mIncomingSubscribeRequest.mHandlerEventCallback;
            uint32_t maxSize        = WDM_MAX_NOTIFICATION_SIZE;

            WEAVE_FAULT_INJECT_WITH_ARGS(
                FaultInjection::kFault_WDM_NotificationSize,
                // Code executed with the Manager's lock:
                if (numFaultArgs > 0) { maxSize = static_cast<uint32_t>(faultArgs[0]); } else {
                    maxSize = WDM_MAX_NOTIFICATION_SIZE / 2;
                },
                // Code executed withouth the Manager's lock:
                WeaveLogDetail(DataManagement, "Handler[%d] Payload size set to %d", pEngine->GetHandlerId(handler), maxSize));

            handler->SetMaxNotificationSize(maxSize);

            handler->InitWithIncomingRequest(binding, subscriptionId, aEC, aPktInfo, aMsgInfo, aPayload);
            aEC      = NULL;
            aPayload = NULL;
        }
    }

exit:
    WeaveLogFunctError(err);

    if (NULL != aPayload)
    {
        PacketBuffer::Free(aPayload);
        aPayload = NULL;
    }

    if (NULL != aEC)
    {
        err = SendStatusReport(aEC, reasonProfileId, reasonStatusCode);
        WeaveLogFunctError(err);

        aEC->Close();
        aEC = NULL;
    }

    if (NULL != binding)
    {
        binding->Release();
    }
}

void SubscriptionEngine::OnSubscribeConfirmRequest(nl::Weave::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                                   const nl::Weave::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId,
                                                   uint8_t aMsgType, PacketBuffer * aPayload)
{
    WEAVE_ERROR err                    = WEAVE_NO_ERROR;
    SubscriptionEngine * const pEngine = reinterpret_cast<SubscriptionEngine *>(aEC->AppState);
    uint32_t reasonProfileId           = nl::Weave::Profiles::kWeaveProfile_Common;
    uint16_t reasonStatusCode          = nl::Weave::Profiles::Common::kStatus_InternalServerProblem;
    uint64_t subscriptionId;

    {
        nl::Weave::TLV::TLVReader reader;
        SubscribeConfirmRequest::Parser request;

        reader.Init(aPayload);

        err = reader.Next();
        SuccessOrExit(err);

        err = request.Init(reader);
        SuccessOrExit(err);

        err = request.GetSubscriptionID(&subscriptionId);
        SuccessOrExit(err);
    }

    // Discard the buffer so that it may be reused by the code below.
    PacketBuffer::Free(aPayload);
    aPayload = NULL;

    if (pEngine->mIsPublisherEnabled)
    {
        // find a matching subscription
        bool found = false;

#if WDM_ENABLE_SUBSCRIPTION_CLIENT
        if (pEngine->UpdateClientLiveness(aEC->PeerNodeId, subscriptionId))
        {
            found = true;
        }
#endif // WDM_ENABLE_SUBSCRIPTION_CLIENT

#if WDM_ENABLE_SUBSCRIPTION_PUBLISHER
        if (pEngine->UpdateHandlerLiveness(aEC->PeerNodeId, subscriptionId))
        {
            found = true;
        }
#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

        if (found)
        {
            reasonStatusCode = nl::Weave::Profiles::Common::kStatus_Success;
        }
        else
        {
            reasonProfileId  = nl::Weave::Profiles::kWeaveProfile_WDM;
            reasonStatusCode = kStatus_InvalidSubscriptionID;
        }
    }
    else
    {
        reasonStatusCode = nl::Weave::Profiles::Common::kStatus_Busy;
    }

    {
        err = SendStatusReport(aEC, reasonProfileId, reasonStatusCode);
        SuccessOrExit(err);
    }

exit:
    WeaveLogFunctError(err);

    if (aPayload != NULL)
    {
        PacketBuffer::Free(aPayload);
    }

    // aEC is guaranteed to be non-NULL.
    aEC->Close();
}

#if WDM_PUBLISHER_ENABLE_CUSTOM_COMMAND_HANDLER
void SubscriptionEngine::OnCustomCommand(nl::Weave::ExchangeContext * aEC, const nl::Inet::IPPacketInfo * aPktInfo,
                                         const nl::Weave::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId, uint8_t aMsgType,
                                         PacketBuffer * aPayload)
{
    WEAVE_ERROR err                    = WEAVE_NO_ERROR;
    SubscriptionEngine * const pEngine = reinterpret_cast<SubscriptionEngine *>(aEC->AppState);
    Command * command                  = NULL;
    uint32_t statusReportProfile       = nl::Weave::Profiles::kWeaveProfile_WDM;
    uint16_t statusReportCode          = nl::Weave::Profiles::DataManagement::kStatus_InvalidPath;

    for (size_t i = 0; i < kMaxNumCommandObjs; ++i)
    {
        if (pEngine->mCommandObjs[i].IsFree())
        {
            SYSTEM_STATS_INCREMENT(nl::Weave::System::Stats::kWDM_NumCommands);
            command = &(pEngine->mCommandObjs[i]);
            command->Init(aEC);
            aEC = NULL;
            break;
        }
    }
    VerifyOrExit(NULL != command, err = WEAVE_ERROR_NO_MEMORY);

    if (!pEngine->mIsPublisherEnabled)
    {
        // Has to be a publisher to be processing a command
        statusReportProfile = nl::Weave::Profiles::kWeaveProfile_Common;
        statusReportCode    = nl::Weave::Profiles::Common::kStatus_UnsupportedMessage;
        ExitNow(err = WEAVE_ERROR_INVALID_MESSAGE_TYPE);
    }

    // Set the flag indicating whether this is a OneWay Command or not.

    if (aMsgType == kMsgType_OneWayCommand)
    {
        command->SetIsOneWay(true);
    }

    // Parse Trait Data

    {
        nl::Weave::TLV::TLVReader reader;
        TraitDataSource * dataSource = NULL;

        reader.Init(aPayload);

        err = reader.Next();
        SuccessOrExit(err);

        {
            CustomCommand::Parser cmdParser;
            TraitDataHandle traitDataHandle;
            nl::Weave::TLV::TLVReader pathReader;
            SchemaVersionRange requestedSchemaVersion, computedVersionIntersection;

            err = cmdParser.Init(reader);
            SuccessOrExit(err);

#if WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK
            err = cmdParser.CheckSchemaValidity();
            SuccessOrExit(err);
#endif // WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK

            err = cmdParser.GetReaderOnPath(&pathReader);
            SuccessOrExit(err);

            err = pEngine->mPublisherCatalog->AddressToHandle(pathReader, traitDataHandle, requestedSchemaVersion);
            SuccessOrExit(err);

            err = SubscriptionEngine::GetInstance()->mPublisherCatalog->Locate(traitDataHandle, &dataSource);
            SuccessOrExit(err);

            if (!dataSource->GetSchemaEngine()->GetVersionIntersection(requestedSchemaVersion, computedVersionIntersection))
            {
                WeaveLogDetail(DataManagement, "Mismatch in requested version on handle %u (requested: %u, %u)", traitDataHandle,
                               requestedSchemaVersion.mMaxVersion, requestedSchemaVersion.mMinVersion);

                statusReportProfile = nl::Weave::Profiles::kWeaveProfile_WDM;
                statusReportCode    = kStatus_IncompatibleDataSchemaVersion;
                ExitNow(err = WEAVE_ERROR_INCOMPATIBLE_SCHEMA_VERSION);
            }

            err = cmdParser.GetCommandType(&command->commandType);
            SuccessOrExit(err);

            err = cmdParser.GetInitiationTimeMicroSecond(&command->initiationTimeMicroSecond);
            if (WEAVE_NO_ERROR == err)
            {
                command->SetInitiationTimeValid(true);
            }
            else if (WEAVE_END_OF_TLV == err)
            {
                err = WEAVE_NO_ERROR;
            }
            else
            {
                ExitNow();
            }

            err = cmdParser.GetActionTimeMicroSecond(&command->actionTimeMicroSecond);
            if (WEAVE_NO_ERROR == err)
            {
                command->SetActionTimeValid(true);
            }
            else if (WEAVE_END_OF_TLV == err)
            {
                err = WEAVE_NO_ERROR;
            }
            else
            {
                ExitNow();
            }

            err = cmdParser.GetExpiryTimeMicroSecond(&command->expiryTimeMicroSecond);
            if (WEAVE_NO_ERROR == err)
            {
                command->SetExpiryTimeValid(true);
            }
            else if (WEAVE_END_OF_TLV == err)
            {
                err = WEAVE_NO_ERROR;
            }
            else
            {
                ExitNow();
            }

            err = cmdParser.GetMustBeVersion(&command->mustBeVersion);
            if (WEAVE_NO_ERROR == err)
            {
                command->SetMustBeVersionValid(true);
            }
            else if (WEAVE_END_OF_TLV == err)
            {
                err = WEAVE_NO_ERROR;
            }
            else
            {
                ExitNow();
            }

            err = cmdParser.GetReaderOnArgument(&reader);
            SuccessOrExit(err);
        }

#if WDM_ENFORCE_EXPIRY_TIME
        if (command->IsExpiryTimeValid())
        {
            uint64_t now_usec;
            err = System::Layer::GetClock_RealTime(now_usec);
            if (WEAVE_SYSTEM_ERROR_NOT_SUPPORTED == err)
            {
                statusReportCode = nl::Weave::Profiles::DataManagement::kStatus_ExpiryTimeNotSupported;
                ExitNow();
            }
            else if (WEAVE_SYSTEM_ERROR_REAL_TIME_NOT_SYNCED == err)
            {
                statusReportCode = nl::Weave::Profiles::DataManagement::kStatus_NotTimeSyncedYet;
                ExitNow();
            }
            else if (now_usec >= (uint64_t) command->expiryTimeMicroSecond)
            {
                statusReportCode = nl::Weave::Profiles::DataManagement::kStatus_RequestExpiredInTime;
                ExitNow();
            }
            WeaveLogDetail(DataManagement, "Command ExpiryTime 0x%" PRIX64 ", now: 0x% " PRIX64 " ", command->expiryTimeMicroSecond,
                           now_usec);
        }
#endif // WDM_ENFORCE_EXPIRY_TIME

        if (command->IsMustBeVersionValid())
        {
            uint64_t currentVersion = dataSource->GetVersion();

            if (command->mustBeVersion != currentVersion)
            {
                WeaveLogDetail(DataManagement, "Version required 0x%" PRIX64 ", current: 0x% " PRIX64 " ", command->mustBeVersion,
                               currentVersion);
                statusReportCode = nl::Weave::Profiles::DataManagement::kStatus_VersionMismatch;
                ExitNow();
            }
        }
        // Note we cannot just use pathReader at here because the TDM related functions
        // generally assume they can move the reader at their will.
        // Note that callee is supposed to cache whatever is useful in the TLV stream into its own memory
        // when this callback returns, we'd destroy the TLV object

        dataSource->OnCustomCommand(command, aMsgInfo, aPayload, command->commandType, command->IsExpiryTimeValid(),
                                    command->expiryTimeMicroSecond, command->IsMustBeVersionValid(), command->mustBeVersion,
                                    reader);

        command  = NULL;
        aPayload = NULL;
    }

exit:
    WeaveLogFunctError(err);

    if (NULL != aPayload)
    {
        PacketBuffer::Free(aPayload);
        aPayload = NULL;
    }

    // Note that when dispatched == true, ownership of aEC is already passed on to OnCustomCommand, and hence set to NULL
    if (NULL != command)
    {
        err = command->SendError(statusReportProfile, statusReportCode, err);
        WeaveLogFunctError(err);
    }

    if (NULL != aEC)
    {
        aEC->Close();
        aEC = NULL;
    }
}
#endif // WDM_PUBLISHER_ENABLE_CUSTOM_COMMAND_HANDLER

#if WDM_ENABLE_PUBLISHER_UPDATE_SERVER_SUPPORT
WEAVE_ERROR SubscriptionEngine::AllocateRightSizedBuffer(PacketBuffer *& buf, const uint32_t desiredSize, const uint32_t minSize,
                                                         uint32_t & outMaxPayloadSize)
{
    WEAVE_ERROR err          = WEAVE_NO_ERROR;
    uint32_t bufferAllocSize = 0;
    uint32_t maxWeavePayloadSize;
    uint32_t weaveTrailerSize = WEAVE_TRAILER_RESERVE_SIZE;
    uint32_t weaveHeaderSize  = WEAVE_SYSTEM_CONFIG_HEADER_RESERVE_SIZE;

    bufferAllocSize = nl::Weave::min(
        desiredSize, static_cast<uint32_t>(WEAVE_SYSTEM_CONFIG_PACKETBUFFER_CAPACITY_MAX - weaveHeaderSize - weaveTrailerSize));

    // Add the Weave Trailer size as NewWithAvailableSize() includes that in
    // availableSize.
    bufferAllocSize += weaveTrailerSize;

    buf = PacketBuffer::NewWithAvailableSize(weaveHeaderSize, bufferAllocSize);
    VerifyOrExit(buf != NULL, err = WEAVE_ERROR_NO_MEMORY);

    maxWeavePayloadSize = WeaveMessageLayer::GetMaxWeavePayloadSize(buf, true, WEAVE_CONFIG_DEFAULT_UDP_MTU_SIZE);

    outMaxPayloadSize = nl::Weave::min(maxWeavePayloadSize, bufferAllocSize);

    if (outMaxPayloadSize < minSize)
    {
        err = WEAVE_ERROR_BUFFER_TOO_SMALL;

        PacketBuffer::Free(buf);
        buf = NULL;
    }

exit:
    return err;
}

/**
 * Initialize StatusDataHandleList
 */
WEAVE_ERROR SubscriptionEngine::InitializeStatusDataHandleList(Weave::TLV::TLVReader & aReader,
                                                               StatusDataHandleElement * apStatusDataHandleList,
                                                               uint32_t & aNumDataElements, uint8_t * apBufEndAddr)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;
    Weave::TLV::TLVReader dataReader;
    dataReader.Init(aReader);

    for (aNumDataElements = 0; WEAVE_NO_ERROR == (err = dataReader.Next()); aNumDataElements++)
    {
        // Check if apStatusDataHandleList[aNumDataElements] overflow the end of the buffer.
        VerifyOrExit((uint8_t *) (apStatusDataHandleList + aNumDataElements + 1) <= apBufEndAddr, err = WEAVE_ERROR_NO_MEMORY);
        apStatusDataHandleList[aNumDataElements].mProfileId       = Weave::Profiles::kWeaveProfile_Common;
        apStatusDataHandleList[aNumDataElements].mStatusCode      = Weave::Profiles::Common::kStatus_InternalError;
        apStatusDataHandleList[aNumDataElements].mTraitDataHandle = 0;
    }

    // if we have exhausted this container
    if (WEAVE_END_OF_TLV == err)
    {
        err = WEAVE_NO_ERROR;
    }

exit:
    return err;
}

void SubscriptionEngine::ConstructStatusListVersionList(nl::Weave::TLV::TLVWriter & aWriter, void * apContext)
{
    WEAVE_ERROR err                                 = WEAVE_NO_ERROR;
    nl::Weave::TLV::TLVWriter checkpoint            = aWriter;
    TraitDataSource * dataSource                    = NULL;
    struct StatusDataHandleElement * elementTracker = NULL;
    UpdateResponseWriterContext * context           = NULL;
    UpdateResponse::Builder updateResponseBuilder;

    VerifyOrExit(NULL != apContext, err = WEAVE_ERROR_INCORRECT_STATE);
    context = static_cast<UpdateResponseWriterContext *>(apContext);

    err = updateResponseBuilder.Init(&aWriter);
    SuccessOrExit(err);

    elementTracker = static_cast<struct StatusDataHandleElement *>(context->mpFirstStatusDataHandleElement);
    {
        VersionList::Builder & lVLBuilder = updateResponseBuilder.CreateVersionListBuilder();
        for (uint32_t i = 0; i < context->mNumDataElements; i++)
        {
            if ((elementTracker->mProfileId == Weave::Profiles::kWeaveProfile_Common) &&
                (elementTracker->mStatusCode == Weave::Profiles::Common::kStatus_AccessDenied))
            {
                lVLBuilder.AddNull();
            }
            else if ((elementTracker->mProfileId == Weave::Profiles::kWeaveProfile_WDM) &&
                     (elementTracker->mStatusCode == kStatus_InvalidPath))
            {
                lVLBuilder.AddNull();
            }
            else if (context->mpCatalog->Locate(elementTracker->mTraitDataHandle, &dataSource) == WEAVE_NO_ERROR)
            {
                lVLBuilder.AddVersion(dataSource->GetVersion());
            }
            else
            {
                lVLBuilder.AddNull();
            }
            elementTracker++;
        }
        lVLBuilder.EndOfVersionList();
        SuccessOrExit(lVLBuilder.GetError());
    }

    elementTracker = static_cast<struct StatusDataHandleElement *>(context->mpFirstStatusDataHandleElement);
    {
        StatusList::Builder & lSLBuilder = updateResponseBuilder.CreateStatusListBuilder();
        for (uint32_t j = 0; j < context->mNumDataElements; j++)
        {
            lSLBuilder.AddStatus(elementTracker->mProfileId, elementTracker->mStatusCode);
            elementTracker++;
        }
        lSLBuilder.EndOfStatusList();
        SuccessOrExit(lSLBuilder.GetError());
    }

    updateResponseBuilder.EndOfResponse();
    SuccessOrExit(updateResponseBuilder.GetError());

    err = aWriter.Finalize();
    SuccessOrExit(err);

    WeaveLogDetail(DataManagement, "ConstructStatusListVersionList success with number of elements %d", context->mNumDataElements);

exit:
    if (err != WEAVE_NO_ERROR)
    {
        aWriter = checkpoint;
    }
}

void SubscriptionEngine::UpdateStatusDataHandleElement(StatusDataHandleElement * apStatusDataHandleList,
                                                       TraitDataHandle aTraitDataHandle, WEAVE_ERROR & err, uint32_t aCurrentIndex)
{
    uint32_t profileId  = 0;
    uint16_t statusCode = 0;

    if (WEAVE_ERROR_ACCESS_DENIED == err)
    {
        profileId  = Weave::Profiles::kWeaveProfile_Common;
        statusCode = Weave::Profiles::Common::kStatus_AccessDenied;
        err        = WEAVE_NO_ERROR;
    }
    else if (WEAVE_ERROR_INVALID_PROFILE_ID == err)
    {
        profileId  = Weave::Profiles::kWeaveProfile_WDM;
        statusCode = kStatus_InvalidPath;
        err        = WEAVE_NO_ERROR;
    }
    else if (WEAVE_ERROR_WDM_VERSION_MISMATCH == err)
    {
        profileId  = Weave::Profiles::kWeaveProfile_WDM;
        statusCode = kStatus_VersionMismatch;
        err        = WEAVE_NO_ERROR;
    }
    else if (WEAVE_ERROR_WRONG_TLV_TYPE == err || WEAVE_ERROR_TLV_TAG_NOT_FOUND == err)
    {
        profileId  = Weave::Profiles::kWeaveProfile_WDM;
        statusCode = kStatus_InvalidTLVInUpdate;
        err        = WEAVE_NO_ERROR;
    }
    else if (WEAVE_NO_ERROR == err)
    {
        profileId  = Weave::Profiles::kWeaveProfile_Common;
        statusCode = Weave::Profiles::Common::kStatus_Success;
    }
    else
    {
        profileId  = Weave::Profiles::kWeaveProfile_Common;
        statusCode = Weave::Profiles::Common::kStatus_InternalError;
    }

    apStatusDataHandleList[aCurrentIndex].mProfileId       = profileId;
    apStatusDataHandleList[aCurrentIndex].mStatusCode      = statusCode;
    apStatusDataHandleList[aCurrentIndex].mTraitDataHandle = aTraitDataHandle;
}

/**
 * Check if this triat path is the starting one
 */
bool SubscriptionEngine::IsStartingPath(StatusDataHandleElement * apStatusDataHandleList, TraitDataHandle aTraitDataHandle,
                                        uint32_t aCurrentIndex)
{
    bool isStarting = true;
    // TODO: Optimize to reduce lookup loop
    for (uint32_t index = 0; index < aCurrentIndex; index++)
    {
        if ((nl::Weave::Profiles::kWeaveProfile_Common == apStatusDataHandleList[index].mProfileId) &&
            (nl::Weave::Profiles::Common::kStatus_Success == apStatusDataHandleList[index].mStatusCode) &&
            (aTraitDataHandle == apStatusDataHandleList[index].mTraitDataHandle))
        {
            isStarting = false;
        }
    }
    return isStarting;
}

/**
 * Update version for all traits according to temporary statusDataHandleList, and bump version once at starting path
 * for each trait
 */
WEAVE_ERROR SubscriptionEngine::UpdateTraitVersions(StatusDataHandleElement * apStatusDataHandleList,
                                                    const TraitCatalogBase<TraitDataSource> * apCatalog, uint32_t aNumDataElements)
{
    WEAVE_ERROR err                            = WEAVE_NO_ERROR;
    TraitDataSource * dataSource               = NULL;
    TraitUpdatableDataSource * updatableSource = NULL;

    for (uint32_t index = 0; index < aNumDataElements; index++)
    {
        if ((nl::Weave::Profiles::kWeaveProfile_Common == apStatusDataHandleList[index].mProfileId) &&
            (nl::Weave::Profiles::Common::kStatus_Success == apStatusDataHandleList[index].mStatusCode))
        {

            err = apCatalog->Locate(apStatusDataHandleList[index].mTraitDataHandle, &dataSource);
            SuccessOrExit(err);

            updatableSource = static_cast<TraitUpdatableDataSource *>(dataSource);

            if (IsStartingPath(apStatusDataHandleList, apStatusDataHandleList[index].mTraitDataHandle, index))
            {
                updatableSource->IncrementVersion();
                WeaveLogDetail(DataManagement, "<UpdateTraitVersions> [Trait %08x] bumped version: 0x%" PRIx64 " ",
                               updatableSource->GetSchemaEngine()->GetProfileId(), updatableSource->GetVersion());

                updatableSource->OnEvent(TraitUpdatableDataSource::kEventUpdateProcessingComplete, NULL);
            }
            else
            {
                WeaveLogDetail(DataManagement, "<UpdateTraitVersions> [Trait %08x] version: 0x%" PRIx64 " (no-change)",
                               updatableSource->GetSchemaEngine()->GetProfileId(), updatableSource->GetVersion());
            }
        }
    }

exit:
    return err;
}

/**
 * If update request is malformed, it would send status report along with error status.
 */
WEAVE_ERROR SubscriptionEngine::SendFaultyUpdateResponse(Weave::ExchangeContext * apEC)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;
    uint8_t * p;
    uint8_t statusReportLen = 6;
    PacketBuffer * msgBuf   = PacketBuffer::NewWithAvailableSize(statusReportLen);
    VerifyOrExit(NULL != msgBuf, err = WEAVE_ERROR_NO_MEMORY);

    p = msgBuf->Start();
    nl::Weave::Encoding::LittleEndian::Write32(p, Weave::Profiles::kWeaveProfile_Common);
    nl::Weave::Encoding::LittleEndian::Write16(p, Weave::Profiles::Common::kStatus_BadRequest);
    msgBuf->SetDataLength(statusReportLen);

    err    = apEC->SendMessage(Profiles::kWeaveProfile_Common, Profiles::Common::kMsgType_StatusReport, msgBuf);
    msgBuf = NULL;
    SuccessOrExit(err);

exit:
    if (msgBuf != NULL)
    {
        PacketBuffer::Free(msgBuf);
        msgBuf = NULL;
    }
    return err;
}

/**
 * Relocating statusDataHandleList to the end of Update Response buffer, based upon existFailure and
 * statusDataHandleList, it constructs and sends status report.
 */
WEAVE_ERROR SubscriptionEngine::SendUpdateResponse(Weave::ExchangeContext * apEC, uint32_t aNumDataElements,
                                                   const TraitCatalogBase<TraitDataSource> * apCatalog, PacketBuffer * apBuf,
                                                   bool existFailure, uint32_t aMaxPayloadSize)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;
    UpdateResponseWriterContext context;
    StatusReport statusReport;
    ReferencedTLVData referenceTLVData;
    uint32_t totalStatusDataHandleListBytes = 0;
    uint32_t overalStatusProfile            = Weave::Profiles::kWeaveProfile_Common;
    uint16_t overalStatusCode               = Weave::Profiles::Common::kStatus_Success;
    uint8_t * movedStartAddr                = NULL;
    uint8_t * pBufStartAddr                 = WEAVE_WDM_ALIGNED_TYPE(apBuf->Start(), uint8_t);

    totalStatusDataHandleListBytes = aNumDataElements * sizeof(StatusDataHandleElement);

    WeaveLogDetail(DataManagement, "relocating the %d bytes statusDataHandleList to the end, NumDataElements is %d",
                   totalStatusDataHandleListBytes, aNumDataElements);

    movedStartAddr = WEAVE_WDM_ALIGNED_TYPE(pBufStartAddr + aMaxPayloadSize - totalStatusDataHandleListBytes - 3, uint8_t);

    memmove(movedStartAddr, pBufStartAddr, totalStatusDataHandleListBytes);

    context.mpFirstStatusDataHandleElement = movedStartAddr;
    context.mpCatalog                      = apCatalog;
    context.mNumDataElements               = aNumDataElements;

    // TODO: Refactor StatusReport and remove referenceTLVData
    err = referenceTLVData.init(ConstructStatusListVersionList, &context);
    SuccessOrExit(err);

    if (existFailure)
    {
        overalStatusProfile = Weave::Profiles::kWeaveProfile_WDM;
        overalStatusCode    = kStatus_MultipleFailures;
    }
    else
    {
        overalStatusProfile = Weave::Profiles::kWeaveProfile_Common;
        overalStatusCode    = Weave::Profiles::Common::kStatus_Success;
    }

    err = statusReport.init(overalStatusProfile, overalStatusCode, &referenceTLVData);
    SuccessOrExit(err);

    err = statusReport.pack(apBuf, aMaxPayloadSize - totalStatusDataHandleListBytes);
    SuccessOrExit(err);

    WeaveLogDetail(DataManagement, "Send Update Response with profileId 0x%" PRIX32 " statusCode 0x%" PRIX16 " ",
                   overalStatusProfile, overalStatusCode);
    err   = apEC->SendMessage(Weave::Profiles::kWeaveProfile_Common, Weave::Profiles::Common::kMsgType_StatusReport, apBuf);
    apBuf = NULL;

exit:
    if (apBuf != NULL)
    {
        PacketBuffer::Free(apBuf);
        apBuf = NULL;
    }
    return err;
}

/**
 * Run access check to check if DE is allowed, then during conditional DE loop, if DE does not have required version,
 * skip this one, otherwises its required version for current DE should be same as current trait.
 * During unconditional loop, if its required version is not 0, skip this one.
 * Finally it starts to store data element
 */
WEAVE_ERROR SubscriptionEngine::ProcessUpdateRequestDataElement(Weave::TLV::TLVReader & aReader, TraitDataHandle & aHandle,
                                                                PropertyPathHandle & aPathHandle,
                                                                const TraitCatalogBase<TraitDataSource> * apCatalog,
                                                                IUpdateRequestDataElementAccessControlDelegate & acDelegate,
                                                                bool aConditionalLoop, uint32_t aCurrentIndex, bool & aExistFailure,
                                                                StatusDataHandleElement * apStatusDataHandleList)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;
    Weave::TLV::TLVReader pathReader;
    Weave::TLV::TLVReader dataReader;
    TraitPath traitPath;
    DataElement::Parser element;
    uint64_t requiredVersion = 0;
    uint64_t versionInTrait  = 0;
    bool isPartialChange     = false;
    bool isLocked            = false;
    bool needSkip            = false;
    bool isConditionalDE     = true;
    SchemaVersionRange versionRange;
    TraitDataSource * dataSource               = NULL;
    TraitUpdatableDataSource * updatableSource = NULL;

    dataReader.Init(aReader);

    err = element.Init(aReader);
    SuccessOrExit(err);
    err = element.GetReaderOnPath(&pathReader);
    SuccessOrExit(err);

    // Not support partial change handling
    isPartialChange = false;
    element.GetPartialChangeFlag(&isPartialChange);
    VerifyOrExit(isPartialChange == false, err = WEAVE_ERROR_INCORRECT_STATE);

    err = apCatalog->AddressToHandle(pathReader, aHandle, versionRange);
    SuccessOrExit(err);

    err = apCatalog->Locate(aHandle, &dataSource);
    SuccessOrExit(err);
    err = dataSource->GetSchemaEngine()->MapPathToHandle(pathReader, aPathHandle);
#if TDM_DISABLE_STRICT_SCHEMA_COMPLIANCE
    if (err == WEAVE_ERROR_TLV_TAG_NOT_FOUND)
    {
        WeaveLogDetail(DataManagement, "Ignoring un-mappable path!");
        err = WEAVE_NO_ERROR;
    }
#endif
    SuccessOrExit(err);

    traitPath.mTraitDataHandle    = aHandle;
    traitPath.mPropertyPathHandle = aPathHandle;

    err = acDelegate.DataElementAccessCheck(traitPath, *apCatalog);
    SuccessOrExit(err);

    updatableSource = static_cast<TraitUpdatableDataSource *>(dataSource);
    updatableSource->Lock();
    isLocked = true;

    versionInTrait = updatableSource->GetVersion();

    err = element.GetVersion(&requiredVersion);
    if (WEAVE_END_OF_TLV == err)
    {
        err             = WEAVE_NO_ERROR;
        isConditionalDE = false;
    }
    SuccessOrExit(err);

    if (aConditionalLoop)
    {
        VerifyOrExit(isConditionalDE, needSkip = true);
        VerifyOrExit(versionInTrait == requiredVersion, err = WEAVE_ERROR_WDM_VERSION_MISMATCH);
    }
    else
    {
        VerifyOrExit(!isConditionalDE, needSkip = true);
    }
    WeaveLogDetail(DataManagement, "processing %s DE, index %d", aConditionalLoop ? "conditional" : "unconditional", aCurrentIndex);

    err = updatableSource->StoreDataElement(aPathHandle, dataReader, 0, NULL, NULL);
    SuccessOrExit(err);

    updatableSource->SetDirty(aPathHandle);

    updatableSource->Unlock(true);
    isLocked = false;

exit:
    if (isLocked && updatableSource != NULL)
    {
        updatableSource->Unlock(true);
    }

    if (WEAVE_NO_ERROR != err)
    {
        WeaveLogDetail(DataManagement, "There exists %d DE with err %d", aCurrentIndex, err);
        aExistFailure = true;
    }

    if (!needSkip)
    {
        UpdateStatusDataHandleElement(apStatusDataHandleList, aHandle, err, aCurrentIndex);
    }

    return err;
}

/**
 * Loop through all data elements in list and process either conditional data elements or unconditional data elements in
 * one loop, and build temporary statusDataHandleList. Later it would use this list to construct update response.
 */
WEAVE_ERROR SubscriptionEngine::ProcessUpdateRequestDataListWithConditionality(
    Weave::TLV::TLVReader & aReader, StatusDataHandleElement * apStatusDataHandleList,
    const TraitCatalogBase<TraitDataSource> * apCatalog, IUpdateRequestDataElementAccessControlDelegate & acDelegate,
    bool & aExistFailure, bool aConditionalLoop)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;
    Weave::TLV::TLVReader dataReader;
    dataReader.Init(aReader);

    for (uint32_t index = 0; WEAVE_NO_ERROR == (err = dataReader.Next()); index++)
    {
        TraitDataHandle handle;
        PropertyPathHandle pathHandle;
        if (!(apStatusDataHandleList[index].mProfileId == nl::Weave::Profiles::kWeaveProfile_Common && apStatusDataHandleList[index].mStatusCode == nl::Weave::Profiles::Common::kStatus_Success))
        {
            // if it is running conditional loop, needs to skip unconditional elements, vice versa.
            err = ProcessUpdateRequestDataElement(dataReader, handle, pathHandle, apCatalog, acDelegate, aConditionalLoop, index,
                                                  aExistFailure, apStatusDataHandleList);
            SuccessOrExit(err);
        }
    }

    // if we have exhausted this container
    if (WEAVE_END_OF_TLV == err)
    {
        err = WEAVE_NO_ERROR;
    }

exit:
    return err;
}

/**
 * Process Data list in Update requests and update trait version for processed data elements
 */
WEAVE_ERROR SubscriptionEngine::ProcessUpdateRequestDataList(Weave::TLV::TLVReader & aReader,
                                                             StatusDataHandleElement * apStatusDataHandleList,
                                                             const TraitCatalogBase<TraitDataSource> * apCatalog,
                                                             IUpdateRequestDataElementAccessControlDelegate & acDelegate,
                                                             bool & aExistFailure, uint32_t aNumDataElements)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;

    // process conditional DEs
    err =
        ProcessUpdateRequestDataListWithConditionality(aReader, apStatusDataHandleList, apCatalog, acDelegate, aExistFailure, true);
    SuccessOrExit(err);


    // process unconditional DEs
    err = ProcessUpdateRequestDataListWithConditionality(aReader, apStatusDataHandleList, apCatalog, acDelegate, aExistFailure,
                                                         false);
    SuccessOrExit(err);

exit:
    UpdateTraitVersions(apStatusDataHandleList, apCatalog, aNumDataElements);
    return err;
}

/**
 * Apply all conditional DEs first, then apply all unconditional DEs second, during loop, add status and dataHandle to
 * temporary result list. Then update starting version for traits and generate update response.
 */
WEAVE_ERROR SubscriptionEngine::ProcessUpdateRequest(Weave::ExchangeContext * apEC, Weave::TLV::TLVReader & aReader,
                                                     const TraitCatalogBase<TraitDataSource> * apCatalog,
                                                     IUpdateRequestDataElementAccessControlDelegate & acDelegate)
{
    WEAVE_ERROR err                                = WEAVE_NO_ERROR;
    PacketBuffer * pBuf                            = NULL;
    bool existFailure                              = false;
    uint32_t numDataElements                       = 0;
    uint32_t maxPayloadSize                        = 0;
    StatusDataHandleElement * statusDataHandleList = NULL;
    uint8_t * pBufEndAddr                          = NULL;
    VerifyOrExit(apCatalog != NULL, err = WEAVE_ERROR_INVALID_ARGUMENT);

    err = AllocateRightSizedBuffer(pBuf, WDM_MAX_UPDATE_RESPONSE_SIZE, WDM_MIN_UPDATE_RESPONSE_SIZE, maxPayloadSize);
    SuccessOrExit(err);

    statusDataHandleList = WEAVE_WDM_ALIGNED_TYPE(pBuf->Start(), StatusDataHandleElement);
    pBufEndAddr          = pBuf->Start() + maxPayloadSize;
    err                  = InitializeStatusDataHandleList(aReader, statusDataHandleList, numDataElements, pBufEndAddr);
    SuccessOrExit(err);

    err = ProcessUpdateRequestDataList(aReader, statusDataHandleList, apCatalog, acDelegate, existFailure, numDataElements);
    SuccessOrExit(err);

    err  = SendUpdateResponse(apEC, numDataElements, apCatalog, pBuf, existFailure, maxPayloadSize);
    pBuf = NULL;

exit:
    if (pBuf != NULL)
    {
        PacketBuffer::Free(pBuf);
    }

    if (WEAVE_NO_ERROR != err)
    {
        SendFaultyUpdateResponse(apEC);
    }
    return err;
}

/**
 * Process UpdateRequest if Data list is present and send notification if subscription exists
 */
void SubscriptionEngine::OnUpdateRequest(Weave::ExchangeContext * apEC, const Inet::IPPacketInfo * aPktInfo,
                                         const Weave::WeaveMessageInfo * aMsgInfo, uint32_t aProfileId, uint8_t aMsgType,
                                         PacketBuffer * aPayload)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;
    UpdateRequest::Parser update;
    Weave::TLV::TLVReader reader;
    bool isDataListPresent     = false;
    bool hasUpdateRequestBegin = false;
    InEventParam inParam;
    OutEventParam outParam;

    SubscriptionEngine * const pEngine = reinterpret_cast<SubscriptionEngine *>(apEC->AppState);

    inParam.Clear();
    outParam.Clear();

    inParam.mIncomingUpdateRequest.processingError            = err;
    inParam.mIncomingUpdateRequest.mMsgInfo                   = aMsgInfo;
    outParam.mIncomingUpdateRequest.mShouldContinueProcessing = true;

    if (pEngine->mEventCallback)
    {
        pEngine->mEventCallback(pEngine->mAppState, kEvent_OnIncomingUpdateRequest, inParam, outParam);
    }

    if (!outParam.mIncomingUpdateRequest.mShouldContinueProcessing)
    {
        WeaveLogDetail(DataManagement, "Update not allowed");
        ExitNow();
    }

    pEngine->mPublisherCatalog->DispatchEvent(TraitUpdatableDataSource::kEventUpdateRequestBegin, NULL);
    hasUpdateRequestBegin = true;

    reader.Init(aPayload);

    err = reader.Next();
    SuccessOrExit(err);

    err = update.Init(reader);
    SuccessOrExit(err);

#if WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK
    err = update.CheckSchemaValidity();
    SuccessOrExit(err);
#endif // WEAVE_CONFIG_DATA_MANAGEMENT_ENABLE_SCHEMA_CHECK

    {
        DataList::Parser dataList;

        err = update.GetDataList(&dataList);
        if (WEAVE_NO_ERROR == err)
        {
            isDataListPresent = true;
        }
        else if (WEAVE_END_OF_TLV == err)
        {
            isDataListPresent = false;
            err               = WEAVE_NO_ERROR;
        }
        SuccessOrExit(err);

        // re-initialize the reader to point to individual date element (reuse to save stack depth).
        dataList.GetReader(&reader);
    }

    if (isDataListPresent)
    {
        UpdateRequestDataElementAccessControlDelegate acDelegate(aMsgInfo);
        IUpdateRequestDataElementAccessControlDelegate & acDelegateRef = acDelegate;

        err = ProcessUpdateRequest(apEC, reader, pEngine->mPublisherCatalog, acDelegateRef);
        SuccessOrExit(err);

        pEngine->GetNotificationEngine()->ScheduleRun();
    }

exit:
    if (hasUpdateRequestBegin)
    {
        pEngine->mPublisherCatalog->DispatchEvent(TraitUpdatableDataSource::kEventUpdateRequestEnd, NULL);
    }

    if (NULL != aPayload)
    {
        PacketBuffer::Free(aPayload);
        aPayload = NULL;
    }

    if (NULL != apEC)
    {
        apEC->Abort();
        apEC = NULL;
    }

    if (NULL != pEngine->mEventCallback)
    {
        inParam.Clear();
        outParam.Clear();

        inParam.mIncomingUpdateRequest.processingError = err;
        inParam.mIncomingUpdateRequest.mMsgInfo        = aMsgInfo;
        // Update completion event indication.
        pEngine->mEventCallback(pEngine->mAppState, kEvent_UpdateRequestProcessingComplete, inParam, outParam);
    }
}

WEAVE_ERROR SubscriptionEngine::UpdateRequestDataElementAccessControlDelegate::DataElementAccessCheck(
    const TraitPath & aTraitPath, const TraitCatalogBase<TraitDataSource> & aCatalog)
{
    WEAVE_ERROR err = WEAVE_NO_ERROR;
    TraitDataSource * dataSource;
    InEventParam inParam;
    OutEventParam outParam;
    SubscriptionEngine * pEngine = SubscriptionEngine::GetInstance();

    err = aCatalog.Locate(aTraitPath.mTraitDataHandle, &dataSource);
    SuccessOrExit(err);

    inParam.Clear();
    outParam.Clear();

    if (dataSource->IsUpdatableDataSource())
    {
        outParam.mDataElementAccessControlForUpdateRequest.mRejectUpdateRequest = false;
        outParam.mDataElementAccessControlForUpdateRequest.mReason              = WEAVE_NO_ERROR;
    }
    else
    {
        outParam.mDataElementAccessControlForUpdateRequest.mRejectUpdateRequest = true;
        outParam.mDataElementAccessControlForUpdateRequest.mReason              = WEAVE_ERROR_ACCESS_DENIED;
    }

    inParam.mDataElementAccessControlForUpdateRequest.mPath    = &aTraitPath;
    inParam.mDataElementAccessControlForUpdateRequest.mCatalog = &aCatalog;
    inParam.mDataElementAccessControlForUpdateRequest.mMsgInfo = mMsgInfo;

    if (NULL != pEngine->mEventCallback)
    {
        pEngine->mEventCallback(pEngine->mAppState, kEvent_UpdateRequestDataElementAccessControlCheck, inParam, outParam);
    }

    // If application rejects it then deny access, else set reason to whatever
    // reason is set by application.
    if (outParam.mDataElementAccessControlForUpdateRequest.mRejectUpdateRequest == true)
    {
        err = WEAVE_ERROR_ACCESS_DENIED;
    }
    else
    {
        err = outParam.mDataElementAccessControlForUpdateRequest.mReason;
    }

exit:

    return err;
}

#endif // WDM_ENABLE_PUBLISHER_UPDATE_SERVER_SUPPORT

#endif // WDM_ENABLE_SUBSCRIPTION_PUBLISHER

}; // namespace WeaveMakeManagedNamespaceIdentifier(DataManagement, kWeaveManagedNamespaceDesignation_Current)
}; // namespace Profiles
}; // namespace Weave
}; // namespace nl
