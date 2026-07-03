#include "ast.h"

namespace {

Terms clone_terms(const Terms& terms) {
  if (!terms) return nullptr;
  auto out = std::make_unique<std::vector<std::unique_ptr<Term>>>();
  for (const auto& term : *terms) out->push_back(term->clone());
  return out;
}

NafLiterals clone_naf_literals(const NafLiterals& literals) {
  if (!literals) return nullptr;
  auto out = std::make_unique<std::vector<std::unique_ptr<NafLiteral>>>();
  for (const auto& literal : *literals) out->push_back(literal->clone());
  return out;
}

}  // namespace

std::unique_ptr<Term> Atom::clone() const {
  return std::make_unique<Atom>(name, clone_terms(args));
}

std::unique_ptr<Term> Number::clone() const {
  return std::make_unique<Number>(value);
}

std::unique_ptr<Term> String::clone() const {
  return std::make_unique<String>(value);
}

std::unique_ptr<Term> Variable::clone() const {
  return std::make_unique<Variable>(name);
}

std::unique_ptr<Term> AnonymousVariable::clone() const {
  return std::make_unique<AnonymousVariable>();
}

std::unique_ptr<Term> NegatedTerm::clone() const {
  return std::make_unique<NegatedTerm>(term->clone());
}

std::unique_ptr<Term> TermOperation::clone() const {
  return std::make_unique<TermOperation>(op, left->clone(), right->clone());
}

BuiltinAtom* BuiltinAtom::clone_impl() const {
  auto out = new BuiltinAtom();
  out->left = left->clone();
  out->right = right->clone();
  out->op = op;
  return out;
}

ClassicalLiteral* ClassicalLiteral::clone_impl() const {
  auto out = new ClassicalLiteral();
  out->negated = negated;
  out->id = id;
  out->args = clone_terms(args);
  return out;
}

NafLiteral* NafLiteral::clone_impl() const {
  auto out = new NafLiteral();
  out->naf = naf;
  out->literal = literal->clone();
  return out;
}

std::unique_ptr<AggregateElement> AggregateElement::clone() const {
  return std::make_unique<AggregateElement>(clone_terms(terms),
                                            clone_naf_literals(literals));
}

Aggregate* Aggregate::clone_impl() const {
  auto out = new Aggregate();
  out->naf = naf;
  out->lb_term = lb_term ? lb_term->clone() : nullptr;
  out->lb_op = lb_op;
  out->ub_term = ub_term ? ub_term->clone() : nullptr;
  out->ub_op = ub_op;
  out->function = function;
  if (elements) {
    out->elements =
        std::make_unique<std::vector<std::unique_ptr<AggregateElement>>>();
    for (const auto& element : *elements) {
      out->elements->push_back(element->clone());
    }
  }
  return out;
}
