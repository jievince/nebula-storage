/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "storage/BaseProcessor.h"

namespace nebula {
namespace storage {

template<typename RESP>
ErrorCode BaseProcessor<RESP>::to(kvstore::ResultCode code) {
    return CommonUtils::to(code);
}

template <typename RESP>
ErrorCode BaseProcessor<RESP>::writeResultTo(WriteResult code, bool isEdge) {
    switch (code) {
    case WriteResult::SUCCEEDED:
        return ErrorCode::SUCCEEDED;
    case WriteResult::UNKNOWN_FIELD:
        if (isEdge) {
            return ErrorCode::E_EDGE_PROP_NOT_FOUND;
        }
        return ErrorCode::E_TAG_PROP_NOT_FOUND;
    case WriteResult::NOT_NULLABLE:
        return ErrorCode::E_NOT_NULLABLE;
    case WriteResult::TYPE_MISMATCH:
        return ErrorCode::E_DATA_TYPE_MISMATCH;
    case WriteResult::FIELD_UNSET:
        return ErrorCode::E_FIELD_UNSET;
    case WriteResult::OUT_OF_RANGE:
        return ErrorCode::E_OUT_OF_RANGE;
    case WriteResult::INCORRECT_VALUE:
        return ErrorCode::E_INVALID_FIELD_VALUE;
    default:
        return ErrorCode::E_UNKNOWN;
    }
}

template <typename RESP>
void BaseProcessor<RESP>::handleAsync(GraphSpaceID spaceId,
                                      PartitionID partId,
                                      kvstore::ResultCode code) {
    VLOG(3) << "partId:" << partId << ", code:" << static_cast<int32_t>(code);

    bool finished = false;
    {
        std::lock_guard<std::mutex> lg(this->lock_);
        handleErrorCode(code, spaceId, partId);
        this->callingNum_--;
        if (this->callingNum_ == 0) {
            finished = true;
        }
    }

    if (finished) {
        this->onFinished();
    }
}

template <typename RESP>
void BaseProcessor<RESP>::handleAsync(GraphSpaceID,
                                      PartitionID partId,
                                      ErrorCode code) {
    VLOG(3) << "partId:" << partId << ", code:" << static_cast<int32_t>(code);

    bool finished = false;
    {
        std::lock_guard<std::mutex> lg(this->lock_);
        pushResultCode(code, partId);
        this->callingNum_--;
        if (this->callingNum_ == 0) {
            finished = true;
        }
    }

    if (finished) {
        this->onFinished();
    }
}

template <typename RESP>
meta::cpp2::ColumnDef BaseProcessor<RESP>::columnDef(std::string name,
                                                     meta::cpp2::PropertyType type) {
    nebula::meta::cpp2::ColumnDef column;
    column.set_name(std::move(name));
    column.set_type(type);
    return column;
}

template <typename RESP>
void BaseProcessor<RESP>::pushResultCode(ErrorCode code, PartitionID partId) {
    if (code != ErrorCode::SUCCEEDED) {
        nebula::PartitionResult thriftRet;
        thriftRet.code = code;
        thriftRet.partId = partId;
        codes_.emplace_back(std::move(thriftRet));
    }
}

template <typename RESP>
void BaseProcessor<RESP>::pushResultCode(ErrorCode code,
                                         PartitionID partId,
                                         HostAddr leader) {
    if (code != ErrorCode::SUCCEEDED) {
        nebula::PartitionResult thriftRet;
        thriftRet.code = code;
        thriftRet.partId = partId;
        thriftRet.leader.reset(new HostAddr(leader));
        codes_.emplace_back(std::move(thriftRet));
    }
}

template <typename RESP>
void BaseProcessor<RESP>::handleErrorCode(kvstore::ResultCode code,
                                          GraphSpaceID spaceId,
                                          PartitionID partId) {
    if (code != kvstore::ResultCode::SUCCEEDED) {
        if (code == kvstore::ResultCode::ERR_LEADER_CHANGED) {
            handleLeaderChanged(spaceId, partId);
        } else {
            pushResultCode(to(code), partId);
        }
    }
}

template <typename RESP>
void BaseProcessor<RESP>::handleLeaderChanged(GraphSpaceID spaceId,
                                              PartitionID partId) {
    auto addrRet = env_->kvstore_->partLeader(spaceId, partId);
    if (ok(addrRet)) {
        auto leader = value(std::move(addrRet));
        this->pushResultCode(ErrorCode::E_LEADER_CHANGED, partId, leader);
    } else {
        LOG(ERROR) << "Fail to get part leader, spaceId: " << spaceId
                   << ", partId: " << partId << ", ResultCode: " << error(addrRet);
        this->pushResultCode(to(error(addrRet)), partId);
    }
}

template <typename RESP>
void BaseProcessor<RESP>::doPut(GraphSpaceID spaceId,
                                PartitionID partId,
                                std::vector<kvstore::KV>&& data) {
    this->env_->kvstore_->asyncMultiPut(
        spaceId, partId, std::move(data), [spaceId, partId, this](kvstore::ResultCode code) {
            handleAsync(spaceId, partId, code);
    });
}

template <typename RESP>
kvstore::ResultCode BaseProcessor<RESP>::doSyncPut(GraphSpaceID spaceId,
                                                   PartitionID partId,
                                                   std::vector<kvstore::KV>&& data) {
    folly::Baton<true, std::atomic> baton;
    kvstore::ResultCode ret = kvstore::ResultCode::SUCCEEDED;
    env_->kvstore_->asyncMultiPut(spaceId,
                                  partId,
                                  std::move(data),
                                  [&ret, &baton] (kvstore::ResultCode code) {
        if (kvstore::ResultCode::SUCCEEDED != code) {
            ret = code;
        }
        baton.post();
    });
    baton.wait();
    return ret;
}

template <typename RESP>
void BaseProcessor<RESP>::doRemove(GraphSpaceID spaceId,
                                   PartitionID partId,
                                   std::vector<std::string>&& keys) {
    this->env_->kvstore_->asyncMultiRemove(
        spaceId, partId, std::move(keys), [spaceId, partId, this](kvstore::ResultCode code) {
        handleAsync(spaceId, partId, code);
    });
}

template <typename RESP>
void BaseProcessor<RESP>::doRemoveRange(GraphSpaceID spaceId,
                                        PartitionID partId,
                                        const std::string& start,
                                        const std::string& end) {
    this->env_->kvstore_->asyncRemoveRange(
        spaceId, partId, start, end, [spaceId, partId, this](kvstore::ResultCode code) {
            handleAsync(spaceId, partId, code);
        });
}

template <typename RESP>
StatusOr<std::string>
BaseProcessor<RESP>::encodeRowVal(const meta::NebulaSchemaProvider* schema,
                                  const std::vector<std::string>& propNames,
                                  const std::vector<Value>& props,
                                  WriteResult& wRet) {
    RowWriterV2 rowWrite(schema);
    // If req.prop_names is not empty, use the property name in req.prop_names
    // Otherwise, use property name in schema
    if (!propNames.empty()) {
        for (size_t i = 0; i < propNames.size(); i++) {
            wRet = rowWrite.setValue(propNames[i], props[i]);
            if (wRet != WriteResult::SUCCEEDED) {
                return Status::Error("Add field faild");
            }
        }
    } else {
        for (size_t i = 0; i < props.size(); i++) {
            wRet = rowWrite.setValue(i, props[i]);
            if (wRet != WriteResult::SUCCEEDED) {
                return Status::Error("Add field faild");
            }
        }
    }

    wRet = rowWrite.finish();
    if (wRet != WriteResult::SUCCEEDED) {
        return Status::Error("Add field faild");
    }

    return std::move(rowWrite).moveEncodedStr();
}

}  // namespace storage
}  // namespace nebula
