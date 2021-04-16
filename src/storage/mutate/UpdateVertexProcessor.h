/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#ifndef STORAGE_MUTATE_UPDATEVERTEXROCESSOR_H_
#define STORAGE_MUTATE_UPDATEVERTEXROCESSOR_H_

#include "storage/query/QueryBaseProcessor.h"
#include "storage/exec/StoragePlan.h"
#include "common/expression/Expression.h"
#include "storage/context/StorageExpressionContext.h"

namespace nebula {
namespace storage {

extern ProcessorCounters kUpdateVertexCounters;

class UpdateVertexProcessor
    : public QueryBaseProcessor<cpp2::UpdateVertexRequest, cpp2::UpdateResponse> {
public:
    static UpdateVertexProcessor* instance(
            StorageEnv* env,
            const ProcessorCounters* counters = &kUpdateVertexCounters,
            folly::Executor* executor = nullptr,
            VertexCache* cache = nullptr) {
        return new UpdateVertexProcessor(env, counters, executor, cache);
    }

    void process(const cpp2::UpdateVertexRequest& req) override;

    void doProcess(const cpp2::UpdateVertexRequest& req);

private:
    UpdateVertexProcessor(StorageEnv* env,
                          const ProcessorCounters* counters,
                          folly::Executor* executor,
                          VertexCache* cache)
        : QueryBaseProcessor<cpp2::UpdateVertexRequest, cpp2::UpdateResponse>(env,
                                                                              counters,
                                                                              executor,
                                                                              cache) {}

    ErrorCode checkAndBuildContexts(const cpp2::UpdateVertexRequest& req) override;

    StoragePlan<VertexID> buildPlan(nebula::DataSet* result);

    // Get the schema of all versions of all tags in the spaceId
    ErrorCode buildTagSchema();

    // Build TagContext by parsing return props expressions,
    // filter expression, update props expression
    ErrorCode buildTagContext(const cpp2::UpdateVertexRequest& req);

    void onProcessFinished() override;

    std::vector<Expression*> getReturnPropsExp() {
        std::vector<Expression*> result;
        result.resize(returnPropsExp_.size());
        auto get = [] (auto &ptr) {return ptr.get(); };
        std::transform(returnPropsExp_.begin(), returnPropsExp_.end(), result.begin(), get);
        return result;
    }

private:
    bool                                                                 insertable_{false};

    // update tagId
    TagID                                                                tagId_;

    std::vector<std::shared_ptr<nebula::meta::cpp2::IndexItem>>          indexes_;

    std::unique_ptr<StorageExpressionContext>                            expCtx_;

    // update <prop name, new value expression>
    std::vector<storage::cpp2::UpdatedProp>                              updatedProps_;

    // return props expression
    std::vector<std::unique_ptr<Expression>>                             returnPropsExp_;

    // condition expression
    std::unique_ptr<Expression>                                          filterExp_;

    // updatedProps_ dependent props in value expression
    std::vector<std::pair<std::string, std::unordered_set<std::string>>> depPropMap_;
};

}  // namespace storage
}  // namespace nebula
#endif  // STORAGE_MUTATE_UPDATEVERTEXROCESSOR_H_
