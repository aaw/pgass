#include "aspif.h"

#include <gtest/gtest.h>

#include <string>

#include "absl/status/status.h"
#include "gmock/gmock.h"

namespace aspif {
namespace {

using ::testing::HasSubstr;

TEST(AspifTest, EmptyProgramIsHeaderAndTerminator) {
  EXPECT_EQ(to_aspif(Program{}), "asp 1 0 0\n0\n");
}

// Listing 49 of the aspif paper (arXiv:2008.06692): the program
// '{a}. b :- a. c :- not a.' with atoms a=1, b=2, c=3.
TEST(AspifTest, PaperListing49) {
  Program prog;
  Atom a = prog.new_atom();
  Atom b = prog.new_atom();
  Atom c = prog.new_atom();

  prog.rules.push_back(Rule{.head_type = Rule::HeadType::kChoice, .head = {a}});
  prog.rules.push_back(Rule{.head = {b}, .body = {a}});
  prog.rules.push_back(Rule{.head = {c}, .body = {-a}});
  prog.outputs.push_back(Output{.name = "a", .condition = {a}});
  prog.outputs.push_back(Output{.name = "b", .condition = {b}});
  prog.outputs.push_back(Output{.name = "c", .condition = {c}});

  EXPECT_EQ(to_aspif(prog),
            "asp 1 0 0\n"
            "1 1 1 1 0 0\n"
            "1 0 1 2 0 1 1\n"
            "1 0 1 3 0 1 -1\n"
            "4 1 a 1 1\n"
            "4 1 b 1 2\n"
            "4 1 c 1 3\n"
            "0\n");
}

TEST(AspifTest, IntegrityConstraintHasEmptyHead) {
  Program prog;
  Atom a = prog.new_atom();
  Atom b = prog.new_atom();
  prog.rules.push_back(Rule{.body = {a, -b}});

  EXPECT_EQ(to_aspif(prog),
            "asp 1 0 0\n"
            "1 0 0 0 2 1 -2\n"
            "0\n");
}

TEST(AspifTest, WeightBody) {
  Program prog;
  Atom a = prog.new_atom();
  Atom b = prog.new_atom();
  Atom aux = prog.new_atom();
  // aux holds when at least 2 of {a, b, not b-weighted-3} are satisfied:
  // aux :- 2 <= #sum{ 1:a; 1:b; 3:not b }.
  prog.rules.push_back(Rule{.head = {aux},
                            .body_type = Rule::BodyType::kWeight,
                            .lower_bound = 2,
                            .weighted_body = {{a, 1}, {b, 1}, {-b, 3}}});

  EXPECT_EQ(to_aspif(prog),
            "asp 1 0 0\n"
            "1 0 1 3 1 2 3 1 1 2 1 -2 3\n"
            "0\n");
}

TEST(AspifTest, MinimizeStatementPerPriority) {
  Program prog;
  Atom v1 = prog.new_atom();
  Atom v2 = prog.new_atom();
  Atom v3 = prog.new_atom();
  prog.minimize.push_back(Minimize{.priority = 0, .lits = {{v1, 5}, {v2, 1}}});
  prog.minimize.push_back(Minimize{.priority = 1, .lits = {{v3, 2}}});

  EXPECT_EQ(to_aspif(prog),
            "asp 1 0 0\n"
            "2 0 2 1 5 2 1\n"
            "2 1 1 3 2\n"
            "0\n");
}

TEST(AspifTest, FactOutputHasEmptyCondition) {
  Program prog;
  prog.new_atom();
  prog.outputs.push_back(Output{.name = "p(1,2)"});

  EXPECT_EQ(to_aspif(prog),
            "asp 1 0 0\n"
            "4 6 p(1,2) 0\n"
            "0\n");
}

TEST(AspifTest, GroundQueryBecomesAssumption) {
  Program prog;
  Atom q = prog.new_atom();
  prog.rules.push_back(Rule{.head = {q}});
  prog.query = {{q}};

  EXPECT_EQ(to_aspif(prog),
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "6 1 1\n"
            "0\n");
}

// The atoms a query matched are alternatives, and an assumption asks for all
// of its literals at once. So a query of several atoms prints nothing.
TEST(AspifTest, QueryOfSeveralAtomsPrintsNoAssumption) {
  Program prog;
  Atom p1 = prog.new_atom();
  Atom p2 = prog.new_atom();
  prog.rules.push_back(Rule{.head = {p1}});
  prog.rules.push_back(Rule{.head = {p2}});
  prog.query = {p1, p2};

  EXPECT_EQ(to_aspif(prog),
            "asp 1 0 0\n"
            "1 0 1 1 0 0\n"
            "1 0 1 2 0 0\n"
            "0\n");
}

// Reading a document and writing it back out gives the document again.
TEST(FromAspifTest, RoundTripsEveryStatementTypeItReads) {
  const std::string text =
      "asp 1 0 0\n"
      "1 1 1 1 0 0\n"                   // choice head
      "1 0 2 2 3 0 2 1 -2\n"            // disjunctive head, normal body
      "1 0 0 0 1 -3\n"                  // integrity constraint
      "1 0 1 4 1 2 3 1 1 2 1 -2 3\n"    // weight body
      "2 0 2 1 5 2 1\n"
      "2 1 1 3 2\n"
      "4 1 a 1 1\n"
      "4 6 p(1,2) 0\n"
      "6 1 4\n"
      "0\n";

  auto prog = from_aspif(text);
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_EQ(to_aspif(*prog), text);
}

// A name is taken by its length prefix, not up to the next space.
TEST(FromAspifTest, OutputNameHoldsSpaces) {
  auto prog = from_aspif(
      "asp 1 0 0\n"
      "4 7 p(a, b) 0\n"
      "0\n");
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_EQ(prog->outputs.size(), 1u);
  EXPECT_EQ(prog->outputs.front().name, "p(a, b)");
}

// Solving allocates atoms of its own, so the ids in use are spoken for.
TEST(FromAspifTest, NextAtomIsPastEveryAtomNamed) {
  auto prog = from_aspif(
      "asp 1 0 0\n"
      "1 0 1 7 0 1 -12\n"
      "0\n");
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_EQ(prog->next_atom, 13);
}

TEST(FromAspifTest, EmptyProgramIsHeaderAndTerminator) {
  auto prog = from_aspif("asp 1 0 0\n0\n");
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_TRUE(prog->rules.empty());
  EXPECT_EQ(prog->next_atom, 1);
}

TEST(FromAspifTest, CommentIsSkipped) {
  auto prog = from_aspif(
      "asp 1 0 0\n"
      "10 anything at all\n"
      "1 0 1 1 0 0\n"
      "0\n");
  ASSERT_TRUE(prog.ok()) << prog.status();
  EXPECT_EQ(prog->rules.size(), 1u);
}

// A one-literal assumption is how to_aspif writes a ground query.
TEST(FromAspifTest, AssumptionBecomesTheQuery) {
  auto prog = from_aspif(
      "asp 1 0 0\n"
      "1 0 1 1 0 0\n"
      "6 1 1\n"
      "0\n");
  ASSERT_TRUE(prog.ok()) << prog.status();
  ASSERT_TRUE(prog->query.has_value());
  EXPECT_THAT(*prog->query, testing::ElementsAre(1));
}

TEST(FromAspifTest, AssumptionOfSeveralLiteralsIsRefused) {
  auto prog = from_aspif(
      "asp 1 0 0\n"
      "6 2 1 2\n"
      "0\n");
  ASSERT_FALSE(prog.ok());
  EXPECT_THAT(std::string(prog.status().message()),
              HasSubstr("all of them at once"));
}

TEST(FromAspifTest, StatementTypeItCannotHonourIsRefused) {
  // Statement 5, an external atom.
  auto prog = from_aspif(
      "asp 1 0 0\n"
      "5 1 0\n"
      "0\n");
  ASSERT_FALSE(prog.ok());
  EXPECT_THAT(std::string(prog.status().message()),
              HasSubstr("statement type 5 is not supported"));
}

TEST(FromAspifTest, MalformedDocumentsAreRefused) {
  EXPECT_FALSE(from_aspif("").ok());
  // No header.
  EXPECT_FALSE(from_aspif("1 0 1 1 0 0\n0\n").ok());
  // An incremental document, which pgass has no way to solve in one go.
  EXPECT_FALSE(from_aspif("asp 1 0 0 incremental\n0\n").ok());
  // No terminator.
  EXPECT_FALSE(from_aspif("asp 1 0 0\n1 0 1 1 0 0\n").ok());
  // A statement after the terminator.
  EXPECT_FALSE(from_aspif("asp 1 0 0\n0\n1 0 1 1 0 0\n").ok());
  // A rule that stops in the middle.
  EXPECT_FALSE(from_aspif("asp 1 0 0\n1 0 1 1 0\n0\n").ok());
  // More fields than the statement asked for.
  EXPECT_FALSE(from_aspif("asp 1 0 0\n1 0 1 1 0 0 9\n0\n").ok());
  // 0 names no atom, and a head atom cannot be negative.
  EXPECT_FALSE(from_aspif("asp 1 0 0\n1 0 1 1 0 1 0\n0\n").ok());
  EXPECT_FALSE(from_aspif("asp 1 0 0\n1 0 1 -1 0 0\n0\n").ok());
  // Counts cannot be negative.
  EXPECT_FALSE(from_aspif("asp 1 0 0\n1 0 -1 0 0\n0\n").ok());
  // Head and body types are 0 or 1.
  EXPECT_FALSE(from_aspif("asp 1 0 0\n1 2 1 1 0 0\n0\n").ok());
  EXPECT_FALSE(from_aspif("asp 1 0 0\n1 0 1 1 2 0\n0\n").ok());
  // An output name longer than the line that holds it.
  EXPECT_FALSE(from_aspif("asp 1 0 0\n4 9 p(1,2) 0\n0\n").ok());
}

TEST(ChoiceRulesTest, EachElementBecomesADisjunctionWithAFreshAtom) {
  auto prog = from_aspif(
      "asp 1 0 0\n"
      "1 1 2 1 2 0 1 3\n"
      "0\n");
  ASSERT_TRUE(prog.ok()) << prog.status();
  replace_choice_rules(*prog);

  EXPECT_EQ(to_aspif(*prog),
            "asp 1 0 0\n"
            "1 0 2 1 4 0 1 3\n"
            "1 0 2 2 5 0 1 3\n"
            "0\n");
}

TEST(ChoiceRulesTest, WeightBodyIsCarriedToEachDisjunction) {
  auto prog = from_aspif(
      "asp 1 0 0\n"
      "1 1 1 1 1 2 2 2 1 3 1\n"
      "0\n");
  ASSERT_TRUE(prog.ok()) << prog.status();
  replace_choice_rules(*prog);

  EXPECT_EQ(to_aspif(*prog),
            "asp 1 0 0\n"
            "1 0 2 1 4 1 2 2 2 1 3 1\n"
            "0\n");
}

// A choice of no atoms derives nothing and rules nothing out.
TEST(ChoiceRulesTest, EmptyChoiceHeadLeavesNoRule) {
  auto prog = from_aspif(
      "asp 1 0 0\n"
      "1 1 0 0 1 1\n"
      "0\n");
  ASSERT_TRUE(prog.ok()) << prog.status();
  replace_choice_rules(*prog);
  EXPECT_TRUE(prog->rules.empty());
}

TEST(ChoiceRulesTest, DisjunctiveRuleIsLeftAlone) {
  const std::string text =
      "asp 1 0 0\n"
      "1 0 2 1 2 0 1 -3\n"
      "0\n";
  auto prog = from_aspif(text);
  ASSERT_TRUE(prog.ok()) << prog.status();
  replace_choice_rules(*prog);
  EXPECT_EQ(to_aspif(*prog), text);
}

}  // namespace
}  // namespace aspif
