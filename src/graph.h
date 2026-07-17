#ifndef GRAPH_H_
#define GRAPH_H_

#include <cstddef>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "ast.h"

// A predicate signature: name plus arity. Two atoms denote the same graph node
// iff they agree on both, so 'p(X)' and 'p(X, Y)' are distinct nodes.
struct PredKey {
  std::string name;
  size_t arity;

  bool operator==(const PredKey& other) const {
    return arity == other.arity && name == other.name;
  }

  template <typename H>
  friend H AbslHashValue(H h, const PredKey& key) {
    return H::combine(std::move(h), key.name, key.arity);
  }
};

// The signature of the predicate `literal` refers to.
PredKey pred_key(const ClassicalLiteral& literal);

// The predicate dependency graph of a program: one node per distinct predicate
// signature, with an edge from each body predicate to each head predicate of
// every rule. Positive and negative edges are kept apart so callers can pick the
// subgraph they need -- head-cycle-freeness looks only at positive edges,
// whereas grounding order needs both. Node ids are dense: 0 .. preds.size() - 1.
struct PredGraph {
  // id -> signature.
  std::vector<PredKey> preds;
  // pos_succ[b] lists the head predicates reachable from body predicate b
  // through a non-negated body literal; neg_succ[b] the same through a
  // default-negated ('not') one. Both are indexed by node id and sized to
  // preds.size().
  std::vector<std::vector<int>> pos_succ;
  std::vector<std::vector<int>> neg_succ;
  // signature -> id, for interning and lookups.
  absl::flat_hash_map<PredKey, int> id_of;

  // Returns the id for `key`, allocating a fresh node (and empty adjacency rows)
  // the first time it is seen.
  int intern(const PredKey& key);
};

// Builds the predicate dependency graph of `prog`. Atoms inside aggregates and
// choice-element conditions count as body dependencies; a default-negated
// aggregate makes all of its atoms negative dependencies. Builtin comparisons
// carry no predicate and are ignored.
PredGraph build_pred_graph(const Program& prog);

// Returns, for each of the n = succ.size() nodes, the id of its strongly
// connected component. Component ids are arbitrary but two nodes share one iff
// they are mutually reachable through `succ`. Callers pass the adjacency they
// care about, e.g. graph.pos_succ for the positive dependency graph.
std::vector<int> strongly_connected_components(
    const std::vector<std::vector<int>>& succ);

#endif  // GRAPH_H_
