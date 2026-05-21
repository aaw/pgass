#include "safety.h"

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"

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


// Collects all variable names appearing anywhere in `term` into `vars`.
void collect_vars_in_term(const std::unique_ptr<Term>& term,
                          absl::flat_hash_set<std::string_view>& vars) {
  switch (term->kind) {
    case Term::PredicateKind: {
      auto* predicate = static_cast<Predicate*>(term.get());
      for (const auto& pterm : *(predicate->args)) {
        collect_vars_in_term(pterm, vars);
      }
      break;
    }
    case Term::NumberKind:
      break;
    case Term::StringKind:
      break;
    case Term::VariableKind: {
      auto* variable = static_cast<Variable*>(term.get());
      vars.insert(variable->name);
      break;
    }
    case Term::AnonymousVariableKind:
      break;
    case Term::NegatedTermKind: {
      auto* negated_term = static_cast<NegatedTerm*>(term.get());
      collect_vars_in_term(negated_term->term, vars);
      break;
    }
    case Term::TermOperationKind: {
      auto* term_op = static_cast<TermOperation*>(term.get());
      collect_vars_in_term(term_op->left, vars);
      collect_vars_in_term(term_op->right, vars);
      break;
    }
  }
}

// Updates `bound_vars` and `vars` for a builtin atom (e.g. `X = Y + 1`).
// Binds the lone variable on either side of an equality if the other side is
// fully bound. Always records all variables in the atom in `vars`.
void propagate_atom_bindings(const BuiltinAtom* atom,
                             absl::flat_hash_set<std::string_view>& bound_vars,
                             absl::flat_hash_set<std::string_view>& vars) {
  absl::flat_hash_set<std::string_view> lhs_vars, rhs_vars;
  collect_vars_in_term(atom->left, lhs_vars);
  collect_vars_in_term(atom->right, rhs_vars);
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
  for (std::string_view v : lhs_vars) { vars.insert(v); }
  for (std::string_view v : rhs_vars) { vars.insert(v); }
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
      for (const auto& term : *(classical_lit->args)) {
        collect_vars_in_term(term, cl_vars);
      }
      for (const std::string_view var : cl_vars) {
        vars.insert(var);
        if (!naf_lit->naf) bound_vars.insert(var);
      }
      break;
    }
  }
}

}  // namespace

bool verify_safe(const Program& prog) {
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
              collect_vars_in_term(aggregate->lb_term, vars);
            }
            if (aggregate->ub_term != nullptr) {
              collect_vars_in_term(aggregate->ub_term, vars);
            }

            // Outer bound variables are in scope inside the aggregate body.
            absl::flat_hash_set<std::string_view> agg_bound_vars = bound_vars;
            absl::flat_hash_set<std::string_view> agg_vars;
            std::size_t prev_num_agg_bound = 0;
            do {
              prev_num_agg_bound = agg_bound_vars.size();
              for (const auto& agg_element : *(aggregate->elements)) {
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

    if (!is_subset(vars, bound_vars)) return false;
    if (!is_subset(aggregates, bound_aggregates)) return false;
  }

  // We've checked all the statements in the program for safety at this point.
  // The program can also contain a query as the final statement, but this query
  // can only contain a single classical literal. Variables only appear in
  // classical literal args, and the binding there is intentionally existential
  // (e.g., p(X, 1, a)? means "does there exist an X such that p(X, 1, a) is
  // produced?") so there are no more questions of binding/safety at this point.

  return true;
}
