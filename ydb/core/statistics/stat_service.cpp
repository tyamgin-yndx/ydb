#include "stat_service.h"
#include "events.h"

#include <ydb/library/services/services.pb.h>
#include <ydb/core/base/feature_flags.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/cms/console/configs_dispatcher.h>
#include <ydb/core/cms/console/console.h>
#include <ydb/core/base/appdata_fwd.h>

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>

namespace NKikimr {
namespace NStat {

class TStatService : public TActorBootstrapped<TStatService> {
public:
    using TBase = TActorBootstrapped<TStatService>;

    static constexpr auto ActorActivityType() {
        return NKikimrServices::TActivity::STAT_SERVICE;
    }

    void Bootstrap() {
        EnableStatistics = AppData()->FeatureFlags.GetEnableStatistics();

        ui32 configKind = (ui32) NKikimrConsole::TConfigItem::FeatureFlagsItem;
        Send(NConsole::MakeConfigsDispatcherID(SelfId().NodeId()),
            new NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionRequest({configKind}));

        Become(&TStatService::StateWork);
    }

    STFUNC(StateWork) {
        switch(ev->GetTypeRewrite()) {
            hFunc(NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionResponse, HandleConfig)
            hFunc(NConsole::TEvConsole::TEvConfigNotificationRequest, HandleConfig)
            hFunc(TEvStatistics::TEvGetStatistics, Handle);
            hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
            hFunc(TEvStatistics::TEvPropagateStatistics, Handle);
            hFunc(TEvTabletPipe::TEvClientConnected, Handle);
            hFunc(TEvTabletPipe::TEvClientDestroyed, Handle);
            cFunc(TEvents::TEvPoison::EventType, PassAway);
            default:
                LOG_CRIT_S(TlsActivationContext->AsActorContext(), NKikimrServices::STATISTICS,
                    "NStat::TStatService: unexpected event# " << ev->GetTypeRewrite());
        }
    }

private:
    bool IsSAUnavailable() {
        return ResolveSAStage == RSA_FINISHED && StatisticsAggregatorId == 0;
    }

    void HandleConfig(NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionResponse::TPtr&) {
        LOG_INFO_S(TlsActivationContext->AsActorContext(), NKikimrServices::STATISTICS,
            "Subscribed for config changes");
    }

    void HandleConfig(NConsole::TEvConsole::TEvConfigNotificationRequest::TPtr& ev) {
        const auto& record = ev->Get()->Record;
        const auto& featureFlags = record.GetConfig().GetFeatureFlags();
        EnableStatistics = featureFlags.GetEnableStatistics();

        auto response = std::make_unique<NConsole::TEvConsole::TEvConfigNotificationResponse>(record);
        Send(ev->Sender, response.release(), 0, ev->Cookie);
    }

    void Handle(TEvStatistics::TEvGetStatistics::TPtr& ev) {
        ui64 requestId = NextRequestId++;

        auto& request = InFlight[requestId];
        request.ReplyToActorId = ev->Sender;
        request.EvCookie = ev->Cookie;
        request.StatRequests.swap(ev->Get()->StatRequests);

        if (!EnableStatistics || IsSAUnavailable()) {
            ReplyFailed(requestId, true);
            return;
        }

        using TNavigate = NSchemeCache::TSchemeCacheNavigate;
        auto navigate = std::make_unique<TNavigate>();
        for (const auto& req : request.StatRequests) {
            auto& entry = navigate->ResultSet.emplace_back();
            entry.TableId = TTableId(req.PathId.OwnerId, req.PathId.LocalPathId);
            entry.Operation = TNavigate::EOp::OpPath;
            entry.RequestType = TNavigate::TEntry::ERequestType::ByTableId;
        }
        navigate->Cookie = requestId;

        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(navigate.release()));
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
        using TNavigate = NSchemeCache::TSchemeCacheNavigate;
        std::unique_ptr<TNavigate> navigate(ev->Get()->Request.Release());

        auto cookie = navigate->Cookie;

        if (cookie == ResolveSACookie) {
            Y_ABORT_UNLESS(navigate->ResultSet.size() == 1);
            auto& entry = navigate->ResultSet.back();
            if (entry.Status != TNavigate::EStatus::Ok) {
                StatisticsAggregatorId = 0;
            } else {
                StatisticsAggregatorId = entry.DomainInfo->Params.GetStatisticsAggregator();
            }
            ResolveSAStage = RSA_FINISHED;

            if (StatisticsAggregatorId != 0) {
                ConnectToSA();
                SyncNode();
            } else {
                ReplyAllFailed();
            }
            return;
        }

        ui64 requestId = cookie;
        auto itRequest = InFlight.find(requestId);
        if (itRequest == InFlight.end()) {
            return;
        }
        auto& request = itRequest->second;

        if (!EnableStatistics || IsSAUnavailable()) {
            ReplyFailed(requestId, true);
            return;
        }

        std::unordered_set<ui64> ssIds;
        bool isServerless = false;
        ui64 aggregatorId = 0;
        TPathId resourcesDomainKey;
        for (const auto& entry : navigate->ResultSet) {
            if (entry.Status != TNavigate::EStatus::Ok) {
                continue;
            }
            auto& domainInfo = entry.DomainInfo;
            ssIds.insert(domainInfo->ExtractSchemeShard());
            aggregatorId = domainInfo->Params.GetStatisticsAggregator();
            isServerless = domainInfo->IsServerless();
            resourcesDomainKey = domainInfo->ResourcesDomainKey;
        }
        if (ssIds.size() != 1) {
            ReplyFailed(requestId, true);
            return;
        }
        request.SchemeShardId = *ssIds.begin();

        if (Statistics.find(request.SchemeShardId) != Statistics.end()) {
            ReplySuccess(requestId, true);
            return;
        }

        switch (ResolveSAStage) {
        case RSA_NOT_RUN:
            if (!isServerless) {
                StatisticsAggregatorId = aggregatorId;
                ResolveSAStage = RSA_FINISHED;
            } else {
                using TNavigate = NSchemeCache::TSchemeCacheNavigate;
                auto navigate = std::make_unique<TNavigate>();
                auto& entry = navigate->ResultSet.emplace_back();
                entry.TableId = TTableId(resourcesDomainKey.OwnerId, resourcesDomainKey.LocalPathId);
                entry.Operation = TNavigate::EOp::OpPath;
                entry.RequestType = TNavigate::TEntry::ERequestType::ByTableId;
                entry.RedirectRequired = false;
                navigate->Cookie = ResolveSACookie;
                Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(navigate.release()));
                ResolveSAStage = RSA_IN_FLIGHT;
                return;
            }
            break;
        case RSA_IN_FLIGHT:
            return;
        default:
            break;
        }

        if (IsSAUnavailable()) {
            ReplyFailed(requestId, true);
            return;
        }

        if (!SAPipeClientId) {
            ConnectToSA();
            SyncNode();
        } else {
            auto requestStats = std::make_unique<TEvStatistics::TEvRequestStats>();
            requestStats->Record.SetNodeId(SelfId().NodeId());
            requestStats->Record.AddNeedSchemeShards(request.SchemeShardId);
            NTabletPipe::SendData(SelfId(), SAPipeClientId, requestStats.release());
        }
    }

    void Handle(TEvStatistics::TEvPropagateStatistics::TPtr& ev) {
        LOG_DEBUG_S(TlsActivationContext->AsActorContext(), NKikimrServices::STATISTICS,
            "EvPropagateStatistics, node id = " << SelfId().NodeId());

        auto* record = ev->Get()->MutableRecord();
        for (const auto& entry : record->GetEntries()) {
            ui64 schemeShardId = entry.GetSchemeShardId();
            auto& statisticsState = Statistics[schemeShardId];

            if (entry.GetStats().empty()) {
                continue; // stats are not ready in SA, wait for next cycle
            }

            statisticsState.Map.clear();

            NKikimrStat::TSchemeShardStats statRecord;
            Y_PROTOBUF_SUPPRESS_NODISCARD statRecord.ParseFromString(entry.GetStats());

            for (const auto& pathEntry : statRecord.GetEntries()) {
                TPathId pathId(pathEntry.GetPathId().GetOwnerId(), pathEntry.GetPathId().GetLocalId());
                auto& mapEntry = statisticsState.Map[pathId];
                mapEntry.RowCount = pathEntry.GetRowCount();
                mapEntry.BytesSize = pathEntry.GetBytesSize();
            }
        }

        for (auto itReq = InFlight.begin(); itReq != InFlight.end(); ) {
            auto requestId = itReq->first;
            auto requestState = itReq->second;
            if (requestState.SchemeShardId == 0) {
                ++itReq;
                continue;
            }
            if (Statistics.find(requestState.SchemeShardId) != Statistics.end()) {
                ReplySuccess(requestId, false);
                itReq = InFlight.erase(itReq);
            } else {
                ++itReq;
            }
        }

        if (record->NodeIdsSize() == 0) {
            return;
        }

        std::vector<ui32> nodeIds;
        nodeIds.reserve(record->NodeIdsSize());
        for (const auto nodeId : record->GetNodeIds()) {
            nodeIds.push_back(nodeId);
        }

        size_t step = 0;
        if (nodeIds.size() <= StatFanOut + 1) {
            step = 0;
        } else if (nodeIds.size() <= StatFanOut * (StatFanOut + 1)) {
            step = StatFanOut;
        } else {
            step = nodeIds.size() / StatFanOut;
        }

        auto serialized = std::make_unique<TEvStatistics::TEvPropagateStatistics>();
        serialized->MutableRecord()->MutableEntries()->Swap(record->MutableEntries());
        TString preSerializedStats;
        Y_PROTOBUF_SUPPRESS_NODISCARD serialized->GetRecord().SerializeToString(&preSerializedStats);

        for (size_t i = 0; i < nodeIds.size(); ) {
            ui32 leadingNodeId = nodeIds[i++];

            auto propagate = std::make_unique<TEvStatistics::TEvPropagateStatistics>();
            propagate->MutableRecord()->MutableNodeIds()->Reserve(step);
            for (size_t j = 0; i < nodeIds.size() && j < step; ++i, ++j) {
                propagate->MutableRecord()->AddNodeIds(nodeIds[i]);
            }
            propagate->PreSerializedData = preSerializedStats;
            Send(MakeStatServiceID(leadingNodeId), propagate.release());
        }
    }

    void Handle(TEvTabletPipe::TEvClientConnected::TPtr& ev) {
        LOG_DEBUG_S(TlsActivationContext->AsActorContext(), NKikimrServices::STATISTICS,
            "EvClientConnected"
            << ", node id = " << ev->Get()->ClientId.NodeId()
            << ", client id = " << ev->Get()->ClientId
            << ", server id = " << ev->Get()->ServerId
            << ", status = " << ev->Get()->Status);

        if (ev->Get()->Status != NKikimrProto::OK) {
            SAPipeClientId = TActorId();
            ConnectToSA();
            SyncNode();
        }
    }

    void Handle(TEvTabletPipe::TEvClientDestroyed::TPtr& ev) {
        LOG_DEBUG_S(TlsActivationContext->AsActorContext(), NKikimrServices::STATISTICS,
            "EvClientDestroyed"
            << ", node id = " << ev->Get()->ClientId.NodeId()
            << ", client id = " << ev->Get()->ClientId
            << ", server id = " << ev->Get()->ServerId);

        SAPipeClientId = TActorId();
        ConnectToSA();
        SyncNode();
    }

    void ConnectToSA() {
        if (SAPipeClientId || !StatisticsAggregatorId) {
            return;
        }
        auto policy = NTabletPipe::TClientRetryPolicy::WithRetries();
        NTabletPipe::TClientConfig pipeConfig{policy};
        SAPipeClientId = Register(NTabletPipe::CreateClient(SelfId(), StatisticsAggregatorId, pipeConfig));

        LOG_DEBUG_S(TlsActivationContext->AsActorContext(), NKikimrServices::STATISTICS,
            "ConnectToSA(), pipe client id = " << SAPipeClientId);
    }

    void SyncNode() {
        if (!SAPipeClientId || !StatisticsAggregatorId) {
            return;
        }
        auto connect = std::make_unique<TEvStatistics::TEvConnectNode>();
        auto& record = connect->Record;

        record.SetNodeId(SelfId().NodeId());
        for (const auto& [ssId, ssState] : Statistics) {
            auto* entry = record.AddHaveSchemeShards();
            entry->SetSchemeShardId(ssId);
            entry->SetTimestamp(ssState.Timestamp);
        }
        std::unordered_set<ui64> ssIds;
        for (const auto& [reqId, reqState] : InFlight) {
            if (reqState.SchemeShardId != 0) {
                ssIds.insert(reqState.SchemeShardId);
            }
        }
        for (const auto& ssId : ssIds) {
            record.AddNeedSchemeShards(ssId);
        }
        NTabletPipe::SendData(SelfId(), SAPipeClientId, connect.release());

        LOG_DEBUG_S(TlsActivationContext->AsActorContext(), NKikimrServices::STATISTICS,
            "SyncNode(), pipe client id = " << SAPipeClientId);
    }

    void ReplySuccess(ui64 requestId, bool eraseRequest) {
        auto itRequest = InFlight.find(requestId);
        if (itRequest == InFlight.end()) {
            return;
        }
        auto& request = itRequest->second;

        LOG_DEBUG_S(TlsActivationContext->AsActorContext(), NKikimrServices::STATISTICS,
            "ReplySuccess(), request id = " << requestId);

        auto itStatistics = Statistics.find(request.SchemeShardId);
        if (itStatistics == Statistics.end()) {
            return;
        }
        auto& statisticsMap = itStatistics->second.Map;

        auto result = std::make_unique<TEvStatistics::TEvGetStatisticsResult>();
        result->Success = true;

        for (auto& req : request.StatRequests) {
            TResponse rsp;
            rsp.Success = true;
            rsp.Req = req;

            TStatSimple stat;
            auto itStat = statisticsMap.find(req.PathId);
            if (itStat != statisticsMap.end()) {
                stat.RowCount = itStat->second.RowCount;
                stat.BytesSize = itStat->second.BytesSize;
            } else {
                stat.RowCount = 0;
                stat.BytesSize = 0;
            }
            rsp.Statistics = stat;

            result->StatResponses.push_back(rsp);
        }

        Send(request.ReplyToActorId, result.release(), 0, request.EvCookie);

        if (eraseRequest) {
            InFlight.erase(requestId);
        }
    }

    void ReplyFailed(ui64 requestId, bool eraseRequest) {
        auto itRequest = InFlight.find(requestId);
        if (itRequest == InFlight.end()) {
            return;
        }
        auto& request = itRequest->second;

        LOG_DEBUG_S(TlsActivationContext->AsActorContext(), NKikimrServices::STATISTICS,
            "ReplyFailed(), request id = " << requestId);

        auto result = std::make_unique<TEvStatistics::TEvGetStatisticsResult>();
        result->Success = false;

        for (auto& req : request.StatRequests) {
            TResponse rsp;
            rsp.Success = false;
            rsp.Req = req;

            TStatSimple stat;
            stat.RowCount = 0;
            stat.BytesSize = 0;
            rsp.Statistics = stat;

            result->StatResponses.push_back(rsp);
        }

        Send(request.ReplyToActorId, result.release(), 0, request.EvCookie);

        if (eraseRequest) {
            InFlight.erase(requestId);
        }
    }

    void ReplyAllFailed() {
        for (const auto& [requestId, _] : InFlight) {
            ReplyFailed(requestId, false);
        }
        InFlight.clear();
    }

    void PassAway() {
        if (SAPipeClientId) {
            NTabletPipe::CloseClient(SelfId(), SAPipeClientId);
        }
        TBase::PassAway();
    }

private:
    bool EnableStatistics = false;

    static constexpr size_t StatFanOut = 10;

    struct TRequestState {
        NActors::TActorId ReplyToActorId;
        ui64 EvCookie = 0;
        ui64 SchemeShardId = 0;
        std::vector<TRequest> StatRequests;
    };
    std::unordered_map<ui64, TRequestState> InFlight; // request id -> state
    ui64 NextRequestId = 1;

    struct TStatEntry {
        ui64 RowCount = 0;
        ui64 BytesSize = 0;
    };
    typedef std::unordered_map<TPathId, TStatEntry> TStatisticsMap;
    struct TStatisticsState {
        TStatisticsMap Map;
        ui64 Timestamp = 0;
    };
    std::unordered_map<ui64, TStatisticsState> Statistics; // ss id -> stats

    ui64 StatisticsAggregatorId = 0;
    TActorId SAPipeClientId;

    static const ui64 ResolveSACookie = std::numeric_limits<ui64>::max();
    enum EResolveSAStage {
        RSA_NOT_RUN,
        RSA_IN_FLIGHT,
        RSA_FINISHED
    };
    EResolveSAStage ResolveSAStage = RSA_NOT_RUN;
};

THolder<IActor> CreateStatService() {
    return MakeHolder<TStatService>();
}

} // NStat
} // NKikimr
