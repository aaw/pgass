#ifndef SAFETY_H_
#define SAFETY_H_

#include "absl/status/status.h"
#include "ast.h"

// Returns OK if every variable in every rule body of `prog` is safely bound.
// A variable is safely bound if it appears in a positive classical literal, or
// can be determined via equality once other variables are bound. Aggregates bind
// their output variable only if all variables in the aggregate body are bound.
// NAF literals and NAF aggregates never bind variables. On failure, returns an
// error identifying the rule (by line and head) and the unsafe variables.
absl::Status verify_safe(const Program& prog);

#endif  // SAFETY_H_
