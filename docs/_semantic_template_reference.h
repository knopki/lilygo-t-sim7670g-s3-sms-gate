/**
 * Module brief.
 * #region MODULE_CONTRACT
 * PURPOSE: [Describe the GOAL of this public interface — what business/operational need it fulfills, why.]
 * SCOPE:
 *   - [Main interface responsibilities.]
 *   - NOT: [Implementation details or responsibilities owned by another module.]
 * INVARIANTS:
 *   - [Condition/state callers may rely on.]
 * USECASES:
 *   - [Entity]: [Actor] => [Action] => [Goal]
 * DEPENDENCIES: [Non-trivial deps — USES API: ..., READS: ..., WRITES: ...]
 * RATIONALE:
 *   - Q: [Why does this interface have this boundary?]
 *     A: [Justification and environmental constraints.]
 * KEYWORDS: [Comma-separated keywords for grep search]
 * #endregion MODULE_CONTRACT
 */

#pragma once

#include <Arduino.h>

// #region CLASS_ExampleClass
/**
 * Class brief.
 *
 * PURPOSE: [Goal of this public type and why — what it enables the user/agent to do.]
 * INVARIANTS:
 *   - [State condition preserved by its public operations.]
 */
class ExampleClass {
 public:
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
  String exampleMethod(int param1);
  // #endregion METHOD_exampleMethod

 private:
  void privateMethodMarkupNotNeededForTrivialMethods();
};
// #endregion CLASS_ExampleClass

// #region FUNC_exampleFunction
/**
 * PURPOSE: [Describe the GOAL of this public function and why.]
 * REQUIRES: [Optional preconditions.]
 * ENSURES: [Optional postconditions.]
 * @param data [Optional semantic meaning of a non-trivial input.]
 * @return [Optional semantic meaning of a non-trivial output.]
 */
String exampleFunction(const String& data);
// #endregion FUNC_exampleFunction
