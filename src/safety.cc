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
//   Classical atoms: q(X) binds a variable standing as a whole argument, or
//                    inside a function term's arguments. A variable under
//                    arithmetic, like the X of q(X+1), stays unbound.
//   Equality: V = <expr> binds V when all variables in <expr> are already bound
//             (and vice versa when V is on the right).
//   Aggregates: an aggregate propagates binding to its output variable once
//               its elements are bound and the rule has bound whatever those
//               elements share with the rest of it. NAF aggregates never bind.

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

// Inserts into `out` the variables of `term` that a positive atom binds by
// matching against a stored value: the ones standing as a whole argument, and
// the ones inside a function term's arguments.
//
// A variable under arithmetic binds nothing. Grounding evaluates 'X + 1' and
// compares the result, it does not solve for X, so the X of 'q(X+1)' has to be
// bound somewhere else.
void collect_binding_variables(const Term& term,
                               absl::flat_hash_set<std::string_view>& out) {
  switch (term.kind) {
    case Term::VariableKind:
      out.insert(static_cast<const Variable&>(term).name);
      return;
    case Term::AtomKind: {
      const auto& atom = static_cast<const Atom&>(term);
      if (atom.args == nullptr) return;
      for (const auto& arg : *atom.args) collect_binding_variables(*arg, out);
      return;
    }
    case Term::NegatedTermKind:
    case Term::TermOperationKind:
    case Term::NumberKind:
    case Term::StringKind:
    case Term::AnonymousVariableKind:
      return;
  }
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
      }
      if (naf_lit->naf || classical_lit->args == nullptr) break;
      for (const auto& arg : *(classical_lit->args)) {
        collect_binding_variables(*arg, bound_vars);
      }
      break;
    }
  }
}

// Inserts into `out` every variable of `head`, with no filtering.
void collect_every_head_variable(const Head& head,
                                 absl::flat_hash_set<std::string_view>& out) {
  switch (head.kind) {
    case Head::DisjunctionKind:
      for (const auto& literal :
           static_cast<const Disjunction&>(head).literals) {
        collect::collect_variables(*literal, out);
      }
      return;
    case Head::ChoiceKind: {
      const auto& choice = static_cast<const Choice&>(head);
      if (choice.lb_term) collect::collect_variables(*choice.lb_term, out);
      if (choice.ub_term) collect::collect_variables(*choice.ub_term, out);
      if (choice.elements == nullptr) return;
      for (const auto& element : *(choice.elements)) {
        collect::collect_variables(*element->literal, out);
        if (element->conditions == nullptr) continue;
        for (const auto& condition : *(element->conditions)) {
          collect::collect_variables(*condition->literal, out);
        }
      }
      return;
    }
  }
}

// Inserts into `out` the global variables of `statement`: the ones standing
// anywhere other than inside an aggregate element, so an aggregate contributes
// only its bounds.
//
// The rest of the rule has to bind a global variable, while a local one is the
// element's own to bind. The X of '#count{X : q(X)}' asks nothing of the rule,
// but the X of '#count{X : q(X)} = Y, r(X)' is the same X in both places.
void collect_global_variables(const Statement& statement,
                              absl::flat_hash_set<std::string_view>& out) {
  if (statement.head) collect_every_head_variable(*statement.head, out);
  if (statement.weight) {
    const Weight& weight = *statement.weight;
    collect::collect_variables(*weight.weight, out);
    if (weight.level) collect::collect_variables(*weight.level, out);
    if (weight.terms) {
      for (const auto& term : *(weight.terms)) {
        collect::collect_variables(*term, out);
      }
    }
  }
  if (statement.body == nullptr) return;
  for (const auto& item : *(statement.body->items)) {
    switch (item->kind) {
      case BodyItem::NafLiteralKind:
        collect::collect_variables(
            *static_cast<const NafLiteral*>(item.get())->literal, out);
        break;
      case BodyItem::AggregateKind: {
        const auto& aggregate = *static_cast<const Aggregate*>(item.get());
        if (aggregate.lb_term) {
          collect::collect_variables(*aggregate.lb_term, out);
        }
        if (aggregate.ub_term) {
          collect::collect_variables(*aggregate.ub_term, out);
        }
        break;
      }
    }
  }
}

// True once every variable the aggregate's elements share with the rest of the
// rule is bound. Until then the aggregate has no value to pass on, so
// 'p(Y) :- #count{X : q(X, W)} = Y, W = Y.' leaves W and Y waiting on each
// other.
bool element_globals_are_bound(
    const Aggregate& aggregate,
    const absl::flat_hash_set<std::string_view>& global_vars,
    const absl::flat_hash_set<std::string_view>& bound_vars) {
  if (aggregate.elements == nullptr) return true;
  absl::flat_hash_set<std::string_view> element_vars;
  for (const auto& element : *(aggregate.elements)) {
    if (element->terms) {
      for (const auto& term : *(element->terms)) {
        collect::collect_variables(*term, element_vars);
      }
    }
    if (element->literals == nullptr) continue;
    for (const auto& naf_literal : *(element->literals)) {
      collect::collect_variables(*naf_literal->literal, element_vars);
    }
  }
  for (std::string_view v : element_vars) {
    if (global_vars.contains(v) && !bound_vars.contains(v)) return false;
  }
  return true;
}

// Records in `vars` the head variables the body has to bind. A disjunction's
// are all of them. A choice's are the ones in its bounds plus the ones its
// elements leave unbound. An element's conditions bind its own locals, so the X
// of '{ p(X) : d(X) }' asks nothing of the body.
void collect_head_vars(const Head& head,
                       const absl::flat_hash_set<std::string_view>& bound_vars,
                       absl::flat_hash_set<std::string_view>& vars) {
  switch (head.kind) {
    case Head::DisjunctionKind: {
      const auto& disjunction = static_cast<const Disjunction&>(head);
      for (const auto& literal : disjunction.literals) {
        collect::collect_variables(*literal, vars);
      }
      break;
    }
    case Head::ChoiceKind: {
      const auto& choice = static_cast<const Choice&>(head);
      if (choice.lb_term) collect::collect_variables(*choice.lb_term, vars);
      if (choice.ub_term) collect::collect_variables(*choice.ub_term, vars);
      if (choice.elements == nullptr) break;

      for (const auto& element : *(choice.elements)) {
        absl::flat_hash_set<std::string_view> elem_bound_vars = bound_vars;
        absl::flat_hash_set<std::string_view> elem_vars;
        std::size_t prev_num_elem_bound = 0;
        do {
          prev_num_elem_bound = elem_bound_vars.size();
          // '{ p(1) }' has no condition to bind anything.
          if (element->conditions == nullptr) break;
          for (const auto& condition : *(element->conditions)) {
            propagate_naf_literal(condition.get(), elem_bound_vars, elem_vars);
          }
        } while (prev_num_elem_bound < elem_bound_vars.size());

        collect::collect_variables(*element->literal, elem_vars);
        for (std::string_view v : elem_vars) {
          if (!elem_bound_vars.contains(v)) vars.insert(v);
        }
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
    absl::flat_hash_set<std::string_view> bound_vars, vars;
    absl::flat_hash_set<Aggregate*> bound_aggregates, aggregates;
    absl::flat_hash_set<std::string_view> global_vars;
    collect_global_variables(*statement, global_vars);

    std::size_t prev_num_bound = 0;
    do {
      prev_num_bound = bound_vars.size();
      // A fact has no body to bind anything, so only its head is left to check.
      if (statement->body == nullptr) break;
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

            // An element's terms bind nothing on their own: the tuple of
            // '#count{ Z : q(1) }' is what gets counted, not what gets matched,
            // so its conditions have to bind Z.
            if (aggregate->elements != nullptr) {
              for (const auto& agg_element : *(aggregate->elements)) {
                if (agg_element->terms == nullptr) continue;
                for (const auto& term : *(agg_element->terms)) {
                  collect::collect_variables(*term, agg_vars);
                }
              }
            }

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

            // A bound aggregate propagates to its output variable if
            // connected by equality (e.g. `#sum{...} = X` binds X), once the
            // rule has bound whatever its elements share with the rest of it.
            const bool propagates =
                bound && !aggregate->naf &&
                element_globals_are_bound(*aggregate, global_vars, bound_vars);

            if (propagates && aggregate->lb_term != nullptr &&
                aggregate->lb_term->kind == Term::VariableKind &&
                aggregate->lb_op == BinopType::kEQUAL) {
              bound_vars.insert(
                  static_cast<Variable*>(aggregate->lb_term.get())->name);
            }
            if (propagates && aggregate->ub_term != nullptr &&
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

    // A head variable is safe only if the body binds it. 'p(X) :- q(1).' never
    // says which X to derive. Grounding rejects that rule only once it reaches
    // it, which it never does when the body cannot hold.
    if (statement->head != nullptr) {
      collect_head_vars(*statement->head, bound_vars, vars);
    }

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
