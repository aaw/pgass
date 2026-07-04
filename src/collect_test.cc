#include "collect.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "parse.h"

using namespace ::testing;

namespace {

// Parses `source` as a single term (e.g. "f(X, g(Y), X)").
std::unique_ptr<Term> ParseTerm(std::string_view source) {
  Parser parser(source);
  auto term = parser.parse_term();
  EXPECT_TRUE(term.ok()) << term.status();
  return std::move(*term);
}

// Parses `source` as a single naf literal (e.g. "not p(X, Y)") and returns its
// underlying literal.
std::unique_ptr<Literal> ParseLiteral(std::string_view source) {
  Parser parser(source);
  auto naf = parser.parse_naf_literal();
  EXPECT_TRUE(naf.ok()) << naf.status();
  return std::move((*naf)->literal);
}

TEST(CollectTest, OrderedTermVariablesAreDistinctAndFirstOccurrence) {
  auto term = ParseTerm("f(Y, g(X, Y), X)");
  std::vector<std::string> vars;
  collect::collect_variables(*term, vars);
  EXPECT_THAT(vars, ElementsAre("Y", "X"));
}

TEST(CollectTest, OrderedTermVariablesRecurseIntoArithmetic) {
  auto term = ParseTerm("X + (Y * Z)");
  std::vector<std::string> vars;
  collect::collect_variables(*term, vars);
  EXPECT_THAT(vars, ElementsAre("X", "Y", "Z"));
}

TEST(CollectTest, SetTermVariablesAreDistinct) {
  auto term = ParseTerm("f(X, X, Y)");
  absl::flat_hash_set<std::string_view> vars;
  collect::collect_variables(*term, vars);
  EXPECT_THAT(vars, UnorderedElementsAre("X", "Y"));
}

TEST(CollectTest, NoVariablesInGroundTerm) {
  auto term = ParseTerm("f(a, 1, \"s\")");
  std::vector<std::string> vars;
  collect::collect_variables(*term, vars);
  EXPECT_THAT(vars, IsEmpty());
}

TEST(CollectTest, ClassicalLiteralVariables) {
  auto literal = ParseLiteral("p(X, Y, X)");
  std::vector<std::string> vars;
  collect::collect_variables(*literal, vars);
  EXPECT_THAT(vars, ElementsAre("X", "Y"));
}

TEST(CollectTest, BuiltinAtomVariablesSpanBothSides) {
  auto literal = ParseLiteral("X = Y + 1");
  std::vector<std::string> vars;
  collect::collect_variables(*literal, vars);
  EXPECT_THAT(vars, ElementsAre("X", "Y"));
}

TEST(CollectTest, AppendsToExistingVectorWithoutDuplicating) {
  std::vector<std::string> vars = {"X"};
  auto term = ParseTerm("f(X, Z)");
  collect::collect_variables(*term, vars);
  EXPECT_THAT(vars, ElementsAre("X", "Z"));
}

}  // namespace
