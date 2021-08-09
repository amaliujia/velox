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
#include "velox/functions/sparksql/Register.h"

#include "velox/functions/common/DateTimeFunctions.h"
#include "velox/functions/common/Hash.h"
#include "velox/functions/common/JsonExtractScalar.h"
#include "velox/functions/common/Rand.h"
#include "velox/functions/common/StringFunctions.h"
#include "velox/functions/lib/Re2Functions.h"
#include "velox/functions/lib/RegistrationHelpers.h"
#include "velox/functions/sparksql/LeastGreatest.h"
#include "velox/functions/sparksql/RegexFunctions.h"
#include "velox/functions/sparksql/RegisterArithmetic.h"
#include "velox/functions/sparksql/RegisterCompare.h"

namespace facebook::velox::functions {

static void workAroundRegistrationMacro(const std::string& prefix) {
  // VELOX_REGISTER_VECTOR_FUNCTION must be invoked in the same namespace as the
  // vector function definition.
  VELOX_REGISTER_VECTOR_FUNCTION(udf_array_contains, prefix + "array_contains");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_element_at, prefix + "element_at");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_transform, prefix + "transform");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_reduce, prefix + "reduce");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_coalesce, prefix + "coalesce");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_is_null, prefix + "isnull");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_in, prefix + "in");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_array_constructor, prefix + "array");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_filter, prefix + "filter");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_length, prefix + "length");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_map_entries, prefix + "map_entries");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_substr, prefix + "substring");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_lower, prefix + "lower");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_upper, prefix + "upper");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_concat, prefix + "concat");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_strpos, prefix + "strpos");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_replace, prefix + "replace");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_concat_row, prefix + "ROW");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_not, prefix + "not");
}

namespace sparksql {

void registerFunctions(const std::string& prefix) {
  registerFunction<udf_rand, double>({"rand"});

  registerUnaryScalar<udf_hash, int64_t>({"hash"});

  registerFunction<udf_json_extract_scalar, Varchar, Varchar, Varchar>(
      {prefix + "get_json_object"});

  // Register string functions.
  registerFunction<udf_chr, Varchar, int64_t>();
  registerFunction<udf_codepoint, int32_t, Varchar>();
  registerFunction<
      udf_xxhash64int<int64_t, Varchar>,
      int64_t,
      Varchar,
      int64_t>({prefix + "xxhash64"});
  registerFunction<udf_xxhash64int<int64_t, Varchar>, int64_t, Varchar>(
      {prefix + "xxhash64"});
  registerFunction<udf_xxhash64<Varbinary, Varbinary>, Varbinary, Varbinary>(
      {prefix + "xxhash64"});
  registerFunction<udf_md5<Varbinary, Varbinary>, Varbinary, Varbinary>(
      {prefix + "md5"});
  registerFunction<udf_md5_radix<Varchar, Varchar>, Varchar, Varchar, int32_t>(
      {prefix + "md5"});
  registerFunction<udf_md5_radix<Varchar, Varchar>, Varchar, Varchar, int64_t>(
      {prefix + "md5"});
  registerFunction<udf_md5_radix<Varchar, Varchar>, Varchar, Varchar>({"md5"});
  VELOX_REGISTER_VECTOR_FUNCTION(udf_subscript, prefix + "subscript");
  VELOX_REGISTER_VECTOR_FUNCTION(udf_regexp_split, prefix + "split");
  exec::registerStatefulVectorFunction(
      "regexp_extract", re2ExtractSignatures(), makeRegexExtract);
  exec::registerStatefulVectorFunction(
      "rlike", re2MatchSignatures(), makeRLike);

  // Datetime.
  registerFunction<udf_to_unixtime, double, Timestamp>(
      {prefix + "to_unixtime", prefix + "to_unix_timestamp"});
  registerFunction<udf_from_unixtime, Timestamp, double>();

  exec::registerStatefulVectorFunction(
      prefix + "least", leastSignatures(), makeLeast);
  exec::registerStatefulVectorFunction(
      prefix + "greatest", greatestSignatures(), makeGreatest);
  // These vector functions are only accessible via the
  // VELOX_REGISTER_VECTOR_FUNCTION macro, which must be invoked in the same
  // namespace as the function definition.
  workAroundRegistrationMacro(prefix);

  // These groups of functions involve instantiating many templates. They're
  // broken out into a separate compilation unit to improve build latency.
  registerArithmeticFunctions(prefix);
  registerCompareFunctions(prefix);
}

} // namespace sparksql
} // namespace facebook::velox::functions