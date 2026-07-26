#ifndef NORMALIZE_H_
#define NORMALIZE_H_

#include <string_view>

#include "absl/status/status.h"
#include "ast.h"

// The prefix on a classically negated predicate: '-p(1)' is '_neg_p(1)' after
// normalization. Later stages match on it to print the atom as '-p(1)' again.
inline constexpr std::string_view kClassicalNegationPrefix = "_neg_";

absl::Status normalize(Program& prog);

#endif  // NORMALIZE_H_
