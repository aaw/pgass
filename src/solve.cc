#include "solve.h"

#include <cvc5/cvc5.h>

#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "aspif.h"

absl::StatusOr<std::vector<AnswerSet>> solve(const aspif::Program& prog,
                                             const SolveOptions& options) {
  (void)prog;
  (void)options;

  cvc5::TermManager term_manager;
  cvc5::Solver solver(term_manager);

  // TODO: build the QF_IDL encoding, then loop check-sat and add a clause
  // blocking the answer set just found until max_answer_sets is reached.
  return absl::UnimplementedError(
      absl::StrCat("solving is not implemented yet (linked against cvc5 ",
                   solver.getVersion(), ")"));
}
