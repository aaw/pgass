#include "safety.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/status/status_matchers.h"
#include "parse.h"
#include "test_macros.h"

using namespace ::testing;
using absl_testing::IsOk;

namespace {

class SafetyTest : public ::testing::Test {};

TEST_F(SafetyTest, TestSimpleMixedBinding) {
  std::string_view src = "p(X,Z) :- p(X,Y), p(Y,Z).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestNegationAsFailure) {
  std::string_view src = "p(X) :- not q(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestNegationAsFailureButBoundElsewhere) {
  std::string_view src = "p(X) :- not q(X), r(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestBoundByEquality) {
  std::string_view src = "p(X,Y) :- q(X), X = Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestNotBoundByLessThan) {
  // Non-equality binary operations do not bind (compare to previous example).
  std::string_view src = "p(X,Y) :- q(X), X < Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestNotBoundByArithmeticInsideAnAtom) {
  // Grounding evaluates X + 1 and compares the result, so q(X+1) cannot bind X.
  std::string_view src = "p(X) :- q(X+1).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: p(X) :- q(X+1).\n"
            "unsafe variable in rule 'p/1': X");
}

TEST_F(SafetyTest, TestArithmeticInsideAnAtomIsFineOnceBoundElsewhere) {
  std::string_view src = "p(X) :- q(X), r(X+1).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestBoundInsideAFunctionTerm) {
  // Matching descends into f(...), so the argument position still binds X.
  std::string_view src = "p(X) :- q(f(X)).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestAggregateElementTermsDoNotBind) {
  // Z is what gets counted, not what gets matched, so 'q(1)' has to bind it.
  std::string_view src = "p :- #count{ Z : q(1) } = 1.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: p :- #count{ Z : q(1) } = 1.\n"
            "unsafe aggregate in rule 'p/0'");
}

TEST_F(SafetyTest, TestAggregateElementTermsBoundByTheirCondition) {
  std::string_view src = "p(N) :- #count{ X+1 : q(X) } = N.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestFixedPointIterations) {
  // q(X) binds X, then Y = X + 1 binds Y, then Z = Y * 2 binds Z.
  std::string_view src = "q(1). p(X,Y,Z) :- Z = Y * 2, Y = X + 1, q(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestUnsafeCircularBind) {
  std::string_view src = "p(X,Y) :- q(Z), X = Y + 1, Y = X - 1.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestSafeAggregation) {
  // Safe aggregate example from ASP-Core 2 spec.
  std::string_view src =
      "p(X,Y) :- q(X), #sum{S,X : r(T,X), S = (2 * T) - X} = Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestUnsafeAggregation) {
  // Unsafe aggregate example from ASP-Core 2 spec.
  std::string_view src =
      "p(X,Y) :- q(X), #sum{S,X : r(T,X), S + X = (2 * T)} = Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestAggregationBoundPropagation) {
  // Multiple rounds of binding with aggregates involved:
  // *  X is bound by q(X).
  // *  Z is then bound by X + 2.
  // *  The #sum aggregation is then bound by Z.
  // *  A is bound by the #sum aggregation's binding.
  // *  Finally, B is bound by A's binding.
  std::string_view src =
      "p(X,B) :- q(X), Z = X + 2, #sum{S : S = Z + 3} = A, B = A + 1.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestAggregationBoundAlmostPropagation) {
  // Similar to the previous example but the aggregation isn't fully bound (it
  // includes a reference to an unbound "C") so binding propagation to B never
  // happens.
  std::string_view src =
      "p(X,B) :- q(X), Z = X + 2, #sum{S : S = Z + C + 3} = A, B = A + 1.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestAggregationLocalScopeDoesNotLeak) {
  // We should recognize variables local to aggregations _only_ in their scopes
  // and not let them leak outside to other expressions.
  std::string_view src = "p(X) :- q(X), #sum{S : S = X + 1}, B = S + X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestNafAggregateDoesNotBind) {
  // A NAF aggregate should not bind its bound variable; X is only referenced
  // inside a negated aggregate so it is unsafe.
  std::string_view src = "p(X) :- not #sum{S : r(S)} = X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestGlobalsDoNotLeakAcrossStatements) {
  std::string_view src = "p(X, Y) :- q(X), Y = X. r(X, Y) :- q(X), Y + 1 = X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

// Error message examples. These tests document what safety errors look like.

TEST_F(SafetyTest, ErrorMessageSingleUnsafeVariable) {
  std::string_view src = "p(X) :- not q(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: p(X) :- not q(X).\n"
            "unsafe variable in rule 'p/1': X");
}

TEST_F(SafetyTest, ErrorMessageMultipleUnsafeVariables) {
  std::string_view src = "p(X,Y) :- q(Z), X = Y + 1, Y = X - 1.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: p(X,Y) :- q(Z), X = Y + 1, Y = X - 1.\n"
            "unsafe variables in rule 'p/2': X, Y");
}

TEST_F(SafetyTest, ErrorMessageCorrectLineNumber) {
  std::string_view src = "p(X) :- q(X).\nr(X, Y) :- q(X), Y + 1 = X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 2: r(X, Y) :- q(X), Y + 1 = X.\n"
            "unsafe variable in rule 'r/2': Y");
}

TEST_F(SafetyTest, ErrorMessageUnsafeAggregate) {
  // An aggregate whose internal body has unbound variables (S is only in a NAF
  // literal so it is never bound) triggers the aggregate-safety check.
  std::string_view src = "p(X) :- q(X), #sum{S : not r(S)}.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: p(X) :- q(X), #sum{S : not r(S)}.\n"
            "unsafe aggregate in rule 'p/1'");
}

TEST_F(SafetyTest, TestUnboundHeadVariable) {
  // Nothing says which X to derive. No q(1) fact means grounding never reaches
  // the rule, so this check is the only one that catches it.
  std::string_view src = "p(X) :- q(1).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: p(X) :- q(1).\n"
            "unsafe variable in rule 'p/1': X");
}

TEST_F(SafetyTest, TestUnboundHeadVariableInFact) {
  std::string_view src = "p(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestUnboundDisjunctionVariable) {
  std::string_view src = "p(X) | r(Y) :- q(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: p(X) | r(Y) :- q(X).\n"
            "unsafe variable in rule 'p/1 | r/1': Y");
}

TEST_F(SafetyTest, TestChoiceElementConditionBindsItsOwnVariable) {
  // X is local to the element and d(X) binds it there, so the body owes it
  // nothing.
  std::string_view src = "d(1). { p(X) : d(X) }.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestUnboundChoiceElementVariable) {
  std::string_view src = "{ p(X) }.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: { p(X) }.\n"
            "unsafe variable in rule '{p/1}': X");
}

TEST_F(SafetyTest, TestUnboundChoiceBoundVariable) {
  std::string_view src = "q(1). N <= { p(1) } :- q(1).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestChoiceVariableBoundByBody) {
  std::string_view src = "d(1). { p(X) } :- d(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestRecursiveAggregateRejected) {
  // ASP-Core-2 spec example: an aggregate over the very predicate it defines.
  std::string_view src = "p(X) :- dom(X), #count{ Y : p(Y) } >= X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestAggregateOverUnrelatedRecursivePredicateAllowed) {
  // ASP-Core-2 spec example: 'reach' is recursive, but the aggregate lives in
  // a rule for 'big', which isn't part of that recursion, so this is fine.
  std::string_view src =
      "reach(X, Z) :- reach(X, Y), edge(Y, Z).\n"
      "big :- #count{ X, Y : reach(X, Y) } >= 3.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestRecursiveAggregateThroughMultipleRulesRejected) {
  // p depends on q, q's aggregate depends on p: a two-rule positive cycle
  // through the aggregate.
  std::string_view src =
      "p(X) :- dom(X), q(X).\n"
      "q(X) :- dom(X), #count{ Y : p(Y) } >= X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestNegatedLiteralInAggregateDoesNotCountAsRecursive) {
  // p(Y) only appears default-negated inside the aggregate, so it never
  // creates a positive dependency and the rule is not rejected as recursive.
  // dom(Y) binds Y so the aggregate itself stays safe.
  std::string_view src = "p(X) :- dom(X), #count{ Y : dom(Y), not p(Y) } >= X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, ErrorMessageRecursiveAggregate) {
  std::string_view src = "p(X) :- dom(X), #count{ Y : p(Y) } >= X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: p(X) :- dom(X), #count{ Y : p(Y) } >= X.\n"
            "recursive aggregate in rule 'p/1': 'p/1' is recursive with the "
            "rule's head and cannot be used inside an aggregate");
}

TEST_F(SafetyTest, TestWeakConstraintSafe) {
  std::string_view src = ":~ p(X). [1@0, X]";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestWeakConstraintUnsafeDistinctnessTerm) {
  // Y appears only in the distinctness terms, never in the body.
  std::string_view src = ":~ p(X). [1@0, Y]";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestWeakConstraintUnsafeWeightAndLevel) {
  // W and L are the weight and level themselves, neither bound by the body.
  std::string_view src = ":~ p(X). [W@L, X]";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

// An element with no condition, like the '1' of '#count{ 1 }', puts its tuple
// in the set unconditionally. There is nothing there to bind a variable and
// nothing needing one, so the aggregate is safe.
TEST_F(SafetyTest, TestAggregateElementWithNoCondition) {
  std::string_view src = "q :- #count{ 1 } >= 1.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestAggregateWithNoElements) {
  std::string_view src = "q :- #count{ } >= 0.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

// W is shared between the elements and the rest of the rule, and the only
// literal that could bind it waits on the aggregate in turn. Neither W nor Y
// ever gets a value.
TEST_F(SafetyTest, TestAggregateGuardDoesNotBindThroughAGlobalCycle) {
  std::string_view src = "p(Y) :- #count{X : q(X, W)} = Y, W = Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

// The same aggregate is safe when another literal binds the shared W.
TEST_F(SafetyTest, TestAggregateGuardBindsOnceGlobalsAreBound) {
  std::string_view src = "p(Y) :- r(W), #count{X : q(X, W)} = Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

// X stays inside the elements, so the element's own q(X) binds it.
TEST_F(SafetyTest, TestAggregateElementBindsItsOwnLocalVariable) {
  std::string_view src = "p(Y) :- #count{X : q(X)} = Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

// A guard stands outside the elements, so this X is shared rather than local
// and q(X) alone does not bind it.
TEST_F(SafetyTest, TestAggregateGuardVariableAlsoInsideElements) {
  std::string_view src = "p :- #count{X : q(X)} = X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestShowTermBoundByItsCondition) {
  std::string_view src = "p(1). #show foo(X) : p(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestShowSignatureIsAlwaysSafe) {
  std::string_view src = "p(1). #show p/1. #show.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

// A query names its substitutions with the outputs '#show' decides, so the two
// together have no one answer.
TEST_F(SafetyTest, TestQueryAndShowTogetherIsRejected) {
  std::string_view src = "p(1). #show p/1. p(X)?";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_THAT(std::string(status.message()),
              HasSubstr("cannot have both a query and a '#show'"));
}

TEST_F(SafetyTest, ErrorMessageUnsafeShowTerm) {
  // The condition holds without ever saying which foo to print.
  std::string_view src = "p(1). #show foo(X) : p(1).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: p(1). #show foo(X) : p(1).\n"
            "unsafe variable in rule '#show': X");
}

TEST_F(SafetyTest, ErrorMessageUnsafeWeakConstraint) {
  std::string_view src = ":~ p(X). [1@0, Y]";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: :~ p(X). [1@0, Y]\n"
            "unsafe variable in rule 'weak constraint': Y");
}

}  // namespace
