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

#pragma once

#include <gtest/gtest_prod.h>

#include "velox/dwio/dwrf/common/Compression.h"
#include "velox/dwio/dwrf/common/wrap/dwrf-proto-wrapper.h"
#include "velox/dwio/dwrf/writer/IndexBuilder.h"
#include "velox/dwio/dwrf/writer/IntegerDictionaryEncoder.h"
#include "velox/dwio/dwrf/writer/RatioTracker.h"

namespace facebook::velox::dwrf {

enum class MemoryUsageCategory { DICTIONARY, OUTPUT_STREAM, GENERAL };

class WriterContext : public CompressionBufferPool {
 public:
  WriterContext(
      const std::shared_ptr<const Config>& config,
      std::unique_ptr<memory::ScopedMemoryPool> scopedPool,
      const dwio::common::MetricsLogPtr& metricLogger =
          dwio::common::MetricsLog::voidLog(),
      std::unique_ptr<encryption::EncryptionHandler> handler = nullptr)
      : config_{config},
        scopedPool_{std::move(scopedPool)},
        pool_{scopedPool_->getPool()},
        dictionaryPool_{pool_.addChild(".dictionary")},
        outputStreamPool_{pool_.addChild(".compression")},
        generalPool_{pool_.addChild(".general")},
        handler_{std::move(handler)},
        compression{getConfig(Config::COMPRESSION)},
        compressionBlockSize{getConfig(Config::COMPRESSION_BLOCK_SIZE)},
        isIndexEnabled{getConfig(Config::CREATE_INDEX)},
        indexStride{getConfig(Config::ROW_INDEX_STRIDE)},
        shareFlatMapDictionaries{getConfig(Config::MAP_FLAT_DICT_SHARE)},
        stripeSizeFlushThreshold{getConfig(Config::STRIPE_SIZE)},
        dictionarySizeFlushThreshold{getConfig(Config::MAX_DICTIONARY_SIZE)},
        isStreamSizeAboveThresholdCheckEnabled{
            getConfig(Config::STREAM_SIZE_ABOVE_THRESHOLD_CHECK_ENABLED)},
        // Currently logging with no metadata. Might consider populating
        // metadata with dwio::common::request::AccessDescriptor upstream and
        // pass down the metric log.
        metricLogger{metricLogger} {
    bool forceLowMemoryMode{getConfig(Config::FORCE_LOW_MEMORY_MODE)};
    bool disableLowMemoryMode{getConfig(Config::DISABLE_LOW_MEMORY_MODE)};
    DWIO_ENSURE(!(forceLowMemoryMode && disableLowMemoryMode));
    checkLowMemoryMode_ = !forceLowMemoryMode && !disableLowMemoryMode;
    if (forceLowMemoryMode) {
      setLowMemoryMode();
    }
    if (!handler_) {
      handler_ = std::make_unique<encryption::EncryptionHandler>();
    }
    validateConfigs();
    compressionBuffer_ = std::make_unique<dwio::common::DataBuffer<char>>(
        generalPool_, compressionBlockSize + PAGE_HEADER_SIZE);
  }

  bool hasStream(const StreamIdentifier& stream) const {
    return streams_.find(stream) != streams_.end();
  }

  const DataBufferHolder& getStream(const StreamIdentifier& stream) const {
    return streams_.at(stream);
  }

  void addBuffer(const StreamIdentifier& stream, folly::StringPiece buffer) {
    streams_.at(stream).take(buffer);
  }

  size_t getStreamCount() const {
    return streams_.size();
  }

  // Stream content is not compressed until flush (stripe flush or full buffer),
  // so accounting for the memory usage can be inflated even aside from the
  // capacity vs actual usage problem. However, this is ok as an upperbound for
  // flush policy evaluation and would be more accurate after flush.
  std::unique_ptr<BufferedOutputStream> newStream(
      const StreamIdentifier& stream) {
    DWIO_ENSURE(
        !hasStream(stream), "Stream already exists ", stream.toString());
    streams_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(stream),
        std::forward_as_tuple(
            getMemoryPool(MemoryUsageCategory::OUTPUT_STREAM),
            compressionBlockSize,
            getConfig(Config::COMPRESSION_BLOCK_SIZE_MIN),
            getConfig(Config::COMPRESSION_BLOCK_SIZE_EXTEND_RATIO)));
    auto& holder = streams_.at(stream);
    auto encrypter = handler_->isEncrypted(stream.node)
        ? std::addressof(handler_->getEncryptionProvider(stream.node))
        : nullptr;
    return newStream(compression, holder, encrypter);
  }

  std::unique_ptr<DataBufferHolder> newDataBufferHolder(
      dwio::common::DataSink* sink = nullptr) {
    return std::make_unique<DataBufferHolder>(
        getMemoryPool(MemoryUsageCategory::OUTPUT_STREAM),
        compressionBlockSize,
        getConfig(Config::COMPRESSION_BLOCK_SIZE_MIN),
        getConfig(Config::COMPRESSION_BLOCK_SIZE_EXTEND_RATIO),
        sink);
  }

  std::unique_ptr<BufferedOutputStream> newStream(
      CompressionKind kind,
      DataBufferHolder& holder,
      const dwio::common::encryption::Encrypter* encrypter = nullptr) {
    return createCompressor(kind, *this, holder, *config_, encrypter);
  }

  template <typename T>
  IntegerDictionaryEncoder<T>& getIntDictionaryEncoder(
      const EncodingKey& ek,
      velox::memory::MemoryPool& dictionaryPool,
      velox::memory::MemoryPool& generalPool) {
    auto result = dictEncoders_.find(ek);
    if (result == dictEncoders_.end()) {
      auto emplaceResult = dictEncoders_.emplace(
          ek,
          std::make_unique<IntegerDictionaryEncoder<T>>(
              dictionaryPool,
              generalPool,
              getConfig(Config::DICTIONARY_SORT_KEYS),
              IntEncoder</* isSigned = */ true>::createDirect(
                  newStream(
                      {ek.node,
                       ek.sequence,
                       0,
                       StreamKind::StreamKind_DICTIONARY_DATA}),
                  getConfig(Config::USE_VINTS),
                  sizeof(T))));
      result = emplaceResult.first;
    } else {
      result->second->bumpRefCount();
    }
    return static_cast<IntegerDictionaryEncoder<T>&>(*result->second);
  }

  std::unique_ptr<IndexBuilder> newIndexBuilder(
      std::unique_ptr<BufferedOutputStream> stream) const {
    return indexBuilderFactory_
        ? indexBuilderFactory_(std::move(stream))
        : std::make_unique<IndexBuilder>(std::move(stream));
  }

  void suppressStream(const StreamIdentifier& stream) {
    DWIO_ENSURE(hasStream(stream));
    auto& collector = streams_.at(stream);
    collector.suppress();
  }

  bool isStreamPaged(uint32_t nodeId) const {
    return (compression != CompressionKind::CompressionKind_NONE) ||
        handler_->isEncrypted(nodeId);
  }

  void nextStripe() {
    fileRowCount += stripeRowCount;
    stripeRowCount = 0;
    indexRowCount = 0;
    fileRawSize += stripeRawSize;
    stripeRawSize = 0;
    stripeIndex += 1;

    for (auto& pair : streams_) {
      pair.second.reset();
    }
  }

  void incRowCount(uint64_t count) {
    stripeRowCount += count;
    if (isIndexEnabled) {
      indexRowCount += count;
    }
  }

  void incRawSize(uint64_t size) {
    stripeRawSize += size;
  }

  memory::MemoryPool& getMemoryPool(const MemoryUsageCategory& category) {
    switch (category) {
      case MemoryUsageCategory::DICTIONARY:
        return dictionaryPool_;
      case MemoryUsageCategory::OUTPUT_STREAM:
        return outputStreamPool_;
      case MemoryUsageCategory::GENERAL:
        return generalPool_;
    }
    VELOX_FAIL("Unreachable");
  }

  const memory::MemoryPool& getMemoryUsage(
      const MemoryUsageCategory& category) const {
    switch (category) {
      case MemoryUsageCategory::DICTIONARY:
        return dictionaryPool_;
      case MemoryUsageCategory::OUTPUT_STREAM:
        return outputStreamPool_;
      case MemoryUsageCategory::GENERAL:
        return generalPool_;
    }
    VELOX_FAIL("Unreachable");
  }

  const memory::MemoryPool& getWriterMemoryUsage() const {
    return pool_;
  }

  int64_t getMemoryBudget() const {
    return pool_.getCap();
  }

  const encryption::EncryptionHandler& getEncryptionHandler() const {
    return *handler_;
  }

  template <typename T>
  T getConfig(const Config::Entry<T>& config) const {
    return config_->get(config);
  }

  const Config& getConfigs() const {
    return *config_;
  }

  void iterateUnSuppressedStreams(
      std::function<void(std::pair<const StreamIdentifier, DataBufferHolder>&)>
          callback) {
    for (auto& pair : streams_) {
      if (!pair.second.isSuppressed()) {
        callback(pair);
      }
    }
  }

  // Used by FlatMapColumnWriter to remove previously registered
  // dictionary encoders. This logic exists due to how FlatMapColumnWriter
  // cleans up its value writer streams upon reset().
  void removeAllIntDictionaryEncodersOnNode(
      std::function<bool(uint32_t)> predicate) {
    auto iter = dictEncoders_.begin();
    while (iter != dictEncoders_.end()) {
      if (predicate(iter->first.node)) {
        iter = dictEncoders_.erase(iter);
      } else {
        ++iter;
      }
    }
  }

  virtual void removeStreams(
      std::function<bool(const StreamIdentifier&)> predicate) {
    auto it = streams_.begin();
    while (it != streams_.end()) {
      if (predicate(it->first)) {
        it = streams_.erase(it);
        continue;
      }
      ++it;
    }
  }

  std::unique_ptr<dwio::common::DataBuffer<char>> getBuffer(
      uint64_t size) override {
    DWIO_ENSURE_NOT_NULL(compressionBuffer_);
    DWIO_ENSURE_GE(compressionBuffer_->size(), size);
    return std::move(compressionBuffer_);
  }

  void returnBuffer(
      std::unique_ptr<dwio::common::DataBuffer<char>> buffer) override {
    DWIO_ENSURE_NOT_NULL(buffer);
    DWIO_ENSURE(!compressionBuffer_);
    compressionBuffer_ = std::move(buffer);
  }

  void incrementNodeSize(uint32_t node, uint64_t size) {
    nodeSize[node] += size;
  }

  uint64_t getNodeSize(uint32_t node) {
    if (nodeSize.count(node) > 0) {
      return nodeSize[node];
    }
    return 0;
  }

  void recordCompressionRatio(uint64_t compressedSize) {
    compressionRatioTracker_.takeSample(stripeRawSize, compressedSize);
  }

  void recordFlushOverhead(uint64_t flushOverhead) {
    flushOverheadRatioTracker_.takeSample(
        stripeRawSize +
            getMemoryUsage(MemoryUsageCategory::DICTIONARY).getCurrentBytes(),
        flushOverhead);
  }

  void recordAverageRowSize() {
    rowSizeTracker_.takeSample(stripeRowCount, stripeRawSize);
  }

  float getCompressionRatio() const {
    return compressionRatioTracker_.getEstimatedRatio();
  }

  float getFlushOverheadRatio() const {
    return flushOverheadRatioTracker_.getEstimatedRatio();
  }

  float getAverageRowSize() const {
    return rowSizeTracker_.getEstimatedRatio();
  }

  // This is parity with bbio. Doesn't seem like we do anything special when
  // estimated compression ratio is larger than 1.0f. In fact, given how
  // compression works, we should cap the ratio used for estimate at 1.0f.
  // Estimates prior to first flush can be quite inaccurate depending on
  // encoding, so we rely on a tuned compression ratio initial guess unless we
  // want to produce estimates at ColumnWriter level.
  // TODO: expose config for initial guess?
  int64_t getEstimatedStripeSize(size_t dataRawSize) const {
    return ceil(compressionRatioTracker_.getEstimatedRatio() * dataRawSize);
  }

  // The additional memory usage of writers during flush typically comes from
  // flushing remaining data to output buffer, or all of it in the case of
  // dictionary encoding. In either case, the maximal memory consumption is
  // O(k * raw data size). The actual coefficient k can differ
  // from encoding to encoding, and thus should be schema aware.
  size_t getEstimatedFlushOverhead(size_t dataRawSize) const {
    return ceil(flushOverheadRatioTracker_.getEstimatedRatio() * dataRawSize);
  }

  // At this point we won't have data to estimate flush overhead.
  int64_t getEstimatedEncodingSwitchOverhead() const {
    return stripeRawSize;
  }

  bool checkLowMemoryMode() const {
    return checkLowMemoryMode_;
  }

  void setLowMemoryMode() {
    lowMemoryMode_ = true;
  }

  bool isLowMemoryMode() const {
    return lowMemoryMode_;
  }

 private:
  void validateConfigs() const;

  std::shared_ptr<const Config> config_;
  std::unique_ptr<memory::ScopedMemoryPool> scopedPool_;
  memory::MemoryPool& pool_;
  memory::MemoryPool& dictionaryPool_;
  memory::MemoryPool& outputStreamPool_;
  memory::MemoryPool& generalPool_;
  // Map needs referential stability because reference to map value is stored by
  // another class.
  folly::F14NodeMap<StreamIdentifier, DataBufferHolder, StreamIdentifierHash>
      streams_;
  folly::F14FastMap<
      EncodingKey,
      std::unique_ptr<AbstractIntegerDictionaryEncoder>,
      EncodingKeyHash>
      dictEncoders_;
  std::function<std::unique_ptr<IndexBuilder>(
      std::unique_ptr<BufferedOutputStream>)>
      indexBuilderFactory_;
  std::unique_ptr<dwio::common::DataBuffer<char>> compressionBuffer_;

  std::unique_ptr<encryption::EncryptionHandler> handler_;
  folly::F14FastMap<uint32_t, uint64_t> nodeSize;
  CompressionRatioTracker compressionRatioTracker_;
  FlushOverheadRatioTracker flushOverheadRatioTracker_;
  // This might not be the best idea if client actually sends batches
  // of similar sizes. We will find out through production traffic.
  AverageRowSizeTracker rowSizeTracker_;
  bool checkLowMemoryMode_;
  bool lowMemoryMode_{false};

 public:
  // stats
  uint32_t stripeIndex = 0;

  uint64_t fileRowCount = 0;
  uint64_t stripeRowCount = 0;
  uint32_t indexRowCount = 0;

  uint64_t fileRawSize = 0;
  uint64_t stripeRawSize = 0;

  // config
  const CompressionKind compression;
  const uint64_t compressionBlockSize;
  const bool isIndexEnabled;
  const uint32_t indexStride;
  const bool shareFlatMapDictionaries;
  const uint64_t stripeSizeFlushThreshold;
  const uint64_t dictionarySizeFlushThreshold;
  const bool isStreamSizeAboveThresholdCheckEnabled;
  const dwio::common::MetricsLogPtr metricLogger;

  template <typename TestType>
  friend class WriterEncodingIndexTest;
  friend class IntegerColumnWriterDirectEncodingIndexTest;
  friend class StringColumnWriterDictionaryEncodingIndexTest;
  friend class StringColumnWriterDirectEncodingIndexTest;
  FRIEND_TEST(TestWriterContext, GetIntDictionaryEncoder);
  FRIEND_TEST(TestWriterContext, RemoveIntDictionaryEncoderForNode);
  // TODO: remove once writer code is consolidated
  template <typename TestType>
  friend class WriterEncodingIndexTest2;
  friend class IntegerColumnWriterDirectEncodingIndexTest2;
  friend class StringColumnWriterDictionaryEncodingIndexTest2;
  friend class StringColumnWriterDirectEncodingIndexTest2;
};

} // namespace facebook::velox::dwrf