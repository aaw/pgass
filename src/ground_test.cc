#include "ground.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "absl/status/statusor.h"
#include "macros.h"
#include "normalize.h"
#include "parse.h"

using ::testing::HasSubstr;
using ::testing::Not;

namespace {

// Parses, normalizes, and grounds `source`, returning the aspif text.
absl::StatusOr<std::string> ground_source(const std::string& source) {
  Parser parser(source);
  ASSIGN_OR_RETURN(auto program, parser.parse_program());
  RETURN_IF_ERROR(normalize(*program));
  ASSIGN_OR_RETURN(aspif::Program grounded, ground(*program));
  return to_aspif(grounded);
}

TEST(GroundTest, Facts) {
  auto out = ground_source("p(1). p(2).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "0\n");
}

TEST(GroundTest, StringAtomPrintsWithOnePairOfQuotes) {
  auto out = ground_source("p(\"a b\").");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "4 8 p(\"a b\") 1 1\n"
            "0\n");
}

TEST(GroundTest, VariableRuleGroundsOncePerMatch) {
  auto out = ground_source("p(1). p(2). q(X) :- p(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 0 1 2\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(1) 1 3\n"
            "4 4 q(2) 1 4\n"
            "0\n");
}

TEST(GroundTest, RecursionReachesFixpoint) {
  auto out = ground_source(
      "e(a, b). e(b, c).\n"
      "r(X, Y) :- e(X, Y).\n"
      "r(X, Z) :- r(X, Y), e(Y, Z).");
  ASSERT_TRUE(out.ok()) << out.status();
  // e(a,b)=1, e(b,c)=2, r(a,b)=3, r(b,c)=4, r(a,c)=5. The last rule fires
  // once: r(a,b) joined with e(b,c) gives r(a,c).
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 0 1 2\n"
            "1 0 1 5 0 2 3 2\n"
            "4 6 e(a,b) 1 1\n"
            "4 6 e(b,c) 1 2\n"
            "4 6 r(a,b) 1 3\n"
            "4 6 r(b,c) 1 4\n"
            "4 6 r(a,c) 1 5\n"
            "0\n");
}

TEST(GroundTest, NonRecursiveRuleSeesFullRecursiveFixpoint) {
  // t/1 depends on r/2, which is recursive. If grounding ever processes t's
  // rule before r has reached its fixpoint, t will be missing atoms. This
  // pins down the current (correct) behavior so a future component-by-
  // component rewrite of derive_atoms can be checked against it.
  auto out = ground_source(
      "e(a, b). e(b, c).\n"
      "r(X, Y) :- e(X, Y).\n"
      "r(X, Z) :- r(X, Y), e(Y, Z).\n"
      "t(X) :- r(X, Y).");
  ASSERT_TRUE(out.ok()) << out.status();
  // e(a,b)=1, e(b,c)=2, r(a,b)=3, r(b,c)=4, r(a,c)=5, t(a)=6, t(b)=7. t(a)
  // has two support rules: one from r(a,b), one from r(a,c), the atom r's
  // recursion only derives on its second pass.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 0 1 2\n"
            "1 0 1 5 0 2 3 2\n"
            "1 0 1 6 0 1 3\n"
            "1 0 1 7 0 1 4\n"
            "1 0 1 6 0 1 5\n"
            "4 6 e(a,b) 1 1\n"
            "4 6 e(b,c) 1 2\n"
            "4 6 r(a,b) 1 3\n"
            "4 6 r(b,c) 1 4\n"
            "4 6 r(a,c) 1 5\n"
            "4 4 t(a) 1 6\n"
            "4 4 t(b) 1 7\n"
            "0\n");
}

TEST(GroundTest, NegationAgainstUnrelatedRecursiveComponentKeepsDerivedAtom) {
  // r/2 is its own recursive component and has no positive-dependency link
  // to p/1 or s/1; s only refers to r through 'not'. r(a, c) only shows up
  // once the recursion reaches its second pass, so this checks that emission
  // sees it as derived and keeps 'not r(a, c)' in the body. If rules were
  // ever emitted before r's component had fully run, this would wrongly look
  // like r(a, c) is underivable and drop the negation instead.
  auto out = ground_source(
      "p(1). p(2).\n"
      "e(a, b). e(b, c).\n"
      "r(X, Y) :- e(X, Y).\n"
      "r(X, Z) :- r(X, Y), e(Y, Z).\n"
      "s(N) :- p(N), not r(a, c).");
  ASSERT_TRUE(out.ok()) << out.status();
  // Components now derive in topological order before anything is emitted,
  // so e/r (r's component has no positive edge to p or s) derive before
  // p/s: e(a,b)=1, e(b,c)=2, r(a,b)=3, r(b,c)=4, r(a,c)=5, p(1)=6, p(2)=7.
  // r(a,c) is derived through the recursion, so 'not r(a, c)' stays in both
  // s(1) and s(2)'s bodies, negated: 's(1) :- p(1), not r(a, c).' and
  // likewise for s(2).
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 0 1 2\n"
            "1 0 1 5 0 2 3 2\n"
            "1 0 1 6 0 0\n"
            "1 0 1 7 0 0\n"
            "1 0 1 8 0 2 6 -5\n"
            "1 0 1 9 0 2 7 -5\n"
            "4 6 e(a,b) 1 1\n"
            "4 6 e(b,c) 1 2\n"
            "4 6 r(a,b) 1 3\n"
            "4 6 r(b,c) 1 4\n"
            "4 6 r(a,c) 1 5\n"
            "4 4 p(1) 1 6\n"
            "4 4 p(2) 1 7\n"
            "4 4 s(1) 1 8\n"
            "4 4 s(2) 1 9\n"
            "0\n");
}

TEST(GroundTest, NegationAgainstLaterComponentKeepsDerivedAtom) {
  // c/2 is defined before q/1 and p/1 in the source, so its component id ends
  // up *higher* than p's: p's component has no positive edge into c (only
  // 'not c' references it), while q positively reaches p, pulling p's whole
  // component earlier in topological order. If grounding ever emits a
  // component's rules right after deriving that component, instead of
  // deriving every component first, it would process p before c has any
  // atoms and wrongly conclude 'not c(x, z)' can never fail, dropping the
  // negation instead of keeping it.
  auto out = ground_source(
      "a(x, y). a(y, z).\n"
      "c(X, Y) :- a(X, Y).\n"
      "c(X, Z) :- c(X, Y), a(Y, Z).\n"
      "q(1). q(2).\n"
      "p(N) :- q(N), not c(x, z).");
  ASSERT_TRUE(out.ok()) << out.status();
  // q's component (id 0) derives before p's (id 1), which derives before
  // a/c's (ids 2-3): q(1)=1, q(2)=2, p(1)=3, p(2)=4, a(x,y)=5, a(y,z)=6,
  // c(x,y)=7, c(y,z)=8, c(x,z)=9. c(x,z) is only derived on c's second pass,
  // after p has already been derived, but since nothing is emitted until
  // every component has derived, 'not c(x, z)' still stays in both p(1) and
  // p(2)'s bodies, negated.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 2 1 -9\n"
            "1 0 1 4 0 2 2 -9\n"
            "1 0 1 5 0 0\n"
            "1 0 1 6 0 0\n"
            "1 0 1 7 0 1 5\n"
            "1 0 1 8 0 1 6\n"
            "1 0 1 9 0 2 7 6\n"
            "4 4 q(1) 1 1\n"
            "4 4 q(2) 1 2\n"
            "4 4 p(1) 1 3\n"
            "4 4 p(2) 1 4\n"
            "4 6 a(x,y) 1 5\n"
            "4 6 a(y,z) 1 6\n"
            "4 6 c(x,y) 1 7\n"
            "4 6 c(y,z) 1 8\n"
            "4 6 c(x,z) 1 9\n"
            "0\n");
}

TEST(GroundTest, NegationKeptWhenPossibleDroppedWhenNot) {
  auto out = ground_source("p(1). p(2). q(1). s(X) :- p(X), not q(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  // q/1 has no positive-dependency link to p/1 or s/1, so its singleton
  // component derives before p's: q(1)=1, p(1)=2, p(2)=3, s(1)=4, s(2)=5.
  // q(1) is derivable, so 's(1) :- p(1), not q(1).' keeps the negation.
  // q(2) is not, so 'not q(2)' is dropped: 's(2) :- p(2).'.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 2 2 -1\n"
            "1 0 1 5 0 1 3\n"
            "4 4 q(1) 1 1\n"
            "4 4 p(1) 1 2\n"
            "4 4 p(2) 1 3\n"
            "4 4 s(1) 1 4\n"
            "4 4 s(2) 1 5\n"
            "0\n");
}

TEST(GroundTest, ComparisonFiltersInstances) {
  auto out = ground_source("p(1). p(2). q(X) :- p(X), X < 2.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(1) 1 3\n"
            "0\n");
}

TEST(GroundTest, ProgramWithNoPredicates) {
  // Nothing here names a predicate, so the dependency graph has no nodes at
  // all and there are no components to ground. The constraint still comes
  // out, with a body that's empty because '1 < 2' is decided at grounding.
  auto out = ground_source(":- 1 < 2.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 0 0 0\n"
            "0\n");
}

TEST(GroundTest, ConstraintHasNoHead) {
  auto out = ground_source("p(1). q(1). :- p(X), q(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  // p and q are each their own singleton component with no edges between
  // them (a constraint's body predicates get no outgoing edges at all), so
  // q's component happens to derive first: q(1)=1, p(1)=2.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 0 0 2 2 1\n"
            "4 4 q(1) 1 1\n"
            "4 4 p(1) 1 2\n"
            "0\n");
}

TEST(GroundTest, ZeroArityAtoms) {
  auto out = ground_source("p. q :- not p.");
  ASSERT_TRUE(out.ok()) << out.status();
  // p and q are each singleton components with no positive edge between
  // them ('not p' doesn't count), so q's component happens to derive first:
  // q=1, p=2. 'not p' is still kept since p is derivable by emit time.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 1 -2\n"
            "1 0 1 2 0 0\n"
            "4 1 q 1 1\n"
            "4 1 p 1 2\n"
            "0\n");
}

TEST(GroundTest, CountAggregateLowerBound) {
  // p=1, q=2. Emitting q's rule allocates the element's aux atom (3, backed
  // by 'p') and the '>= 1' bound-check atom (4, a weight rule over {3: 1}).
  auto out = ground_source("p. q :- #count{ 1 : p } > 0.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 1 1 1 3 1\n"
            "1 0 1 2 0 1 4\n"
            "4 1 p 1 1\n"
            "4 1 q 1 2\n"
            "0\n");
}

TEST(GroundTest, SumAggregateWeighsByFirstTerm) {
  // w(1,3)=1, w(2,5)=2, q=3. The element's first term (the weight) is V, not
  // I: aux atoms 4 and 5 back the two element instances, and the weight rule
  // sums their weights (3 and 5) rather than counting them.
  auto out = ground_source("w(1,3). w(2,5). q :- #sum{ V,I : w(I,V) } >= 4.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 4 0 1 1\n"
            "1 0 1 5 0 1 2\n"
            "1 0 1 6 1 4 2 4 3 5 5\n"
            "1 0 1 3 0 1 6\n"
            "4 6 w(1,3) 1 1\n"
            "4 6 w(2,5) 1 2\n"
            "4 1 q 1 3\n"
            "0\n");
}

TEST(GroundTest, CountAggregateCombinedLowerAndUpperBound) {
  // Both bound sides are present, so both a "low_ok" atom (8, count >= 1)
  // and a "high_bad" atom (9, count >= 3) get defined, and q's rule requires
  // low_ok and not high_bad together.
  auto out =
      ground_source("p(1). p(2). p(3). q :- 1 <= #count{ X : p(X) } <= 2.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 5 0 1 1\n"
            "1 0 1 6 0 1 2\n"
            "1 0 1 7 0 1 3\n"
            "1 0 1 8 1 1 3 5 1 6 1 7 1\n"
            "1 0 1 9 1 3 3 5 1 6 1 7 1\n"
            "1 0 1 4 0 2 8 -9\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 p(3) 1 3\n"
            "4 1 q 1 4\n"
            "0\n");
}

TEST(GroundTest, NegatedAggregateNegatesTheWholeBoundConjunction) {
  // This is the shape normalize.cc's choice-cardinality translation produces
  // (':- body, not #count{...} <bounds>.'). Since both bound sides are
  // present, negating requires a conjunction atom (7 := low_ok(5) and not
  // high_bad(6)) before the constraint can require 'not' of that.
  auto out = ground_source("p(1). p(2). :- not 1 <= #count{ X : p(X) } <= 2.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 0 1 2\n"
            "1 0 1 5 1 1 2 3 1 4 1\n"
            "1 0 1 6 1 3 2 3 1 4 1\n"
            "1 0 1 7 0 2 5 -6\n"
            "1 0 0 0 1 -7\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "0\n");
}

TEST(GroundTest, CountAggregateEqualityUsesLowAndHighAuxAtoms) {
  // '= 2' needs both a low_eq (count >= 2) and high_bad_eq (count >= 3) atom,
  // combined into eq_ok (6 := low_eq and not high_bad_eq); q's rule requires
  // eq_ok directly, != would require 'not eq_ok' instead.
  auto out = ground_source("p(1). p(2). q :- #count{ X : p(X) } = 2.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 4 0 1 1\n"
            "1 0 1 5 0 1 2\n"
            "1 0 1 6 1 2 2 4 1 5 1\n"
            "1 0 1 7 1 3 2 4 1 5 1\n"
            "1 0 1 3 0 2 6 -7\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 1 q 1 3\n"
            "0\n");
}

TEST(GroundTest, AggregateDedupesEqualTuplesAcrossElements) {
  // Two elements ('X : p(X)' and 'X : r(X)') both produce the tuple [1], so
  // they share one aux atom (4) supported by two rules (one per element),
  // and the weight rule only counts that shared atom once: the count can
  // never reach 2, so q never derives even though both p(1) and r(1) hold.
  auto out =
      ground_source("p(1). r(1). q :- #count{ X : p(X) ; X : r(X) } >= 2.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 4 0 1 2\n"
            "1 0 1 4 0 1 1\n"
            "1 0 1 5 1 2 1 4 1\n"
            "1 0 1 3 0 1 5\n"
            "4 4 r(1) 1 1\n"
            "4 4 p(1) 1 2\n"
            "4 1 q 1 3\n"
            "0\n");
}

TEST(GroundTest, WeakConstraintBecomesMinimize) {
  auto out = ground_source("p(1). p(2). :~ p(X). [1@0, X]");
  ASSERT_TRUE(out.ok()) << out.status();
  // p(1)=1, p(2)=2, then the _viol atoms normalization derived from the weak
  // constraint: _viol(0,1,1)=3 and _viol(0,1,2)=4. Both sit at level 0 with
  // weight 1, so they share one minimize statement. _viol starts with '_',
  // so it stays out of the output statements.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 0 1 2\n"
            "2 0 2 3 1 4 1\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "0\n");
}

TEST(GroundTest, WeakConstraintsGroupByLevel) {
  // Three weak constraints across two levels. The last one carries no terms,
  // so it becomes _viol/2 while the others become _viol/3 -- different
  // predicates in the store, but level 2 collects literals from both.
  auto out = ground_source(
      "a(1). b(2).\n"
      ":~ a(X). [1@0, X]\n"
      ":~ b(X). [3@2, X]\n"
      ":~ a(X). [5@2]");
  ASSERT_TRUE(out.ok()) << out.status();
  // b(2)=1, a(1)=2, _viol(2,5)=3, _viol(0,1,1)=4, _viol(2,3,2)=5. Levels come
  // out ascending: level 0 holds _viol(0,1,1) at weight 1; level 2 holds
  // _viol(2,5) at weight 5 and _viol(2,3,2) at weight 3.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 2\n"
            "1 0 1 4 0 1 2\n"
            "1 0 1 5 0 1 1\n"
            "2 0 1 4 1\n"
            "2 2 2 3 5 5 3\n"
            "4 4 b(2) 1 1\n"
            "4 4 a(1) 1 2\n"
            "0\n");
}

TEST(GroundTest, WeakConstraintCountsEqualViolationsOnce) {
  // Both weak constraints produce the tuple (0, 1, 1), which is one ground
  // _viol atom however many rules derive it. So atom 3 gets two support
  // rules but appears once in the minimize statement: violating both costs 1
  // in total, not 2.
  auto out = ground_source(
      "a(1). b(1).\n"
      ":~ a(X). [1@0, X]\n"
      ":~ b(X). [1@0, X]");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 2\n"
            "1 0 1 3 0 1 1\n"
            "2 0 1 3 1\n"
            "4 4 b(1) 1 1\n"
            "4 4 a(1) 1 2\n"
            "0\n");
}

TEST(GroundTest, WeakConstraintNonNumericWeightCostsNothing) {
  // The cost at a level sums the integer weights and ignores the rest, so
  // the violation weighing 'a' drops out and only the one weighing 1 is
  // minimized.
  auto out = ground_source("p(a). p(1). :~ p(X). [X@0]");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 0 1 2\n"
            "2 0 1 4 1\n"
            "4 4 p(a) 1 1\n"
            "4 4 p(1) 1 2\n"
            "0\n");
}

TEST(GroundTest, SumIgnoresTuplesWithoutAnIntegerFirstTerm) {
  // #sum adds up only the tuples whose first term is an integer, so q(a)
  // contributes nothing and the weight body holds one literal, for p(1).
  auto out = ground_source("p(1). q(a). r :- #sum{ X : p(X); Y : q(Y) } >= 1.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("1 0 1 5 1 1 1 4 1\n"));
}

TEST(GroundTest, CountIncludesTuplesWithoutAnIntegerFirstTerm) {
  // #count counts every tuple in the set, unlike #sum: both p(1) and q(a)
  // land in the weight body, so the count reaches 2.
  auto out =
      ground_source("p(1). q(a). r :- #count{ X : p(X); Y : q(Y) } >= 2.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("1 0 1 6 1 2 2 4 1 5 1\n"));
}

TEST(GroundTest, NegativeSumWeightFlipsTheLiteral) {
  // ASPIF weights must be positive, so '-1 * [tuple 3]' becomes
  // '-1 + 1 * [not tuple 3]' and the bound moves from 0 to 1: atom 4 holds
  // exactly when p's tuple is false, which is when the sum reaches 0.
  auto out = ground_source("p. q :- #sum{ -1 : p } >= 0.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 1 1 1 -3 1\n"
            "1 0 1 2 0 1 4\n"
            "4 1 p 1 1\n"
            "4 1 q 1 2\n"
            "0\n");
}

TEST(GroundTest, BoundNoPositiveWeightCanMissBecomesAFact) {
  // The sum is -1 or 0, so '>= -1' always holds. After the flip the bound is
  // 0, which positive weights always reach, so atom 4 is a plain fact.
  auto out = ground_source("p. q :- #sum{ -1 : p } >= -1.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 0 0\n"
            "1 0 1 2 0 1 4\n"
            "4 1 p 1 1\n"
            "4 1 q 1 2\n"
            "0\n");
}

TEST(GroundTest, MixedSumWeightsFlipOnlyTheNegativeOnes) {
  // '2 * [p] - 1 * [q] >= 2' becomes '2 * [p] + 1 * [not q] >= 3', which
  // holds exactly when p is true and q is false.
  auto out = ground_source("{p}. {q}. r :- #sum{ 2 : p; -1 : q } >= 2.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("1 0 1 8 1 3 2 6 2 -7 1\n"));
}

TEST(GroundTest, ZeroSumWeightIsLeftOutOfTheWeightBody) {
  // A tuple weighing 0 never changes the sum, so it gets no literal at all
  // and the bound of 0 leaves atom 4 a fact.
  auto out = ground_source("p. q :- #sum{ 0 : p } >= 0.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("1 0 1 4 0 0\n"));
}

TEST(GroundTest, MinMaxAggregatesRejected) {
  auto out = ground_source("p(1). q :- #max{ X : p(X) } >= 1.");
  ASSERT_FALSE(out.ok());
  EXPECT_THAT(std::string(out.status().message()), HasSubstr("#max"));
}

TEST(GroundTest, ArithmeticInHeadTerm) {
  auto out = ground_source("p(1). p(2). q(X + 1) :- p(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 0 1 2\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(2) 1 3\n"
            "4 4 q(3) 1 4\n"
            "0\n");
}

TEST(GroundTest, ArithmeticInComparison) {
  auto out = ground_source("p(1). p(2). p(3). q(X) :- p(X), X < 1 + 1.");
  ASSERT_TRUE(out.ok()) << out.status();
  // Only p(1) passes 'X < 2', so q(1) is the sole q atom.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 1 1\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 p(3) 1 3\n"
            "4 4 q(1) 1 4\n"
            "0\n");
}

TEST(GroundTest, MultiplicationEvaluatesBeforeAddition) {
  auto out = ground_source("p(1). q(2 + X * 3) :- p(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  // 2 + 1 * 3 is 5, not 9.
  EXPECT_THAT(*out, HasSubstr("q(5)"));
}

TEST(GroundTest, ParenthesesEvaluateBeforeMultiplication) {
  auto out = ground_source("p(1). q((2 + X) * 3) :- p(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("q(9)"));
}

TEST(GroundTest, DivisionTruncates) {
  auto out = ground_source("p(7). q(X / 2) :- p(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("q(3)"));
}

TEST(GroundTest, UnaryMinusGivesNegativeNumbers) {
  auto out = ground_source("p(1). q(-X) :- p(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 1 1\n"
            "4 4 p(1) 1 1\n"
            "4 5 q(-1) 1 2\n"
            "0\n");
}

TEST(GroundTest, NegativeNumbersCompareBelowPositiveOnes) {
  auto out = ground_source("v(1). v(2). lt(X, Y) :- v(X), v(Y), -X < -Y.");
  ASSERT_TRUE(out.ok()) << out.status();
  // -2 < -1, so lt(2,1) holds and lt(1,2) does not.
  EXPECT_THAT(*out, HasSubstr("lt(2,1)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("lt(1,2)")));
}

// ASP-Core-2 grounds a rule only over well-formed substitutions: one that
// leaves an arithmetic term undefined builds no ground instance. So the
// instances below are dropped one by one, and grounding still succeeds.

TEST(GroundTest, ArithmeticOnNonNumberDropsTheInstance) {
  auto out = ground_source("p(a). p(1). q(X) :- p(X), 1 < X + 1.");
  ASSERT_TRUE(out.ok()) << out.status();
  // 'a + 1' has no value, so X = a is dropped and only q(1) survives.
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(a)")));
}

TEST(GroundTest, DivisionByZeroDropsTheInstance) {
  auto out = ground_source("p(0). p(1). q(X) :- p(X), 2 / X > 1.");
  ASSERT_TRUE(out.ok()) << out.status();
  // '2 / 0' has no value, so X = 0 is dropped; 2 / 1 > 1 gives q(1).
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(0)")));
}

TEST(GroundTest, IllFormedHeadTermDropsTheInstance) {
  auto out = ground_source("p(0). p(2). q(4 / X) :- p(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 2\n"
            "4 4 p(0) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(2) 1 3\n"
            "0\n");
}

TEST(GroundTest, IllFormedNegativeLiteralDropsTheInstance) {
  auto out = ground_source("p(0). p(2). r(1). q(X) :- p(X), not r(4 / X).");
  ASSERT_TRUE(out.ok()) << out.status();
  // X = 0 makes 'not r(4 / 0)' ill-formed, so that whole instance is gone,
  // not just the literal. X = 2 gives 'not r(2)', which r/1 never derived,
  // so q(2) comes out as a fact.
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(0)")));
}

TEST(GroundTest, IllFormedAggregateBoundDropsTheInstance) {
  auto out = ground_source(
      "d(0). d(2). p(1). q(X) :- d(X), #count{ Y : p(Y) } >= 4 / X.");
  ASSERT_TRUE(out.ok()) << out.status();
  // X = 0 makes the bound '4 / 0' ill-formed, so no q(0) is derived at all.
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(0)")));
}

TEST(GroundTest, FunctionTermsMatchArgumentByArgument) {
  auto out = ground_source(
      "p(f(1)). p(f(2)). p(g(a, b)).\n"
      "q(X) :- p(f(X)).\n"
      "r(Y) :- p(g(a, Y)).");
  ASSERT_TRUE(out.ok()) << out.status();
  // p(f(1))=1, p(f(2))=2, p(g(a,b))=3, r(b)=4, q(1)=5, q(2)=6. r's rule comes
  // first because r/1 and q/1 are separate components and r's is grounded
  // first.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 1 3\n"
            "1 0 1 5 0 1 1\n"
            "1 0 1 6 0 1 2\n"
            "4 7 p(f(1)) 1 1\n"
            "4 7 p(f(2)) 1 2\n"
            "4 9 p(g(a,b)) 1 3\n"
            "4 4 r(b) 1 4\n"
            "4 4 q(1) 1 5\n"
            "4 4 q(2) 1 6\n"
            "0\n");
}

TEST(GroundTest, NestedFunctionTerms) {
  auto out = ground_source(
      "p(f(g(1))).\n"
      "q(X) :- p(f(g(X))).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 1 1\n"
            "4 10 p(f(g(1))) 1 1\n"
            "4 4 q(1) 1 2\n"
            "0\n");
}

TEST(GroundTest, FunctionTermNeedsSameNameAndArity) {
  auto out = ground_source(
      "p(f(1)). p(g(1)). p(f(1, 2)).\n"
      "q(X) :- p(f(X)).");
  ASSERT_TRUE(out.ok()) << out.status();
  // Only p(f(1)) matches 'p(f(X))': g(1) has a different name and f(1,2) a
  // different arity, so q(1) is the sole q atom.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 1 1\n"
            "4 7 p(f(1)) 1 1\n"
            "4 7 p(g(1)) 1 2\n"
            "4 9 p(f(1,2)) 1 3\n"
            "4 4 q(1) 1 4\n"
            "0\n");
}

TEST(GroundTest, RepeatedVariableInsideFunctionTerm) {
  auto out = ground_source(
      "p(f(1, 1)). p(f(1, 2)).\n"
      "q(X) :- p(f(X, X)).");
  ASSERT_TRUE(out.ok()) << out.status();
  // f(1,2) can't bind X to both 1 and 2, so only f(1,1) gives q(1).
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "4 9 p(f(1,1)) 1 1\n"
            "4 9 p(f(1,2)) 1 2\n"
            "4 4 q(1) 1 3\n"
            "0\n");
}

TEST(GroundTest, FunctionTermsSortAfterEveryAtomicValue) {
  // ASP-Core-2 orders integers < symbolic constants < string constants <
  // function terms, so lt/2 holds for every pair in that order.
  auto out = ground_source(
      "v(1). v(a). v(\"s\"). v(f(1)).\n"
      "lt(X, Y) :- v(X), v(Y), X < Y.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("lt(1,a)"));
  EXPECT_THAT(*out, HasSubstr("lt(a,\"s\")"));
  EXPECT_THAT(*out, HasSubstr("lt(1,f(1))"));
  EXPECT_THAT(*out, HasSubstr("lt(a,f(1))"));
  EXPECT_THAT(*out, HasSubstr("lt(\"s\",f(1))"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("lt(f(1),")));
}

// An equality with an unbound variable on one side is an assignment: it gives
// the variable the value of the other side rather than comparing the two.

TEST(GroundTest, AssignmentBindsAVariable) {
  auto out = ground_source("p(1). p(2). q(Y) :- p(X), Y = X + 1.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 4 0 1 2\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(2) 1 3\n"
            "4 4 q(3) 1 4\n"
            "0\n");
}

TEST(GroundTest, AssignmentWorksWithTheVariableOnTheRight) {
  auto out = ground_source("p(1). q(Y) :- p(X), X + 1 = Y.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("q(2)"));
}

TEST(GroundTest, AssignmentChainsThroughAnotherAssignment) {
  // Z takes its value from Y, which the assignment to its right binds, so the
  // two are made in the opposite order from how they are written.
  auto out = ground_source("p(1). q(Z) :- p(X), Z = Y + 1, Y = X + 1.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("q(3)"));
}

TEST(GroundTest, AssignmentWithNoPositiveLiteralAtAll) {
  auto out = ground_source("q(X) :- X = 1 + 1.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "4 4 q(2) 1 1\n"
            "0\n");
}

TEST(GroundTest, AssignmentBindsANonNumberValue) {
  auto out = ground_source("p(a). q(Y) :- p(X), Y = f(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("q(f(a))"));
}

TEST(GroundTest, AssignmentToABoundVariableIsAComparison) {
  auto out =
      ground_source("p(1, 2). p(3, 4). p(5, 5). q(X) :- p(X, Y), X = Y.");
  ASSERT_TRUE(out.ok()) << out.status();
  // X is already bound by p, so 'X = Y' filters instead of assigning.
  EXPECT_THAT(*out, HasSubstr("q(5)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(1)")));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(3)")));
}

TEST(GroundTest, IllFormedAssignmentDropsTheInstance) {
  auto out = ground_source("p(0). p(2). q(Y) :- p(X), Y = 4 / X.");
  ASSERT_TRUE(out.ok()) << out.status();
  // '4 / 0' has no value, so X = 0 binds nothing and derives no q at all.
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_EQ(out->find("q("), out->rfind("q("));
}

TEST(GroundTest, AssignmentBoundVariableUsedByALaterLiteral) {
  auto out = ground_source(
      "p(1). p(2). r(2).\n"
      "q(X) :- p(X), Y = X + 1, not r(Y).");
  ASSERT_TRUE(out.ok()) << out.status();
  // X = 1 gives Y = 2, and r(2) is derived, so q(1) keeps 'not r(2)' in its
  // body. X = 2 gives Y = 3, which r/1 never derived, so q(2) is a fact.
  // r(2)=1, p(1)=2, p(2)=3, q(1)=4, q(2)=5: r/1 is its own component and is
  // grounded before p/1.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 2 2 -1\n"
            "1 0 1 5 0 1 3\n"
            "4 4 r(2) 1 1\n"
            "4 4 p(1) 1 2\n"
            "4 4 p(2) 1 3\n"
            "4 4 q(1) 1 4\n"
            "4 4 q(2) 1 5\n"
            "0\n");
}

TEST(GroundTest, InequalityDoesNotAssign) {
  auto out = ground_source("p(1). p(2). q(X) :- p(X), X != 2.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(2)")));
}

// '#count{...} = S' with S unbound binds S to the aggregate's value. The
// value depends on which atoms the solver makes true, so the rule is ground
// once per value the aggregate can take, and each instance keeps the literals
// that check for that value.

TEST(GroundTest, CountAggregateValueBindsAVariable) {
  auto out = ground_source("p. q(S) :- #count{ 1 : p } = S.");
  ASSERT_TRUE(out.ok()) << out.status();
  // The count is 0 or 1, so q(0)=2 and q(1)=3 are both ground. Each gets its
  // own copy of the element's aux atom and bound checks: q(0) needs 'count >=
  // 0' (5, a fact) and not 'count >= 1' (6), q(1) needs 'count >= 1' (8) and
  // not 'count >= 2' (9). p is a fact, so a solver keeps only q(1).
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 4 0 1 1\n"
            "1 0 1 5 0 0\n"
            "1 0 1 6 1 1 1 4 1\n"
            "1 0 1 2 0 2 5 -6\n"
            "1 0 1 7 0 1 1\n"
            "1 0 1 8 1 1 1 7 1\n"
            "1 0 1 9 1 2 1 7 1\n"
            "1 0 1 3 0 2 8 -9\n"
            "4 1 p 1 1\n"
            "4 4 q(0) 1 2\n"
            "4 4 q(1) 1 3\n"
            "0\n");
}

TEST(GroundTest, EmptyAggregateValueBindsZero) {
  // No element can produce a tuple, so the only value the count can take is 0.
  auto out = ground_source("q(S) :- #count{ X : p(X) } = S.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 1 1 0\n"
            "1 0 1 1 0 2 2 -3\n"
            "4 4 q(0) 1 1\n"
            "0\n");
}

TEST(GroundTest, AggregateValueBindsFromEitherSide) {
  auto out = ground_source("p(1). p(2). q(S) :- S = #count{ X : p(X) }.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("q(0)"));
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(3)")));
}

TEST(GroundTest, SumAggregateValueRangesOverSubsetSums) {
  // Any subset of the two tuples can be in the set, so the sum is 0, 3, 5, or
  // 8 -- not every number in between.
  auto out =
      ground_source("w(1,3). w(2,5). total(S) :- #sum{ V,I : w(I,V) } = S.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("total(0)"));
  EXPECT_THAT(*out, HasSubstr("total(3)"));
  EXPECT_THAT(*out, HasSubstr("total(5)"));
  EXPECT_THAT(*out, HasSubstr("total(8)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("total(1)")));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("total(4)")));
}

TEST(GroundTest, NegativeSumWeightsReachNegativeValues) {
  auto out = ground_source("w(-2). w(3). total(S) :- #sum{ W : w(W) } = S.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("total(-2)"));
  EXPECT_THAT(*out, HasSubstr("total(0)"));
  EXPECT_THAT(*out, HasSubstr("total(1)"));
  EXPECT_THAT(*out, HasSubstr("total(3)"));
}

TEST(GroundTest, AggregateValueFeedsAnAssignment) {
  auto out =
      ground_source("p(1). p(2). q(T) :- #count{ X : p(X) } = S, T = S + 1.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, HasSubstr("q(3)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(0)")));
}

TEST(GroundTest, ComparisonOnAnAggregateValueDropsInstances) {
  // 'S > 1' is checked once the aggregate binds S, and rules out the
  // instances for the values 0 and 1.
  auto out = ground_source(
      "p(1). p(2). p(3). big(S) :- #count{ X : p(X) } = S, S > 1.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("big(2)"));
  EXPECT_THAT(*out, HasSubstr("big(3)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("big(0)")));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("big(1)")));
}

TEST(GroundTest, AggregateValueBindsOncePerOuterInstance) {
  auto out = ground_source(
      "g(1). g(2). e(1,a). e(1,b). e(2,c).\n"
      "deg(G, C) :- g(G), #count{ Y : e(G, Y) } = C.");
  ASSERT_TRUE(out.ok()) << out.status();
  // Group 1 has two edges and group 2 has one, so the counts range up to 2
  // for g(1) and up to 1 for g(2).
  EXPECT_THAT(*out, HasSubstr("deg(1,2)"));
  EXPECT_THAT(*out, HasSubstr("deg(2,1)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("deg(2,2)")));
}

TEST(GroundTest, AggregateWaitsForAVariableAnotherAggregateBinds) {
  // The #count mentions X, which the #sum to its right binds, so the #count
  // is ground once the #sum has picked a value for X. To the #count on its
  // own, an unbound X reads as a local variable.
  auto out = ground_source(
      "n(2). e(0,x). e(2,b). e(2,c).\n"
      "q(C) :- #count{ Y : e(X, Y) } = C, X = #sum{ Z : n(Z) }.");
  ASSERT_TRUE(out.ok()) << out.status();
  // The #sum is 0 or 2. X = 0 matches only e(0,x), so C is 0 or 1; X = 2
  // matches e(2,b) and e(2,c), so C reaches 2.
  EXPECT_THAT(*out, HasSubstr("q(0)"));
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(3)")));
}

TEST(GroundTest, TwoAggregatesEachBindTheirOwnVariable) {
  auto out = ground_source(
      "p(1). q(1).\n"
      "r(A, B) :- #count{ X : p(X) } = A, #count{ Y : q(Y) } = B.");
  ASSERT_TRUE(out.ok()) << out.status();
  // Each count is 0 or 1 on its own, so the rule grounds over all four pairs.
  EXPECT_THAT(*out, HasSubstr("r(0,0)"));
  EXPECT_THAT(*out, HasSubstr("r(0,1)"));
  EXPECT_THAT(*out, HasSubstr("r(1,0)"));
  EXPECT_THAT(*out, HasSubstr("r(1,1)"));
}

TEST(GroundTest, NegatedAggregateDoesNotBindItsValue) {
  // 'not #count{...} = S' only says the count differs from S, which pins S to
  // nothing, so S stays unbound.
  auto out = ground_source("p(1). q(S) :- not #count{ X : p(X) } = S.");
  EXPECT_FALSE(out.ok());
  EXPECT_THAT(out.status().message(),
              HasSubstr("variable 'S' is not bound by the rule body"));
}

TEST(GroundTest, TooManyAggregateValuesRejected) {
  // Powers of two give every sum from 0 to 2^13 - 1, past the cap on how many
  // values one rule may be ground over.
  auto out = ground_source(
      "w(1). w(2). w(4). w(8). w(16). w(32). w(64). w(128). w(256). w(512).\n"
      "w(1024). w(2048). w(4096).\n"
      "total(S) :- #sum{ W : w(W) } = S.");
  EXPECT_FALSE(out.ok());
  EXPECT_THAT(out.status().message(), HasSubstr("more than 4096"));
}

TEST(GroundTest, GroundQueryAssumesItsAtom) {
  auto out = ground_source("p(1). p(2). p(2)?");
  ASSERT_TRUE(out.ok()) << out.status();
  // p(1)=1, p(2)=2. Only p(2) matches, so it is assumed directly.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "6 1 2\n"
            "0\n");
}

TEST(GroundTest, QueryWithAVariableAssumesAnyMatch) {
  auto out = ground_source("p(1). p(2). p(X)?");
  ASSERT_TRUE(out.ok()) << out.status();
  // p(1)=1, p(2)=2, and atom 3 holds when either of them does.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 1\n"
            "1 0 1 3 0 1 2\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "6 1 3\n"
            "0\n");
}

TEST(GroundTest, QueryMatchesOnlyTheAtomsWithTheSameArguments) {
  auto out = ground_source("p(1, a). p(2, b). p(X, a)?");
  ASSERT_TRUE(out.ok()) << out.status();
  // Only p(1,a)=1 matches, so it is assumed directly.
  EXPECT_THAT(*out, HasSubstr("6 1 1\n"));
}

TEST(GroundTest, QueryOverADerivedPredicate) {
  auto out = ground_source("p(1). q(X) :- p(X). q(1)?");
  ASSERT_TRUE(out.ok()) << out.status();
  // p(1)=1, q(1)=2.
  EXPECT_THAT(*out, HasSubstr("6 1 2\n"));
}

TEST(GroundTest, QueryThatMatchesNothingCanNeverHold) {
  auto out = ground_source("p(1). p(3)?");
  ASSERT_TRUE(out.ok()) << out.status();
  // Atom 2 has no rule deriving it, so assuming it leaves no answer set.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "4 4 p(1) 1 1\n"
            "6 1 2\n"
            "0\n");
}

TEST(GroundTest, QueryOverAPredicateWithNoAtomsAtAll) {
  auto out = ground_source("p(1). q(X)?");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("6 1 2\n"));
}

TEST(GroundTest, QueryOnAClassicallyNegatedLiteral) {
  // normalize() rewrites '-p(1)' into '_neg_p(1)', in the query as well as in
  // the rules, so the query still finds the atom.
  auto out = ground_source("-p(1). -p(1)?");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("6 1 1\n"));
}

TEST(GroundTest, ClassicallyNegatedAtomPrintsUnderItsOriginalName) {
  auto out = ground_source("-p(1). p(2).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("4 4 p(2) 1 1\n"));
  EXPECT_THAT(*out, HasSubstr("4 5 -p(1) 1 2\n"));
}

TEST(GroundTest, ZeroArityClassicallyNegatedAtomPrintsUnderItsOriginalName) {
  auto out = ground_source("-p.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, HasSubstr("4 2 -p 1 1\n"));
}

TEST(GroundTest, PredicatesNormalizationInventsStayOutOfTheOutput) {
  auto out = ground_source("p(1). { q(X) } :- p(X). :~ p(X). [1@0]");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_THAT(*out, Not(HasSubstr("_cr")));
  EXPECT_THAT(*out, Not(HasSubstr("_viol")));
}

}  // namespace
