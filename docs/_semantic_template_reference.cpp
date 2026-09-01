/**
 * Module brief.
 * #region MODULE_CONTRACT
 * PURPOSE: [Describe the GOAL of this implementation — what business/operational need it fulfills, why.]
 * SCOPE:
 *   - [Main implementation responsibilities.]
 *   - NOT: [Public interface declarations or responsibilities owned by another module.]
 * INVARIANTS:
 *   - [Condition/state preserved while this module runs.]
 * USECASES:
 *   - [Entity]: [Actor] => [Action] => [Goal]
 * DEPENDENCIES: [Non-trivial deps — USES API: ..., READS: ..., WRITES: ...]
 * RATIONALE:
 *   - Q: [Why is this implementation organized this way?]
 *     A: [Justification and environmental constraints.]
 * KEYWORDS: [Comma-separated keywords for grep search]
 * #endregion MODULE_CONTRACT
 */

#include "example_class.h"

namespace {

void privateFunctionMarkupNotNeededForTrivialFunctions() {
  // no-op
}

}  // namespace

// #region METHOD_exampleMethod
/**
 * Optional brief.
 *
 * PURPOSE: [Goal of the method and why.]
 * REQUIRES: [Optional preconditions.]
 * ENSURES: [Optional postconditions.]
 * RATIONALE: [Optional rationale.]
 * @param param1 [Optional semantic meaning of a non-trivial input.]
 * @return [Optional semantic meaning of a non-trivial output.]
 */
String ExampleClass::exampleMethod(int param1) {
  // #region BLOCK_calculateResult Transform the validated input into the caller's result.
  privateMethodMarkupNotNeededForTrivialMethods();
  Serial.printf("event=example_calculation_start param1=%d\n", param1);
  const String result = String(param1 * 2);
  // #endregion BLOCK_calculateResult

  Serial.printf("event=example_calculation_result result_length=%u\n",
                static_cast<unsigned>(result.length()));
  return result;
}
// #endregion METHOD_exampleMethod

// #region FUNC_exampleFunction
/**
 * PURPOSE: [Describe the GOAL of this public function and why.]
 * REQUIRES: [Optional preconditions.]
 * ENSURES: [Optional postconditions.]
 * @param data [Optional semantic meaning of a non-trivial input.]
 * @return [Optional semantic meaning of a non-trivial output.]
 */
String exampleFunction(const String& data) {
  Serial.printf("event=example_function_start data_length=%u\n",
                static_cast<unsigned>(data.length()));
  privateFunctionMarkupNotNeededForTrivialFunctions();

  // #region BLOCK_buildResult Accumulate a result for each input character.
  size_t total = 0;
  for (size_t index = 0; index < data.length(); ++index) {
    ++total;
    Serial.printf("event=example_function_step index=%u running_total=%u\n",
                  static_cast<unsigned>(index), static_cast<unsigned>(total));
  }
  // #endregion BLOCK_buildResult

  Serial.printf("event=example_function_result sum=%u\n",
                static_cast<unsigned>(total));
  return String(total);
}
// #endregion FUNC_exampleFunction
