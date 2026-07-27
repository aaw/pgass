#include "safety.h"

#include <algorithm>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "collect.h"
#include "graph.h"

// ASP safety requires that every variable in a rule body be bound: reachable
// by ground values without guessing. The three binding sources are:
//
//   Classical atoms: q(X) binds every variable in its argument list.
//   Equality: V = <expr> binds V when all variables in <expr> are already bound
//             (and vice versa when V is on the right).
//   Aggregates: a fully-bound aggregate propagates binding to its output
//               variable. NAF aggregates never bind.

namespace {

template <typename T>
bool is_subset(const absl::flat_hash_set<T>& a,
               const absl::flat_hash_set<T>& b) {
  if (a.size() > b.size()) return false;
  for (const T s : a) {
    if (!b.contains(s)) {
      return false;
    }
  }
  return true;
}

// Updates `bound_vars` and `vars` for a builtin atom (e.g. `X = Y + 1`).
// Binds the lone variable on either side of an equality if the other side is
// fully bound. Always records all variables in the atom in `vars`.
void propagate_atom_bindings(const BuiltinAtom* atom,
                             absl::flat_hash_set<std::string_view>& bound_vars,
                             absl::flat_hash_set<std::string_view>& vars) {
  absl::flat_hash_set<std::string_view> lhs_vars, rhs_vars;
  collect::collect_variables(*atom->left, lhs_vars);
  collect::collect_variables(*atom->right, rhs_vars);
  if (atom->left->kind == Term::VariableKind && atom->op == BinopType::kEQUAL &&
      is_subset(rhs_vars, bound_vars)) {
    CHECK_EQ(lhs_vars.size(), static_cast<std::size_t>(1));
    std::string_view v = *lhs_vars.begin();
    bound_vars.insert(v);
  }
  if (atom->right->kind == Term::VariableKind &&
      atom->op == BinopType::kEQUAL && is_subset(lhs_vars, bound_vars)) {
    CHECK_EQ(rhs_vars.size(), static_cast<std::size_t>(1));
    std::string_view v = *rhs_vars.begin();
    bound_vars.insert(v);
  }
  for (std::string_view v : lhs_vars) {
    vars.insert(v);
  }
  for (std::string_view v : rhs_vars) {
    vars.insert(v);
  }
}

// Updates `bound_vars` and `vars` for a (possibly negated) literal.
// NAF literals record variables in `vars` but never add to `bound_vars`.
void propagate_naf_literal(const NafLiteral* naf_lit,
                           absl::flat_hash_set<std::string_view>& bound_vars,
                           absl::flat_hash_set<std::string_view>& vars) {
  auto* lit = naf_lit->literal.get();
  switch (lit->kind) {
    case Literal::BuiltinAtomKind: {
      auto* atom = static_cast<BuiltinAtom*>(lit);
      absl::flat_hash_set<std::string_view> throwaway;  // NAF atoms don't bind.
      auto& effective_bound = naf_lit->naf ? throwaway : bound_vars;
      propagate_atom_bindings(atom, effective_bound, vars);
      break;
    }
    case Literal::ClassicalLiteralKind: {
      auto* classical_lit = static_cast<ClassicalLiteral*>(lit);
      absl::flat_hash_set<std::string_view> cl_vars;
      collect::collect_variables(*classical_lit, cl_vars);
      for (const std::string_view var : cl_vars) {
        vars.insert(var);
        if (!naf_lit->naf) bound_vars.insert(var);
      }
      break;
    }
  }
}

// Returns "line N: <source line text>" for the line at byte offset pos.
std::string format_source_line(std::string_view source, size_t pos) {
  size_t clamped = std::min(pos, source.size());
  int line = 1;
  size_t line_start = 0;
  for (size_t i = 0; i < clamped; ++i) {
    if (source[i] == '\n') {
      ++line;
      line_start = i + 1;
    }
  }
  size_t line_end = line_start;
  while (line_end < source.size() && source[line_end] != '\n') ++line_end;
  return absl::StrCat("line ", line, ": ",
                      source.substr(line_start, line_end - line_start));
}

// Returns a human-readable label for a rule's head (e.g. "p/2", "{p/1; q/2}",
// or "integrity constraint").
std::string head_description(const Statement& stmt) {
  if (stmt.weight) return "weak constraint";
  if (stmt.head == nullptr) return "integrity constraint";
  if (auto* disj = dynamic_cast<const Disjunction*>(stmt.head.get())) {
    std::string result;
    for (const auto& lit : disj->literals) {
      if (!result.empty()) result += " | ";
      absl::StrAppend(&result, lit->id, "/", lit->args ? lit->args->size() : 0);
    }
    return result;
  }
  if (auto* choice = dynamic_cast<const Choice*>(stmt.head.get())) {
    if (!choice->elements) return "{}";
    std::string result = "{";
    bool first = true;
    for (const auto& elem : *(choice->elements)) {
      if (!first) result += "; ";
      first = false;
      absl::StrAppend(&result, elem->literal->id, "/",
                      elem->literal->args ? elem->literal->args->size() : 0);
    }
    result += "}";
    return result;
  }
  return "rule";
}

}  // namespace

absl::Status verify_safe(const Program& prog) {
  std::string_view source = prog.source;
  for (const auto& statement : *(prog.statements)) {
    if (statement->body == nullptr) continue;

    absl::flat_hash_set<std::string_view> bound_vars, vars;
    absl::flat_hash_set<Aggregate*> bound_aggregates, aggregates;

    std::size_t prev_num_bound = 0;
    do {
      prev_num_bound = bound_vars.size();
      for (const auto& item : *(statement->body->items)) {
        switch (item->kind) {
          case BodyItem::NafLiteralKind: {
            propagate_naf_literal(static_cast<NafLiteral*>(item.get()),
                                  bound_vars, vars);
            break;
          }
          case BodyItem::AggregateKind: {
            auto* aggregate = static_cast<Aggregate*>(item.get());
            aggregates.insert(aggregate);

            // First, collect vars in aggregate lower and upper bounds, if any.
            if (aggregate->lb_term != nullptr) {
              collect::collect_variables(*aggregate->lb_term, vars);
            }
            if (aggregate->ub_term != nullptr) {
              collect::collect_variables(*aggregate->ub_term, vars);
            }

            // Outer bound variables are in scope inside the aggregate body.
            absl::flat_hash_set<std::string_view> agg_bound_vars = bound_vars;
            absl::flat_hash_set<std::string_view> agg_vars;
            std::size_t prev_num_agg_bound = 0;
            do {
              prev_num_agg_bound = agg_bound_vars.size();
              // '#count{ }' has no elements to bind anything.
              if (aggregate->elements == nullptr) break;
              for (const auto& agg_element : *(aggregate->elements)) {
                // An element with no condition, e.g. the '1' of '#count{ 1 }',
                // puts its tuple in the set whatever else holds, so it binds
                // nothing and needs nothing bound.
                if (agg_element->literals == nullptr) continue;
                for (const auto& naf_literal : *(agg_element->literals)) {
                  propagate_naf_literal(naf_literal.get(), agg_bound_vars,
                                        agg_vars);
                }
              }
            } while (prev_num_agg_bound < agg_bound_vars.size());

            bool bound = is_subset(agg_vars, agg_bound_vars);
            if (bound) bound_aggregates.insert(aggregate);

            // A bound aggregate propagates to its output variable if connected
            // by equality (e.g. `#sum{...} = X` binds X).
            if (bound && !aggregate->naf && aggregate->lb_term != nullptr &&
                aggregate->lb_term->kind == Term::VariableKind &&
                aggregate->lb_op == BinopType::kEQUAL) {
              bound_vars.insert(
                  static_cast<Variable*>(aggregate->lb_term.get())->name);
            }
            if (bound && !aggregate->naf && aggregate->ub_term != nullptr &&
                aggregate->ub_term->kind == Term::VariableKind &&
                aggregate->ub_op == BinopType::kEQUAL) {
              bound_vars.insert(
                  static_cast<Variable*>(aggregate->ub_term.get())->name);
            }

            break;
          }
        }
      }
    } while (prev_num_bound < bound_vars.size());

    // A weak constraint's weight, level, and distinctness terms must be bound
    // by the body, same as any other variable use: they're the "head" of a
    // weak constraint in everything but syntax.
    if (statement->weight) {
      collect::collect_variables(*statement->weight->weight, vars);
      if (statement->weight->level) {
        collect::collect_variables(*statement->weight->level, vars);
      }
      if (statement->weight->terms) {
        for (const auto& term : *statement->weight->terms) {
          collect::collect_variables(*term, vars);
        }
      }
    }

    if (!is_subset(vars, bound_vars)) {
      std::vector<std::string> unbound;
      for (std::string_view v : vars) {
        if (!bound_vars.contains(v)) unbound.push_back(std::string(v));
      }
      std::sort(unbound.begin(), unbound.end());
      return absl::InvalidArgumentError(absl::StrCat(
          format_source_line(source, statement->source_pos), "\n",
          "unsafe variable", (unbound.size() == 1 ? "" : "s"), " in rule '",
          head_description(*statement), "': ", absl::StrJoin(unbound, ", ")));
    }
    if (!is_subset(aggregates, bound_aggregates)) {
      return absl::InvalidArgumentError(absl::StrCat(
          format_source_line(source, statement->source_pos), "\n",
          "unsafe aggregate in rule '", head_description(*statement), "'"));
    }
  }

  // We've checked all the statements in the program for safety at this point.
  // The program can also contain a query as the final statement, but this query
  // can only contain a single classical literal. Variables only appear in
  // classical literal args, and the binding there is intentionally existential
  // (e.g., p(X, 1, a)? means "does there exist an X such that p(X, 1, a) is
  // produced?") so there are no more questions of binding/safety at this point.

  // ASP-Core-2 section 5 forbids recursive aggregates: no predicate occurring
  // un-negated inside an aggregate element may share a positive-dependency
  // strongly connected component with the head of the rule containing that
  // aggregate. E.g. 'p(X) :- dom(X), #count{Y : p(Y)} >= X.' is rejected
  // because p depends on itself through the aggregate, but a #count over a
  // recursive 'reach' predicate is fine as long as the counting rule's own
  // head isn't part of that recursion.
  const PredGraph graph = build_pred_graph(prog);
  const std::vector<int> component =
      strongly_connected_components(graph.pos_succ);
  for (const auto& edge : graph.agg_edges) {
    if (component[edge.body_id] != component[edge.head_id]) continue;
    return absl::InvalidArgumentError(absl::StrCat(
        format_source_line(source, edge.statement->source_pos), "\n",
        "recursive aggregate in rule '", head_description(*edge.statement),
        "': '", graph.preds[edge.body_id].name, "/",
        graph.preds[edge.body_id].arity,
        "' is recursive with the rule's head and cannot be used inside an "
        "aggregate"));
  }

  return absl::OkStatus();
}
