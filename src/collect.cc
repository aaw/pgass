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
