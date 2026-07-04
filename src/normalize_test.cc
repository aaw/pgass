#include "normalize.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "format.h"
#include "parse.h"

using namespace ::testing;

namespace {

class NormalizeTest : public ::testing::Test {};

// Trims leading/trailing whitespace and collapses whitespace runs containing
// newlines into a single space.
std::string Strip(const std::string& str) {
  size_t start = str.find_first_not_of(" \n");
  if (start == std::string::npos) return "";
  size_t end = str.find_last_not_of(" \n") + 1;

  std::string result;
  size_t i = start;
  while (i < end) {
    if (!std::isspace(str[i])) {
      result += str[i++];
      continue;
    }
    size_t ws_start = i;
    bool has_newline = false;
    while (i < end && std::isspace(str[i])) {
      if (str[i] == '\n') has_newline = true;
      i++;
    }
    if (has_newline) {
      result += ' ';
    } else {
      result.append(str, ws_start, i - ws_start);
    }
  }
  return result;
}

MATCHER_P(EquivalentToSource, expected, "") {
  std::string actual = Strip(format(arg));
  std::string want = Strip(expected);
  if (actual == want) return true;
  *result_listener << "formatted source did not match.\n  actual:   " << actual
                   << "\n  expected: " << want;
  return false;
}

TEST_F(NormalizeTest, TestNoNormalization) {
  std::string program = R"(
    edge(a, b).
    reachable(X, Y) :- edge(X, Y).
    reachable(a, b)?
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(program));
}

TEST_F(NormalizeTest, TestNormalizeChoiceRuleSimple) {
  std::string program = R"(
    { p1; p2; p3 } :- q1.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    p1 | _cr0 :- q1.
    p2 | _cr1 :- q1.
    p3 | _cr2 :- q1.
  )"));
}

TEST_F(NormalizeTest, TestNormalizeChoiceRuleComplex) {
  std::string program = R"(
    1 < { a : a1, a2; b; c : c1, c2; d; e } <= 4 :- x, y, not z.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    a | _cr0 :- a1, a2, x, y, not z.
    b | _cr1 :- x, y, not z.
    c | _cr2 :- c1, c2, x, y, not z.
    d | _cr3 :- x, y, not z.
    e | _cr4 :- x, y, not z.
    :- x, y, not z, not 1 < #count{ _cr0: a, a1, a2, _cr1: b, _cr2: c, c1, c2, _cr3: d, _cr4: e } <= 4.
  )"));
}

TEST_F(NormalizeTest, TestNormalizeMultipleChoiceRules) {
  std::string program = R"(
    { p1; p2 } :- q1.
    { r1; r2 } :- s1.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    p1 | _cr0 :- q1.
    p2 | _cr1 :- q1.
    r1 | _cr2 :- s1.
    r2 | _cr3 :- s1.
  )"));
}

TEST_F(NormalizeTest, TestRemoveClassicalNegationSimple) {
  std::string program = R"(
    -p :- q.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    _neg_p :- q.
    :- p, _neg_p.
  )"));
}

TEST_F(NormalizeTest, TestRemoveClassicalNegationWithArgs) {
  std::string program = R"(
    -p(X) :- q(X).
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    _neg_p(X) :- q(X).
    :- p(X0), _neg_p(X0).
  )"));
}

TEST_F(NormalizeTest, TestRemoveClassicalNegationInBodyAndQuery) {
  std::string program = R"(
    r :- -p, not -q.
    -p?
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    r :- _neg_p, not _neg_q.
    :- p, _neg_p.
    :- q, _neg_q.
    _neg_p?
  )"));
}

TEST_F(NormalizeTest, TestRemoveClassicalNegationOneConstraintPerPredicate) {
  std::string program = R"(
    -p :- a.
    -p :- b.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    _neg_p :- a.
    _neg_p :- b.
    :- p, _neg_p.
  )"));
}

TEST_F(NormalizeTest, TestNormalizeChoiceWithVars) {
  std::string program = R"(
    { p(X, Y) : q(X) } <= 1 :- r(Y).
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    p(X, Y) | _cr0(X, Y) :- q(X), r(Y).
    :- r(Y), not #count{ _cr0(X, Y): p(X, Y), q(X) } <= 1.
  )"));
}

}  // namespace
