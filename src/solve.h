#ifndef SOLVE_H_
#define SOLVE_H_

#include <cstdint>
#include <vector>

#include "absl/status/statusor.h"
#include "aspif.h"

// One answer set: the atoms true in it. Every atom of the program that is not
// listed is false. The ids are the ones the solved aspif::Program used, so
// aspif::Output turns them back into names.
struct AnswerSet {
  std::vector<aspif::Atom> atoms;
  // One entry per priority level of the program's aspif::Minimize statements,
  // the most important level first. Each is the weights of that level's true
  // literals, added up. Empty for a program with no minimize statements.
  std::vector<std::int64_t> costs;
};

struct SolveOptions {
  // How many answer sets to look for. 0 asks for all of them, which can be a
  // very large number. Under weak constraints only optimal answer sets count.
  int max_answer_sets = 1;

  // How to find the least cost of a weak-constraint priority level. Both settle
  // on the same cost, so this only changes how long the search takes. solve.cc
  // says when each one wins.
  enum class Optimizer {
    // Ask for a cost below the best one found so far until there is none.
    kLinear,
    // Halve the range the cost is known to lie in until it holds one value.
    kBisect,
  };
  Optimizer optimizer = Optimizer::kLinear;
};

// Translates a ground program to QF_IDL along the lines of Niemelä, "Stable
// models and difference logic", and hands it to cvc5.
//
// Returns the answer sets in the order cvc5 produced them, so at most
// options.max_answer_sets of them. A program with no answer sets returns an
// empty vector; note that this is not the same as returning one AnswerSet whose
// atoms are empty, which is what a program whose only answer set is the empty
// one returns.
//
// Weak constraints are minimized level by level, most important level first,
// before any answer set is returned. Every answer set returned is optimal.
//
// Choice and disjunctive rule heads return an UnimplementedError. solve.cc says
// what each would take.
absl::StatusOr<std::vector<AnswerSet>> solve(const aspif::Program& prog,
                                             const SolveOptions& options);

#endif  // SOLVE_H_
