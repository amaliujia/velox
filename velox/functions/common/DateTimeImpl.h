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

#include "velox/type/Timestamp.h"

namespace facebook::velox::functions {
namespace {
constexpr double kNanosecondsInSecond = 1'000'000'000;
}

FOLLY_ALWAYS_INLINE double toUnixtime(const Timestamp& timestamp) {
  double result = timestamp.getSeconds();
  result += static_cast<double>(timestamp.getNanos()) / kNanosecondsInSecond;
  return result;
}

FOLLY_ALWAYS_INLINE Timestamp fromUnixtime(double unixtime) {
  auto seconds = std::floor(unixtime);
  auto nanos = unixtime - seconds;
  return Timestamp(seconds, nanos * kNanosecondsInSecond);
}

} // namespace facebook::velox::functions