#pragma once
#include "defs.h"

#include "dsproxy.h"

#include <ydb/core/blobstorage/base/batched_vec.h>
#include <ydb/core/blobstorage/groupinfo/blobstorage_groupinfo.h>
#include <ydb/core/blobstorage/groupinfo/blobstorage_groupinfo_partlayout.h>
#include <ydb/core/util/fragmented_buffer.h>
#include <ydb/core/util/interval_set.h>
#include <library/cpp/containers/stack_vector/stack_vec.h>

namespace NKikimr {

struct EStrategyOutcome {
    enum TValue {
        IN_PROGRESS,
        ERROR, // unrecoverable request error
        DONE,
    };

    EStrategyOutcome() = default;
    EStrategyOutcome(TValue value) : Value(value) {}
    EStrategyOutcome(const EStrategyOutcome&) = default;
    EStrategyOutcome(EStrategyOutcome&&) = default;
    EStrategyOutcome& operator =(const EStrategyOutcome&) = default;
    EStrategyOutcome& operator =(EStrategyOutcome&&) = default;

    static EStrategyOutcome Error(TString err) {
        EStrategyOutcome res(ERROR);
        res.ErrorReason = std::move(err);
        return res;
    }

    operator TValue() const { return Value; }

    TValue Value;
    TString ErrorReason;
};

struct TBlobState {
    enum class ESituation {
        Unknown,
        Error,
        Absent,
        Lost,
        Present, // Restore strategy takes action only on theese
        Sent,    // For Restore and Put strategies
    };
    struct TState {
        TFragmentedBuffer Data;

        void AddResponseData(ui32 fullSize, ui32 shift, TRope&& data);
        void AddPartToPut(TRope&& partData);
        TString ToString() const;
        TIntervalSet<i32> Here() const { return Data.GetIntervalSet(); }
    };
    struct TWholeState : TState {
        TIntervalSet<i32> Needed;  // Requested by the external user

        TString ToString() const;
        TIntervalSet<i32> NotHere() const { return Needed - Here(); }
    };
    struct TDiskPart {
        TIntervalSet<i32> Requested;
        ESituation Situation = ESituation::Unknown;

        TString ToString() const;
    };
    struct TDisk {
        ui32 OrderNumber;
        bool IsSlow = false;
        TStackVec<TDiskPart, TypicalPartsInBlob> DiskParts;

        TString ToString() const;
    };

    TLogoBlobID Id;
    TWholeState Whole;
    ESituation WholeSituation = ESituation::Unknown;  // TODO(cthulhu): Use a specially tailored enum here
    TStackVec<TState, TypicalPartsInBlob> Parts;
    TStackVec<TDisk, TypicalDisksInSubring> Disks;
    TVector<TEvBlobStorage::TEvGetResult::TPartMapItem> PartMap;
    NKikimrProto::EReplyStatus Status = NKikimrProto::UNKNOWN;
    ui8 BlobIdx;
    bool IsChanged = false;

    void Init(const TLogoBlobID &id, const TBlobStorageGroupInfo &Info);
    void AddNeeded(ui64 begin, ui64 size);
    void AddPartToPut(ui32 partIdx, TRope&& partData);
    void MarkBlobReadyToPut(ui8 blobIdx = 0);
    bool Restore(const TBlobStorageGroupInfo &info);
    void AddResponseData(const TBlobStorageGroupInfo &info, const TLogoBlobID &id, ui32 diskIdxInSubring,
            ui32 shift, TRope&& data);
    void AddPutOkResponse(const TBlobStorageGroupInfo &info, const TLogoBlobID &id, ui32 orderNumber);
    void AddNoDataResponse(const TBlobStorageGroupInfo &info, const TLogoBlobID &id, ui32 diskIdxInSubring);
    void AddErrorResponse(const TBlobStorageGroupInfo &info, const TLogoBlobID &id, ui32 diskIdxInSubring);
    void AddNotYetResponse(const TBlobStorageGroupInfo &info, const TLogoBlobID &id, ui32 diskIdxInSubring);
    ui64 GetPredictedDelayNs(const TBlobStorageGroupInfo &info, TGroupQueues &groupQueues,
            ui32 diskIdxInSubring, NKikimrBlobStorage::EVDiskQueueId queueId) const;
    void GetWorstPredictedDelaysNs(const TBlobStorageGroupInfo &info, TGroupQueues &groupQueues,
            NKikimrBlobStorage::EVDiskQueueId queueId,
            ui64 *outWorstNs, ui64 *outNextToWorstNs, i32 *outWorstSubgroupIdx) const;
    TString ToString() const;
    bool HasWrittenQuorum(const TBlobStorageGroupInfo& info, const TBlobStorageGroupInfo::TGroupVDisks& expired) const;
            
    static TString SituationToString(ESituation situation);
};

struct TDiskGetRequest {
    const TLogoBlobID Id;
    const ui32 Shift;
    const ui32 Size;
    ssize_t PartMapIndex = -1;

    TDiskGetRequest(const TLogoBlobID &id, const ui32 shift, const ui32 size)
        : Id(id)
        , Shift(shift)
        , Size(size)
    {}
};

struct TDiskPutRequest {
    enum EPutReason {
        ReasonError,
        ReasonErrorOrNotReady,
        ReasonInitial,
        ReasonAccelerate
    };
    const TLogoBlobID Id;
    TRope Buffer;
    EPutReason Reason;
    bool IsHandoff;
    ui8 BlobIdx;

    TDiskPutRequest(const TLogoBlobID &id, TRope buffer, EPutReason reason, bool isHandoff, ui8 blobIdx)
        : Id(id)
        , Buffer(std::move(buffer))
        , Reason(reason)
        , IsHandoff(isHandoff)
        , BlobIdx(blobIdx)
    {}
};

struct TDiskRequests {
    TDeque<TDiskGetRequest> GetsToSend;
    TStackVec<TDiskPutRequest, TypicalPartsInBlob> PutsToSend;
    ui32 FirstUnsentRequestIdx = 0;
    ui32 FirstUnsentPutIdx = 0;
};

struct TGroupDiskRequests {
    TStackVec<TDiskRequests, TypicalDisksInGroup> DiskRequestsForOrderNumber;

    TGroupDiskRequests(ui32 disks);
    void AddGet(const ui32 diskOrderNumber, const TLogoBlobID &id, const TIntervalSet<i32> &intervalSet);
    void AddGet(const ui32 diskOrderNumber, const TLogoBlobID &id, const ui32 shift, const ui32 size);
    void AddPut(const ui32 diskOrderNumber, const TLogoBlobID &id, TRope buffer,
        TDiskPutRequest::EPutReason putReason, bool isHandoff, ui8 blobIdx);
};

struct TBlackboard;

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual EStrategyOutcome Process(TLogContext &logCtx, TBlobState &state, const TBlobStorageGroupInfo &info,
            TBlackboard &blackboard, TGroupDiskRequests &groupDiskRequests) = 0;
};

struct TBlackboard {
    enum EAccelerationMode {
        AccelerationModeSkipOneSlowest,
        AccelerationModeSkipMarked
    };

    using TBlobStates = TMap<TLogoBlobID, TBlobState>;
    TBlobStates BlobStates;
    TBlobStates DoneBlobStates;
    TGroupDiskRequests GroupDiskRequests;
    TIntrusivePtr<TBlobStorageGroupInfo> Info;
    TIntrusivePtr<TGroupQueues> GroupQueues; // To obtain FlowRecords only
    EAccelerationMode AccelerationMode;
    const NKikimrBlobStorage::EPutHandleClass PutHandleClass;
    const NKikimrBlobStorage::EGetHandleClass GetHandleClass;
    const bool IsAllRequestsTogether;

    TBlackboard(const TIntrusivePtr<TBlobStorageGroupInfo> &info, const TIntrusivePtr<TGroupQueues> &groupQueues,
            NKikimrBlobStorage::EPutHandleClass putHandleClass, NKikimrBlobStorage::EGetHandleClass getHandleClass,
            bool isAllRequestsTogether = true)
        : GroupDiskRequests(info->GetTotalVDisksNum())
        , Info(info)
        , GroupQueues(groupQueues)
        , AccelerationMode(AccelerationModeSkipOneSlowest)
        , PutHandleClass(putHandleClass)
        , GetHandleClass(getHandleClass)
        , IsAllRequestsTogether(isAllRequestsTogether)
    {}

    void AddNeeded(const TLogoBlobID &id, ui32 inShift, ui32 inSize);
    void AddPartToPut(const TLogoBlobID &id, ui32 partIdx, TRope&& partData);
    void MarkBlobReadyToPut(const TLogoBlobID &id, ui8 blobIdx = 0);
    void MoveBlobStateToDone(const TLogoBlobID &id);
    void AddResponseData(const TLogoBlobID &id, ui32 orderNumber, ui32 shift, TRope&& data);
    void AddPutOkResponse(const TLogoBlobID &id, ui32 orderNumber);
    void AddNoDataResponse(const TLogoBlobID &id, ui32 orderNumber);
    void AddErrorResponse(const TLogoBlobID &id, ui32 orderNumber);
    void AddNotYetResponse(const TLogoBlobID &id, ui32 orderNumber);
    EStrategyOutcome RunStrategies(TLogContext& logCtx, const TStackVec<IStrategy*, 1>& strategies,
        TBatchedVec<TBlobStates::value_type*> *finished = nullptr, const TBlobStorageGroupInfo::TGroupVDisks *expired = nullptr);
    EStrategyOutcome RunStrategy(TLogContext &logCtx, const IStrategy& s, TBatchedVec<TBlobStates::value_type*> *finished = nullptr,
            const TBlobStorageGroupInfo::TGroupVDisks *expired = nullptr);
    TBlobState& GetState(const TLogoBlobID &id);
    ssize_t AddPartMap(const TLogoBlobID &id, ui32 diskOrderNumber, ui32 requestIndex);
    void ReportPartMapStatus(const TLogoBlobID &id, ssize_t partMapIndex, ui32 responseIndex, NKikimrProto::EReplyStatus status);
    void GetWorstPredictedDelaysNs(const TBlobStorageGroupInfo &info, TGroupQueues &groupQueues,
            NKikimrBlobStorage::EVDiskQueueId queueId,
            ui64 *outWorstNs, ui64 *outNextToWorstNs, i32 *outWorstOrderNumber) const;
    TString ToString() const;

    void ChangeAll() {
        for (auto &[id, blob] : BlobStates) {
            blob.IsChanged = true;
        }
    }

    void InvalidatePartStates(ui32 orderNumber);

    void RegisterBlobForPut(const TLogoBlobID& id);

    TBlobState& operator [](const TLogoBlobID& id);
};

inline bool RestoreWholeFromMirror(TBlobState& state) {
    for (const TBlobState::TState& part : state.Parts) {
        state.Whole.Data.CopyFrom(part.Data, part.Here() & state.Whole.NotHere());
    }
    return !state.Whole.NotHere();
}

}//NKikimr
