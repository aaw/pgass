#include "safety.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/status/status_matchers.h"
#include "parse.h"

using namespace ::testing;
using absl_testing::IsOk;

namespace {

class SafetyTest : public ::testing::Test {};

TEST_F(SafetyTest, TestSimpleMixedBinding) {
  std::string_view src = "p(X,Z) :- p(X,Y), p(Y,Z).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestNegationAsFailure) {
  std::string_view src = "p(X) :- not q(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestNegationAsFailureButBoundElsewhere) {
  std::string_view src = "p(X) :- not q(X), r(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestBoundByEquality) {
  std::string_view src = "p(X,Y) :- q(X), X = Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestNotBoundByLessThan) {
  // Non-equality binary operations do not bind (compare to previous example).
  std::string_view src = "p(X,Y) :- q(X), X < Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestFixedPointIterations) {
  // q(X) binds X, then Y = X + 1 binds Y, then Z = Y * 2 binds Z.
  std::string_view src = "q(1). p(X,Y,Z) :- Z = Y * 2, Y = X + 1, q(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestUnsafeCircularBind) {
  std::string_view src = "p(X,Y) :- q(Z), X = Y + 1, Y = X - 1.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestSafeAggregation) {
  // Safe aggregate example from ASP-Core 2 spec.
  std::string_view src =
      "p(X,Y) :- q(X), #sum{S,X : r(T,X), S = (2 * T) - X} = Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestUnsafeAggregation) {
  // Unsafe aggregate example from ASP-Core 2 spec.
  std::string_view src =
      "p(X,Y) :- q(X), #sum{S,X : r(T,X), S + X = (2 * T)} = Y.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
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
      "p(X,Y) :- q(X), Z = X + 2, #sum{S : S = Z + 3} = A, B = A + 1.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), IsOk());
}

TEST_F(SafetyTest, TestAggregationBoundAlmostPropagation) {
  // Similar to the previous example but the aggregation isn't fully bound (it
  // includes a reference to an unbound "C") so binding propagation to B never
  // happens.
  std::string_view src =
      "p(X,Y) :- q(X), Z = X + 2, #sum{S : S = Z + C + 3} = A, B = A + 1.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestAggregationLocalScopeDoesNotLeak) {
  // We should recognize variables local to aggregations _only_ in their scopes
  // and not let them leak outside to other expressions.
  std::string_view src = "p(X) :- q(X), #sum{S : S = X + 1}, B = S + X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestNafAggregateDoesNotBind) {
  // A NAF aggregate should not bind its bound variable; X is only referenced
  // inside a negated aggregate so it is unsafe.
  std::string_view src = "p(X) :- not #sum{S : r(S)} = X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

TEST_F(SafetyTest, TestGlobalsDoNotLeakAcrossStatements) {
  std::string_view src = "p(X, Y) :- q(X), Y = X. r(X, Y) :- q(X), Y + 1 = X.";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_THAT(verify_safe(**prog), Not(IsOk()));
}

// Error message examples. These tests document what safety errors look like.

TEST_F(SafetyTest, ErrorMessageSingleUnsafeVariable) {
  std::string_view src = "p(X) :- not q(X).";
  Parser parser(src);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
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
  ASSERT_TRUE(prog.ok()) << prog.status();
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
  ASSERT_TRUE(prog.ok()) << prog.status();
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
  ASSERT_TRUE(prog.ok()) << prog.status();
  auto status = verify_safe(**prog);
  ASSERT_THAT(status, Not(IsOk()));
  EXPECT_EQ(status.message(),
            "line 1: p(X) :- q(X), #sum{S : not r(S)}.\n"
            "unsafe aggregate in rule 'p/1'");
}

}  // namespace
