#ifndef SOLVE_H_
#define SOLVE_H_

#include <memory>
#include <optional>
#include <vector>

#include "absl/status/statusor.h"
#include "aspif.h"
#include "bigint.h"

// One answer set: the atoms true in it. Every atom of the program that is not
// listed is false. The ids are the ones the solved aspif::Program used, so
// aspif::Output turns them back into names.
struct AnswerSet {
  std::vector<aspif::Atom> atoms;
  // One entry per priority level of the program's aspif::Minimize statements,
  // the most important level first. Each is the weights of that level's true
  // literals, added up. Empty for a program with no minimize statements.
  std::vector<BigInt> costs;
};

// What a solve() call found.
struct SolveResult {
  std::vector<AnswerSet> answer_sets;
  // Whether the search ran out of answer sets rather than stopping because it
  // had already found options.max_answer_sets of them. Only an exhausted search
  // proves that no further answer set exists.
  //
  // Weak constraints leave only optimal answer sets to find, so an exhausted
  // search of a program with them has found every optimum.
  bool exhausted = false;
};

struct SolveOptions {
  // How many answer sets to look for. 0 asks for all of them, which can be a
  // very large number. Under weak constraints only optimal answer sets count.
  int max_answer_sets = 1;
};

// Solves a ground program: builds the encoding of encode.h and hands it to
// cvc5.
//
// Nothing here reasons about rules or cycles. That lives in the encoding, the
// one --encode=smtlib prints. What is left is asking cvc5 for a model,
// checking it, and ruling it out to ask again.
//
// Returns the answer sets in the order cvc5 produced them, so at most
// options.max_answer_sets of them, along with whether the search was exhausted.
// A program with no answer sets returns an empty vector. That is not the same as
// one AnswerSet holding no atoms, which is what a program whose only answer set
// is the empty one returns.
//
// Weak constraints settle before the first answer set comes back. Each
// priority level is brought down one answer set at a time until nothing
// cheaper is left, and its least cost is then asserted, so every answer set
// returned is optimal. The script --encode=smtlib prints does the same walk,
// written out as steps for a reader to follow by hand.
//
// A disjunctive head with two atoms on a common positive cycle leaves the
// encoding admitting models that are not answer sets, so each model is checked
// before it is reported. The check is a second solver. A model that fails it
// hands back an unfounded set, and the loop nogood of that set goes to the
// first solver, ruling out every model that leaves the set unsupported.
//
// A query asks about the answer sets rather than restricting them, so it does
// not narrow what comes back here. answer_query() answers it.
//
// Choice rule heads return an UnimplementedError. Nothing produces one:
// normalization rewrites choice rules into disjunctive ones.
absl::StatusOr<SolveResult> solve(const aspif::Program& prog,
                                  const SolveOptions& options);

struct Search;

// The search behind solve(), one answer set at a time. solve() runs this to
// exhaustion or a fixed count. A caller that wants only a few, or wants to
// stop as soon as one looks right, uses it directly instead and never pays to
// find the rest.
class AnswerSetIterator {
 public:
  // Builds the encoding and settles weak constraint costs, so every answer
  // set next() returns afterwards is optimal.
  static absl::StatusOr<AnswerSetIterator> start(const aspif::Program& prog);

  AnswerSetIterator(AnswerSetIterator&&) noexcept;
  AnswerSetIterator& operator=(AnswerSetIterator&&) noexcept;
  ~AnswerSetIterator();

  // The next answer set, or nullopt once the search is exhausted: a proof
  // that no further answer set exists.
  absl::StatusOr<std::optional<AnswerSet>> next();

 private:
  explicit AnswerSetIterator(std::unique_ptr<Search> search);
  std::unique_ptr<Search> search_;
};

// Whether a program's query holds.
enum class QueryAnswer {
  // Every answer set satisfies the query.
  kYes,
  // Some answer set leaves it unsatisfied.
  kNo,
};

// The answer to a program's query.
struct QueryResult {
  QueryAnswer answer = QueryAnswer::kNo;
  // The atoms of aspif::Program::query that hold in every answer set, in the
  // order grounding matched them. Each one stands for a substitution the query
  // holds under. 'p(1). p(2). p(X)?' lists p(1) and p(2), which is to say X of
  // 1 and X of 2. A kNo answer holds under nothing, so this is empty.
  std::vector<aspif::Lit> holds;
  // Whether the program has no answer set, which is a kYes of its own kind:
  // every substitution answers the query. Those range over the whole Herbrand
  // universe, every integer included, so `holds` is empty rather than listing
  // them.
  bool no_answer_set = false;
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
// A program with no answer set answers kYes. ASP-Core-2 says every query of
// such a program is true, there being no answer set left to fail it.
//
// Weak constraints leave only the optimal answer sets, the ones solve()
// returns, and the query is asked of those.
absl::StatusOr<QueryResult> answer_query(const aspif::Program& prog);

#endif  // SOLVE_H_
