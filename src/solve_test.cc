#include "solve.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "aspif.h"
#include "ground.h"
#include "macros.h"
#include "normalize.h"
#include "parse.h"

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

namespace {

// Parses, normalizes, grounds, and solves `source`, returning one string per
// answer set: the names that answer set makes true, sorted and separated by
// spaces, so that a test can state what it expects literally. The empty answer
// set renders as the empty string.
//
// Sorting matters because neither the order of the names within an answer set
// nor the order cvc5 hands the answer sets back in is part of the contract, so
// tests compare them as an unordered collection of sorted strings.
absl::StatusOr<std::vector<std::string>> solve_source(const std::string& source,
                                                      int max_answer_sets = 0) {
  Parser parser(source);
  ASSIGN_OR_RETURN(auto program, parser.parse_program());
  RETURN_IF_ERROR(normalize(*program));
  ASSIGN_OR_RETURN(aspif::Program grounded, ground(*program));

  SolveOptions options;
  options.max_answer_sets = max_answer_sets;
  ASSIGN_OR_RETURN(std::vector<AnswerSet> answer_sets,
                   solve(grounded, options));

  std::vector<std::string> rendered;
  rendered.reserve(answer_sets.size());
  for (const AnswerSet& answer_set : answer_sets) {
    const absl::flat_hash_set<aspif::Atom> is_true(answer_set.atoms.begin(),
                                                   answer_set.atoms.end());
    std::vector<std::string> names;
    for (const aspif::Output& output : grounded.outputs) {
      bool holds = true;
      for (aspif::Lit lit : output.condition) {
        if (is_true.contains(std::abs(lit)) != (lit > 0)) {
          holds = false;
          break;
        }
      }
      if (holds) names.push_back(output.name);
    }
    std::sort(names.begin(), names.end());
    rendered.push_back(absl::StrJoin(names, " "));
  }
  return rendered;
}

// A positive cycle holds only where something outside it offers support. Under
// 'p' the cycle is reached through 'a :- p'; under 'q' nothing reaches it, and
// the level ranking is what stops a and b from supporting each other.
TEST(SolveTest, PositiveCycleHoldsOnlyWhenSupportedFromOutside) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    a :- p.
    a :- b.
    b :- a.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre("a b p", "q"));
}

// A cycle nothing can reach leaves the empty answer set, not an answer set
// holding the cycle. This is the case a completion on its own gets wrong.
TEST(SolveTest, UnreachableCycleLeavesTheEmptyAnswerSet) {
  auto out = solve_source(R"(
    a :- b.
    b :- a.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre(""));
}

// A rule deriving an atom from itself can never justify it, so 'a' holds only
// through 'a :- p'. Its component has one member, so this is the case that
// needs a positive self-edge to count as a cycle.
TEST(SolveTest, SelfSupportingRuleCannotJustifyItsOwnHead) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    a :- p.
    a :- a, p.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre("a p", "q"));
}

TEST(SolveTest, ThreeAtomCycle) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    a :- b.
    b :- c.
    c :- a.
    a :- p.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre("a b c p", "q"));
}

TEST(SolveTest, FiveAtomCycle) {
  auto out = solve_source(R"(
    q :- not p.
    p :- not q.
    l1 :- p.
    l1 :- l5.
    l2 :- l1.
    l3 :- l2.
    l4 :- l3.
    l5 :- l4.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre("l1 l2 l3 l4 l5 p", "q"));
}

// The support reaching the cycle sits in a different component from the cycle
// itself, so the rank conditions have to skip it: 'a :- c' earns none.
TEST(SolveTest, CycleReachedFromASeparateComponent) {
  auto out = solve_source(R"(
    a :- b.
    b :- a.
    a :- c.
    c :- not d.
    d :- not c.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre("a b c", "d"));
}

TEST(SolveTest, StratifiedProgram) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    r :- p.
    s :- r.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre("p r s", "q"));
}

TEST(SolveTest, IntegrityConstraint) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    :- p.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre("q"));
}

TEST(SolveTest, NoAnswerSets) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    :- p.
    :- q.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, IsEmpty());
}

// A recursive predicate over acyclic data grounds to an acyclic atom graph, so
// no atom lies on a positive cycle and nothing needs ranking.
TEST(SolveTest, RecursivePredicateOverAcyclicData) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    edge(1,2) :- p.
    edge(2,3) :- p.
    reachable(X,Y) :- edge(X,Y).
    reachable(X,Z) :- reachable(X,Y), edge(Y,Z).
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre(
                        "edge(1,2) edge(2,3) p reachable(1,2) reachable(1,3) "
                        "reachable(2,3)",
                        "q"));
}

// The same rules over cyclic data do put atoms on positive cycles, so this
// needs the level ranking where the acyclic case above needs none.
TEST(SolveTest, RecursivePredicateOverCyclicData) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    edge(1,2) :- p.
    edge(2,3) :- p.
    edge(3,1) :- p.
    reachable(X,Y) :- edge(X,Y).
    reachable(X,Z) :- reachable(X,Y), edge(Y,Z).
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(
      *out, UnorderedElementsAre("edge(1,2) edge(2,3) edge(3,1) p "
                                 "reachable(1,1) reachable(1,2) reachable(1,3) "
                                 "reachable(2,1) reachable(2,2) reachable(2,3) "
                                 "reachable(3,1) reachable(3,2) reachable(3,3)",
                                 "q"));
}

// Whether the closure is cyclic depends on which branch is taken, so one
// program exercises both the ranked and the unranked shape.
TEST(SolveTest, TransitiveClosureWithDataDependentCycle) {
  auto out = solve_source(R"(
    q :- not p.
    p :- not q.
    edge(1,2) :- p.
    edge(2,3) :- p.
    edge(3,1) :- p.
    edge(1,2) :- q.
    tc(X,Y) :- edge(X,Y).
    tc(X,Z) :- tc(X,Y), edge(Y,Z).
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre(
                        "edge(1,2) edge(2,3) edge(3,1) p "
                        "tc(1,1) tc(1,2) tc(1,3) tc(2,1) tc(2,2) tc(2,3) "
                        "tc(3,1) tc(3,2) tc(3,3)",
                        "edge(1,2) q tc(1,2)"));
}

TEST(SolveTest, GraphColoring) {
  auto out = solve_source(R"(
    n(1). n(2). n(3).
    e(1,2). e(2,3). e(1,3).
    r(X) :- n(X), not g(X), not b(X).
    g(X) :- n(X), not r(X), not b(X).
    b(X) :- n(X), not r(X), not g(X).
    :- e(X,Y), r(X), r(Y).
    :- e(X,Y), g(X), g(Y).
    :- e(X,Y), b(X), b(Y).
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre(
                        "b(1) e(1,2) e(1,3) e(2,3) g(2) n(1) n(2) n(3) r(3)",
                        "b(1) e(1,2) e(1,3) e(2,3) g(3) n(1) n(2) n(3) r(2)",
                        "b(2) e(1,2) e(1,3) e(2,3) g(1) n(1) n(2) n(3) r(3)",
                        "b(2) e(1,2) e(1,3) e(2,3) g(3) n(1) n(2) n(3) r(1)",
                        "b(3) e(1,2) e(1,3) e(2,3) g(1) n(1) n(2) n(3) r(2)",
                        "b(3) e(1,2) e(1,3) e(2,3) g(2) n(1) n(2) n(3) r(1)"));
}

// A #count over atoms the solver is still free to choose survives grounding as
// an aspif weight body, which puts the program in QF_LIA.
TEST(SolveTest, CountAggregateOverUndeterminedAtoms) {
  auto out = solve_source(R"(
    p(1). p(2). p(3).
    q(X) :- p(X), not r(X).
    r(X) :- p(X), not q(X).
    s :- #count{ X : q(X) } >= 2.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(
      *out,
      UnorderedElementsAre(
          "p(1) p(2) p(3) q(1) q(2) q(3) s", "p(1) p(2) p(3) q(1) q(2) r(3) s",
          "p(1) p(2) p(3) q(1) q(3) r(2) s", "p(1) p(2) p(3) q(1) r(2) r(3)",
          "p(1) p(2) p(3) q(2) q(3) r(1) s", "p(1) p(2) p(3) q(2) r(1) r(3)",
          "p(1) p(2) p(3) q(3) r(1) r(2)", "p(1) p(2) p(3) r(1) r(2) r(3)"));
}

// #sum weights each literal by the term it sums, so the bound is reached by
// weight rather than by count: three of the four numbers suffice, and so do the
// two largest.
TEST(SolveTest, SumAggregateWithWeights) {
  auto out = solve_source(R"(
    n(1). n(2). n(3). n(4).
    in(X) :- n(X), not out(X).
    out(X) :- n(X), not in(X).
    heavy :- #sum{ X : in(X) } >= 6.
    :- not heavy.
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre(
                        "heavy in(1) in(2) in(3) in(4) n(1) n(2) n(3) n(4)",
                        "heavy in(1) in(2) in(3) n(1) n(2) n(3) n(4) out(4)",
                        "heavy in(1) in(2) in(4) n(1) n(2) n(3) n(4) out(3)",
                        "heavy in(1) in(3) in(4) n(1) n(2) n(3) n(4) out(2)",
                        "heavy in(2) in(3) in(4) n(1) n(2) n(3) n(4) out(1)",
                        "heavy in(2) in(4) n(1) n(2) n(3) n(4) out(1) out(3)",
                        "heavy in(3) in(4) n(1) n(2) n(3) n(4) out(1) out(2)"));
}

// Reachability used as a constraint: 'r' recurses over 'sel' atoms the solver
// chooses, so reaching node 2 forces sel(1,2) while arc(2,1) stays free.
TEST(SolveTest, ReachabilityConstraint) {
  auto out = solve_source(R"(
    node(1). node(2).
    arc(1,2). arc(2,1).
    sel(X,Y) :- arc(X,Y), not skip(X,Y).
    skip(X,Y) :- arc(X,Y), not sel(X,Y).
    r(1).
    r(Y) :- r(X), sel(X,Y).
    :- node(X), not r(X).
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(
      *out,
      UnorderedElementsAre(
          "arc(1,2) arc(2,1) node(1) node(2) r(1) r(2) sel(1,2) sel(2,1)",
          "arc(1,2) arc(2,1) node(1) node(2) r(1) r(2) sel(1,2) skip(2,1)"));
}

// A query becomes an aspif assumption, which every answer set has to satisfy.
TEST(SolveTest, QueryBecomesAnAssumption) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    p?
  )");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, UnorderedElementsAre("p"));
}

// Which answer set comes back under a limit is up to cvc5, so only the count is
// part of the contract.
TEST(SolveTest, MaxAnswerSetsLimitsHowManyComeBack) {
  const std::string source = R"(
    p :- not q.
    q :- not p.
    a :- p.
    a :- b.
    b :- a.
  )";
  auto all = solve_source(source, 0);
  ASSERT_TRUE(all.ok()) << all.status();
  EXPECT_THAT(*all, SizeIs(2));

  auto one = solve_source(source, 1);
  ASSERT_TRUE(one.ok()) << one.status();
  EXPECT_THAT(*one, SizeIs(1));
}

TEST(SolveTest, NegativeMaxAnswerSetsIsAnError) {
  auto out = solve_source("p.", -1);
  ASSERT_FALSE(out.ok());
  EXPECT_THAT(std::string(out.status().message()),
              HasSubstr("cannot be negative"));
}

// Optimization needs a branch-and-bound loop cvc5 cannot do for us, so a weak
// constraint is rejected rather than silently ignored.
TEST(SolveTest, WeakConstraintIsRejected) {
  auto out = solve_source(R"(
    p(1). p(2).
    q(X) :- p(X).
    :~ q(X). [1@1, X]
  )");
  ASSERT_FALSE(out.ok());
  EXPECT_THAT(std::string(out.status().message()), HasSubstr("optimization"));
}

}  // namespace
