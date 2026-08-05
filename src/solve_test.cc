#include "solve.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "aspif.h"
#include "ground.h"
#include "macros.h"
#include "normalize.h"
#include "parse.h"
#include "test_macros.h"

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

namespace {

// Parses, normalizes, and grounds `source`, the three steps every test here
// takes before it solves anything.
absl::StatusOr<aspif::Program> ground_source(const std::string& source) {
  Parser parser(source);
  ASSIGN_OR_RETURN(auto program, parser.parse_program());
  RETURN_IF_ERROR(normalize(*program));
  return ground(*program);
}

// One string per answer set: the names that answer set makes true, sorted and
// separated by spaces, so that a test can state what it expects literally. The
// empty answer set renders as the empty string. An answer set with a cost,
// which only a program with weak constraints has, ends with '| cost' and the
// cost of each priority level, most important level first.
//
// Sorting matters because neither the order of the names within an answer set
// nor the order cvc5 hands the answer sets back in is part of the contract, so
// tests compare them as an unordered collection of sorted strings.
absl::StatusOr<std::vector<std::string>> render_solutions(
    const aspif::Program& grounded, const SolveOptions& options) {
  ASSIGN_OR_RETURN(SolveResult result, solve(grounded, options));

  std::vector<std::string> rendered;
  rendered.reserve(result.answer_sets.size());
  for (const AnswerSet& answer_set : result.answer_sets) {
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
    std::string line = absl::StrJoin(names, " ");
    if (!answer_set.costs.empty()) {
      if (!line.empty()) line += ' ';
      absl::StrAppend(&line, "| cost ", absl::StrJoin(answer_set.costs, " "));
    }
    rendered.push_back(std::move(line));
  }
  return rendered;
}

// Parses, normalizes, grounds, and solves `source`, rendering the answer sets
// as above.
absl::StatusOr<std::vector<std::string>> solve_source(const std::string& source,
                                                      int max_answer_sets = 0) {
  ASSIGN_OR_RETURN(aspif::Program grounded, ground_source(source));

  SolveOptions options;
  options.max_answer_sets = max_answer_sets;
  return render_solutions(grounded, options);
}

// Grounds `source` and answers its query.
absl::StatusOr<QueryResult> query_result(const std::string& source) {
  ASSIGN_OR_RETURN(aspif::Program grounded, ground_source(source));
  return answer_query(grounded);
}

// Whether the query of `source` holds.
absl::StatusOr<QueryAnswer> query_source(const std::string& source) {
  ASSIGN_OR_RETURN(QueryResult result, query_result(source));
  return result.answer;
}

// The names of the atoms the query of `source` holds under, sorted, so that a
// test can state them literally. The order answer_query reports them in follows
// grounding, which is not part of the contract.
absl::StatusOr<std::vector<std::string>> query_matches(
    const std::string& source) {
  ASSIGN_OR_RETURN(aspif::Program grounded, ground_source(source));
  ASSIGN_OR_RETURN(QueryResult result, answer_query(grounded));

  const absl::flat_hash_set<aspif::Lit> answers(result.holds.begin(),
                                                result.holds.end());
  std::vector<std::string> names;
  for (const aspif::Output& output : grounded.outputs) {
    if (output.condition.size() == 1 &&
        answers.contains(output.condition.front())) {
      names.push_back(output.name);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
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
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("a b p", "q"));
}

// A cycle nothing can reach leaves the empty answer set, not an answer set
// holding the cycle. This is the case a completion on its own gets wrong.
TEST(SolveTest, UnreachableCycleLeavesTheEmptyAnswerSet) {
  auto out = solve_source(R"(
    a :- b.
    b :- a.
  )");
  ASSERT_OK(out);
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
  ASSERT_OK(out);
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
  ASSERT_OK(out);
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
  ASSERT_OK(out);
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
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("a b c", "d"));
}

TEST(SolveTest, StratifiedProgram) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    r :- p.
    s :- r.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("p r s", "q"));
}

TEST(SolveTest, IntegrityConstraint) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    :- p.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("q"));
}

TEST(SolveTest, NoAnswerSets) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    :- p.
    :- q.
  )");
  ASSERT_OK(out);
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
  ASSERT_OK(out);
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
  ASSERT_OK(out);
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
  ASSERT_OK(out);
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
  ASSERT_OK(out);
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
  ASSERT_OK(out);
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
  ASSERT_OK(out);
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
  ASSERT_OK(out);
  EXPECT_THAT(
      *out,
      UnorderedElementsAre(
          "arc(1,2) arc(2,1) node(1) node(2) r(1) r(2) sel(1,2) sel(2,1)",
          "arc(1,2) arc(2,1) node(1) node(2) r(1) r(2) sel(1,2) skip(2,1)"));
}

// A query holds where every answer set satisfies it. Whichever of p and q
// holds, r follows.
TEST(SolveTest, QueryHoldsInEveryAnswerSet) {
  auto out = query_source(R"(
    p :- not q.
    q :- not p.
    r :- p.
    r :- q.
    r?
  )");
  ASSERT_OK(out);
  EXPECT_EQ(*out, QueryAnswer::kYes);
}

// One answer set satisfying the query is not enough. The answer set holding q
// leaves p out, and that answers 'p?' no.
TEST(SolveTest, QueryOneAnswerSetHoldingItIsNotEnough) {
  auto out = query_source(R"(
    p :- not q.
    q :- not p.
    p?
  )");
  ASSERT_OK(out);
  EXPECT_EQ(*out, QueryAnswer::kNo);
}

// A disjunction picks one of its atoms without saying which, so neither atom
// holds throughout.
TEST(SolveTest, QueryOverADisjunctionIsNo) {
  auto out = query_source("a | b. a?");
  ASSERT_OK(out);
  EXPECT_EQ(*out, QueryAnswer::kNo);
}

// A query with a variable is asked about each atom it matched. Both p(1) and
// p(2) are facts, so either one answers it.
TEST(SolveTest, QueryWithAVariableAsksAboutEachMatch) {
  auto out = query_source("p(1). p(2). p(X)?");
  ASSERT_OK(out);
  EXPECT_EQ(*out, QueryAnswer::kYes);
}

// Every substitution the query holds under is reported, not just the first one
// found. X of 1 and X of 2 both answer 'p(X)?' here.
TEST(SolveTest, QueryReportsEverySubstitutionItHoldsUnder) {
  auto out = query_matches("p(1). p(2). p(X)?");
  ASSERT_OK(out);
  EXPECT_THAT(*out, ElementsAre("p(1)", "p(2)"));
}

// A substitution that fails in some answer set is left out. Only p(1) is a
// fact, so p(2) holds in one answer set and not the other.
TEST(SolveTest, QueryLeavesOutASubstitutionThatFailsSomewhere) {
  auto out = query_matches(R"(
    p(1).
    p(2) :- not q.
    q :- not p(2).
    p(X)?
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, ElementsAre("p(1)"));
}

// A query that holds under nothing reports nothing.
TEST(SolveTest, QueryThatIsNoHoldsUnderNoSubstitution) {
  auto out = query_matches(R"(
    p(1) :- not p(2).
    p(2) :- not p(1).
    p(X)?
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, IsEmpty());
}

// A ground query holds under the empty substitution, which is the query atom
// itself.
TEST(SolveTest, GroundQueryReportsItsOwnAtom) {
  auto out = query_matches("p(1). p(1)?");
  ASSERT_OK(out);
  EXPECT_THAT(*out, ElementsAre("p(1)"));
}

// Every answer set holds p of something, but neither p(1) nor p(2) holds in
// both, so no substitution answers the query.
TEST(SolveTest, QueryWithAVariableNeedsOneMatchThroughout) {
  auto out = query_source(R"(
    p(1) :- not p(2).
    p(2) :- not p(1).
    p(X)?
  )");
  ASSERT_OK(out);
  EXPECT_EQ(*out, QueryAnswer::kNo);
}

// A query no atom matches has nothing that can make it hold.
TEST(SolveTest, QueryMatchingNothingIsNo) {
  auto out = query_source("p(1). q(2)?");
  ASSERT_OK(out);
  EXPECT_EQ(*out, QueryAnswer::kNo);
}

// A program with no answer set holds every query, even one over a predicate it
// never mentions. Every substitution answers it, and those are not the atoms
// grounding matched, so none are reported.
TEST(SolveTest, QueryAgainstAnIncoherentProgramHolds) {
  auto out = query_result(R"(
    p :- not p.
    q?
  )");
  ASSERT_OK(out);
  EXPECT_EQ(out->answer, QueryAnswer::kYes);
  EXPECT_TRUE(out->no_answer_set);
  EXPECT_THAT(out->holds, IsEmpty());
}

// The question is asked of the optimal answer sets only. Both {a} and {b} are
// answer sets, but {b} costs 1, so only {a} is left to ask about.
TEST(SolveTest, QueryIsAskedOfOptimalAnswerSetsOnly) {
  auto out = query_source(R"(
    a | b.
    :~ b. [1@1]
    a?
  )");
  ASSERT_OK(out);
  EXPECT_EQ(*out, QueryAnswer::kYes);
}

// Under a head cycle a model is only a candidate answer set, so the search
// checks each one against the reduct. The answer sets here are {a, b} and {c},
// and the first of them leaves c out.
TEST(SolveTest, QueryUnderAHeadCycleChecksTheReduct) {
  auto out = query_source(R"(
    a | b | c.
    a :- b.
    b :- a.
    c?
  )");
  ASSERT_OK(out);
  EXPECT_EQ(*out, QueryAnswer::kNo);
}

// The same cycle without the third disjunct has {a, b} as its only answer set,
// so a holds throughout.
TEST(SolveTest, QueryHoldsInsideAHeadCycle) {
  auto out = query_source(R"(
    a | b.
    a :- b.
    b :- a.
    a?
  )");
  ASSERT_OK(out);
  EXPECT_EQ(*out, QueryAnswer::kYes);
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
  ASSERT_OK(all);
  EXPECT_THAT(*all, SizeIs(2));

  auto one = solve_source(source, 1);
  ASSERT_OK(one);
  EXPECT_THAT(*one, SizeIs(1));
}

TEST(SolveTest, NegativeMaxAnswerSetsIsAnError) {
  auto out = solve_source("p.", -1);
  ASSERT_FALSE(out.ok());
  EXPECT_THAT(std::string(out.status().message()),
              HasSubstr("cannot be negative"));
}

/* #min and #max range over the ASP-Core-2 order on terms, where every integer
   sits below every symbolic constant. So the #min is at least 3 exactly in the
   answer sets without p(1), and the #max is below a exactly in those without
   p(a).

   The empty answer set gets both. Its set of tuples is empty, which is
   +infinity for #min and -infinity for #max. */
TEST(SolveTest, MinMaxAggregatesRangeOverTheTermOrder) {
  auto out = solve_source(R"(
    { p(1) }. { p(3) }. { p(a) }.
    q :- #min{ X : p(X) } >= 3.
    r :- #max{ X : p(X) } < a.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("q r",                //
                                         "p(1) r",             //
                                         "p(3) q r",           //
                                         "p(a) q",             //
                                         "p(1) p(3) r",        //
                                         "p(1) p(a)",          //
                                         "p(3) p(a) q",        //
                                         "p(1) p(3) p(a)"));
}

// A #max binds a variable to a term, so s takes whichever of 1 and a the answer
// set has. The empty answer set derives no s. Its #max is -infinity, which
// equals no term, so S has nothing to take.
TEST(SolveTest, MaxAggregateBindsATermPerAnswerSet) {
  auto out = solve_source(R"(
    { p(1) }. { p(a) }.
    s(S) :- #max{ X : p(X) } = S.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("",  //
                                         "p(1) s(1)",
                                         "p(a) s(a)",
                                         "p(1) p(a) s(a)"));
}

// A weak constraint costs one unit per node picked, so the cheapest answer set
// picks only the node the integrity constraint forces. The answer sets that
// pick more are optimal for nothing and never come back.
TEST(SolveTest, WeakConstraintMinimizesHowManyAtomsHold) {
  auto out = solve_source(R"(
    n(1). n(2). n(3).
    in(X) :- n(X), not out(X).
    out(X) :- n(X), not in(X).
    :- not in(1).
    :~ in(X). [1@0, X]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre(
                        "in(1) n(1) n(2) n(3) out(2) out(3) | cost 1"));
}

// A weight other than 1 makes the cost a sum rather than a count, so picking
// the two cheapest nodes beats picking any other pair.
TEST(SolveTest, WeakConstraintMinimizesASumOfWeights) {
  auto out = solve_source(R"(
    n(1). n(2). n(3).
    in(X) :- n(X), not out(X).
    out(X) :- n(X), not in(X).
    :- #count{ X : in(X) } < 2.
    :~ in(X). [X@0, X]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(
      *out, UnorderedElementsAre("in(1) in(2) n(1) n(2) n(3) out(3) | cost 3"));
}

// Levels are settled one at a time from the most important down, not added
// together. Level 1 is worth less here in raw weight, but paying 5 at level 0
// to save 1 at level 1 is still the right trade.
TEST(SolveTest, PriorityLevelsAreSettledMostImportantFirst) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    :~ p. [1@1]
    :~ q. [5@0]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("q | cost 0 5"));
}

// Every answer set of the optimal cost comes back, not just one of them.
// Covering node 1 and covering node 2 both cost 1.
TEST(SolveTest, AllOptimalAnswerSetsAreEnumerated) {
  auto out = solve_source(R"(
    n(1). n(2).
    in(X) :- n(X), not out(X).
    out(X) :- n(X), not in(X).
    :- out(1), out(2).
    :~ in(X). [1@0, X]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("in(1) n(1) n(2) out(2) | cost 1",
                                         "in(2) n(1) n(2) out(1) | cost 1"));
}

// Two weak constraints whose violations share a weight, level, and terms are
// one violation, so holding both costs 1 rather than 2.
TEST(SolveTest, WeakConstraintsWithTheSameTupleCostOnce) {
  auto out = solve_source(R"(
    a. b.
    :~ a. [1@0, 1]
    :~ b. [1@0, 1]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("a b | cost 1"));
}

// A program with no answer sets still has none once weak constraints are added,
// and reports that rather than an optimal answer set of some cost.
TEST(SolveTest, WeakConstraintOnAnUnsatisfiableProgram) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    :- p.
    :- q.
    :~ p. [1@0]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, IsEmpty());
}

// A weak constraint no answer set can violate leaves a cost of zero. That is
// still a cost, because the program has a level to report on.
TEST(SolveTest, UnviolatedWeakConstraintCostsZero) {
  auto out = solve_source(R"(
    p.
    { q }.
    :- q.
    :~ q. [1@0]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("p | cost 0"));
}

// A negative weight makes violating a weak constraint a reward, so the cheapest
// answer set is the one that violates it. This is the case where the walk down
// the costs cannot stop at zero.
TEST(SolveTest, NegativeWeightIsWorthViolating) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    :~ p. [-2@0]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("p | cost -2"));
}

// Weights of both signs at one level, where the cheapest cost is neither the
// most nor the least violated. Taking the -3 and leaving the 2 pays -3.
TEST(SolveTest, MixedSignWeightsAtOneLevel) {
  auto out = solve_source(R"(
    n(1). n(2).
    in(X) :- n(X), not out(X).
    out(X) :- n(X), not in(X).
    :~ in(1). [-3@0]
    :~ in(2). [2@0]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("in(1) n(1) n(2) out(2) | cost -3"));
}

// A disjunction holds by holding one of its atoms. Holding both satisfies it
// too, but not minimally, so {a, b} is no answer set.
TEST(SolveTest, DisjunctionHoldsOneAtomAtATime) {
  auto out = solve_source("a | b.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("a", "b"));
}

// Two atoms of one predicate are still two atoms, and neither lies on a
// positive cycle.
TEST(SolveTest, DisjunctionOverOnePredicate) {
  auto out = solve_source("p(1) | p(2).");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("p(1)", "p(2)"));
}

// Overlapping disjunctions are where minimality earns its keep. All of {b},
// {a, c} and {a, b} satisfy both rules, but {a, b} holds a without needing it:
// dropping a leaves {b}, which satisfies them both.
TEST(SolveTest, OverlappingDisjunctionsKeepOnlyMinimalModels) {
  auto out = solve_source("a | b. b | c.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("b", "a c"));
}

TEST(SolveTest, ConstraintRulesOutADisjunct) {
  auto out = solve_source("a | b. :- a.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("b"));
}

// A fact among the head atoms satisfies the rule by itself, so the disjunction
// asks nothing of the other atoms and b is never derived.
TEST(SolveTest, DisjunctionSatisfiedByAFactDerivesNothing) {
  auto out = solve_source("a. a | b.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("a"));
}

// One disjunctive rule per node, grounded from the rule's body.
TEST(SolveTest, DisjunctionGroundedOverABody) {
  auto out = solve_source(R"(
    v(1). v(2).
    red(X) | blue(X) :- v(X).
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre(
                        "blue(1) blue(2) v(1) v(2)", "blue(2) red(1) v(1) v(2)",
                        "blue(1) red(2) v(1) v(2)", "red(1) red(2) v(1) v(2)"));
}

// A disjunction feeding a positive cycle: the cycle holds where the disjunction
// reaches it and not otherwise, so both the shift and the level ranking have to
// hold at once.
TEST(SolveTest, DisjunctionFeedingAPositiveCycle) {
  auto out = solve_source(R"(
    a | b.
    p :- a.
    p :- q.
    q :- p.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("a p q", "b"));
}

TEST(SolveTest, DisjunctionUnderAWeakConstraint) {
  auto out = solve_source(R"(
    a | b.
    :~ a. [1@0]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("b | cost 0"));
}

/* A head cycle: a and b head one rule and lie on a common positive cycle. The
   one answer set is {a, b}, where each of the two rests on the other.

   No single query finds it. Shifting the disjunction gives 'a :- not b.' and
   'b :- not a.', which with the cycle has no answer set at all, and no ranking
   puts a below b and b below a at once. The minimality check is what finds it.
   No proper subset of {a, b} models the reduct, since dropping either atom
   leaves the other unsupported. */
TEST(SolveTest, HeadCycleIsDecidedByMinimality) {
  auto out = solve_source(R"(
    a | b.
    a :- b.
    b :- a.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("a b"));
}

// A head cycle that a constraint rules out. {a, b} is the only answer set of
// the rules and it is forbidden, so nothing is left. Every candidate the solver
// offers has to fail the check before the program comes back unsatisfiable.
TEST(SolveTest, HeadCycleWithNoAnswerSet) {
  auto out = solve_source(R"(
    a | b.
    a :- b.
    b :- a.
    :- a.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, IsEmpty());
}

// Both {a, b} and {c} satisfy the rules. The cycle holds together, or c holds
// on its own. Neither contains the other, so both are answer sets and the check
// accepts as well as rejects.
TEST(SolveTest, HeadCycleBesideAnotherDisjunct) {
  auto out = solve_source(R"(
    a | b | c.
    a :- b.
    b :- a.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("a b", "c"));
}

// The minimality check runs inside optimization too, so a cost is always the
// cost of a real answer set. {a, b} costs 2 and {c} costs 0.
TEST(SolveTest, HeadCycleUnderAWeakConstraint) {
  auto out = solve_source(R"(
    a | b | c.
    a :- b.
    b :- a.
    :~ a. [1@0]
    :~ b. [1@0]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("c | cost 0"));
}

// A head cycle over a predicate, grounded per instance, with a rule that only
// the two-atom answer set can fire.
TEST(SolveTest, HeadCycleOverAPredicate) {
  auto out = solve_source(R"(
    n(1). n(2).
    p(1) | p(2).
    p(1) :- p(2).
    p(2) :- p(1).
    q :- p(1), p(2).
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("n(1) n(2) p(1) p(2) q"));
}

// Optimizing has to respect the level ranking like everything else. The cycle
// 'a :- b. b :- a.' cannot support itself, so the cheapest answer set is the
// one where neither holds even though nothing forbids them.
TEST(SolveTest, OptimizationOverAPositiveCycle) {
  auto out = solve_source(R"(
    p :- not q.
    q :- not p.
    a :- p.
    a :- b.
    b :- a.
    :~ a. [1@0]
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("q | cost 0"));
}

// The tests below start from aspif, which is the only way a choice head reaches
// the encoding: pgass normalization writes a choice rule as a disjunction.
absl::StatusOr<std::vector<std::string>> solve_aspif(const std::string& text) {
  ASSIGN_OR_RETURN(aspif::Program prog, aspif::from_aspif(text));
  return render_solutions(prog, SolveOptions{.max_answer_sets = 0});
}

// '{a; b}.' Each atom of a choice is free on its own, which is four answer
// sets rather than the two a disjunction would give.
TEST(SolveTest, ChoiceHeadLeavesEveryCombination) {
  auto out = solve_aspif(
      "asp 1 0 0\n"
      "1 1 2 1 2 0 0\n"
      "4 1 a 1 1\n"
      "4 1 b 1 2\n"
      "0\n");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("", "a", "b", "a b"));
}

// '{a}. b :- a. :- not b.' A choice atom still has to be supported to hold, and
// what it supports in turn holds with it.
TEST(SolveTest, ChoiceHeadSupportsWhatFollowsFromIt) {
  auto out = solve_aspif(
      "asp 1 0 0\n"
      "1 1 1 1 0 0\n"
      "1 0 1 2 0 1 1\n"
      "1 0 0 0 1 -2\n"
      "4 1 a 1 1\n"
      "4 1 b 1 2\n"
      "0\n");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("a b"));
}

// '{a}. b :- a, c. c :- b. c :- a.' The cycle over b and c is reached only
// through the choice, so choosing a is what lets either of them hold.
TEST(SolveTest, ChoiceHeadReachesAPositiveCycle) {
  auto out = solve_aspif(
      "asp 1 0 0\n"
      "1 1 1 1 0 0\n"
      "1 0 1 2 0 2 1 3\n"
      "1 0 1 3 0 1 2\n"
      "1 0 1 3 0 1 1\n"
      "4 1 a 1 1\n"
      "4 1 b 1 2\n"
      "4 1 c 1 3\n"
      "0\n");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("", "a b c"));
}

// 'a :- b. b :- a. a | b :- c. c :- not d. d :- not c. {z} :- d.' The head
// cycle over a and b puts the minimality check in charge of them, and that
// check reads every rule of the program, choice rules included. A choice
// atom the model leaves out forces nothing, so {a, b, d} stays out: a and b
// support only each other once d holds.
TEST(SolveTest, ChoiceHeadLeftOutDoesNotExcuseTheMinimalityCheck) {
  auto out = solve_aspif(
      "asp 1 0 0\n"
      "1 0 1 1 0 1 2\n"
      "1 0 1 2 0 1 1\n"
      "1 0 2 1 2 0 1 3\n"
      "1 0 1 3 0 1 -4\n"
      "1 0 1 4 0 1 -3\n"
      "1 1 1 5 0 1 4\n"
      "4 1 a 1 1\n"
      "4 1 b 1 2\n"
      "4 1 c 1 3\n"
      "4 1 d 1 4\n"
      "4 1 z 1 5\n"
      "0\n");
  ASSERT_OK(out);
  EXPECT_THAT(*out, UnorderedElementsAre("a b c", "d", "d z"));
}

}  // namespace
