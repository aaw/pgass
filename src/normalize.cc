#include "normalize.h"

#include "absl/strings/str_cat.h"
#include "macros.h"

namespace {

// Transform each rule of the form ':- a1, a2, ..., am' into a rule of the form
// '_x :- a1, a2, ..., am, not _x' for a new variable _x.
absl::Status normalize_integrity_constraints(Program& prog) {
  int i = 0;
  for (auto& statement : *(prog.statements)) {
    if (statement->head != nullptr) continue;
    std::string id = absl::StrCat("_ic", i++);
    auto disjunction = std::make_unique<Disjunction>();
    auto literal = std::make_unique<ClassicalLiteral>();
    literal->negated = false;
    literal->id = id;
    disjunction->literals.push_back(std::move(literal));
    statement->head = std::move(disjunction);
    auto naf_literal = std::make_unique<NafLiteral>();
    naf_literal->naf = true;
    auto body_literal = std::make_unique<ClassicalLiteral>();
    body_literal->negated = false;
    body_literal->id = id;
    naf_literal->literal = std::move(body_literal);
    statement->body->items->push_back(std::move(naf_literal));
  }
  return absl::OkStatus();
}
}  // namespace

absl::Status normalize(Program& prog) {
  RETURN_IF_ERROR(normalize_integrity_constraints(prog));
  return absl::OkStatus();
}
