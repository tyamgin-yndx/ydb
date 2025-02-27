#pragma once
#include "request_features.h"
#include <ydb/library/accessor/accessor.h>
#include <ydb/library/yql/core/expr_nodes/yql_expr_nodes.h>

#include <util/generic/string.h>
#include <util/generic/typetraits.h>
#include <map>
#include <optional>

namespace NYql {

namespace NObjectOptionsParsing {
Y_HAS_MEMBER(ExistingOk); // for create
Y_HAS_MEMBER(MissingOk); // for drop
} // namespace NObjectOptionsParsing

class TObjectSettingsImpl {
public:
    using TFeaturesExtractor = NYql::TFeaturesExtractor;
private:
    using TFeatures = THashMap<TString, TString>;
    YDB_READONLY_DEF(TString, TypeId);
    YDB_READONLY_DEF(TString, ObjectId);
    YDB_READONLY_DEF(bool, ExistingOk); // for create
    YDB_READONLY_DEF(bool, MissingOk); // for drop
    TFeatures Features;
    std::shared_ptr<TFeaturesExtractor> FeaturesExtractor;
public:
    TObjectSettingsImpl() = default;

    TObjectSettingsImpl(const TString& typeId, const TString& objectId, const TFeatures& features)
        : TypeId(typeId)
        , ObjectId(objectId)
        , Features(features)
        , FeaturesExtractor(std::make_shared<TFeaturesExtractor>(Features))
        {}

    TFeaturesExtractor& GetFeaturesExtractor() const {
        Y_ABORT_UNLESS(!!FeaturesExtractor);
        return *FeaturesExtractor;
    }

    template <class TKiObject>
    bool DeserializeFromKi(const TKiObject& data) {
        ObjectId = data.ObjectId();
        TypeId = data.TypeId();
        if constexpr (NObjectOptionsParsing::THasExistingOk<TKiObject>::value) {
            ExistingOk = (data.ExistingOk().Value() == "1");
        }
        if constexpr (NObjectOptionsParsing::THasMissingOk<TKiObject>::value) {
            MissingOk = (data.MissingOk().Value() == "1");
        }
        for (auto&& i : data.Features()) {
            if (auto maybeAtom = i.template Maybe<NYql::NNodes::TCoAtom>()) {
                Features.emplace(maybeAtom.Cast().StringValue(), "");
            } else if (auto maybeTuple = i.template Maybe<NNodes::TCoNameValueTuple>()) {
                auto tuple = maybeTuple.Cast();
                if (auto tupleValue = tuple.Value().template Maybe<NNodes::TCoAtom>()) {
                    Features.emplace(tuple.Name().Value(), tupleValue.Cast().Value());
                }
            }
        }
        FeaturesExtractor = std::make_shared<TFeaturesExtractor>(Features);
        return true;
    }
};

using TCreateObjectSettings = TObjectSettingsImpl;
using TUpsertObjectSettings = TObjectSettingsImpl;
using TAlterObjectSettings = TObjectSettingsImpl;
using TDropObjectSettings = TObjectSettingsImpl;

}
