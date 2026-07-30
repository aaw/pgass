#ifndef NORMALIZE_H_
#define NORMALIZE_H_

#include <string_view>

#include "absl/status/status.h"
#include "ast.h"

// The prefix on a classically negated predicate: '-p(1)' is '_neg_p(1)' after
// normalization. Later stages match on it to print the atom as '-p(1)' again.
inline constexpr std::string_view kClassicalNegationPrefix = "_neg_";

// Rewrites `prog` into the shape ground() expects: weak constraints become
// '_viol' rules, classical negation becomes fresh '_neg_' predicates, and choice
// rules become disjunctive rules plus a counting constraint.
//
// A disjunctive head passes through as it stands. Turning 'a | b :- body.' into
// normal rules is only sound while no two head atoms lie on a common positive
// cycle, which is a question about ground atoms. solve.cc asks it.
absl::Status normalize(Program& prog);

#endif  // NORMALIZE_H_
