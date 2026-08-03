#ifndef FORMAT_H_
#define FORMAT_H_

#include <string>

#include "ast.h"

std::string format(const Program& prog);

// One statement on its own, e.g. "p(X) :- q(X)."
std::string format(const Statement& statement);

// One term on its own, e.g. "X + 1".
std::string format(const Term& term);

// The query as written, e.g. "p(X)?".
std::string format(const Query& query);

#endif  // FORMAT_H_
