#include "safety.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "parse.h"

using namespace ::testing;

namespace {

class SafetyTest : public ::testing::Test {};

TEST_F(SafetyTest, TestSimpleMixedBinding) {
  Parser parser("p(X,Z) :- p(X,Y), p(Y,Z).");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_TRUE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestNegationAsFailure) {
  Parser parser("p(X) :- not q(X).");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_FALSE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestNegationAsFailureButBoundElsewhere) {
  Parser parser("p(X) :- not q(X), r(X).");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_TRUE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestBoundByEquality) {
  Parser parser("p(X,Y) :- q(X), X = Y.");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_TRUE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestNotBoundByLessThan) {
  // Non-equality binary operations do not bind (compare to previous example).
  Parser parser("p(X,Y) :- q(X), X < Y.");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_FALSE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestFixedPointIterations) {
  // q(X) binds X, then Y = X + 1 binds Y, then Z = Y * 2 binds Z.
  Parser parser("q(1). p(X,Y,Z) :- Z = Y * 2, Y = X + 1, q(X).");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_TRUE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestUnsafeCircularBind) {
  Parser parser("p(X,Y) :- q(Z), X = Y + 1, Y = X - 1.");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_FALSE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestSafeAggregation) {
  // Safe aggregate example from ASP-Core 2 spec.
  Parser parser("p(X,Y) :- q(X), #sum{S,X : r(T,X), S = (2 * T) - X} = Y.");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_TRUE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestUnsafeAggregation) {
  // Unsafe aggregate example from ASP-Core 2 spec.
  Parser parser("p(X,Y) :- q(X), #sum{S,X : r(T,X), S + X = (2 * T)} = Y.");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_FALSE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestAggregationBoundPropagation) {
  // Multiple rounds of binding with aggregates involved:
  // *  X is bound by q(X).
  // *  Z is then bound by X + 2.
  // *  The #sum aggregation is then bound by Z.
  // *  A is bound by the #sum aggregation's binding.
  // *  Finally, B is bound by A's binding.
  Parser parser(
      "p(X,Y) :- q(X), Z = X + 2, #sum{S : S = Z + 3} = A, B = A + 1.");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_TRUE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestAggregationBoundAlmostPropagation) {
  // Similar to the previous example but the aggregation isn't fully bound (it
  // includes a reference to an unbound "C") so binding propagation to B never
  // happens.
  Parser parser(
      "p(X,Y) :- q(X), Z = X + 2, #sum{S : S = Z + C + 3} = A, B = A + 1.");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_FALSE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestAggregationLocalScopeDoesNotLeak) {
  // We should recognize variables local to aggregations _only_ in their scopes
  // and not let them leak outside to other expressions.
  Parser parser("p(X) :- q(X), #sum{S : S = X + 1}, B = S + Z.");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_FALSE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestNafAggregateDoesNotBind) {
  // A NAF aggregate should not bind its bound variable; X is only referenced
  // inside a negated aggregate so it is unsafe.
  Parser parser("p(X) :- not #sum{S : r(S)} = X.");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_FALSE(verify_safe(**prog));
}

TEST_F(SafetyTest, TestGlobalsDoNotLeakAcrossStatements) {
  Parser parser("p(X, Y) :- q(X), Y = X. r(X, Y) :- q(X), Y + 1 = X.");
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_FALSE(verify_safe(**prog));
}

}  // namespace
