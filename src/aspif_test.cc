#include "aspif.h"

#include <gtest/gtest.h>

namespace aspif {
namespace {

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

}  // namespace
}  // namespace aspif
