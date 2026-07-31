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

// Solves a ground program: builds the encoding of encode.h and hands it to
// cvc5.
//
// Returns the answer sets in the order cvc5 produced them, so at most
// options.max_answer_sets of them. A program with no answer sets returns an
// empty vector. That is not the same as one AnswerSet holding no atoms, which
// is what a program whose only answer set is the empty one returns.
//
// Weak constraints are minimized level by level, most important level first,
// before any answer set is returned. Every answer set returned is optimal.
//
// A disjunctive head costs a second solver call per candidate answer set, but
// only where two of its atoms lie on a common positive cycle. Every other
// program, normal or disjunctive, is decided by one translation. encode.cc says
// why.
//
// A query asks about the answer sets rather than restricting them, so it does
// not narrow what comes back here. answer_query() answers it.
//
// Choice rule heads return an UnimplementedError. Nothing produces one:
// normalization rewrites choice rules into disjunctive ones.
absl::StatusOr<std::vector<AnswerSet>> solve(const aspif::Program& prog,
                                             const SolveOptions& options);

// Whether a program's query holds.
enum class QueryAnswer {
  // One of the atoms the query matched holds in every answer set.
  kYes,
  // Each of them is left out of some answer set.
  kNo,
  // The program has no answer set, so there is nothing to ask about.
  kNoAnswerSet,
};

// The answer to a program's query.
struct QueryResult {
  QueryAnswer answer = QueryAnswer::kNo;
  // The atoms of aspif::Program::query that hold in every answer set, in the
  // order grounding matched them. Each one stands for a substitution the query
  // holds under. 'p(1). p(2). p(X)?' lists p(1) and p(2), which is to say X of
  // 1 and X of 2. Non-empty exactly when `answer` is kYes.
  std::vector<aspif::Lit> holds;
};

// Answers the program's query, the atoms grounding left in
// aspif::Program::query.
//
// A query asks about every answer set at once. 'a?' asks whether a holds in all
// of them, not whether one of them holds a. So 'a | b. a?' answers kNo, because
// b on its own is an answer set too.
//
// The atoms a non-ground query matched are asked about one at a time, and every
// one that holds throughout comes back in QueryResult::holds. Take 'p(X)?' over
// a program whose answer sets are {p(1)} and {p(2)}. The answer is kNo. Each
// answer set holds p of something, but neither p(1) nor p(2) holds in both. A
// program with no query has no atom to hold, so it answers kNo as well.
//
// Weak constraints leave only the optimal answer sets, the ones solve()
// returns, and the query is asked of those.
absl::StatusOr<QueryResult> answer_query(const aspif::Program& prog,
                                         const SolveOptions& options);

#endif  // SOLVE_H_
