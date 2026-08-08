#include "normalize.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "format.h"
#include "parse.h"
#include "test_macros.h"

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
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(program));
}

TEST_F(NormalizeTest, TestShowTermBecomesAShowRule) {
  std::string program = R"(
    p(1).
    #show foo(X) : p(X).
  )";
  std::string expected = R"(
    p(1).
    _show(foo(X)) :- p(X).
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(expected));
  EXPECT_FALSE((*prog)->show_filter.has_value());
}

TEST_F(NormalizeTest, TestShowTermWithoutAConditionBecomesAFact) {
  std::string program = R"(
    p(1).
    #show foo.
  )";
  std::string expected = R"(
    p(1).
    _show(foo).
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(expected));
}

TEST_F(NormalizeTest, TestShowSignaturesBecomeTheFilter) {
  std::string program = R"(
    p(1).
    #show p/1.
    #show -q/2.
  )";
  std::string expected = R"(
    p(1).
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(expected));
  ASSERT_TRUE((*prog)->show_filter.has_value());
  ASSERT_EQ((*prog)->show_filter->size(), 2u);
  EXPECT_FALSE((*prog)->show_filter->at(0).negated);
  EXPECT_EQ((*prog)->show_filter->at(0).name, "p");
  EXPECT_EQ((*prog)->show_filter->at(0).arity, 1u);
  EXPECT_TRUE((*prog)->show_filter->at(1).negated);
  EXPECT_EQ((*prog)->show_filter->at(1).name, "q");
  EXPECT_EQ((*prog)->show_filter->at(1).arity, 2u);
}

// '#show.' names no predicate, but it still turns hiding on, which is what
// makes it print nothing rather than everything.
TEST_F(NormalizeTest, TestShowNothingLeavesAnEmptyFilter) {
  std::string program = R"(
    p(1).
    #show.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  ASSERT_TRUE((*prog)->show_filter.has_value());
  EXPECT_TRUE((*prog)->show_filter->empty());
}

// A condition is a rule body like any other, so classical negation in one is
// rewritten along with the rest of the program.
TEST_F(NormalizeTest, TestShowConditionGetsClassicalNegationRemoved) {
  std::string program = R"(
    -p(1).
    #show foo(X) : -p(X).
  )";
  std::string expected = R"(
    _neg_p(1).
    _show(foo(X)) :- _neg_p(X).
    :- p(X), _neg_p(X).
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(expected));
}

TEST_F(NormalizeTest, TestArithmeticKeepsOnlyTheParenthesesItNeeds) {
  // Parentheses that precedence already implies are dropped when printing,
  // and the ones that change the grouping are kept.
  std::string program = R"(
    p(1 + 2 * 3, 1 + (2 * 3), (1 + 2) * 3, 1 - (2 - 3), (1 - 2) - 3, -(1 + 2)).
  )";
  std::string expected = R"(
    p(1 + 2 * 3, 1 + 2 * 3, (1 + 2) * 3, 1 - (2 - 3), 1 - 2 - 3, -(1 + 2)).
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(expected));
}

TEST_F(NormalizeTest, TestNormalizeChoiceRuleSimple) {
  std::string program = R"(
    { p1; p2; p3 } :- q1.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    p1 | _ch_p1 :- q1.
    p2 | _ch_p2 :- q1.
    p3 | _ch_p3 :- q1.
  )"));
}

TEST_F(NormalizeTest, TestNormalizeChoiceRuleComplex) {
  std::string program = R"(
    1 < { a : a1, a2; b; c : c1, c2; d; e } <= 4 :- x, y, not z.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    a | _ch_a :- a1, a2, x, y, not z.
    b | _ch_b :- x, y, not z.
    c | _ch_c :- c1, c2, x, y, not z.
    d | _ch_d :- x, y, not z.
    e | _ch_e :- x, y, not z.
    :- x, y, not z, not 1 < #count{ _ch_a: a, a1, a2, _ch_b: b, _ch_c: c, c1, c2, _ch_d: d, _ch_e: e } <= 4.
  )"));
}

TEST_F(NormalizeTest, TestNormalizeMultipleChoiceRules) {
  std::string program = R"(
    { p1; p2 } :- q1.
    { r1; r2 } :- s1.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    p1 | _ch_p1 :- q1.
    p2 | _ch_p2 :- q1.
    r1 | _ch_r1 :- s1.
    r2 | _ch_r2 :- s1.
  )"));
}

TEST_F(NormalizeTest, TestNormalizeChoiceWithVars) {
  std::string program = R"(
    { p(X, Y) : q(X) } <= 1 :- r(Y).
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    p(X, Y) | _ch_p(X, Y) :- q(X), r(Y).
    :- r(Y), not #count{ _ch_p(X, Y): p(X, Y), q(X) } <= 1.
  )"));
}

TEST_F(NormalizeTest, TestNormalizeChoiceCountsAnAtomOfferedTwiceOnce) {
  // Two elements offering the same atom share one auxiliary atom, so the bound
  // is read against a count of atoms. Counting elements instead would put this
  // choice over its bound the moment 'a' is chosen.
  std::string program = R"(
    { a : q; a : r } <= 1.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    a | _ch_a :- q.
    a | _ch_a :- r.
    :- not #count{ _ch_a: a, q, _ch_a: a, r } <= 1.
  )"));
}

TEST_F(NormalizeTest, TestNormalizeChoiceAuxAtomLeavesOutConditionVariables) {
  // The auxiliary atom carries the chosen atom's arguments and nothing else.
  // With the condition's X in there too, this would count one tuple per q fact
  // and the bound of 1 would forbid choosing 'a' at all.
  std::string program = R"(
    { a : q(X) } <= 1.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    a | _ch_a :- q(X).
    :- not #count{ _ch_a: a, q(X) } <= 1.
  )"));
}

TEST_F(NormalizeTest, TestRemoveClassicalNegationSimple) {
  std::string program = R"(
    -p :- q.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

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
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

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
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

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
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

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
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    _neg_p :- a.
    _neg_p :- b.
    :- p, _neg_p.
  )"));
}

/* A disjunctive head is left as it stands, head cycle or not. Rewriting it into
   normal rules by shifting the other disjuncts into the body,

     a | b :- body.   becomes   a :- body, not b.   and   b :- body, not a.

   only preserves answer sets while no two head atoms lie on a common positive
   cycle, and which ground atoms do is not known until grounding, so solve.cc
   both asks the question and does the shift. */
TEST_F(NormalizeTest, TestDisjunctiveHeadIsLeftAlone) {
  std::string program = R"(
    a | b | c :- d, e, not f.
    p(X) | p(Y) :- q(X, Y).
    a | b.
    a :- b.
    b :- a.
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    a | b | c :- d, e, not f.
    p(X) | p(Y) :- q(X, Y).
    a | b.
    a :- b.
    b :- a.
  )"));
}

TEST_F(NormalizeTest, TestRewriteWeakConstraintSimple) {
  std::string program = R"(
    :~ node(X), color(X, red). [1@0, X]
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    _viol(0, 1, X) :- node(X), color(X, red).
  )"));
}

TEST_F(NormalizeTest, TestRewriteWeakConstraintDefaultLevel) {
  std::string program = R"(
    :~ q(X). [1, X]
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    _viol(0, 1, X) :- q(X).
  )"));
}

TEST_F(NormalizeTest, TestRewriteWeakConstraintNoTerms) {
  std::string program = R"(
    :~ q. [1@2]
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    _viol(2, 1) :- q.
  )"));
}

TEST_F(NormalizeTest, TestRewriteWeakConstraintMultiple) {
  std::string program = R"(
    :~ a(X). [1@0, X]
    :~ b(X). [2@1, X]
  )";

  Parser parser(program);
  auto prog = parser.parse_program();
  ASSERT_OK(prog);
  ASSERT_OK(normalize(**prog));

  EXPECT_THAT(**prog, EquivalentToSource(R"(
    _viol(0, 1, X) :- a(X).
    _viol(1, 2, X) :- b(X).
  )"));
}

}  // namespace
