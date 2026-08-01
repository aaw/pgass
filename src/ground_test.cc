#include "ground.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "macros.h"
#include "normalize.h"
#include "parse.h"
#include "test_macros.h"

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

// Parses, normalizes, and grounds `source`, returning what grounding warned
// about rather than the program it built.
absl::StatusOr<std::vector<std::string>> ground_warnings(
    const std::string& source) {
  Parser parser(source);
  ASSIGN_OR_RETURN(auto program, parser.parse_program());
  RETURN_IF_ERROR(normalize(*program));
  std::vector<std::string> warnings;
  RETURN_IF_ERROR(ground(*program, &warnings).status());
  return warnings;
}

// Parses, normalizes, and grounds `source` under a ground atom limit,
// returning how grounding ended.
absl::Status ground_under_limit(const std::string& source,
                                size_t max_ground_atoms) {
  Parser parser(source);
  ASSIGN_OR_RETURN(auto program, parser.parse_program());
  RETURN_IF_ERROR(normalize(*program));
  return ground(*program, nullptr, max_ground_atoms).status();
}

TEST(GroundTest, Facts) {
  auto out = ground_source("p(1). p(2).");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "0\n");
}

// '_' matches whatever an atom holds in its position, so a join must compare
// against it rather than looking atoms up by it, even when every other
// argument of the literal is already bound.
TEST(GroundTest, AnonymousVariableAlongsideBoundArguments) {
  auto out = ground_source(R"(
    p(1). p(2). r(1, a). r(2, b). r(3, c).
    q(X) :- p(X), r(X, _).
    s(X) :- p(X), r(X, f(_)).
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr(" q(1) "));
  EXPECT_THAT(*out, HasSubstr(" q(2) "));
  EXPECT_THAT(*out, Not(HasSubstr(" q(3) ")));
  EXPECT_THAT(*out, Not(HasSubstr(" s(")));
}

// 'not r(_, 2)' holds only when no stored r with 2 in its second argument is
// true, so the emitted body has to negate every one of them at once.
TEST(GroundTest, NotOverAnonymousVariableNegatesEveryMatchingAtom) {
  auto out = ground_source(R"(
    { r(1, 2) }. { r(3, 2) }. { r(4, 5) }.
    q :- not r(_, 2).
  )");
  ASSERT_OK(out);
  // The output statements below number r(1,2), r(3,2) and r(4,5) as atoms 2, 4
  // and 6, so q rests on 'not 2' and 'not 4'. r(4,5) does not match.
  EXPECT_THAT(*out, HasSubstr("1 0 1 1 0 2 -2 -4\n"));
  EXPECT_THAT(*out, HasSubstr("4 6 r(1,2) 1 2\n"));
  EXPECT_THAT(*out, HasSubstr("4 6 r(3,2) 1 4\n"));
  EXPECT_THAT(*out, HasSubstr("4 6 r(4,5) 1 6\n"));
}

TEST(GroundTest, NotOverAnonymousVariableHoldsWhenNothingMatches) {
  auto out = ground_source("r(1, 3). q :- not r(_, 2).\n");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"  // nothing to rule out, so q is a fact
            "1 0 1 2 0 0\n"
            "4 1 q 1 1\n"
            "4 6 r(1,3) 1 2\n"
            "0\n");
}

// Each rule instance rules out the atoms matching its own binding, so the
// q(2) instance carries no 'not' literal at all. r is left to a choice, which
// makes r(1, a) possible without being a fact, so the negation over it is
// something the emitted rule still has to carry.
TEST(GroundTest, NotOverAnonymousVariableIsPerBinding) {
  auto out = ground_source(R"(
    { r(1, a) }. p(1). p(2).
    q(X) :- p(X), not r(X, _).
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("4 6 r(1,a) 1 5\n"));
  EXPECT_THAT(*out, HasSubstr("1 0 1 3 0 1 -5\n"));  // q(1) :- not r(1,a)
  EXPECT_THAT(*out, HasSubstr("1 0 1 4 0 0\n"));     // q(2), a fact itself
}

TEST(GroundTest, NotOverAnonymousVariableNestedInAFunctionTerm) {
  auto out = ground_source(R"(
    p(f(1)). p(g(2)).
    a :- not p(f(_)).
    b :- not p(h(_)).
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("4 7 p(f(1)) 1 3\n"));
  // p(f(1)) is a fact, so 'not p(f(1))' can never hold. a keeps its name but
  // is left with no rule at all, which is how aspif says it is never true.
  EXPECT_THAT(*out, HasSubstr("4 1 a 1 2\n"));
  EXPECT_THAT(*out, Not(HasSubstr("1 0 1 2 ")));
  EXPECT_THAT(*out, HasSubstr("1 0 1 1 0 0\n"));  // nothing matches h(_)
  EXPECT_THAT(*out, HasSubstr("4 1 b 1 1\n"));
}

// An argument with no value at all is different from one matching no stored
// atom: the rule instance does not exist, so its head is never derived.
TEST(GroundTest, NotWithAnIllFormedArgumentDropsTheInstance) {
  auto out = ground_source(R"(
    p(0). p(2). r(2).
    q(X) :- p(X), not r(4 / X).
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, Not(HasSubstr(" q(0) ")));
  EXPECT_THAT(*out, HasSubstr(" q(2) "));
}

TEST(GroundTest, ProgramWithNoStatementsGroundsToAnEmptyAspifProgram) {
  auto out = ground_source("% just a comment\n");
  ASSERT_OK(out);
  EXPECT_EQ(*out, "asp 1 0 0\n0\n");
}

TEST(GroundTest, StringAtomPrintsWithOnePairOfQuotes) {
  auto out = ground_source("p(\"a b\").");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "4 8 p(\"a b\") 1 1\n"
            "0\n");
}

TEST(GroundTest, VariableRuleGroundsOncePerMatch) {
  auto out = ground_source("p(1). p(2). q(X) :- p(X).");
  ASSERT_OK(out);
  // Each q rests on a fact alone, so each q is a fact and needs no rule body.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(1) 1 3\n"
            "4 4 q(2) 1 4\n"
            "0\n");
}

// A disjunctive head grounds to one aspif rule per instance, holding an atom
// per disjunct.
TEST(GroundTest, DisjunctiveHeadKeepsEveryDisjunct) {
  auto out = ground_source(R"(
    v(1). v(2).
    red(X) | blue(X) :- v(X).
  )");
  ASSERT_OK(out);
  // The v facts drop out of the bodies, leaving the two disjunctions on their
  // own: red(1)=3 or blue(1)=4, and red(2)=5 or blue(2)=6.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 2 3 4 0 0\n"
            "1 0 2 5 6 0 0\n"
            "4 4 v(1) 1 1\n"
            "4 4 v(2) 1 2\n"
            "4 6 red(1) 1 3\n"
            "4 6 red(2) 1 5\n"
            "4 7 blue(1) 1 4\n"
            "4 7 blue(2) 1 6\n"
            "0\n");
}

// A disjunction derives no fact, however solid its body: one of a and b has to
// hold, and neither of them holds in every answer set. So p keeps the rule
// deriving it from a rather than becoming a fact.
TEST(GroundTest, FactnessDoesNotPassThroughADisjunction) {
  auto out = ground_source("d. a | b :- d. p :- a.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 2 2 3 0 0\n"
            "1 0 1 4 0 1 2\n"
            "4 1 d 1 1\n"
            "4 1 a 1 2\n"
            "4 1 b 1 3\n"
            "4 1 p 1 4\n"
            "0\n");
}

// A fact among the head atoms satisfies the rule outright, so the instance is
// dropped: b is named but nothing can derive it.
TEST(GroundTest, ADisjunctionSatisfiedByAFactIsDropped) {
  auto out = ground_source("a. a | b :- c. c.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "4 1 c 1 1\n"
            "4 1 a 1 2\n"
            "4 1 b 1 3\n"
            "0\n");
}

// A rule that keeps its own place still loses the fact literals in its body:
// they hold in every answer set, so requiring them says nothing.
TEST(GroundTest, FactBodyLiteralsDropOutOfARuleThatStays) {
  auto out = ground_source("{ a }. r. p :- a. s :- r, p.");
  ASSERT_OK(out);
  // r=1, then the choice's disjunction 'a | _cr0.' over a=2 and _cr0=3, then
  // p=4 and s=5. a is only possible, not a fact, so p and s are not facts
  // either, but 's :- r, p.' comes out as 's :- p.' with the fact r gone.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 2 2 3 0 0\n"
            "1 0 1 4 0 1 2\n"
            "1 0 1 5 0 1 4\n"
            "4 1 r 1 1\n"
            "4 1 a 1 2\n"
            "4 1 p 1 4\n"
            "4 1 s 1 5\n"
            "0\n");
}

// An atom can be derived by a rule that makes no fact of it and only later be
// derived again by one that does. Whatever reads it has to be reconsidered
// then, even though that pass added no atom for the usual delta to carry.
TEST(GroundTest, AnAtomThatBecomesAFactLateStillSpreadsItsFactness) {
  auto out = ground_source(R"(
    b. c :- b. z :- not zz. zz :- not z.
    p :- b, not z.
    p :- c.
    d :- p.
  )");
  ASSERT_OK(out);
  // p's component runs both p rules in one pass: 'p :- b, not z.' derives p
  // first and makes no fact of it, then 'p :- c.' finds p already there and
  // marks it. d is emitted as a fact only if that marking is noticed.
  EXPECT_THAT(*out, HasSubstr("4 1 p 1 5\n"));
  EXPECT_THAT(*out, HasSubstr("4 1 d 1 6\n"));
  EXPECT_THAT(*out, HasSubstr("1 0 1 5 0 0\n"));  // p, a fact
  EXPECT_THAT(*out, HasSubstr("1 0 1 6 0 0\n"));  // d, a fact
}

// An aggregate over facts alone has a value grounding can work out, so the
// rule needs no weight body, no auxiliary atoms, and no rule of its own: the
// head is a fact.
TEST(GroundTest, AnAggregateOverFactsDerivesAFact) {
  auto out = ground_source("p(1). p(2). q :- #count{ X : p(X) } >= 1.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 1 q 1 3\n"
            "0\n");
}

// The same value settles the other way when it misses the bound: the rule can
// hold in no answer set, so it is not emitted at all.
TEST(GroundTest, AnAggregateOverFactsThatMissesItsBoundDropsTheRule) {
  auto out = ground_source("p(1). p(2). q :- #count{ X : p(X) } >= 5. s.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("4 1 q 1 4\n"));
  EXPECT_THAT(*out, Not(HasSubstr("1 0 1 4 ")));
}

// An aggregate that mentions none of the rule's variables reads the same way
// for every instance, so one encoding serves them all.
TEST(GroundTest, AnAggregateIsGroundOncePerWayOfReadingIt) {
  auto out = ground_source(
      "dom(1). dom(2). { r(9) }. p(X) :- dom(X), #count{ Y : r(Y) } >= 1.");
  ASSERT_OK(out);
  // One '>= 1' atom (7) over r(9), which both p rules rest on. The dom facts
  // drop out of their bodies, leaving the aggregate as the whole of each.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "1 0 1 7 1 1 1 1 1\n"
            "1 0 1 5 0 1 7\n"
            "1 0 1 6 0 1 7\n"
            "4 4 r(9) 1 1\n"
            "4 6 dom(1) 1 3\n"
            "4 6 dom(2) 1 4\n"
            "4 4 p(1) 1 5\n"
            "4 4 p(2) 1 6\n"
            "0\n");
}

// An aggregate that does mention a rule variable reads differently for each
// value of it, so each gets its own encoding: atom 9 counts r(1) for p(1) and
// atom 10 counts r(2) for p(2).
TEST(GroundTest, AnAggregateOverARuleVariableIsGroundPerValue) {
  auto out = ground_source(R"(
    dom(1). dom(2). { r(1) }. { r(2) }.
    p(X) :- dom(X), #count{ Y : r(Y), Y = X } >= 1.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 9 1 1 1 1 1\n"));
  EXPECT_THAT(*out, HasSubstr("1 0 1 7 0 1 9\n"));
  EXPECT_THAT(*out, HasSubstr("1 0 1 10 1 1 1 3 1\n"));
  EXPECT_THAT(*out, HasSubstr("1 0 1 8 0 1 10\n"));
}

// An element with no condition puts its tuple in the set whatever else holds,
// so the count is 1 and nothing about it is left to the solver.
TEST(GroundTest, AggregateElementWithNoConditionCountsUnconditionally) {
  auto out = ground_source("r :- #count{ 1 } >= 1. s :- #count{ 1 } >= 2.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 2 0 0\n"
            "4 1 s 1 1\n"
            "4 1 r 1 2\n"
            "0\n");
}

// An aggregate with no elements at all counts nothing, so '>= 1' fails where
// '>= 0' holds.
TEST(GroundTest, AggregateWithNoElementsCountsZero) {
  auto out = ground_source("q :- #count{ } >= 0. z :- #count{ } >= 1.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 2 0 0\n"
            "4 1 z 1 1\n"
            "4 1 q 1 2\n"
            "0\n");
}

// One tuple the solver still decides is enough to leave the whole value open,
// so the weight body comes back.
TEST(GroundTest, AnAggregateOverAChoiceIsLeftToTheSolver) {
  auto out = ground_source("{ p(1) }. p(2). q :- #count{ X : p(X) } >= 1.");
  ASSERT_OK(out);
  // p(1)=1 is the choice's atom and p(2)=3 a fact. p(1) is counted through
  // its own atom; p(2)'s tuple rests on a fact, so its support is empty and
  // it needs an atom of its own (5) to be counted at all.
  EXPECT_THAT(*out, HasSubstr("1 0 1 5 0 0\n"));
  EXPECT_THAT(*out, HasSubstr("1 0 1 6 1 1 2 1 1 5 1\n"));
  EXPECT_THAT(*out, HasSubstr("1 0 1 4 0 1 6\n"));
}

// A 'not' inside an element condition can point at a predicate this rule's
// component is derived before, which would read here as an empty set rather
// than an undecided one, so an element carrying one is never settled.
TEST(GroundTest, NegationInsideAnElementLeavesTheAggregateToTheSolver) {
  auto out =
      ground_source("p(1). p(2). q :- #count{ X : p(X), not r(X) } >= 1.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 6 1 1 2 4 1 5 1\n"));
  EXPECT_THAT(*out, HasSubstr("1 0 1 3 0 1 6\n"));
}

// An aggregate under 'not' is settled when its rule is emitted, by which time
// every component has derived, but not while atoms are still being derived:
// its element predicates reach the head through negative edges only, which do
// not order components. So q comes out with an empty body, but a rule reading
// q still carries it rather than becoming a fact of its own.
TEST(GroundTest, AnAggregateUnderNotIsSettledOnlyWhenTheRuleIsEmitted) {
  auto out =
      ground_source("p(1). p(2). q :- not #count{ X : p(X) } >= 3. s :- q.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 1 0 0\n"));    // q, with nothing left
  EXPECT_THAT(*out, HasSubstr("1 0 1 2 0 1 1\n"));  // s :- q
}

TEST(GroundTest, RecursionReachesFixpoint) {
  auto out = ground_source(R"(
    e(a, b). e(b, c).
    r(X, Y) :- e(X, Y).
    r(X, Z) :- r(X, Y), e(Y, Z).
  )");
  ASSERT_OK(out);
  // e(a,b)=1, e(b,c)=2, r(a,b)=3, r(b,c)=4, r(a,c)=5. The last rule fires
  // once: r(a,b) joined with e(b,c) gives r(a,c). Every atom follows from the
  // two e facts alone, so the whole program is facts and no rules.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "1 0 1 5 0 0\n"
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
  auto out = ground_source(R"(
    e(a, b). e(b, c).
    r(X, Y) :- e(X, Y).
    r(X, Z) :- r(X, Y), e(Y, Z).
    t(X) :- r(X, Y).
  )");
  ASSERT_OK(out);
  // e(a,b)=1, e(b,c)=2, r(a,b)=3, r(b,c)=4, r(a,c)=5, t(a)=6, t(b)=7. t(a) is
  // derived twice, from r(a,b) and from r(a,c), the atom r's recursion only
  // derives on its second pass, but both make it the same fact, and a fact is
  // stated once however many rules derive it.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "1 0 1 5 0 0\n"
            "1 0 1 6 0 0\n"
            "1 0 1 7 0 0\n"
            "4 6 e(a,b) 1 1\n"
            "4 6 e(b,c) 1 2\n"
            "4 6 r(a,b) 1 3\n"
            "4 6 r(b,c) 1 4\n"
            "4 6 r(a,c) 1 5\n"
            "4 4 t(a) 1 6\n"
            "4 4 t(b) 1 7\n"
            "0\n");
}

TEST(GroundTest, NegationAgainstUnrelatedRecursiveComponentSeesDerivedAtom) {
  // r/2 is its own recursive component and has no positive-dependency link
  // to p/1 or s/1; s only refers to r through 'not'. r(a, c) only shows up
  // once the recursion reaches its second pass, so this checks that emission
  // sees what that pass derived. Everything r rests on is a fact, so r(a, c)
  // is a fact too, 'not r(a, c)' can never hold, and neither s rule survives.
  // If rules were ever emitted before r's component had fully run, this would
  // wrongly look like r(a, c) is underivable and emit the s rules instead.
  auto out = ground_source(R"(
    p(1). p(2).
    e(a, b). e(b, c).
    r(X, Y) :- e(X, Y).
    r(X, Z) :- r(X, Y), e(Y, Z).
    s(N) :- p(N), not r(a, c).
  )");
  ASSERT_OK(out);
  // Components derive in topological order before anything is emitted, so e/r
  // (r's component has no positive edge to p or s) derive before p/s:
  // e(a,b)=1, e(b,c)=2, r(a,b)=3, r(b,c)=4, r(a,c)=5, p(1)=6, p(2)=7. s(1)=8
  // and s(2)=9 keep their names but get no rule, so no answer set holds them.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "1 0 1 5 0 0\n"
            "1 0 1 6 0 0\n"
            "1 0 1 7 0 0\n"
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

TEST(GroundTest, NegationAgainstLaterComponentSeesDerivedAtom) {
  // c/2 is defined before q/1 and p/1 in the source, so its component id ends
  // up *higher* than p's: p's component has no positive edge into c (only
  // 'not c' references it), while q positively reaches p, pulling p's whole
  // component earlier in topological order. If grounding ever emits a
  // component's rules right after deriving that component, instead of
  // deriving every component first, it would process p before c has any
  // atoms and wrongly conclude 'not c(x, z)' can never fail, emitting the p
  // rules instead of dropping them.
  auto out = ground_source(R"(
    a(x, y). a(y, z).
    c(X, Y) :- a(X, Y).
    c(X, Z) :- c(X, Y), a(Y, Z).
    q(1). q(2).
    p(N) :- q(N), not c(x, z).
  )");
  ASSERT_OK(out);
  // q's component (id 0) derives before p's (id 1), which derives before
  // a/c's (ids 2-3): q(1)=1, q(2)=2, p(1)=3, p(2)=4, a(x,y)=5, a(y,z)=6,
  // c(x,y)=7, c(y,z)=8, c(x,z)=9. c(x,z) is only derived on c's second pass,
  // after p has already been derived, but since nothing is emitted until
  // every component has derived, both p rules see it as the fact it is and
  // are dropped: p(1) and p(2) are named but have no rule.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 5 0 0\n"
            "1 0 1 6 0 0\n"
            "1 0 1 7 0 0\n"
            "1 0 1 8 0 0\n"
            "1 0 1 9 0 0\n"
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

TEST(GroundTest, NegationOverAFactKillsTheRuleOverAnUnderivableAtomGoesAway) {
  auto out = ground_source("p(1). p(2). q(1). s(X) :- p(X), not q(X).");
  ASSERT_OK(out);
  // q/1 has no positive-dependency link to p/1 or s/1, so its singleton
  // component derives before p's: q(1)=1, p(1)=2, p(2)=3, s(1)=4, s(2)=5.
  // q(1) is a fact, so 'not q(1)' can never hold and s(1) gets no rule at
  // all. q(2) is underivable, so 'not q(2)' is dropped as satisfied, leaving
  // 's(2) :- p(2).', and p(2) is a fact, so s(2) is one too.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 5 0 0\n"
            "4 4 q(1) 1 1\n"
            "4 4 p(1) 1 2\n"
            "4 4 p(2) 1 3\n"
            "4 4 s(1) 1 4\n"
            "4 4 s(2) 1 5\n"
            "0\n");
}

// An atom a choice leaves open is derivable without being a fact, which is
// where a 'not' over it survives into the emitted rule.
TEST(GroundTest, NegationKeptOverAnAtomThatIsOnlyPossible) {
  auto out = ground_source("{ q(1) }. p(1). p(2). s(X) :- p(X), not q(X).");
  ASSERT_OK(out);
  // p(1)=1, p(2)=2, s(1)=3, s(2)=4, and the choice's disjunction
  // 'q(1) | _cr0.' over q(1)=5 and _cr0=6, one of which has to hold.
  // 's(1) :- p(1), not q(1).' keeps the negation and loses the fact p(1); s(2)
  // has nothing to rule out, since q(2) is underivable, so it is a fact.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 -5\n"
            "1 0 1 4 0 0\n"
            "1 0 2 5 6 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 s(1) 1 3\n"
            "4 4 s(2) 1 4\n"
            "4 4 q(1) 1 5\n"
            "0\n");
}

TEST(GroundTest, ComparisonFiltersInstances) {
  auto out = ground_source("p(1). p(2). q(X) :- p(X), X < 2.");
  ASSERT_OK(out);
  // '1 < 2' held at grounding time, so q(1) rests on the fact p(1) alone and
  // is a fact in turn.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
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
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 0 0 0\n"
            "0\n");
}

TEST(GroundTest, ConstraintHasNoHead) {
  auto out = ground_source("p(1). q(1). :- p(X), q(X).");
  ASSERT_OK(out);
  // p and q are each their own singleton component with no edges between
  // them (a constraint's body predicates get no outgoing edges at all), so
  // q's component happens to derive first: q(1)=1, p(1)=2. Both are facts, so
  // the constraint keeps neither, and a constraint with an empty body rules
  // out every answer set, which is the right reading of ':- p(1), q(1).'
  // when p(1) and q(1) both hold.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 0 0 0\n"
            "4 4 q(1) 1 1\n"
            "4 4 p(1) 1 2\n"
            "0\n");
}

TEST(GroundTest, ZeroArityAtoms) {
  auto out = ground_source("p. q :- not p.");
  ASSERT_OK(out);
  // p and q are each singleton components with no positive edge between
  // them ('not p' doesn't count), so q's component happens to derive first:
  // q=1, p=2. p is a fact, so 'not p' can never hold and q keeps its name but
  // gets no rule.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 2 0 0\n"
            "4 1 q 1 1\n"
            "4 1 p 1 2\n"
            "0\n");
}

TEST(GroundTest, CountAggregateLowerBound) {
  // p is left to a choice so the count stays open: p=1, q=3. The element's one
  // tuple rests on p alone, so the weight body counts p itself, and only the
  // '>= 1' bound-check atom (4) has to be invented.
  auto out = ground_source("{ p }. q :- #count{ 1 : p } > 0.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 1 4 1 1 1 1 1\n"
            "1 0 1 3 0 1 4\n"
            "4 1 p 1 1\n"
            "4 1 q 1 3\n"
            "0\n");
}

TEST(GroundTest, SumAggregateWeighsByFirstTerm) {
  // w(1,3)=1, w(2,5)=3, q=5. The element's first term (the weight) is V, not
  // I: the weight body holds the two w atoms weighing 3 and 5, rather than
  // counting them.
  auto out =
      ground_source("{ w(1,3) }. { w(2,5) }. q :- #sum{ V,I : w(I,V) } >= 4.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 2 3 4 0 0\n"
            "1 0 1 6 1 4 2 1 3 3 5\n"
            "1 0 1 5 0 1 6\n"
            "4 6 w(1,3) 1 1\n"
            "4 6 w(2,5) 1 3\n"
            "4 1 q 1 5\n"
            "0\n");
}

TEST(GroundTest, CountAggregateCombinedLowerAndUpperBound) {
  // Both bound sides are present, so both a "low_ok" atom (8, count >= 1) and
  // a "high_bad" atom (9, count >= 3) get defined over the three p atoms, and
  // q's rule requires low_ok and not high_bad together.
  auto out = ground_source(
      "{ p(1) }. { p(2) }. { p(3) }. q :- 1 <= #count{ X : p(X) } <= 2.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 2 3 4 0 0\n"
            "1 0 2 5 6 0 0\n"
            "1 0 1 8 1 1 3 1 1 3 1 5 1\n"
            "1 0 1 9 1 3 3 1 1 3 1 5 1\n"
            "1 0 1 7 0 2 8 -9\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 3\n"
            "4 4 p(3) 1 5\n"
            "4 1 q 1 7\n"
            "0\n");
}

TEST(GroundTest, NegatedAggregateNegatesTheWholeBoundConjunction) {
  // This is the shape normalize.cc's choice-cardinality translation produces
  // (':- body, not #count{...} <bounds>.'). Since both bound sides are
  // present, negating requires a conjunction atom (7 := low_ok(5) and not
  // high_bad(6)) before the constraint can require 'not' of that.
  auto out =
      ground_source("{ p(1) }. { p(2) }. :- not 1 <= #count{ X : p(X) } <= 2.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 2 3 4 0 0\n"
            "1 0 1 5 1 1 2 1 1 3 1\n"
            "1 0 1 6 1 3 2 1 1 3 1\n"
            "1 0 1 7 0 2 5 -6\n"
            "1 0 0 0 1 -7\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 3\n"
            "0\n");
}

TEST(GroundTest, CountAggregateEqualityUsesLowAndHighAuxAtoms) {
  // '= 2' needs both a low_eq (count >= 2) and high_bad_eq (count >= 3) atom,
  // combined into eq_ok (5 := low_eq and not high_bad_eq); q's rule requires
  // eq_ok directly, != would require 'not eq_ok' instead.
  auto out = ground_source("{ p(1) }. { p(2) }. q :- #count{ X : p(X) } = 2.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 2 3 4 0 0\n"
            "1 0 1 6 1 2 2 1 1 3 1\n"
            "1 0 1 7 1 3 2 1 1 3 1\n"
            "1 0 1 5 0 2 6 -7\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 3\n"
            "4 1 q 1 5\n"
            "0\n");
}

TEST(GroundTest, AggregateDedupesEqualTuplesAcrossElements) {
  // Two elements ('X : p(X)' and 'X : r(X)') both produce the tuple [1], so
  // they share one aux atom (6) supported by two rules (one per element),
  // and the weight rule only counts that shared atom once: the count can
  // never reach 2, so q is never derived even when both p(1) and r(1) hold.
  // Two supports are what makes the atom necessary; a tuple with only one
  // would be counted through the literal backing it.
  auto out = ground_source(
      "{ p(1) }. { r(1) }. q :- #count{ X : p(X) ; X : r(X) } >= 2.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 2 3 4 0 0\n"
            "1 0 1 6 0 1 3\n"
            "1 0 1 6 0 1 1\n"
            "1 0 1 7 1 2 1 6 1\n"
            "1 0 1 5 0 1 7\n"
            "4 4 r(1) 1 1\n"
            "4 4 p(1) 1 3\n"
            "4 1 q 1 5\n"
            "0\n");
}

TEST(GroundTest, WeakConstraintBecomesMinimize) {
  auto out = ground_source("p(1). p(2). :~ p(X). [1@0, X]");
  ASSERT_OK(out);
  // p(1)=1, p(2)=2, then the _viol atoms normalization derived from the weak
  // constraint: _viol(0,1,1)=3 and _viol(0,1,2)=4. Both sit at level 0 with
  // weight 1, so they share one minimize statement. _viol starts with '_',
  // so it stays out of the output statements.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "2 0 2 3 1 4 1\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "0\n");
}

TEST(GroundTest, WeakConstraintsGroupByLevel) {
  // Three weak constraints across two levels. The last one carries no terms,
  // so it becomes _viol/2 while the others become _viol/3, which are
  // predicates in the store, but level 2 collects literals from both.
  auto out = ground_source(R"(
    a(1). b(2).
    :~ a(X). [1@0, X]
    :~ b(X). [3@2, X]
    :~ a(X). [5@2]
  )");
  ASSERT_OK(out);
  // b(2)=1, a(1)=2, _viol(2,5)=3, _viol(0,1,1)=4, _viol(2,3,2)=5. Levels come
  // out ascending: level 0 holds _viol(0,1,1) at weight 1; level 2 holds
  // _viol(2,5) at weight 5 and _viol(2,3,2) at weight 3.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "1 0 1 5 0 0\n"
            "2 0 1 4 1\n"
            "2 2 2 3 5 5 3\n"
            "4 4 b(2) 1 1\n"
            "4 4 a(1) 1 2\n"
            "0\n");
}

TEST(GroundTest, WeakConstraintCountsEqualViolationsOnce) {
  // Both weak constraints produce the tuple (0, 1, 1), which is one ground
  // _viol atom however many rules derive it. So atom 3 appears once in the
  // minimize statement: violating both costs 1 in total, not 2. Both a(1) and
  // b(1) are facts, so the violation is a fact too, and unavoidable.
  auto out = ground_source(R"(
    a(1). b(1).
    :~ a(X). [1@0, X]
    :~ b(X). [1@0, X]
  )");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
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
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "2 0 1 4 1\n"
            "4 4 p(a) 1 1\n"
            "4 4 p(1) 1 2\n"
            "0\n");
}

TEST(GroundTest, SumIgnoresTuplesWithoutAnIntegerFirstTerm) {
  // #sum adds up only the tuples whose first term is an integer, so q(a)
  // contributes nothing and the weight body holds one literal, for p(1).
  auto out = ground_source(
      "{ p(1) }. { q(a) }. r :- #sum{ X : p(X); Y : q(Y) } >= 1.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 6 1 1 1 3 1\n"));
}

TEST(GroundTest, CountIncludesTuplesWithoutAnIntegerFirstTerm) {
  // #count counts every tuple in the set, unlike #sum: both p(1) and q(a)
  // land in the weight body, so the count reaches 2.
  auto out = ground_source(
      "{ p(1) }. { q(a) }. r :- #count{ X : p(X); Y : q(Y) } >= 2.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 6 1 2 2 3 1 1 1\n"));
}

TEST(GroundTest, NegativeSumWeightFlipsTheLiteral) {
  // ASPIF weights must be positive, so '-1 * [p]' becomes '-1 + 1 * [not p]'
  // and the bound moves from 0 to 1: atom 4 holds exactly when p is false,
  // which is when the sum reaches 0.
  auto out = ground_source("{ p }. q :- #sum{ -1 : p } >= 0.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 1 4 1 1 1 -1 1\n"
            "1 0 1 3 0 1 4\n"
            "4 1 p 1 1\n"
            "4 1 q 1 3\n"
            "0\n");
}

TEST(GroundTest, BoundNoPositiveWeightCanMissBecomesAFact) {
  // The sum is -1 or 0, so '>= -1' always holds. After the flip the bound is
  // 0, which positive weights always reach, so atom 4 is a plain fact.
  auto out = ground_source("{ p }. q :- #sum{ -1 : p } >= -1.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 1 4 0 0\n"
            "1 0 1 3 0 1 4\n"
            "4 1 p 1 1\n"
            "4 1 q 1 3\n"
            "0\n");
}

TEST(GroundTest, MixedSumWeightsFlipOnlyTheNegativeOnes) {
  // '2 * [p] - 1 * [q] >= 2' becomes '2 * [p] + 1 * [not q] >= 3', which
  // holds exactly when p is true and q is false.
  auto out = ground_source("{p}. {q}. r :- #sum{ 2 : p; -1 : q } >= 2.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 6 1 3 2 3 2 -1 1\n"));
}

TEST(GroundTest, ZeroSumWeightIsLeftOutOfTheWeightBody) {
  // A tuple weighing 0 never changes the sum, so it gets no literal at all
  // and the bound of 0 leaves atom 4 a fact.
  auto out = ground_source("{ p }. q :- #sum{ 0 : p } >= 0.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 4 0 0\n"));
}

TEST(GroundTest, MinAggregateOverFactsIsSettled) {
  // Both p atoms are facts, so the set is the same in every answer set and
  // grounding works the #min out itself. q is a fact and no rule mentions it.
  auto out = ground_source("p(1). p(3). q :- #min{ X : p(X) } = 1.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(3) 1 2\n"
            "4 1 q 1 3\n"
            "0\n");
}

TEST(GroundTest, MinMaxRangeOverTheTermOrder) {
  // #min and #max compare terms, not just numbers, and ASP-Core-2 orders every
  // integer below every symbolic constant. So the #max here is a, not 1.
  auto out = ground_source("p(1). p(a). q :- #max{ X : p(X) } = a.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("4 1 q 1 3\n"));

  auto missed = ground_source("p(1). p(a). q :- #max{ X : p(X) } = 1.");
  ASSERT_OK(missed);
  EXPECT_THAT(*missed, Not(HasSubstr("1 0 1 3")));
}

TEST(GroundTest, MinAggregateBindsItsValue) {
  // The value a #min binds is a term like any other, so q takes the smallest
  // one the elements produced.
  auto out = ground_source("p(1). p(a). q(S) :- #min{ X : p(X) } = S.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("4 4 q(1) 1 3\n"));
}

TEST(GroundTest, EmptySetMakesMinAggregateInfinite) {
  // Nothing supports p, so the set is empty, which is +infinity for #min and
  // -infinity for #max. The #min bound holds and the #max one cannot, so the
  // #max leaves its atom with no rule at all.
  auto min_out = ground_source("q :- #min{ X : p(X) } >= 5.");
  ASSERT_OK(min_out);
  EXPECT_THAT(*min_out, HasSubstr("1 0 1 1 0 0\n"));

  auto max_out = ground_source("q :- #max{ X : p(X) } >= 5.");
  ASSERT_OK(max_out);
  EXPECT_THAT(*max_out, Not(HasSubstr("1 0 1 1")));
}

TEST(GroundTest, EmptySetBindsNoVariable) {
  // An empty set gives an infinity, which is no term of the program and equal
  // to none, so S has nothing to take and the rule no ground instance.
  auto out = ground_source("q(S) :- #min{ X : p(X) } = S.");
  ASSERT_OK(out);
  EXPECT_EQ(*out, "asp 1 0 0\n0\n");
}

TEST(GroundTest, MinAggregateOverChoicesIsLeftToTheSolver) {
  // Neither p atom is settled, so the bound becomes a rule. The #min is at
  // least 3 exactly when no tuple below 3 is in the set, which is when p(1) is
  // false. Only p(1) is below 3, and a lone tuple needs no atom of its own, so
  // atom 5 rests on the p(1) literal directly.
  auto out = ground_source("{ p(1) }. { p(3) }. q :- #min{ X : p(X) } >= 3.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 2 3 4 0 0\n"
            "1 0 1 5 0 1 -1\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(3) 1 3\n"
            "4 1 q 1 5\n"
            "0\n");
}

TEST(GroundTest, NotMinAggregate) {
  // '#min{...} < 3' holds exactly when p(1) holds, so negating it gives a rule
  // resting on 'not p(1)'.
  auto out = ground_source("{ p(1) }. q :- not #min{ X : p(X) } < 3.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 1 0 1 -2\n"));
}

TEST(GroundTest, NonNumericBoundIsComparedByTermOrder) {
  // A bound is a term, so 'aa' is a legal one. The #max is b, which outranks
  // aa lexicographically, so q holds.
  auto out = ground_source("p(1). p(b). q :- #max{ X : p(X) } >= aa.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("4 1 q 1 3\n"));
}

TEST(GroundTest, ACountAgainstANonNumericBoundIsDecidedByKind) {
  // A #count is always an integer and every integer is below every symbolic
  // constant, so which integer it comes out as makes no difference. '>= aa'
  // can never hold and '< aa' always does.
  auto missed = ground_source("p(1). p(b). q :- #count{ X : p(X) } >= aa.");
  ASSERT_OK(missed);
  EXPECT_THAT(*missed, Not(HasSubstr("1 0 1 3")));

  auto out = ground_source("p(1). p(b). q :- #count{ X : p(X) } < aa.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 3 0 0\n"));
}

TEST(GroundTest, AnElementWithNoTermsIsNoMinMaxTuple) {
  // #min reads a tuple's first term, and '#min{ : p }' produces a tuple with
  // no term at all, so the set stays empty and the #min stays +infinity.
  auto out = ground_source("{ p }. q :- #min{ : p } >= 1.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 3 0 0\n"));
}

/* One instance of a disjunctive rule derives an atom for every predicate in its
   head, so grounding puts those predicates in one component. An aggregate over
   one of them, in a rule heading another, then reads a predicate its own
   component is still deriving.

   Such a rule derives no fact. Settling the count below while q/1 has no atoms
   would settle it at 0, make a fact of p, and lose the answer set holding q(1)
   instead. So p keeps a rule resting on the aggregate and the solver decides
   it. */
TEST(GroundTest, AnAggregateOverADisjunctiveHeadPartnerDerivesNoFact) {
  auto out = ground_source(R"(
    r.
    p :- #count{ X : q(X) } <= 0.
    p | q(1) :- r.
  )");
  ASSERT_OK(out);
  // r=1, p=2, q(1)=3, and the 'count >= 1' atom (4) the '<= 0' bound negates.
  // p rests on 'not 4' rather than being emitted as the fact '1 0 1 2 0 0'.
  EXPECT_THAT(*out, HasSubstr("1 0 1 2 0 1 -4\n"));
  EXPECT_THAT(*out, HasSubstr("1 0 2 2 3 0 0\n"));
  EXPECT_THAT(*out, Not(HasSubstr("1 0 1 2 0 0\n")));
}

// The same shape with the disjunction heading q/2 alongside p. The count over
// q/1 would otherwise settle on the fact q(1) and make p a fact. p=3 keeps the
// weight body (6) instead.
TEST(GroundTest, ADisjunctionOverAnAggregatesPredicateStopsItSettling) {
  auto out = ground_source(R"(
    r.
    q(1).
    p | q(2) :- r.
    p :- #count{ X : q(X) } >= 1.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 3 0 1 6\n"));
  EXPECT_THAT(*out, Not(HasSubstr("1 0 1 3 0 0\n")));
}

// The same aggregate settles as it always did where the disjunction is nowhere
// near it: q(1) is a fact, so the count is one whatever the solver does, and p
// is a fact in turn.
TEST(GroundTest, AnAggregateAwayFromTheDisjunctionStillSettles) {
  auto out = ground_source(R"(
    r.
    s | t :- r.
    q(1).
    p :- #count{ X : q(X) } >= 1.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("1 0 1 2 0 0\n"));
  EXPECT_THAT(*out, HasSubstr("4 1 p 1 2\n"));
}

TEST(GroundTest, ArithmeticInHeadTerm) {
  auto out = ground_source("p(1). p(2). q(X + 1) :- p(X).");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(2) 1 3\n"
            "4 4 q(3) 1 4\n"
            "0\n");
}

TEST(GroundTest, ArithmeticInComparison) {
  auto out = ground_source("p(1). p(2). p(3). q(X) :- p(X), X < 1 + 1.");
  ASSERT_OK(out);
  // Only p(1) passes 'X < 2', so q(1) is the sole q atom.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 p(3) 1 3\n"
            "4 4 q(1) 1 4\n"
            "0\n");
}

TEST(GroundTest, MultiplicationEvaluatesBeforeAddition) {
  auto out = ground_source("p(1). q(2 + X * 3) :- p(X).");
  ASSERT_OK(out);
  // 2 + 1 * 3 is 5, not 9.
  EXPECT_THAT(*out, HasSubstr("q(5)"));
}

TEST(GroundTest, ParenthesesEvaluateBeforeMultiplication) {
  auto out = ground_source("p(1). q((2 + X) * 3) :- p(X).");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("q(9)"));
}

TEST(GroundTest, DivisionTruncates) {
  auto out = ground_source("p(7). q(X / 2) :- p(X).");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("q(3)"));
}

TEST(GroundTest, UnaryMinusGivesNegativeNumbers) {
  auto out = ground_source("p(1). q(-X) :- p(X).");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 5 q(-1) 1 2\n"
            "0\n");
}

TEST(GroundTest, NegativeNumbersCompareBelowPositiveOnes) {
  auto out = ground_source("v(1). v(2). lt(X, Y) :- v(X), v(Y), -X < -Y.");
  ASSERT_OK(out);
  // -2 < -1, so lt(2,1) holds and lt(1,2) does not.
  EXPECT_THAT(*out, HasSubstr("lt(2,1)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("lt(1,2)")));
}

// ASP-Core-2 grounds a rule only over well-formed substitutions: one that
// leaves an arithmetic term undefined builds no ground instance. So the
// instances below are dropped one by one, and grounding still succeeds.

TEST(GroundTest, ArithmeticOnNonNumberDropsTheInstance) {
  auto out = ground_source("p(a). p(1). q(X) :- p(X), 1 < X + 1.");
  ASSERT_OK(out);
  // 'a + 1' has no value, so X = a is dropped and only q(1) survives.
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(a)")));
}

TEST(GroundTest, DivisionByZeroDropsTheInstance) {
  auto out = ground_source("p(0). p(1). q(X) :- p(X), 2 / X > 1.");
  ASSERT_OK(out);
  // '2 / 0' has no value, so X = 0 is dropped; 2 / 1 > 1 gives q(1).
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(0)")));
}

TEST(GroundTest, IllFormedHeadTermDropsTheInstance) {
  auto out = ground_source("p(0). p(2). q(4 / X) :- p(X).");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "4 4 p(0) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(2) 1 3\n"
            "0\n");
}

TEST(GroundTest, IllFormedNegativeLiteralDropsTheInstance) {
  auto out = ground_source("p(0). p(2). r(1). q(X) :- p(X), not r(4 / X).");
  ASSERT_OK(out);
  // X = 0 makes 'not r(4 / 0)' ill-formed, so that whole instance is gone,
  // not just the literal. X = 2 gives 'not r(2)', which r/1 never derived,
  // so q(2) comes out as a fact.
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(0)")));
}

TEST(GroundTest, IllFormedAggregateBoundDropsTheInstance) {
  auto out = ground_source(
      "d(0). d(2). p(1). q(X) :- d(X), #count{ Y : p(Y) } >= 4 / X.");
  ASSERT_OK(out);
  // X = 0 makes the bound '4 / 0' ill-formed, so no q(0) is derived at all.
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(0)")));
}

// A rule losing instances to a term with no value says so, because the answer
// set otherwise comes out quietly short. 'p(-a).' is the case worth catching:
// ASP-Core-2 reads the '-' as arithmetic, so '-a' has no value and the fact
// grounds to nothing, where clingo reads it as classical negation and keeps
// the fact.
TEST(GroundTest, ArithmeticWithNoValueWarns) {
  auto warnings = ground_warnings("p(-a).");
  ASSERT_OK(warnings);
  ASSERT_EQ(warnings->size(), 1u);
  EXPECT_EQ((*warnings)[0],
            "dropped ground instances of 'p(-a).' where 'a' is not an "
            "integer");
}

// One warning per rule, however many of its instances go.
TEST(GroundTest, ARuleLosingSeveralInstancesWarnsOnce) {
  auto warnings = ground_warnings("q(a). q(b). p(X + 1) :- q(X).");
  ASSERT_OK(warnings);
  ASSERT_EQ(warnings->size(), 1u);
  EXPECT_THAT((*warnings)[0],
              HasSubstr("dropped ground instances of 'p(X + 1) :- q(X).'"));
}

TEST(GroundTest, ComparisonAssignmentAndNegationWithNoValueWarn) {
  auto warnings = ground_warnings(R"(
    q(a). r(0).
    p1 :- q(X), X + 1 > 2.
    p2(Y) :- q(X), Y = -X.
    p3(X) :- r(X), not s(4 / X).
  )");
  ASSERT_OK(warnings);
  EXPECT_EQ(warnings->size(), 3u);
}

// A positive literal whose argument has no value matches nothing, so its rule
// loses that instance the same way, and the surviving instances are untouched.
TEST(GroundTest, PositiveLiteralArgumentWithNoValueWarns) {
  auto warnings = ground_warnings("p(0). p(2). r(2). q(X) :- p(X), r(4 / X).");
  ASSERT_OK(warnings);
  ASSERT_EQ(warnings->size(), 1u);
  EXPECT_THAT((*warnings)[0], HasSubstr("'4 / X' divides by zero"));

  auto out = ground_source("p(0). p(2). r(2). q(X) :- p(X), r(4 / X).");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(0)")));
}

// A #min or #max can bind a variable to a constant, which leaves an assignment
// reading it without a value.
TEST(GroundTest, AggregateValueWithNoValueForAnAssignmentWarns) {
  auto warnings =
      ground_warnings("p(a). q(T) :- #min{ X : p(X) } = S, T = S + 1.");
  ASSERT_OK(warnings);
  ASSERT_EQ(warnings->size(), 1u);
  EXPECT_THAT((*warnings)[0], HasSubstr("'S' is not an integer"));
}

TEST(GroundTest, AggregateBoundWithNoValueWarns) {
  auto warnings = ground_warnings(
      "d(0). d(2). p(1). q(X) :- d(X), #count{ Y : p(Y) } >= 4 / X.");
  ASSERT_OK(warnings);
  ASSERT_EQ(warnings->size(), 1u);
  EXPECT_THAT((*warnings)[0], HasSubstr("'4 / X' divides by zero"));
}

// A rule whose arithmetic works out says nothing, and neither does one whose
// instances go for any other reason: a body literal nothing matches, or an
// aggregate the store settles false.
TEST(GroundTest, WellFormedArithmeticWarnsNothing) {
  auto warnings = ground_warnings(R"(
    q(1). q(2). p(X + 1) :- q(X).
    s :- t.
    u :- q(X), #count{ Y : q(Y) } >= 4.
  )");
  ASSERT_OK(warnings);
  EXPECT_THAT(*warnings, ::testing::IsEmpty());
}

TEST(GroundTest, FunctionTermsMatchArgumentByArgument) {
  auto out = ground_source(R"(
    p(f(1)). p(f(2)). p(g(a, b)).
    q(X) :- p(f(X)).
    r(Y) :- p(g(a, Y)).
  )");
  ASSERT_OK(out);
  // p(f(1))=1, p(f(2))=2, p(g(a,b))=3, r(b)=4, q(1)=5, q(2)=6. r's rule comes
  // first because r/1 and q/1 are separate components and r's is grounded
  // first.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "1 0 1 5 0 0\n"
            "1 0 1 6 0 0\n"
            "4 7 p(f(1)) 1 1\n"
            "4 7 p(f(2)) 1 2\n"
            "4 9 p(g(a,b)) 1 3\n"
            "4 4 r(b) 1 4\n"
            "4 4 q(1) 1 5\n"
            "4 4 q(2) 1 6\n"
            "0\n");
}

TEST(GroundTest, NestedFunctionTerms) {
  auto out = ground_source(R"(
    p(f(g(1))).
    q(X) :- p(f(g(X))).
  )");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "4 10 p(f(g(1))) 1 1\n"
            "4 4 q(1) 1 2\n"
            "0\n");
}

TEST(GroundTest, FunctionTermNeedsSameNameAndArity) {
  auto out = ground_source(R"(
    p(f(1)). p(g(1)). p(f(1, 2)).
    q(X) :- p(f(X)).
  )");
  ASSERT_OK(out);
  // Only p(f(1)) matches 'p(f(X))': g(1) has a different name and f(1,2) a
  // different arity, so q(1) is the sole q atom.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "4 7 p(f(1)) 1 1\n"
            "4 7 p(g(1)) 1 2\n"
            "4 9 p(f(1,2)) 1 3\n"
            "4 4 q(1) 1 4\n"
            "0\n");
}

TEST(GroundTest, RepeatedVariableInsideFunctionTerm) {
  auto out = ground_source(R"(
    p(f(1, 1)). p(f(1, 2)).
    q(X) :- p(f(X, X)).
  )");
  ASSERT_OK(out);
  // f(1,2) can't bind X to both 1 and 2, so only f(1,1) gives q(1).
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "4 9 p(f(1,1)) 1 1\n"
            "4 9 p(f(1,2)) 1 2\n"
            "4 4 q(1) 1 3\n"
            "0\n");
}

TEST(GroundTest, FunctionTermsSortAfterEveryAtomicValue) {
  // ASP-Core-2 orders integers < symbolic constants < string constants <
  // function terms, so lt/2 holds for every pair in that order.
  auto out = ground_source(R"(
    v(1). v(a). v("s"). v(f(1)).
    lt(X, Y) :- v(X), v(Y), X < Y.
  )");
  ASSERT_OK(out);
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
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(2) 1 3\n"
            "4 4 q(3) 1 4\n"
            "0\n");
}

TEST(GroundTest, AssignmentWorksWithTheVariableOnTheRight) {
  auto out = ground_source("p(1). q(Y) :- p(X), X + 1 = Y.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("q(2)"));
}

TEST(GroundTest, AssignmentChainsThroughAnotherAssignment) {
  // Z takes its value from Y, which the assignment to its right binds, so the
  // two are made in the opposite order from how they are written.
  auto out = ground_source("p(1). q(Z) :- p(X), Z = Y + 1, Y = X + 1.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("q(3)"));
}

TEST(GroundTest, AssignmentWithNoPositiveLiteralAtAll) {
  auto out = ground_source("q(X) :- X = 1 + 1.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "4 4 q(2) 1 1\n"
            "0\n");
}

TEST(GroundTest, AssignmentBindsANonNumberValue) {
  auto out = ground_source("p(a). q(Y) :- p(X), Y = f(X).");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("q(f(a))"));
}

TEST(GroundTest, AssignmentToABoundVariableIsAComparison) {
  auto out =
      ground_source("p(1, 2). p(3, 4). p(5, 5). q(X) :- p(X, Y), X = Y.");
  ASSERT_OK(out);
  // X is already bound by p, so 'X = Y' filters instead of assigning.
  EXPECT_THAT(*out, HasSubstr("q(5)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(1)")));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(3)")));
}

TEST(GroundTest, IllFormedAssignmentDropsTheInstance) {
  auto out = ground_source("p(0). p(2). q(Y) :- p(X), Y = 4 / X.");
  ASSERT_OK(out);
  // '4 / 0' has no value, so X = 0 binds nothing and derives no q at all.
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_EQ(out->find("q("), out->rfind("q("));
}

TEST(GroundTest, AssignmentBoundVariableUsedByALaterLiteral) {
  auto out = ground_source(R"(
    { r(2) }. p(1). p(2).
    q(X) :- p(X), Y = X + 1, not r(Y).
  )");
  ASSERT_OK(out);
  // X = 1 gives Y = 2, and the choice leaves r(2) open, so q(1) keeps 'not
  // r(2)' in its body. X = 2 gives Y = 3, which r/1 never derived, so q(2) is
  // a fact. p(1)=1, p(2)=2, q(1)=3, q(2)=4, and the choice's disjunction
  // 'r(2) | _cr0.' over r(2)=5 and _cr0=6.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 1 -5\n"
            "1 0 1 4 0 0\n"
            "1 0 2 5 6 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(1) 1 3\n"
            "4 4 q(2) 1 4\n"
            "4 4 r(2) 1 5\n"
            "0\n");
}

TEST(GroundTest, InequalityDoesNotAssign) {
  auto out = ground_source("p(1). p(2). q(X) :- p(X), X != 2.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(2)")));
}

// '#count{...} = S' with S unbound binds S to the aggregate's value. The
// value depends on which atoms the solver makes true, so the rule is ground
// once per value the aggregate can take, and each instance keeps the literals
// that check for that value.

TEST(GroundTest, CountAggregateValueBindsAVariable) {
  auto out = ground_source("{ p }. q(S) :- #count{ 1 : p } = S.");
  ASSERT_OK(out);
  // The count is 0 or 1, so q(0)=3 and q(1)=4 are both ground, each with its
  // own bound checks over p: q(0) needs 'count >= 0' (5, a fact) and not
  // 'count >= 1' (6), q(1) needs 'count >= 1' (7) and not 'count >= 2' (8).
  // A solver keeps whichever matches its choice of p.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 2 1 2 0 0\n"
            "1 0 1 5 0 0\n"
            "1 0 1 6 1 1 1 1 1\n"
            "1 0 1 3 0 2 5 -6\n"
            "1 0 1 7 1 1 1 1 1\n"
            "1 0 1 8 1 2 1 1 1\n"
            "1 0 1 4 0 2 7 -8\n"
            "4 1 p 1 1\n"
            "4 4 q(0) 1 3\n"
            "4 4 q(1) 1 4\n"
            "0\n");
}

TEST(GroundTest, EmptyAggregateValueBindsZero) {
  // No element can produce a tuple, so the count is 0 whatever a solver does:
  // the only value S takes is 0, and q(0) comes out as a fact.
  auto out = ground_source("q(S) :- #count{ X : p(X) } = S.");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "4 4 q(0) 1 1\n"
            "0\n");
}

TEST(GroundTest, AggregateValueBindsFromEitherSide) {
  auto out =
      ground_source("{ p(1) }. { p(2) }. q(S) :- S = #count{ X : p(X) }.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("q(0)"));
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(3)")));
}

TEST(GroundTest, SumAggregateValueRangesOverSubsetSums) {
  // Any subset of the two tuples can be in the set, so the sum is 0, 3, 5, or
  // 8, not every number in between.
  auto out = ground_source(
      "{ w(1,3) }. { w(2,5) }. total(S) :- #sum{ V,I : w(I,V) } = S.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("total(0)"));
  EXPECT_THAT(*out, HasSubstr("total(3)"));
  EXPECT_THAT(*out, HasSubstr("total(5)"));
  EXPECT_THAT(*out, HasSubstr("total(8)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("total(1)")));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("total(4)")));
}

TEST(GroundTest, NegativeSumWeightsReachNegativeValues) {
  auto out =
      ground_source("{ w(-2) }. { w(3) }. total(S) :- #sum{ W : w(W) } = S.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("total(-2)"));
  EXPECT_THAT(*out, HasSubstr("total(0)"));
  EXPECT_THAT(*out, HasSubstr("total(1)"));
  EXPECT_THAT(*out, HasSubstr("total(3)"));
}

TEST(GroundTest, AggregateValueFeedsAnAssignment) {
  auto out = ground_source(
      "{ p(1) }. { p(2) }. q(T) :- #count{ X : p(X) } = S, T = S + 1.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, HasSubstr("q(3)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(0)")));
}

TEST(GroundTest, ComparisonOnAnAggregateValueDropsInstances) {
  // 'S > 1' is checked once the aggregate binds S, and rules out the
  // instances for the values 0 and 1.
  auto out = ground_source(R"(
    { p(1) }. { p(2) }. { p(3) }.
    big(S) :- #count{ X : p(X) } = S, S > 1.
  )");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("big(2)"));
  EXPECT_THAT(*out, HasSubstr("big(3)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("big(0)")));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("big(1)")));
}

TEST(GroundTest, AggregateValueBindsOncePerOuterInstance) {
  auto out = ground_source(R"(
    g(1). g(2). e(1,a). e(1,b). e(2,c).
    deg(G, C) :- g(G), #count{ Y : e(G, Y) } = C.
  )");
  ASSERT_OK(out);
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
  auto out = ground_source(R"(
    { n(2) }. { e(0,x) }. { e(2,b) }. { e(2,c) }.
    q(C) :- #count{ Y : e(X, Y) } = C, X = #sum{ Z : n(Z) }.
  )");
  ASSERT_OK(out);
  // The #sum is 0 or 2. X = 0 matches only e(0,x), so C is 0 or 1; X = 2
  // matches e(2,b) and e(2,c), so C reaches 2.
  EXPECT_THAT(*out, HasSubstr("q(0)"));
  EXPECT_THAT(*out, HasSubstr("q(1)"));
  EXPECT_THAT(*out, HasSubstr("q(2)"));
  EXPECT_THAT(*out, ::testing::Not(HasSubstr("q(3)")));
}

TEST(GroundTest, TwoAggregatesEachBindTheirOwnVariable) {
  auto out = ground_source(R"(
    { p(1) }. { q(1) }.
    r(A, B) :- #count{ X : p(X) } = A, #count{ Y : q(Y) } = B.
  )");
  ASSERT_OK(out);
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
  // values one rule may be ground over. Each w is left to a choice, so every
  // one of those sums is a value the aggregate really can take.
  auto out = ground_source(R"(
    { w(1) }. { w(2) }. { w(4) }. { w(8) }. { w(16) }. { w(32) }.
    { w(64) }. { w(128) }. { w(256) }. { w(512) }. { w(1024) }.
    { w(2048) }. { w(4096) }.
    total(S) :- #sum{ W : w(W) } = S.
  )");
  EXPECT_FALSE(out.ok());
  EXPECT_THAT(out.status().message(), HasSubstr("more than 4096"));
}

TEST(GroundTest, GroundQueryAssumesItsAtom) {
  auto out = ground_source("p(1). p(2). p(2)?");
  ASSERT_OK(out);
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

// A query with a variable matches an atom at a time, and the matches are
// alternatives. An assumption cannot say that, so nothing about the query
// prints and the grounding is the one the program has without it.
TEST(GroundTest, QueryWithAVariableKeepsItsMatchesApart) {
  auto out = ground_source("p(1). p(2). p(X)?");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "0\n");
}

TEST(GroundTest, QueryMatchesOnlyTheAtomsWithTheSameArguments) {
  auto out = ground_source("p(1, a). p(2, b). p(X, a)?");
  ASSERT_OK(out);
  // Only p(1,a)=1 matches, so it is assumed directly.
  EXPECT_THAT(*out, HasSubstr("6 1 1\n"));
}

TEST(GroundTest, QueryOverADerivedPredicate) {
  auto out = ground_source("p(1). q(X) :- p(X). q(1)?");
  ASSERT_OK(out);
  // p(1)=1, q(1)=2.
  EXPECT_THAT(*out, HasSubstr("6 1 2\n"));
}

// A query nothing matches leaves no atom to assume and nothing that can make
// it hold.
TEST(GroundTest, QueryThatMatchesNothingAssumesNothing) {
  auto out = ground_source("p(1). p(3)?");
  ASSERT_OK(out);
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "4 4 p(1) 1 1\n"
            "0\n");
}

TEST(GroundTest, QueryOverAPredicateWithNoAtomsAtAll) {
  auto out = ground_source("p(1). q(X)?");
  ASSERT_OK(out);
  EXPECT_THAT(*out, Not(HasSubstr("\n6 ")));
}

TEST(GroundTest, QueryOnAClassicallyNegatedLiteral) {
  // normalize() rewrites '-p(1)' into '_neg_p(1)', in the query as well as in
  // the rules, so the query still finds the atom.
  auto out = ground_source("-p(1). -p(1)?");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("6 1 1\n"));
}

TEST(GroundTest, ClassicallyNegatedAtomPrintsUnderItsOriginalName) {
  auto out = ground_source("-p(1). p(2).");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("4 4 p(2) 1 1\n"));
  EXPECT_THAT(*out, HasSubstr("4 5 -p(1) 1 2\n"));
}

TEST(GroundTest, ZeroArityClassicallyNegatedAtomPrintsUnderItsOriginalName) {
  auto out = ground_source("-p.");
  ASSERT_OK(out);
  EXPECT_THAT(*out, HasSubstr("4 2 -p 1 1\n"));
}

TEST(GroundTest, PredicatesNormalizationInventsStayOutOfTheOutput) {
  auto out = ground_source("p(1). { q(X) } :- p(X). :~ p(X). [1@0]");
  ASSERT_OK(out);
  EXPECT_THAT(*out, Not(HasSubstr("_cr")));
  EXPECT_THAT(*out, Not(HasSubstr("_viol")));
}

// 'p(X+1) :- p(X).' derives a new p every pass, so without the limit this
// grounds until the machine is out of memory.
TEST(GroundTest, GivesUpOnAProgramWithNoFiniteGrounding) {
  absl::Status status = ground_under_limit("p(1). p(X+1) :- p(X).", 1000);
  EXPECT_EQ(status.code(), absl::StatusCode::kResourceExhausted);
  EXPECT_THAT(std::string(status.message()), HasSubstr("1000 ground atoms"));
  EXPECT_THAT(std::string(status.message()), HasSubstr("p/1"));
}

// A term that grows by nesting rather than by counting runs away just as far.
TEST(GroundTest, GivesUpOnAFunctionTermThatGrowsWithoutBound) {
  absl::Status status = ground_under_limit("p(a). p(f(X)) :- p(X).", 1000);
  EXPECT_EQ(status.code(), absl::StatusCode::kResourceExhausted);
  EXPECT_THAT(std::string(status.message()), HasSubstr("p/1"));
}

// The limit counts atoms, not rule instances, so a program deriving fewer
// atoms than the limit grounds no differently for having one.
TEST(GroundTest, GroundsNormallyUnderTheLimit) {
  EXPECT_OK(ground_under_limit("p(1). p(X+1) :- p(X), X < 100.", 1000));
}

// Every atom counts against the limit whatever predicate it belongs to.
TEST(GroundTest, CountsAtomsAcrossPredicates) {
  EXPECT_EQ(ground_under_limit("p(1). p(2). p(3). q(X) :- p(X).", 5).code(),
            absl::StatusCode::kResourceExhausted);
  EXPECT_OK(ground_under_limit("p(1). p(2). p(3). q(X) :- p(X).", 6));
}

TEST(GroundTest, ZeroMeansNoLimit) {
  EXPECT_OK(ground_under_limit("p(1). p(X+1) :- p(X), X < 100.", 0));
}

}  // namespace
