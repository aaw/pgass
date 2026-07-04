#include "normalize.h"

#include "absl/strings/str_cat.h"
#include "collect.h"
#include "macros.h"

namespace {

// Deep-copies the items of `body` (possibly null) into a fresh item vector.
std::unique_ptr<std::vector<std::unique_ptr<BodyItem>>> clone_body_items(
    const Body* body) {
  auto items = std::make_unique<std::vector<std::unique_ptr<BodyItem>>>();
  if (body && body->items) {
    for (const auto& item : *body->items) items->push_back(item->clone());
  }
  return items;
}

// Collects, in first-occurrence order, the variables of a choice element: the
// ones in its literal followed by the ones introduced by its conditions. The
// auxiliary '_crN' atom must carry these so each ground instance of the element
// is counted (and chosen) separately.
std::vector<std::string> element_variables(const ChoiceElement& element) {
  std::vector<std::string> names;
  collect::collect_variables(*element.literal, names);
  if (element.conditions) {
    for (const auto& cond : *element.conditions) {
      collect::collect_variables(*cond->literal, names);
    }
  }
  return names;
}

// Builds an argument list of variables from `names`, or nullptr when empty so
// the auxiliary atom formats as '_crN' rather than '_crN()'.
Terms variables_as_args(const std::vector<std::string>& names) {
  if (names.empty()) return nullptr;
  auto args = std::make_unique<std::vector<std::unique_ptr<Term>>>();
  for (const auto& name : names) {
    args->push_back(std::make_unique<Variable>(name));
  }
  return args;
}

/* Transform each choice rule of the form:

   { p1: a1, a2 ; p2: b1 ; p3 } < 2 :- x, y

   into rules like:

   p1 | _cr0 :- a1, a2, x, y.
   p2 | _cr1 :- b1, x, y.
   p3 | _cr2 :- x, y.
   :- x, y, not #count{ _cr0 : p1, a1, a2 ; _cr1 : p2, b1 ; _cr2 } < 2.
*/
absl::Status normalize_choice_rules(Program& prog) {
  auto new_statements =
      std::make_unique<std::vector<std::unique_ptr<Statement>>>();
  int i = 0;
  for (auto& statement : *(prog.statements)) {
    if (statement->head == nullptr ||
        statement->head->kind != Head::ChoiceKind) {
      new_statements->push_back(std::move(statement));
      continue;
    }
    const Choice& choice = static_cast<const Choice&>(*statement->head);
    const Body* body = statement->body.get();
    const auto& elements = *choice.elements;

    // Give each element a fresh auxiliary id up front so the disjunctive rules
    // and the counting aggregate below refer to the same variables. The counter
    // is program-wide so ids don't clash across multiple choice rules.
    std::vector<std::string> ids;
    std::vector<std::vector<std::string>> vars;
    ids.reserve(elements.size());
    vars.reserve(elements.size());
    for (size_t e = 0; e < elements.size(); ++e) {
      ids.push_back(absl::StrCat("_cr", i++));
      vars.push_back(element_variables(*elements[e]));
    }

    // One disjunctive rule per element: 'pN | _crN(vars) :- conditions, body.'
    for (size_t e = 0; e < elements.size(); ++e) {
      const ChoiceElement& element = *elements[e];

      auto disjunction = std::make_unique<Disjunction>();
      disjunction->literals.push_back(element.literal->clone());
      auto aux = std::make_unique<ClassicalLiteral>();
      aux->id = ids[e];
      aux->args = variables_as_args(vars[e]);
      disjunction->literals.push_back(std::move(aux));

      auto items = clone_body_items(body);
      if (element.conditions) {
        // Conditions come first, so splice them in ahead of the body items.
        size_t n = element.conditions->size();
        for (size_t c = 0; c < n; ++c) {
          items->insert(items->begin() + c, (*element.conditions)[c]->clone());
        }
      }

      auto rule = std::make_unique<Statement>();
      rule->head = std::move(disjunction);
      if (!items->empty()) {
        rule->body = std::make_unique<Body>();
        rule->body->items = std::move(items);
      }
      new_statements->push_back(std::move(rule));
    }

    // An unbounded choice imposes no cardinality restriction: the disjunctive
    // rules above already model the free choice, so there's nothing to enforce.
    if (choice.lb_term == nullptr && choice.ub_term == nullptr) continue;

    // Integrity constraint enforcing the choice bounds via a #count aggregate:
    // ':- body, not #count{ _cr0 : p1, conditions ; ... } <bounds>.'
    auto aggregate = std::make_unique<Aggregate>();
    aggregate->naf = true;
    aggregate->function = AggregateFunctionType::kAGGREGATE_COUNT;
    aggregate->lb_term = choice.lb_term ? choice.lb_term->clone() : nullptr;
    aggregate->lb_op = choice.lb_op;
    aggregate->ub_term = choice.ub_term ? choice.ub_term->clone() : nullptr;
    aggregate->ub_op = choice.ub_op;
    aggregate->elements =
        std::make_unique<std::vector<std::unique_ptr<AggregateElement>>>();
    for (size_t e = 0; e < elements.size(); ++e) {
      const ChoiceElement& element = *elements[e];

      auto terms = std::make_unique<std::vector<std::unique_ptr<Term>>>();
      terms->push_back(
          std::make_unique<Atom>(ids[e], variables_as_args(vars[e])));

      auto literals =
          std::make_unique<std::vector<std::unique_ptr<NafLiteral>>>();
      auto naf = std::make_unique<NafLiteral>();
      naf->literal = element.literal->clone();
      literals->push_back(std::move(naf));
      if (element.conditions) {
        for (const auto& cond : *element.conditions) {
          literals->push_back(cond->clone());
        }
      }

      aggregate->elements->push_back(std::make_unique<AggregateElement>(
          std::move(terms), std::move(literals)));
    }

    auto ic_items = clone_body_items(body);
    ic_items->push_back(std::move(aggregate));
    auto ic = std::make_unique<Statement>();
    ic->body = std::make_unique<Body>();
    ic->body->items = std::move(ic_items);
    new_statements->push_back(std::move(ic));
  }
  prog.statements = std::move(new_statements);
  return absl::OkStatus();
}

}  // namespace

absl::Status normalize(Program& prog) {
  RETURN_IF_ERROR(normalize_choice_rules(prog));
  return absl::OkStatus();
}
