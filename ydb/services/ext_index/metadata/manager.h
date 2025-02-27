#pragma once
#include "object.h"
#include <ydb/services/metadata/abstract/common.h>
#include <ydb/services/metadata/manager/generic_manager.h>
#include <ydb/library/accessor/accessor.h>

namespace NKikimr::NMetadata::NCSIndex {

class TManager: public NModifications::TGenericOperationsManager<TObject> {
protected:
    virtual void DoPrepareObjectsBeforeModification(std::vector<TObject>&& patchedObjects,
        NModifications::IAlterPreparationController<TObject>::TPtr controller,
        const TInternalModificationContext& context) const override;

    virtual NModifications::TOperationParsingResult DoBuildPatchFromSettings(
        const NYql::TObjectSettingsImpl& settings, TInternalModificationContext& context) const override;

    virtual IOperationsManager::TYqlConclusionStatus DoPrepare(NKqpProto::TKqpSchemeOperation& schemeOperation, const NYql::TObjectSettingsImpl& settings,
        const NMetadata::IClassBehaviour::TPtr& manager, IOperationsManager::TInternalModificationContext& context) const override;

    virtual NThreading::TFuture<IOperationsManager::TYqlConclusionStatus> ExecutePrepared(const NKqpProto::TKqpSchemeOperation& schemeOperation,
        const NMetadata::IClassBehaviour::TPtr& manager, const IOperationsManager::TExternalModificationContext& context) const override;

public:
};

}
