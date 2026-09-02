// #region MODULE_CONTRACT
// PURPOSE: Locks shared form-validation errors to the ranges supplied by callers.
// SCOPE:
// - Exercises parameterized poll-interval parsing.
// - NOT: Persistence or HTTP request handling.
// INVARIANTS:
// - Rejection errors name the supplied inclusive bounds.
// #endregion MODULE_CONTRACT

#include <assert.h>

#include "../sms_gate/persistence/config_store_common.h"

// #region FUNC_main
// PURPOSE: Verifies custom interval bounds reach both invalid-input error paths.
int main() {
  uint16_t interval = 0;
  String error;

  assert(!parsePollInterval("", interval, 7, 42, error));
  assert(error == "Poll interval must be a number between 7 and 42 seconds.");

  assert(!parsePollInterval("43", interval, 7, 42, error));
  assert(error == "Poll interval must be a number between 7 and 42 seconds.");

  assert(parsePollInterval(" 42 ", interval, 7, 42, error));
  assert(interval == 42);
  return 0;
}
// #endregion FUNC_main
