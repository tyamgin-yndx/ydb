#include "manager.h"

#include <ydb/core/base/path.h>
#include <ydb/core/kqp/gateway/actors/scheme.h>
#include <ydb/core/tx/tx_proxy/proxy.h>

namespace NKikimr::NKqp {

namespace {

using TYqlConclusionStatus = TViewManager::TYqlConclusionStatus;
using TInternalModificationContext = TViewManager::TInternalModificationContext;

TString GetByKeyOrDefault(const NYql::TCreateObjectSettings& container, const TString& key) {
    const auto value = container.GetFeaturesExtractor().Extract(key);
    return value ? *value : TString{};
}

void CheckFeatureFlag(TInternalModificationContext& context) {
    auto* const actorSystem = context.GetExternalData().GetActorSystem();
    if (!actorSystem) {
        ythrow TSystemError() << "This place needs an actor system. Please contact internal support";
    }
    if (!AppData(actorSystem)->FeatureFlags.GetEnableViews()) {
        ythrow TSystemError() << "Views are disabled. Please contact your system administrator to enable it";
    }
}

void FillCreateViewProposal(NKikimrSchemeOp::TModifyScheme& modifyScheme,
                           const NYql::TCreateObjectSettings& settings,
                           TInternalModificationContext& context) {

    std::pair<TString, TString> pathPair;
    {
        TString error;
        if (!TrySplitPathByDb(settings.GetObjectId(), context.GetExternalData().GetDatabase(), pathPair, error)) {
            ythrow TBadArgumentException() << error;
        }
    }
    modifyScheme.SetWorkingDir(pathPair.first);
    modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateView);

    auto& viewDesc = *modifyScheme.MutableCreateView();
    viewDesc.SetName(pathPair.second);
    viewDesc.SetQueryText(GetByKeyOrDefault(settings, "query_text"));

    if (!settings.GetFeaturesExtractor().IsFinished()) {
        ythrow TBadArgumentException() << "Unknown property: " << settings.GetFeaturesExtractor().GetRemainedParamsString();
    }
}

void FillDropViewProposal(NKikimrSchemeOp::TModifyScheme& modifyScheme,
                         const NYql::TDropObjectSettings& settings,
                         TInternalModificationContext& context) {

    std::pair<TString, TString> pathPair;
    {
        TString error;
        if (!TrySplitPathByDb(settings.GetObjectId(), context.GetExternalData().GetDatabase(), pathPair, error)) {
            ythrow TBadArgumentException() << error;
        }
    }
    modifyScheme.SetWorkingDir(pathPair.first);
    modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpDropView);

    auto& drop = *modifyScheme.MutableDrop();
    drop.SetName(pathPair.second);
}

NThreading::TFuture<TYqlConclusionStatus> SendSchemeRequest(TEvTxUserProxy::TEvProposeTransaction* request,
                                                            TActorSystem* actorSystem,
                                                            bool failOnAlreadyExists) {
    const auto promiseScheme = NThreading::NewPromise<NKqp::TSchemeOpRequestHandler::TResult>();
    IActor* const requestHandler = new TSchemeOpRequestHandler(request, promiseScheme, failOnAlreadyExists);
    actorSystem->Register(requestHandler);
    return promiseScheme.GetFuture().Apply([](const NThreading::TFuture<NKqp::TSchemeOpRequestHandler::TResult>& opResult) {
        if (opResult.HasValue()) {
            if (!opResult.HasException() && opResult.GetValue().Success()) {
                return TYqlConclusionStatus::Success();
            }
            return TYqlConclusionStatus::Fail(opResult.GetValue().Status(), opResult.GetValue().Issues().ToString());
        }
        return TYqlConclusionStatus::Fail("no value in result");
    });
}

NThreading::TFuture<TYqlConclusionStatus> CreateView(const NYql::TCreateObjectSettings& settings,
                                                     TInternalModificationContext& context) {
    auto proposal = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
    proposal->Record.SetDatabaseName(context.GetExternalData().GetDatabase());
    if (context.GetExternalData().GetUserToken()) {
        proposal->Record.SetUserToken(context.GetExternalData().GetUserToken()->GetSerializedToken());
    }
    auto& schemeTx = *proposal->Record.MutableTransaction()->MutableModifyScheme();
    FillCreateViewProposal(schemeTx, settings, context);

    return SendSchemeRequest(proposal.Release(), context.GetExternalData().GetActorSystem(), true);
}

NThreading::TFuture<TYqlConclusionStatus> DropView(const NYql::TDropObjectSettings& settings,
                                                   TInternalModificationContext& context) {
    auto proposal = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
    proposal->Record.SetDatabaseName(context.GetExternalData().GetDatabase());
    if (context.GetExternalData().GetUserToken()) {
        proposal->Record.SetUserToken(context.GetExternalData().GetUserToken()->GetSerializedToken());
    }
    auto& schemeTx = *proposal->Record.MutableTransaction()->MutableModifyScheme();
    FillDropViewProposal(schemeTx, settings, context);

    return SendSchemeRequest(proposal.Release(), context.GetExternalData().GetActorSystem(), false);
}

void PrepareCreateView(NKqpProto::TKqpSchemeOperation& schemeOperation,
                       const NYql::TObjectSettingsImpl& settings,
                       TInternalModificationContext& context) {
    FillCreateViewProposal(*schemeOperation.MutableCreateView(), settings, context);
}

void PrepareDropView(NKqpProto::TKqpSchemeOperation& schemeOperation,
                     const NYql::TObjectSettingsImpl& settings,
                     TInternalModificationContext& context) {
    FillDropViewProposal(*schemeOperation.MutableDropView(), settings, context);
}

}

NThreading::TFuture<TYqlConclusionStatus> TViewManager::DoModify(const NYql::TObjectSettingsImpl& settings,
                                                                 const ui32 nodeId,
                                                                 const NMetadata::IClassBehaviour::TPtr& manager,
                                                                 TInternalModificationContext& context) const {
    Y_UNUSED(nodeId, manager);

    try {
        CheckFeatureFlag(context);
        switch (context.GetActivityType()) {
            case EActivityType::Alter:
            case EActivityType::Upsert:
            case EActivityType::Undefined:
                ythrow TBadArgumentException() << "not implemented";
            case EActivityType::Create:
                return CreateView(settings, context);
            case EActivityType::Drop:
                return DropView(settings, context);
        }
    } catch (...) {
        return NThreading::MakeFuture<TYqlConclusionStatus>(TYqlConclusionStatus::Fail(CurrentExceptionMessage()));
    }
}

TViewManager::TYqlConclusionStatus TViewManager::DoPrepare(NKqpProto::TKqpSchemeOperation& schemeOperation,
                                                           const NYql::TObjectSettingsImpl& settings,
                                                           const NMetadata::IClassBehaviour::TPtr& manager,
                                                           TInternalModificationContext& context) const {
    Y_UNUSED(manager);
    
    try {
        CheckFeatureFlag(context);
        switch (context.GetActivityType()) {
            case EActivityType::Undefined:
                ythrow TBadArgumentException() << "Undefined operation for a VIEW object";
            case EActivityType::Upsert:
                ythrow TBadArgumentException() << "Upsert operation for VIEW objects is not implemented";
            case EActivityType::Alter:
                ythrow TBadArgumentException() << "Alter operation for VIEW objects is not implemented";
            case EActivityType::Create:
                PrepareCreateView(schemeOperation, settings, context);
                break;
            case EActivityType::Drop:
                PrepareDropView(schemeOperation, settings, context);
                break;
        }
    } catch (...) {
        return TYqlConclusionStatus::Fail(CurrentExceptionMessage());
    }
    return TYqlConclusionStatus::Success();
}

NThreading::TFuture<TYqlConclusionStatus> TViewManager::ExecutePrepared(const NKqpProto::TKqpSchemeOperation& schemeOperation,
                                                                        const NMetadata::IClassBehaviour::TPtr& manager,
                                                                        const TExternalModificationContext& context) const {
    Y_UNUSED(manager);

    auto proposal = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
    proposal->Record.SetDatabaseName(context.GetDatabase());
    if (context.GetUserToken()) {
        proposal->Record.SetUserToken(context.GetUserToken()->GetSerializedToken());
    }

    auto& schemeTx = *proposal->Record.MutableTransaction()->MutableModifyScheme();
    switch (schemeOperation.GetOperationCase()) {
        case NKqpProto::TKqpSchemeOperation::kCreateView:
            schemeTx.CopyFrom(schemeOperation.GetCreateView());
            return SendSchemeRequest(proposal.Release(), context.GetActorSystem(), true);
        case NKqpProto::TKqpSchemeOperation::kDropView:
            schemeTx.CopyFrom(schemeOperation.GetDropView());
            return SendSchemeRequest(proposal.Release(), context.GetActorSystem(), false);
        default:
            return NThreading::MakeFuture(TYqlConclusionStatus::Fail(
                    TStringBuilder()
                        << "Execution of prepare operation for a VIEW object, unsupported operation: "
                        << int(schemeOperation.GetOperationCase())
                )
            );
    }
}

}
