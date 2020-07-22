/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "LookupBaseProcessor.h"
namespace nebula {
namespace storage {

template<typename REQ, typename RESP>
cpp2::ErrorCode LookupBaseProcessor<REQ, RESP>::requestCheck(const cpp2::LookupIndexRequest& req) {
    spaceId_ = req.get_space_id();
    auto retCode = this->getSpaceVidLen(spaceId_);
    if (retCode != cpp2::ErrorCode::SUCCEEDED) {
        return retCode;
    }

    planContext_ = std::make_unique<PlanContext>(this->env_, spaceId_, this->spaceVidLen_);
    const auto& indices = req.get_indices();
    planContext_->isEdge_ = indices.get_is_edge();
    if (planContext_->isEdge_) {
        planContext_->edgeType_ = indices.get_tag_or_edge_id();
    } else {
        planContext_->tagId_ = indices.get_tag_or_edge_id();
    }

    if (indices.get_contexts().empty()) {
        return cpp2::ErrorCode::E_INVALID_OPERATION;
    }
    contexts_ = indices.get_contexts();

    // setup yield columns.
    if (req.__isset.return_columns) {
        const auto& retcols = *req.get_return_columns();
        yieldCols_ = retcols;
    }

    // setup result set columns.
    if (planContext_->isEdge_) {
        resultDataSet_.colNames.emplace_back("_src");
        resultDataSet_.colNames.emplace_back("_ranking");
        resultDataSet_.colNames.emplace_back("_dst");
    } else {
        resultDataSet_.colNames.emplace_back("_vid");
    }

    for (const auto& col : yieldCols_) {
        resultDataSet_.colNames.emplace_back(col);
    }

    return cpp2::ErrorCode::SUCCEEDED;
}

/**
 * lookup plan should be :
 *              +--------+---------+
 *              |       Plan       |
 *              +--------+---------+
 *                       |
 *              +--------+---------+
 *              |  AggregateNode   |
 *              +--------+---------+
 *                       |
 *            +----------+-----------+
 *            +  IndexOutputNode...  +
 *            +----------+-----------+
**/

template<typename REQ, typename RESP>
StatusOr<StoragePlan<IndexID>> LookupBaseProcessor<REQ, RESP>::buildPlan() {
    StoragePlan<IndexID> plan;
    auto IndexAggr = std::make_unique<AggregateNode<IndexID>>(&resultDataSet_);
    for (const auto& ctx : contexts_) {
        const auto& indexId = ctx.get_index_id();
        auto needFilter = ctx.__isset.filter && !ctx.get_filter().empty();

        // Check whether a data node is required.
        // If a non-indexed column appears in the WHERE clause or YIELD clause,
        // That means need to query the corresponding data.
        bool needData = false;
        auto index = planContext_->isEdge_
            ? this->env_->indexMan_->getEdgeIndex(spaceId_, indexId)
            : this->env_->indexMan_->getTagIndex(spaceId_, indexId);
        if (!index.ok()) {
            return Status::IndexNotFound();
        }

        // check nullable column
        bool hasNullableCol = false;
        int32_t vColNum = 0;
        std::vector<std::pair<std::string, Value::Type>> indexCols;
        for (const auto& col : index.value()->get_fields()) {
            indexCols.emplace_back(col.get_name(), IndexKeyUtils::toValueType(col.get_type()));
            if (IndexKeyUtils::toValueType(col.get_type()) == Value::Type::STRING) {
                vColNum++;
            }
            if (!hasNullableCol && col.get_nullable()) {
                hasNullableCol = true;
            }
        }

        for (const auto& yieldCol : yieldCols_) {
            auto it = std::find_if(index.value()->get_fields().begin(),
                                   index.value()->get_fields().end(),
                                   [&yieldCol] (const auto& columnDef) {
                                       return yieldCol == columnDef.get_name();
                                   });
            if (it == index.value()->get_fields().end()) {
                needData = true;
                break;
            }
        }
        auto colHints = ctx.get_column_hints();

        // TODO (sky) ： Check WHERE clause contains columns that ware not indexed

        std::unique_ptr<IndexOutputNode<IndexID>> out;
        if (!needData && !needFilter) {
            out = buildPlanBasic(ctx, plan, indexCols, vColNum, hasNullableCol);
        } else if (needData && !needFilter) {
            out = buildPlanWithData(ctx, plan);
        } else if (!needData && needFilter) {
            out = buildPlanWithFilter(ctx, plan, vColNum, hasNullableCol, indexCols);
        } else {
            out = buildPlanWithDataAndFilter(ctx, plan);
        }
        if (out == nullptr) {
            return Status::Error("Index scan plan error");
        }
        IndexAggr->addDependency(out.get());
        plan.addNode(std::move(out));
    }
    plan.addNode(std::move(IndexAggr));
    return plan;
}

/**
 *
 *            +----------+-----------+
 *            +   IndexOutputNode    +
 *            +----------+-----------+
 *                       |
 *            +----------+-----------+
 *            +    IndexScanNode     +
 *            +----------+-----------+
 *
 * If this is a simple index scan, Just having IndexScanNode is enough. for example :
 * tag (c1, c2, c3)
 * index on tag (c1, c2, c3)
 * hint : lookup index where c1 == 1 and c2 == 1 and c3 == 1 yield c1,c2,c3
 **/
template<typename REQ, typename RESP>
std::unique_ptr<IndexOutputNode<IndexID>>
LookupBaseProcessor<REQ, RESP>::buildPlanBasic(
    const cpp2::IndexQueryContext& ctx,
    StoragePlan<IndexID>& plan,
    std::vector<std::pair<std::string, Value::Type>>& cols,
    int32_t vColNum,
    bool hasNullableCol) {
    auto indexId = ctx.get_index_id();
    auto colHints = ctx.get_column_hints();
    auto indexScan = std::make_unique<IndexScanNode<IndexID>>(planContext_.get(),
                                                              indexId,
                                                              std::move(colHints));

    auto output = std::make_unique<IndexOutputNode<IndexID>>(&resultDataSet_,
                                                             planContext_.get(),
                                                             indexScan.get(),
                                                             cols,
                                                             vColNum,
                                                             hasNullableCol);
    output->addDependency(indexScan.get());
    plan.addNode(std::move(indexScan));
    return output;
}

/**
 *
 *            +----------+-----------+
 *            +   IndexOutputNode    +
 *            +----------+-----------+
 *                       |
 *      +----------------+-----------------+
 *      + IndexEdgeNode or IndexVertexNode +
 *      +----------------+-----------------+
 *                       |
 *            +----------+-----------+
 *            +    IndexScanNode     +
 *            +----------+-----------+
 *
 * If a non-indexed column appears in the YIELD clause, and no expression filtering is required .
 * for example :
 * tag (c1, c2, c3)
 * index on tag (c1, c2)
 * hint : lookup index where c1 == 1 and c2 == 1 yield c3
 **/
template<typename REQ, typename RESP>
std::unique_ptr<IndexOutputNode<IndexID>>
LookupBaseProcessor<REQ, RESP>::buildPlanWithData(const cpp2::IndexQueryContext& ctx,
                                                  StoragePlan<IndexID>& plan) {
    auto indexId = ctx.get_index_id();
    auto colHints = ctx.get_column_hints();
    auto schema = planContext_->isEdge_
                  ? this->env_->schemaMan_->getEdgeSchema(this->spaceId_, planContext_->edgeType_)
                  : this->env_->schemaMan_->getTagSchema(this->spaceId_, planContext_->tagId_);
    if (!schema) {
        return nullptr;
    }

    auto schemaName = planContext_->isEdge_
                      ? this->env_->schemaMan_->toEdgeName(spaceId_, planContext_->edgeType_)
                      : this->env_->schemaMan_->toTagName(spaceId_, planContext_->tagId_);
    if (!schemaName.ok()) {
        return nullptr;
    }

    auto indexScan = std::make_unique<IndexScanNode<IndexID>>(planContext_.get(),
                                                              indexId,
                                                              std::move(colHints));
    if (planContext_->isEdge_) {
        auto edge = std::make_unique<IndexEdgeNode<IndexID>>(planContext_.get(),
                                                             indexScan.get(),
                                                             std::move(schema),
                                                             std::move(schemaName).value());
        edge->addDependency(indexScan.get());
        auto output = std::make_unique<IndexOutputNode<IndexID>>(&resultDataSet_,
                                                                 planContext_.get(),
                                                                 edge.get());
        output->addDependency(edge.get());
        plan.addNode(std::move(indexScan));
        plan.addNode(std::move(edge));
        return output;
    } else {
        auto vertex = std::make_unique<IndexVertexNode<IndexID>>(planContext_.get(),
                                                                 this->vertexCache_,
                                                                 indexScan.get(),
                                                                 std::move(schema),
                                                                 std::move(schemaName).value());
        vertex->addDependency(indexScan.get());
        auto output = std::make_unique<IndexOutputNode<IndexID>>(&resultDataSet_,
                                                                 planContext_.get(),
                                                                 vertex.get());
        output->addDependency(vertex.get());
        plan.addNode(std::move(indexScan));
        plan.addNode(std::move(vertex));
        return output;
    }
}

/**
 *
 *            +----------+-----------+
 *            +   IndexOutputNode    +
 *            +----------+-----------+
 *                       |
 *            +----------+-----------+
 *            +  IndexFilterNode     +
 *            +----------+-----------+
 *                       |
 *            +----------+-----------+
 *            +    IndexScanNode     +
 *            +----------+-----------+
 *
 * If have not non-indexed column appears in the YIELD clause, and expression filtering is required .
 * for example :
 * tag (c1, c2, c3)
 * index on tag (c1, c2)
 * hint : lookup index where c1 > 1 and c2 > 1
 **/
template<typename REQ, typename RESP>
std::unique_ptr<IndexOutputNode<IndexID>>
LookupBaseProcessor<REQ, RESP>::buildPlanWithFilter(const cpp2::IndexQueryContext& ctx,
    StoragePlan<IndexID>& plan, int32_t vColNum, bool hasNullableCol,
    const std::vector<std::pair<std::string, Value::Type>>& indexCols) {
    auto indexId = ctx.get_index_id();
    auto colHints = ctx.get_column_hints();

    auto indexScan = std::make_unique<IndexScanNode<IndexID>>(planContext_.get(),
                                                              indexId,
                                                              std::move(colHints));

    auto filter = std::make_unique<IndexFilterNode<IndexID>>(indexScan.get(),
                                                             ctx.get_filter(),
                                                             planContext_->vIdLen_,
                                                             vColNum,
                                                             hasNullableCol,
                                                             planContext_->isEdge_,
                                                             indexCols);
    filter->addDependency(indexScan.get());
    auto output = std::make_unique<IndexOutputNode<IndexID>>(&resultDataSet_,
                                                             planContext_.get(),
                                                             filter.get(), true);
    output->addDependency(filter.get());
    plan.addNode(std::move(indexScan));
    plan.addNode(std::move(filter));
    return output;
}


/**
 *
 *            +----------+-----------+
 *            +   IndexOutputNode    +
 *            +----------+-----------+
 *                       |
 *            +----------+-----------+
 *            +   IndexFilterNode    +
 *            +----------+-----------+
 *                       |
 *      +----------------+-----------------+
 *      + IndexEdgeNode or IndexVertexNode +
 *      +----------------+-----------------+
 *                       |
 *            +----------+-----------+
 *            +    IndexScanNode     +
 *            +----------+-----------+
 *
 * If a non-indexed column appears in the WHERE clause or YIELD clause,
 * and expression filtering is required .
 * for example :
 * tag (c1, c2, c3)
 * index on tag (c1, c2)
 * hint : lookup index where c1 == 1 and c2 == 1 and c3 > 1 yield c3
 *        lookup index where c1 == 1 and c2 == 1 and c3 > 1
 *        lookup index where c1 == 1 and c3 == 1
 **/
template<typename REQ, typename RESP>
std::unique_ptr<IndexOutputNode<IndexID>>
LookupBaseProcessor<REQ, RESP>::buildPlanWithDataAndFilter(const cpp2::IndexQueryContext& ctx,
                                                           StoragePlan<IndexID>& plan) {
    auto indexId = ctx.get_index_id();
    auto colHints = ctx.get_column_hints();
    auto schema = planContext_->isEdge_
                  ? this->env_->schemaMan_->getEdgeSchema(spaceId_, planContext_->edgeType_)
                  : this->env_->schemaMan_->getTagSchema(spaceId_, planContext_->tagId_);
    if (!schema) {
        return nullptr;
    }

    auto schemaName = planContext_->isEdge_
                      ? this->env_->schemaMan_->toEdgeName(spaceId_, planContext_->edgeType_)
                      : this->env_->schemaMan_->toTagName(spaceId_, planContext_->tagId_);
    if (!schemaName.ok()) {
        return nullptr;
    }
    auto indexScan = std::make_unique<IndexScanNode<IndexID>>(planContext_.get(),
                                                              indexId,
                                                              std::move(colHints));
    if (planContext_->isEdge_) {
        auto edge = std::make_unique<IndexEdgeNode<IndexID>>(planContext_.get(),
                                                             indexScan.get(),
                                                             std::move(schema),
                                                             std::move(schemaName).value());
        edge->addDependency(indexScan.get());
        auto filter = std::make_unique<IndexFilterNode<IndexID>>(planContext_->vIdLen_,
                                                                 edge.get(),
                                                                 ctx.get_filter());
        filter->addDependency(edge.get());

        auto output = std::make_unique<IndexOutputNode<IndexID>>(&resultDataSet_,
                                                                 planContext_.get(),
                                                                 filter.get());
        output->addDependency(filter.get());
        plan.addNode(std::move(indexScan));
        plan.addNode(std::move(edge));
        plan.addNode(std::move(filter));
        return output;
    } else {
        auto vertex = std::make_unique<IndexVertexNode<IndexID>>(planContext_.get(),
                                                                 this->vertexCache_,
                                                                 indexScan.get(),
                                                                 std::move(schema),
                                                                 std::move(schemaName).value());
        vertex->addDependency(indexScan.get());
        auto filter = std::make_unique<IndexFilterNode<IndexID>>(planContext_->vIdLen_,
                                                                 vertex.get(),
                                                                 ctx.get_filter());
        filter->addDependency(vertex.get());

        auto output = std::make_unique<IndexOutputNode<IndexID>>(&resultDataSet_,
                                                                 planContext_.get(),
                                                                 filter.get());
        output->addDependency(filter.get());
        plan.addNode(std::move(indexScan));
        plan.addNode(std::move(vertex));
        plan.addNode(std::move(filter));
        return output;
    }
}

}  // namespace storage
}  // namespace nebula
