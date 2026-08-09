#include "collect.h"

#include <algorithm>

namespace collect {

void for_each_variable(const Term& term, const VariableVisitor& visit) {
  switch (term.kind) {
    case Term::VariableKind:
      visit(static_cast<const Variable&>(term));
      break;
    case Term::AtomKind: {
      const auto& atom = static_cast<const Atom&>(term);
      if (atom.args) {
        for (const auto& arg : *atom.args) for_each_variable(*arg, visit);
      }
      break;
    }
    case Term::NegatedTermKind:
      for_each_variable(*static_cast<const NegatedTerm&>(term).term, visit);
      break;
    case Term::TermOperationKind: {
      const auto& op = static_cast<const TermOperation&>(term);
      for_each_variable(*op.left, visit);
      for_each_variable(*op.right, visit);
      break;
    }
    case Term::IntervalKind: {
      const auto& interval = static_cast<const Interval&>(term);
      for_each_variable(*interval.lower, visit);
      for_each_variable(*interval.upper, visit);
      break;
    }
    case Term::NumberKind:
    case Term::StringKind:
    case Term::AnonymousVariableKind:
      break;
  }
}

void for_each_variable(const Literal& literal, const VariableVisitor& visit) {
  switch (literal.kind) {
    case Literal::ClassicalLiteralKind: {
      const auto& cl = static_cast<const ClassicalLiteral&>(literal);
      if (cl.args) {
        for (const auto& arg : *cl.args) for_each_variable(*arg, visit);
      }
      break;
    }
    case Literal::BuiltinAtomKind: {
      const auto& ba = static_cast<const BuiltinAtom&>(literal);
      if (ba.left) for_each_variable(*ba.left, visit);
      if (ba.right) for_each_variable(*ba.right, visit);
      break;
    }
  }
}

void for_each_variable(const Aggregate& aggregate,
                       const VariableVisitor& visit) {
  if (aggregate.lb_term) for_each_variable(*aggregate.lb_term, visit);
  if (aggregate.ub_term) for_each_variable(*aggregate.ub_term, visit);
  if (!aggregate.elements) return;
  for (const auto& element : *aggregate.elements) {
    if (element->terms) {
      for (const auto& term : *element->terms) for_each_variable(*term, visit);
    }
    if (element->literals) {
      for (const auto& naf : *element->literals) {
        if (naf->literal) for_each_variable(*naf->literal, visit);
      }
    }
  }
}

namespace {

// Appends `name` to `out` unless it is already present.
void push_unique(std::string_view name, std::vector<std::string>& out) {
  if (std::find(out.begin(), out.end(), name) == out.end()) {
    out.emplace_back(name);
  }
}

void for_each_classical_literal(Literal& literal,
                                const ClassicalLiteralVisitor& visit) {
  if (literal.kind == Literal::ClassicalLiteralKind) {
    visit(static_cast<ClassicalLiteral&>(literal));
  }
}

void for_each_classical_literal(std::vector<std::unique_ptr<NafLiteral>>& nafs,
                                const ClassicalLiteralVisitor& visit) {
  for (auto& naf : nafs) {
    if (naf->literal) for_each_classical_literal(*naf->literal, visit);
  }
}

}  // namespace

void collect_variables(const Term& term,
                       absl::flat_hash_set<std::string_view>& out) {
  for_each_variable(term, [&](const Variable& var) { out.insert(var.name); });
}

void collect_variables(const Literal& literal,
                       absl::flat_hash_set<std::string_view>& out) {
  for_each_variable(literal,
                    [&](const Variable& var) { out.insert(var.name); });
}

void collect_variables(const Term& term, std::vector<std::string>& out) {
  for_each_variable(term,
                    [&](const Variable& var) { push_unique(var.name, out); });
}

void collect_variables(const Literal& literal, std::vector<std::string>& out) {
  for_each_variable(literal,
                    [&](const Variable& var) { push_unique(var.name, out); });
}

void for_each_term(Terms& terms, const TermVisitor& visit) {
  if (terms == nullptr) return;
  for (auto& term : *terms) visit(term);
}

void for_each_subterm(std::unique_ptr<Term>& slot, const TermVisitor& visit) {
  switch (slot->kind) {
    case Term::AtomKind: {
      auto& atom = static_cast<Atom&>(*slot);
      if (atom.args != nullptr) {
        for (auto& arg : *atom.args) for_each_subterm(arg, visit);
      }
      break;
    }
    case Term::NegatedTermKind:
      for_each_subterm(static_cast<NegatedTerm&>(*slot).term, visit);
      break;
    case Term::TermOperationKind: {
      auto& operation = static_cast<TermOperation&>(*slot);
      for_each_subterm(operation.left, visit);
      for_each_subterm(operation.right, visit);
      break;
    }
    case Term::IntervalKind: {
      auto& interval = static_cast<Interval&>(*slot);
      for_each_subterm(interval.lower, visit);
      for_each_subterm(interval.upper, visit);
      break;
    }
    case Term::NumberKind:
    case Term::StringKind:
    case Term::VariableKind:
    case Term::AnonymousVariableKind:
      break;
  }
  visit(slot);
}

void for_each_term(Literal& literal, const TermVisitor& visit) {
  switch (literal.kind) {
    case Literal::ClassicalLiteralKind:
      for_each_term(static_cast<ClassicalLiteral&>(literal).args, visit);
      return;
    case Literal::BuiltinAtomKind: {
      auto& builtin = static_cast<BuiltinAtom&>(literal);
      visit(builtin.left);
      visit(builtin.right);
      return;
    }
  }
}

void for_each_term(std::vector<std::unique_ptr<NafLiteral>>& nafs,
                   const TermVisitor& visit) {
  for (auto& naf : nafs) {
    if (naf->literal) for_each_term(*naf->literal, visit);
  }
}

void for_each_term(Weight& weight, const TermVisitor& visit) {
  visit(weight.weight);
  if (weight.level) visit(weight.level);
  for_each_term(weight.terms, visit);
}

void for_each_classical_literal(Head& head,
                                const ClassicalLiteralVisitor& visit) {
  switch (head.kind) {
    case Head::DisjunctionKind: {
      auto& disjunction = static_cast<Disjunction&>(head);
      for (auto& literal : disjunction.literals) visit(*literal);
      break;
    }
    case Head::ChoiceKind: {
      auto& choice = static_cast<Choice&>(head);
      if (choice.elements) {
        for (auto& element : *choice.elements) {
          if (element->literal) visit(*element->literal);
          if (element->conditions) {
            for_each_classical_literal(*element->conditions, visit);
          }
        }
      }
      break;
    }
  }
}

void for_each_classical_literal(Body& body,
                                const ClassicalLiteralVisitor& visit) {
  if (!body.items) return;
  for (auto& item : *body.items) {
    switch (item->kind) {
      case BodyItem::NafLiteralKind: {
        auto& naf = static_cast<NafLiteral&>(*item);
        if (naf.literal) for_each_classical_literal(*naf.literal, visit);
        break;
      }
      case BodyItem::AggregateKind: {
        auto& aggregate = static_cast<Aggregate&>(*item);
        if (aggregate.elements) {
          for (auto& element : *aggregate.elements) {
            if (element->literals) {
              for_each_classical_literal(*element->literals, visit);
            }
          }
        }
        break;
      }
    }
  }
}

void for_each_classical_literal(std::vector<std::unique_ptr<NafLiteral>>& nafs,
                                bool negated_context,
                                const NegatedClassicalLiteralVisitor& visit) {
  for (auto& naf : nafs) {
    if (naf->literal && naf->literal->kind == Literal::ClassicalLiteralKind) {
      auto& cl = static_cast<ClassicalLiteral&>(*naf->literal);
      visit(cl, negated_context || naf->naf);
    }
  }
}

void for_each_classical_literal(Body& body,
                                const NegatedClassicalLiteralVisitor& visit) {
  if (!body.items) return;
  for (auto& item : *body.items) {
    switch (item->kind) {
      case BodyItem::NafLiteralKind: {
        auto& naf = static_cast<NafLiteral&>(*item);
        if (naf.literal && naf.literal->kind == Literal::ClassicalLiteralKind) {
          auto& cl = static_cast<ClassicalLiteral&>(*naf.literal);
          visit(cl, naf.naf);
        }
        break;
      }
      case BodyItem::AggregateKind: {
        auto& aggregate = static_cast<Aggregate&>(*item);
        if (aggregate.elements) {
          for (auto& element : *aggregate.elements) {
            if (element->literals) {
              for_each_classical_literal(*element->literals, aggregate.naf,
                                         visit);
            }
          }
        }
        break;
      }
    }
  }
}

}  // namespace collect
