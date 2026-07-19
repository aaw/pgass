#include "graph.h"

#include <utility>

#include "collect.h"

namespace {

// Invokes `emit(id)` for each head predicate of `head`: the disjuncts of a
// disjunction, or the element atoms of a choice.
template <typename Emit>
void for_each_head_pred(Head& head, PredGraph& graph, const Emit& emit) {
  switch (head.kind) {
    case Head::DisjunctionKind: {
      auto& disjunction = static_cast<Disjunction&>(head);
      for (const auto& literal : disjunction.literals) {
        emit(graph.intern(pred_key(*literal)));
      }
      break;
    }
    case Head::ChoiceKind: {
      auto& choice = static_cast<Choice&>(head);
      if (choice.elements) {
        for (const auto& element : *choice.elements) {
          if (element->literal) emit(graph.intern(pred_key(*element->literal)));
        }
      }
      break;
    }
  }
}

// Invokes `emit(id, negated)` for a choice head's element conditions, which act
// as positive body literals of the rules the choice expands into.
template <typename Emit>
void for_each_head_condition(Head& head, PredGraph& graph, const Emit& emit) {
  if (head.kind != Head::ChoiceKind) return;
  auto& choice = static_cast<Choice&>(head);
  if (!choice.elements) return;
  for (auto& element : *choice.elements) {
    if (element->conditions) {
      collect::for_each_classical_literal(
          *element->conditions, /*negated_context=*/false,
          [&](ClassicalLiteral& cl, bool negated) {
            emit(graph.intern(pred_key(cl)), negated);
          });
    }
  }
}

}  // namespace

PredKey pred_key(const ClassicalLiteral& literal) {
  return PredKey{literal.id, literal.args ? literal.args->size() : 0};
}

int PredGraph::intern(const PredKey& key) {
  auto [it, inserted] = id_of.try_emplace(key, static_cast<int>(preds.size()));
  if (inserted) {
    preds.push_back(key);
    pos_succ.emplace_back();
    neg_succ.emplace_back();
  }
  return it->second;
}

PredGraph build_pred_graph(const Program& const_prog) {
  // collect.h's traversal helpers take mutable references so they can double
  // as rewrite passes elsewhere; this pass only reads, and every real caller
  // holds a genuinely non-const Program, so casting the const away here is
  // safe.
  Program& prog = const_cast<Program&>(const_prog);

  PredGraph graph;
  if (!prog.statements) return graph;

  for (auto& statement : *prog.statements) {
    // Intern the head predicates first so every edge target exists.
    std::vector<int> heads;
    if (statement->head) {
      for_each_head_pred(*statement->head, graph,
                         [&](int id) { heads.push_back(id); });
    }

    auto add_edge = [&](int body_id, bool negated) {
      for (int head_id : heads) {
        auto& succ = negated ? graph.neg_succ : graph.pos_succ;
        succ[body_id].push_back(head_id);
      }
    };
    if (statement->head)
      for_each_head_condition(*statement->head, graph, add_edge);
    if (statement->body && statement->body->items) {
      for (auto& item : *statement->body->items) {
        if (item->kind == BodyItem::NafLiteralKind) {
          auto& naf = static_cast<NafLiteral&>(*item);
          if (naf.literal && naf.literal->kind == Literal::ClassicalLiteralKind) {
            auto& cl = static_cast<ClassicalLiteral&>(*naf.literal);
            add_edge(graph.intern(pred_key(cl)), naf.naf);
          }
          continue;
        }
        auto& aggregate = static_cast<Aggregate&>(*item);
        if (!aggregate.elements) continue;
        for (auto& element : *aggregate.elements) {
          if (!element->literals) continue;
          collect::for_each_classical_literal(
              *element->literals, aggregate.naf,
              [&](ClassicalLiteral& cl, bool negated) {
                int body_id = graph.intern(pred_key(cl));
                add_edge(body_id, negated);
                if (negated) return;
                for (int head_id : heads) {
                  graph.agg_edges.push_back({body_id, head_id, statement.get()});
                }
              });
        }
      }
    }
  }
  return graph;
}

std::vector<int> strongly_connected_components(
    const std::vector<std::vector<int>>& succ) {
  const int N = succ.size();
  std::vector<int> dfn(N, -1), low(N, -1), rep(N, -1), tarjan_stack;
  std::vector<bool> in_stack(N, false);
  int timer = 0;

  for (int i = 0; i < N; ++i) {
    if (dfn[i] != -1) continue;

    // DFS call stack storing: {current_vertex, next_neighbor_index}
    std::vector<std::pair<int, std::size_t>> dfs_stack = {{i, 0}};

    while (!dfs_stack.empty()) {
      auto& [u, idx] = dfs_stack.back();

      // First time visiting node u.
      if (idx == 0) {
        dfn[u] = low[u] = timer++;
        tarjan_stack.push_back(u);
        in_stack[u] = true;
      }

      // If there are still neighbors to explore...
      if (idx < succ[u].size()) {
        int v = succ[u][idx++];
        if (dfn[v] == -1) {
          dfs_stack.push_back({v, 0});  // Descend (recursive call).
        } else if (in_stack[v]) {
          low[u] = std::min(low[u], dfn[v]);
        }
      } else {  // Finished exploring all neighbors of u (post-visit).
        const int done = u;
        dfs_stack.pop_back();

        // Backtrack: update parent's low link.
        if (!dfs_stack.empty()) {
          int parent = dfs_stack.back().first;
          low[parent] = std::min(low[parent], low[done]);
        }

        // If done is root of a strongly connected component, pop its members.
        if (low[done] == dfn[done]) {
          while (true) {
            int v = tarjan_stack.back();
            tarjan_stack.pop_back();
            in_stack[v] = false;
            rep[v] = done;
            if (v == done) break;
          }
        }
      }
    }
  }
  return rep;
}
