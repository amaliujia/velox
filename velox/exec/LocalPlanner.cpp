/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/exec/LocalPlanner.h"
#include "velox/exec/Aggregate.h"
#include "velox/exec/CallbackSink.h"
#include "velox/exec/Exchange.h"
#include "velox/exec/FilterProject.h"
#include "velox/exec/HashAggregation.h"
#include "velox/exec/HashBuild.h"
#include "velox/exec/HashProbe.h"
#include "velox/exec/Limit.h"
#include "velox/exec/Merge.h"
#include "velox/exec/OrderBy.h"
#include "velox/exec/PartitionedOutput.h"
#include "velox/exec/TableScan.h"
#include "velox/exec/TableWriter.h"
#include "velox/exec/TopN.h"
#include "velox/exec/Values.h"

namespace facebook::velox::exec {

namespace detail {

/// Returns true if source nodes must run in a separate pipeline.
bool mustStartNewPipeline(
    std::shared_ptr<const core::PlanNode> planNode,
    int sourceId) {
  if (auto localMerge =
          std::dynamic_pointer_cast<const core::LocalMergeNode>(planNode)) {
    // LocalMerge's source runs on its own pipeline.
    return true;
  }

  if (std::dynamic_pointer_cast<const core::LocalPartitionNode>(planNode)) {
    return true;
  }

  // Non-first sources always run in their own pipeline.
  return sourceId != 0;
}

OperatorSupplier makeConsumerSupplier(
    std::function<BlockingReason(RowVectorPtr, ContinueFuture*)> consumer) {
  if (consumer) {
    return [consumer](int32_t operatorId, DriverCtx* ctx) {
      return std::make_unique<CallbackSink>(operatorId, ctx, consumer);
    };
  }
  return nullptr;
}

OperatorSupplier makeConsumerSupplier(
    const std::shared_ptr<const core::PlanNode>& planNode) {
  if (auto localMerge =
          std::dynamic_pointer_cast<const core::LocalMergeNode>(planNode)) {
    return [](int32_t operatorId, DriverCtx* ctx) {
      auto consumer = [ctx](RowVectorPtr input, ContinueFuture* future) {
        auto mergeSource = ctx->task->getLocalMergeSource(ctx->driverId);
        return mergeSource->enqueue(input, future);
      };
      return std::make_unique<CallbackSink>(operatorId, ctx, consumer);
    };
  }

  if (auto localPartitionNode =
          std::dynamic_pointer_cast<const core::LocalPartitionNode>(planNode)) {
    return [localPartitionNode](int32_t operatorId, DriverCtx* ctx) {
      return std::make_unique<LocalPartition>(
          operatorId, ctx, localPartitionNode);
    };
  }

  if (auto join =
          std::dynamic_pointer_cast<const core::HashJoinNode>(planNode)) {
    return [join](int32_t operatorId, DriverCtx* ctx) {
      return std::make_unique<HashBuild>(operatorId, ctx, join);
    };
  }
  return nullptr;
}

void plan(
    const std::shared_ptr<const core::PlanNode>& planNode,
    std::vector<std::shared_ptr<const core::PlanNode>>* currentPlanNodes,
    OperatorSupplier consumerSupplier,
    std::vector<std::unique_ptr<DriverFactory>>* driverFactories) {
  if (!currentPlanNodes) {
    driverFactories->push_back(std::make_unique<DriverFactory>());
    currentPlanNodes = &driverFactories->back()->planNodes;
    driverFactories->back()->consumerSupplier = consumerSupplier;
  }

  auto sources = planNode->sources();

  for (int32_t i = 0; i < sources.size(); ++i) {
    plan(
        sources[i],
        mustStartNewPipeline(planNode, i) ? nullptr : currentPlanNodes,
        makeConsumerSupplier(planNode),
        driverFactories);
  }

  currentPlanNodes->push_back(planNode);
}

uint32_t maxDrivers(
    const std::vector<std::shared_ptr<const core::PlanNode>>& planNodes) {
  for (auto& node : planNodes) {
    if (auto aggregation =
            std::dynamic_pointer_cast<const core::AggregationNode>(node)) {
      if (aggregation->step() == core::AggregationNode::Step::kFinal ||
          aggregation->step() == core::AggregationNode::Step::kSingle) {
        // final aggregations must run single-threaded
        return 1;
      }
    }
    if (auto topN = std::dynamic_pointer_cast<const core::TopNNode>(node)) {
      if (!topN->isPartial()) {
        // final topN must run single-threaded
        return 1;
      }
    }
    if (auto values = std::dynamic_pointer_cast<const core::ValuesNode>(node)) {
      // values node must run single-threaded, unless in test context
      if (!values->isParallelizable()) {
        return 1;
      }
    }
    if (auto limit = std::dynamic_pointer_cast<const core::LimitNode>(node)) {
      // final limit must run single-threaded
      if (!limit->isPartial()) {
        return 1;
      }
    }
    if (auto orderBy =
            std::dynamic_pointer_cast<const core::OrderByNode>(node)) {
      // final orderby must run single-threaded
      if (!orderBy->isPartial()) {
        return 1;
      }
    }
    if (auto localMerge =
            std::dynamic_pointer_cast<const core::LocalMergeNode>(node)) {
      // Local merge must run single-threaded.
      return 1;
    }

    if (auto mergeExchange =
            std::dynamic_pointer_cast<const core::MergeExchangeNode>(node)) {
      // MergeExchange must run single-threaded.
      return 1;
    }

    if (auto tableWrite =
            std::dynamic_pointer_cast<const core::TableWriteNode>(node)) {
      // Multi-threaded table write is not supported yet.
      return 1;
    }
  }
  return std::numeric_limits<uint32_t>::max();
}
} // namespace detail

// static
void LocalPlanner::plan(
    const std::shared_ptr<const core::PlanNode>& planNode,
    std::function<BlockingReason(RowVectorPtr, ContinueFuture*)> consumer,
    std::vector<std::unique_ptr<DriverFactory>>* driverFactories) {
  detail::plan(
      planNode,
      nullptr,
      detail::makeConsumerSupplier(consumer),
      driverFactories);

  for (auto& factory : *driverFactories) {
    factory->maxDrivers = detail::maxDrivers(factory->planNodes);
  }
}

std::shared_ptr<Driver> DriverFactory::createDriver(
    std::unique_ptr<DriverCtx> ctx,
    std::shared_ptr<ExchangeClient> exchangeClient,
    std::function<int(int pipelineId)> numDrivers) {
  std::vector<std::unique_ptr<Operator>> operators;
  operators.reserve(planNodes.size());
  for (int32_t i = 0; i < planNodes.size(); i++) {
    // Id of the Operator being made. This is not the same as 'i'
    // because some PlanNodes may get fused.
    auto id = operators.size();
    auto planNode = planNodes[i];
    if (auto filterNode =
            std::dynamic_pointer_cast<const core::FilterNode>(planNode)) {
      if (i < planNodes.size() - 1) {
        auto next = planNodes[i + 1];
        if (auto projectNode =
                std::dynamic_pointer_cast<const core::ProjectNode>(next)) {
          operators.push_back(std::make_unique<FilterProject>(
              id, ctx.get(), filterNode, projectNode));
          i++;
          continue;
        }
      }
      operators.push_back(
          std::make_unique<FilterProject>(id, ctx.get(), filterNode, nullptr));
    } else if (
        auto projectNode =
            std::dynamic_pointer_cast<const core::ProjectNode>(planNode)) {
      operators.push_back(
          std::make_unique<FilterProject>(id, ctx.get(), nullptr, projectNode));
    } else if (
        auto valuesNode =
            std::dynamic_pointer_cast<const core::ValuesNode>(planNode)) {
      operators.push_back(std::make_unique<Values>(id, ctx.get(), valuesNode));
    } else if (
        auto tableScanNode =
            std::dynamic_pointer_cast<const core::TableScanNode>(planNode)) {
      operators.push_back(
          std::make_unique<TableScan>(id, ctx.get(), tableScanNode));
    } else if (
        auto tableWriteNode =
            std::dynamic_pointer_cast<const core::TableWriteNode>(planNode)) {
      operators.push_back(
          std::make_unique<TableWriter>(id, ctx.get(), tableWriteNode));
    } else if (
        auto mergeExchangeNode =
            std::dynamic_pointer_cast<const core::MergeExchangeNode>(
                planNode)) {
      operators.push_back(
          std::make_unique<MergeExchange>(i, ctx.get(), mergeExchangeNode));
    } else if (
        auto exchangeNode =
            std::dynamic_pointer_cast<const core::ExchangeNode>(planNode)) {
      operators.push_back(std::make_unique<Exchange>(
          id, ctx.get(), exchangeNode, exchangeClient));
    } else if (
        auto partitionedOutputNode =
            std::dynamic_pointer_cast<const core::PartitionedOutputNode>(
                planNode)) {
      operators.push_back(std::make_unique<PartitionedOutput>(
          id, ctx.get(), partitionedOutputNode));
    } else if (
        auto joinNode =
            std::dynamic_pointer_cast<const core::HashJoinNode>(planNode)) {
      operators.push_back(std::make_unique<HashProbe>(id, ctx.get(), joinNode));
    } else if (
        auto aggregationNode =
            std::dynamic_pointer_cast<const core::AggregationNode>(planNode)) {
      operators.push_back(
          std::make_unique<HashAggregation>(id, ctx.get(), aggregationNode));
    } else if (
        auto topNNode =
            std::dynamic_pointer_cast<const core::TopNNode>(planNode)) {
      operators.push_back(std::make_unique<TopN>(id, ctx.get(), topNNode));
    } else if (
        auto limitNode =
            std::dynamic_pointer_cast<const core::LimitNode>(planNode)) {
      operators.push_back(std::make_unique<Limit>(id, ctx.get(), limitNode));
    } else if (
        auto orderByNode =
            std::dynamic_pointer_cast<const core::OrderByNode>(planNode)) {
      operators.push_back(
          std::make_unique<OrderBy>(id, ctx.get(), orderByNode));
    } else if (
        auto localMerge =
            std::dynamic_pointer_cast<const core::LocalMergeNode>(planNode)) {
      auto numSources = numDrivers(ctx->pipelineId + 1);
      auto localMergeOp =
          std::make_unique<LocalMerge>(id, ctx.get(), numSources, localMerge);
      ctx->task->createLocalMergeSources(
          numSources, localMergeOp->outputType(), localMergeOp->mappedMemory());
      operators.push_back(std::move(localMergeOp));
    } else if (
        auto localPartitionNode =
            std::dynamic_pointer_cast<const core::LocalPartitionNode>(
                planNode)) {
      operators.push_back(std::make_unique<LocalExchangeSourceOperator>(
          id,
          ctx.get(),
          localPartitionNode->outputType(),
          localPartitionNode->id(),
          ctx->driverId));
    } else {
      VELOX_FAIL("Unsupported plan node");
    }
  }
  if (consumerSupplier) {
    operators.push_back(consumerSupplier(operators.size(), ctx.get()));
  }

  return std::make_shared<Driver>(std::move(ctx), std::move(operators));
}
} // namespace facebook::velox::exec