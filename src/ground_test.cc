#include "ground.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "absl/status/statusor.h"
#include "macros.h"
#include "normalize.h"
#include "parse.h"

using ::testing::HasSubstr;

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

TEST(GroundTest, NegationKeptWhenPossibleDroppedWhenNot) {
  auto out = ground_source("p(1). p(2). q(1). s(X) :- p(X), not q(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  // q(1) is derivable, so 's(1) :- p(1), not q(1).' keeps the negation.
  // q(2) is not, so 'not q(2)' is dropped: 's(2) :- p(2).'.
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 1 3 0 0\n"
            "1 0 1 4 0 2 1 -3\n"
            "1 0 1 5 0 1 2\n"
            "4 4 p(1) 1 1\n"
            "4 4 p(2) 1 2\n"
            "4 4 q(1) 1 3\n"
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

TEST(GroundTest, ConstraintHasNoHead) {
  auto out = ground_source("p(1). q(1). :- p(X), q(X).");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "1 0 0 0 2 1 2\n"
            "4 4 p(1) 1 1\n"
            "4 4 q(1) 1 2\n"
            "0\n");
}

TEST(GroundTest, ZeroArityAtoms) {
  auto out = ground_source("p. q :- not p.");
  ASSERT_TRUE(out.ok()) << out.status();
  EXPECT_EQ(*out,
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 1 -1\n"
            "4 1 p 1 1\n"
            "4 1 q 1 2\n"
            "0\n");
}

TEST(GroundTest, AggregatesRejected) {
  auto out = ground_source("p. q :- #count{ 1 : p } > 0.");
  ASSERT_FALSE(out.ok());
  EXPECT_THAT(std::string(out.status().message()), HasSubstr("aggregate"));
}

TEST(GroundTest, ArithmeticRejected) {
  auto out = ground_source("p(1). q(X) :- p(X), X < 1 + 1.");
  ASSERT_FALSE(out.ok());
  EXPECT_THAT(std::string(out.status().message()), HasSubstr("arithmetic"));
}

TEST(GroundTest, FunctionTermsRejected) {
  auto out = ground_source("p(f(1)).");
  ASSERT_FALSE(out.ok());
  EXPECT_THAT(std::string(out.status().message()), HasSubstr("function"));
}

}  // namespace
