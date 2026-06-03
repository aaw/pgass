#ifndef NORMALIZE_H_
#define NORMALIZE_H_

#include "absl/status/status.h"
#include "ast.h"

absl::Status normalize(Program& prog);

#endif  // NORMALIZE_H_
