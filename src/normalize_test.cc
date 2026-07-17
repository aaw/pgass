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
    p1 :- q1, not _cr0.
    _cr0 :- q1, not p1.
    p2 :- q1, not _cr1.
    _cr1 :- q1, not p2.
    p3 :- q1, not _cr2.
    _cr2 :- q1, not p3.
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
    a :- a1, a2, x, y, not z, not _cr0.
    _cr0 :- a1, a2, x, y, not z, not a.
    b :- x, y, not z, not _cr1.
    _cr1 :- x, y, not z, not b.
    c :- c1, c2, x, y, not z, not _cr2.
    _cr2 :- c1, c2, x, y, not z, not c.
    d :- x, y, not z, not _cr3.
    _cr3 :- x, y, not z, not d.
    e :- x, y, not z, not _cr4.
    _cr4 :- x, y, not z, not e.
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
    p1 :- q1, not _cr0.
    _cr0 :- q1, not p1.
    p2 :- q1, not _cr1.
    _cr1 :- q1, not p2.
    r1 :- s1, not _cr2.
    _cr2 :- s1, not r1.
    r2 :- s1, not _cr3.
    _cr3 :- s1, not r2.
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
    p(X, Y) :- q(X), r(Y), not _cr0(X, Y).
    _cr0(X, Y) :- q(X), r(Y), not p(X, Y).
    :- r(Y), not #count{ _cr0(X, Y): p(X, Y), q(X) } <= 1.
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
    :- p(X), _neg_p(X).
  )"));
}

TEST_F(NormalizeTest,
       TestRemoveClassicalNegationConstraintQuantifiesOverTuples) {
  std::string program = R"(
    -p(a, b).
    p(c, d).
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    _neg_p(a, b).
    p(c, d).
    :- p(X1, X2), _neg_p(X1, X2).
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

TEST_F(NormalizeTest, TestSplitHeadDisjunctionSimple) {
  std::string program = R"(
    a | b | c :- d, e, not f.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    a :- d, e, not f, not b, not c.
    b :- d, e, not f, not a, not c.
    c :- d, e, not f, not a, not b.
  )"));
}

TEST_F(NormalizeTest, TestSplitHeadDisjunctionNoBody) {
  std::string program = R"(
    a | b.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    a :- not b.
    b :- not a.
  )"));
}

TEST_F(NormalizeTest, TestSplitHeadDisjunctionSingleAtomUnchanged) {
  std::string program = R"(
    a :- b.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(normalize(**prog).ok());

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    a :- b.
  )"));
}

/* A positive head cycle (a and b are mutually reachable through 'a :- b' and
   'b :- a') makes the split unsound, so normalization must reject it. Example:

  a | b.
  a :- b.
  b :- a.

  has an answer set of {a,b}. but the normalized version:

  a :- not b.
  b :- not a.
  a :- b.
  b :- a.

  has no answer sets.
*/
TEST_F(NormalizeTest, TestSplitHeadDisjunctionRejectsHeadCycle) {
  std::string program = R"(
    a | b.
    a :- b.
    b :- a.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();

  absl::Status status = normalize(**prog);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(std::string(status.message()), HasSubstr("head-cycle"));
}

/* The head-cycle check is at predicate granularity, so a disjunctive rule that
   repeats the same predicate in its head is always rejected, even with no other
   rules defining p. This exercises that conservative (but sound) behavior. */
TEST_F(NormalizeTest, TestSplitHeadDisjunctionRejectsRepeatedHeadPredicate) {
  std::string program = R"(
    p(X) | p(Y) :- q(X, Y).
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_TRUE(prog.ok()) << prog.status();

  absl::Status status = normalize(**prog);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(std::string(status.message()), HasSubstr("head-cycle"));
}

}  // namespace
