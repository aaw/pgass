#include "graph.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "parse.h"
#include "test_macros.h"

using namespace ::testing;

namespace {

class GraphTest : public ::testing::Test {};

// Returns the node id of predicate `name`/`arity`, or -1 if absent.
int IdOf(const PredGraph& graph, const std::string& name, size_t arity) {
  auto it = graph.id_of.find(PredKey{name, arity});
  return it == graph.id_of.end() ? -1 : it->second;
}

// Whether `succ[from]` contains `to`.
bool HasEdge(const std::vector<std::vector<int>>& succ, int from, int to) {
  if (from < 0 || to < 0) return false;
  const auto& out = succ[from];
  return std::find(out.begin(), out.end(), to) != out.end();
}

PredGraph GraphOf(const std::string& program) {
  Parser parser(program);
  auto prog = parser.parse_program();
  EXPECT_OK(prog);
  return build_pred_graph(**prog);
}

TEST_F(GraphTest, PositiveBodyEdge) {
  PredGraph graph = GraphOf("a :- b.");
  EXPECT_TRUE(HasEdge(graph.pos_succ, IdOf(graph, "b", 0), IdOf(graph, "a", 0)));
  EXPECT_FALSE(HasEdge(graph.neg_succ, IdOf(graph, "b", 0), IdOf(graph, "a", 0)));
}

TEST_F(GraphTest, NegatedBodyEdge) {
  PredGraph graph = GraphOf("a :- not b.");
  EXPECT_TRUE(HasEdge(graph.neg_succ, IdOf(graph, "b", 0), IdOf(graph, "a", 0)));
  EXPECT_FALSE(HasEdge(graph.pos_succ, IdOf(graph, "b", 0), IdOf(graph, "a", 0)));
}

TEST_F(GraphTest, DisjunctiveHeadEdgesToEveryDisjunct) {
  PredGraph graph = GraphOf("a | b :- c.");
  EXPECT_TRUE(HasEdge(graph.pos_succ, IdOf(graph, "c", 0), IdOf(graph, "a", 0)));
  EXPECT_TRUE(HasEdge(graph.pos_succ, IdOf(graph, "c", 0), IdOf(graph, "b", 0)));
  // The disjunction itself induces no edge between its own head atoms.
  EXPECT_FALSE(HasEdge(graph.pos_succ, IdOf(graph, "a", 0), IdOf(graph, "b", 0)));
}

TEST_F(GraphTest, ArityDistinguishesPredicates) {
  PredGraph graph = GraphOf("p(X) :- p(X, Y).");
  EXPECT_NE(IdOf(graph, "p", 1), -1);
  EXPECT_NE(IdOf(graph, "p", 2), -1);
  EXPECT_NE(IdOf(graph, "p", 1), IdOf(graph, "p", 2));
}

// The head-cycle example: a and b are mutually reachable through the two normal
// rules, so the positive graph has edges in both directions.
TEST_F(GraphTest, PositiveCycleFromNormalRules) {
  PredGraph graph = GraphOf("a | b. a :- b. b :- a.");
  int a = IdOf(graph, "a", 0);
  int b = IdOf(graph, "b", 0);
  EXPECT_TRUE(HasEdge(graph.pos_succ, b, a));
  EXPECT_TRUE(HasEdge(graph.pos_succ, a, b));
}

TEST_F(GraphTest, FactHasNodeButNoOutgoingEdges) {
  PredGraph graph = GraphOf("a.");
  int a = IdOf(graph, "a", 0);
  ASSERT_NE(a, -1);
  EXPECT_TRUE(graph.pos_succ[a].empty());
  EXPECT_TRUE(graph.neg_succ[a].empty());
}

TEST_F(GraphTest, SccEdgelessGraphGivesEachNodeItsOwnComponent) {
  std::vector<std::vector<int>> succ = {{}, {}, {}};
  std::vector<int> component = strongly_connected_components(succ);
  ASSERT_EQ(component.size(), 3);
  EXPECT_NE(component[0], component[1]);
  EXPECT_NE(component[0], component[2]);
  EXPECT_NE(component[1], component[2]);
}

TEST_F(GraphTest, SccSelfLoopIsItsOwnComponent) {
  std::vector<std::vector<int>> succ = {{0}, {}};
  std::vector<int> component = strongly_connected_components(succ);
  ASSERT_EQ(component.size(), 2);
  EXPECT_NE(component[0], component[1]);
}

TEST_F(GraphTest, SccMultiNodeCycleSharesOneComponent) {
  // 0 -> 1 -> 2 -> 0.
  std::vector<std::vector<int>> succ = {{1}, {2}, {0}};
  std::vector<int> component = strongly_connected_components(succ);
  ASSERT_EQ(component.size(), 3);
  EXPECT_EQ(component[0], component[1]);
  EXPECT_EQ(component[1], component[2]);
}

TEST_F(GraphTest, AggregateLiteralCreatesAggEdgeToRuleHead) {
  PredGraph graph = GraphOf("p(X) :- dom(X), #count{ Y : p(Y) } >= X.");
  int p = IdOf(graph, "p", 1);
  ASSERT_NE(p, -1);
  ASSERT_EQ(graph.agg_edges.size(), 1);
  EXPECT_EQ(graph.agg_edges[0].body_id, p);
  EXPECT_EQ(graph.agg_edges[0].head_id, p);
  // The aggregate literal still contributes a normal positive edge too, since
  // aggregates otherwise participate in the dependency graph like any body
  // literal.
  EXPECT_TRUE(HasEdge(graph.pos_succ, p, p));
}

TEST_F(GraphTest, NegatedAggregateLiteralCreatesNoAggEdge) {
  PredGraph graph = GraphOf("p(X) :- dom(X), #count{ Y : not p(Y) } >= X.");
  EXPECT_TRUE(graph.agg_edges.empty());
}

TEST_F(GraphTest, AggregateEdgeTargetsEveryHeadOfEnclosingRule) {
  PredGraph graph = GraphOf("a | b :- #count{ X : c(X) } >= 1.");
  int a = IdOf(graph, "a", 0);
  int b = IdOf(graph, "b", 0);
  int c = IdOf(graph, "c", 1);
  ASSERT_EQ(graph.agg_edges.size(), 2);
  EXPECT_TRUE((graph.agg_edges[0].body_id == c && graph.agg_edges[0].head_id == a) ||
              (graph.agg_edges[0].body_id == c && graph.agg_edges[0].head_id == b));
  EXPECT_TRUE((graph.agg_edges[1].body_id == c && graph.agg_edges[1].head_id == a) ||
              (graph.agg_edges[1].body_id == c && graph.agg_edges[1].head_id == b));
  EXPECT_NE(graph.agg_edges[0].head_id, graph.agg_edges[1].head_id);
}

TEST_F(GraphTest, SccSeparateComponentsWithCrossEdge) {
  // 0 <-> 1 is one component; 2 <-> 3 is another; 1 -> 2 crosses but doesn't
  // merge them since it isn't part of a cycle back to 1.
  std::vector<std::vector<int>> succ = {{1}, {0, 2}, {3}, {2}};
  std::vector<int> component = strongly_connected_components(succ);
  ASSERT_EQ(component.size(), 4);
  EXPECT_EQ(component[0], component[1]);
  EXPECT_EQ(component[2], component[3]);
  EXPECT_NE(component[0], component[2]);
  EXPECT_LT(component[0], component[2]);
}

TEST_F(GraphTest, SccComponentIdsAscendAlongAChain) {
  // 0 -> 1 -> 2, no cycles: three singleton components.
  std::vector<std::vector<int>> succ = {{1}, {2}, {}};
  std::vector<int> component = strongly_connected_components(succ);
  ASSERT_EQ(component.size(), 3);
  EXPECT_LT(component[0], component[1]);
  EXPECT_LT(component[1], component[2]);
}

TEST_F(GraphTest, SccComponentIdsAscendWithDiamondShapedDependencies) {
  // 0 -> 1, 0 -> 2, 1 -> 3, 2 -> 3: 0 must precede 1 and 2, which must
  // precede 3, regardless of visit order.
  std::vector<std::vector<int>> succ = {{1, 2}, {3}, {3}, {}};
  std::vector<int> component = strongly_connected_components(succ);
  ASSERT_EQ(component.size(), 4);
  EXPECT_LT(component[0], component[1]);
  EXPECT_LT(component[0], component[2]);
  EXPECT_LT(component[1], component[3]);
  EXPECT_LT(component[2], component[3]);
}

}  // namespace
