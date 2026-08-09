#include "normalize.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/str_cat.h"
#include "collect.h"
#include "macros.h"

namespace {

// Builds an argument list of `arity` fresh variables, or nullptr when arity is
// 0 so an atom formats as 'p' rather than 'p()'. A single argument is named
// 'X'; several are numbered 'X1', 'X2', ... by position. The names must be
// distinct per position (so each position of p is tied to the same position of
// _neg_p) but shared between the two atoms in the constraint that uses them.
Terms fresh_variable_args(size_t arity) {
  if (arity == 0) return nullptr;
  auto args = std::make_unique<std::vector<std::unique_ptr<Term>>>();
  for (size_t k = 0; k < arity; ++k) {
    std::string name = arity == 1 ? "X" : absl::StrCat("X", k + 1);
    args->push_back(std::make_unique<Variable>(std::move(name)));
  }
  return args;
}

// Wraps a fresh classical atom 'id(args)' in a non-negated body item.
std::unique_ptr<BodyItem> positive_atom_item(const std::string& id,
                                             Terms args) {
  auto literal = std::make_unique<ClassicalLiteral>();
  literal->id = id;
  literal->args = std::move(args);
  auto naf = std::make_unique<NafLiteral>();
  naf->literal = std::move(literal);
  return naf;
}

// A head holding the single atom 'name(args)'. Every directive this file
// rewrites into a rule ends up with a head of that shape.
std::unique_ptr<Head> single_atom_head(std::string_view name, Terms args) {
  auto literal = std::make_unique<ClassicalLiteral>();
  literal->id = std::string(name);
  literal->args = std::move(args);
  auto disjunction = std::make_unique<Disjunction>();
  disjunction->literals.push_back(std::move(literal));
  return disjunction;
}

// The literals a rewrite adds to one condition. See for_each_scoped_term().
using Added = std::vector<std::unique_ptr<NafLiteral>>;

/* Calls `visit(slot, added)` on every term slot of a statement, where `added`
   is the condition a rewrite of that term can add literals to.

   For most terms that condition is the statement's own body. For a term inside
   an aggregate or choice element it is the element's own condition, which is
   where a variable local to the element belongs: '#count{ 1..3 }' is three
   tuples, so the variable the interval binds is the element's and not the
   rule's. Each list is spliced into place once its scope is done.
*/
using ScopedTermFn = absl::FunctionRef<void(std::unique_ptr<Term>&, Added&)>;

// Appends `added` to `condition`, which is how a rewrite's new literals reach
// the element they belong to. An element with no condition of its own gets one.
void splice(Added added, NafLiterals& condition) {
  if (added.empty()) return;
  if (condition == nullptr) {
    condition = std::make_unique<std::vector<std::unique_ptr<NafLiteral>>>();
  }
  for (auto& literal : added) condition->push_back(std::move(literal));
}

// Walks the terms of one element. Its condition is both a place terms stand
// and the scope its rewrites go to.
void visit_element(Terms& terms, NafLiterals& condition,
                   const ScopedTermFn& visit) {
  Added added;
  auto in_element = [&](std::unique_ptr<Term>& slot) { visit(slot, added); };
  collect::for_each_term(terms, in_element);
  if (condition != nullptr) collect::for_each_term(*condition, in_element);
  splice(std::move(added), condition);
}

void visit_head(Head& head, Added& added, const ScopedTermFn& visit) {
  auto in_rule = [&](std::unique_ptr<Term>& slot) { visit(slot, added); };
  switch (head.kind) {
    case Head::DisjunctionKind:
      for (auto& literal : static_cast<Disjunction&>(head).literals) {
        collect::for_each_term(literal->args, in_rule);
      }
      return;
    case Head::ChoiceKind: {
      auto& choice = static_cast<Choice&>(head);
      if (choice.lb_term) in_rule(choice.lb_term);
      if (choice.ub_term) in_rule(choice.ub_term);
      if (choice.elements == nullptr) return;
      for (auto& element : *choice.elements) {
        visit_element(element->literal->args, element->conditions, visit);
      }
      return;
    }
  }
}

void visit_aggregate(Aggregate& aggregate, Added& added,
                     const ScopedTermFn& visit) {
  if (aggregate.lb_term) visit(aggregate.lb_term, added);
  if (aggregate.ub_term) visit(aggregate.ub_term, added);
  if (aggregate.elements == nullptr) return;
  for (auto& element : *aggregate.elements) {
    visit_element(element->terms, element->literals, visit);
  }
}

void for_each_scoped_term(Statement& statement, const ScopedTermFn& visit) {
  Added added;
  auto in_rule = [&](std::unique_ptr<Term>& slot) { visit(slot, added); };
  if (statement.head) visit_head(*statement.head, added, visit);
  if (statement.weight) collect::for_each_term(*statement.weight, in_rule);
  if (statement.show && statement.show->term) in_rule(statement.show->term);
  if (statement.body) {
    for (auto& item : *statement.body->items) {
      if (item->kind == BodyItem::AggregateKind) {
        visit_aggregate(static_cast<Aggregate&>(*item), added, visit);
        continue;
      }
      collect::for_each_term(*static_cast<NafLiteral&>(*item).literal, in_rule);
    }
  }

  if (added.empty()) return;
  if (statement.body == nullptr) {
    statement.body = std::make_unique<Body>();
    statement.body->items =
        std::make_unique<std::vector<std::unique_ptr<BodyItem>>>();
  }
  for (auto& literal : added) {
    statement.body->items->push_back(std::move(literal));
  }
}

/* Rewrite each '#minimize' or '#maximize' statement into the weak constraints
   it stands for:

     #minimize{ w1@l1, t1 : b1 ; w2@l2, t2 : b2 }.

   becomes

     :~ b1. [w1@l1, t1]
     :~ b2. [w2@l2, t2]

   A '#maximize' is a '#minimize' over negated weights, so its elements get a
   '-' in front of each weight and are otherwise the same.
*/
void rewrite_minimize_statements(Program& prog) {
  auto new_statements =
      std::make_unique<std::vector<std::unique_ptr<Statement>>>();
  for (auto& statement : *prog.statements) {
    if (!statement->minimize) {
      new_statements->push_back(std::move(statement));
      continue;
    }

    Minimize& minimize = *statement->minimize;
    for (auto& element : minimize.elements) {
      auto rule = std::make_unique<Statement>();
      rule->weight = std::move(element->weight);
      if (minimize.maximize) {
        rule->weight->weight =
            std::make_unique<NegatedTerm>(std::move(rule->weight->weight));
      }
      // A weak constraint always has a body, so an element with no condition,
      // e.g. the '1@0' of '#minimize{ 1@0 }', gets an empty one.
      rule->body = std::make_unique<Body>();
      rule->body->items =
          std::make_unique<std::vector<std::unique_ptr<BodyItem>>>();
      if (element->condition != nullptr) {
        for (auto& literal : *element->condition) {
          rule->body->items->push_back(std::move(literal));
        }
      }
      rule->source_pos = statement->source_pos;
      new_statements->push_back(std::move(rule));
    }
  }
  prog.statements = std::move(new_statements);
}

// What a '#const' names, keyed by the name. The value is fully resolved: a
// constant defined as another constant holds what that one came to.
using Constants = absl::flat_hash_map<std::string, std::unique_ptr<Term>>;

// The name a term is, when it is one a '#const' gave a meaning to. Only a name
// standing on its own is a constant. The p of 'p(a)' and the a of 'a(1)' are
// predicates, and neither reaches here as a term.
const std::string* constant_name(const std::unique_ptr<Term>& term,
                                 const Constants& constants) {
  if (term->kind != Term::AtomKind) return nullptr;
  const Atom& atom = static_cast<const Atom&>(*term);
  if (atom.args != nullptr) return nullptr;
  auto it = constants.find(atom.name);
  return it == constants.end() ? nullptr : &it->first;
}

// Replaces each constant name in `slot` with what '#const' said it stands for.
// The values in `constants` are already resolved, so nothing put in needs a
// second pass.
void substitute_constants(std::unique_ptr<Term>& slot,
                          const Constants& constants) {
  collect::for_each_subterm(slot, [&](std::unique_ptr<Term>& term) {
    const std::string* name = constant_name(term, constants);
    if (name != nullptr) term = constants.at(*name)->clone();
  });
}

/* Take each '#const' statement out of the program, putting what it names in
   place of the name everywhere the program uses it.

   A directive holds for the whole program, not just for what follows it, so
   '#const n = 3.' at the end of a file reaches the 'p(n)' at the top. One
   constant may be defined as another, '#const a = b.' with '#const b = 3.',
   which is why each definition is resolved before any of them is substituted.
*/
absl::Status resolve_constants(Program& prog) {
  Constants constants;
  // Kept in source order, so that two definitions of one name are reported at
  // the second one and a cycle is reported the same way every run.
  std::vector<std::string> names;
  for (const auto& statement : *prog.statements) {
    if (!statement->constant) continue;
    const Constant& constant = *statement->constant;
    if (constants.contains(constant.name)) {
      return absl::InvalidArgumentError(
          absl::StrCat("'#const ", constant.name,
                       "' is defined more than once, so it names two terms"));
    }
    constants.emplace(constant.name, constant.value->clone());
    names.push_back(constant.name);
  }

  // Resolve each definition against the others, depth first: what a value
  // names is resolved before the value is substituted, so a value only ever
  // takes in terms that are settled. A name reached twice on one path defines
  // itself, e.g. '#const a = f(a).', which no amount of substituting settles.
  absl::flat_hash_set<std::string> on_path;
  auto resolve = [&](auto& self, const std::string& name) -> absl::Status {
    if (!on_path.insert(name).second) {
      return absl::InvalidArgumentError(absl::StrCat(
          "'#const ", name, "' is defined in terms of itself"));
    }
    std::vector<const std::string*> uses;
    collect::for_each_subterm(constants.at(name),
                              [&](std::unique_ptr<Term>& term) {
                                const std::string* use =
                                    constant_name(term, constants);
                                if (use != nullptr) uses.push_back(use);
                              });
    for (const std::string* use : uses) RETURN_IF_ERROR(self(self, *use));
    substitute_constants(constants.at(name), constants);
    on_path.erase(name);
    return absl::OkStatus();
  };
  for (const std::string& name : names) RETURN_IF_ERROR(resolve(resolve, name));

  auto new_statements =
      std::make_unique<std::vector<std::unique_ptr<Statement>>>();
  for (auto& statement : *prog.statements) {
    if (statement->constant) continue;
    for_each_scoped_term(*statement,
                         [&](std::unique_ptr<Term>& slot, Added&) {
                           substitute_constants(slot, constants);
                         });
    new_statements->push_back(std::move(statement));
  }
  prog.statements = std::move(new_statements);
  if (prog.query && prog.query->lit->args) {
    for (auto& arg : *prog.query->lit->args) {
      substitute_constants(arg, constants);
    }
  }
  return absl::OkStatus();
}

/* Take each '#show' statement out of the program, in one of two ways.

   A signature, '#show p/2.' or '#show.', says which predicates an answer set
   prints, so it goes into prog.show_filter and grounding reads it there.

   A term, '#show t : body.', becomes '_show(t) :- body.'. From here on it is
   an ordinary rule, so the predicate graph, grounding and emission need to
   know nothing about '#show'. name_outputs() reads the '_show' atoms back and
   prints each one's argument.
*/
void rewrite_show_statements(Program& prog) {
  auto new_statements =
      std::make_unique<std::vector<std::unique_ptr<Statement>>>();
  for (auto& statement : *prog.statements) {
    if (!statement->show) {
      new_statements->push_back(std::move(statement));
      continue;
    }

    Show& show = *statement->show;
    if (show.term == nullptr) {
      if (!prog.show_filter.has_value()) prog.show_filter.emplace();
      if (show.signature) prog.show_filter->push_back(*show.signature);
      continue;
    }

    auto args = std::make_unique<std::vector<std::unique_ptr<Term>>>();
    args->push_back(std::move(show.term));

    auto rule = std::make_unique<Statement>();
    rule->head = single_atom_head(kShowPredicate, std::move(args));
    rule->body = std::move(statement->body);
    rule->source_pos = statement->source_pos;
    new_statements->push_back(std::move(rule));
  }
  prog.statements = std::move(new_statements);
}

/* Rewrite each weak constraint

     :~ body. [w@l, t1, ..., tn]

   into a rule defining '_viol', a single global violation-accumulator
   predicate shared by every weak constraint in the program:

     _viol(l, w, t1, ..., tn) :- body.

   A missing level defaults to 0. '_viol' is reserved: later pipeline stages
   (grounding/solving) are expected to treat every true _viol(L, W, ...) atom
   as one unit of cost W at priority level L, and compute each level's total
   as the sum of W over its true _viol atoms. Ordinary set semantics does the
   rest for free: two groundings that agree on (l, w, t1, ..., tn) produce the
   same ground atom, so they count as a single violation rather than two, as
   required by weak-constraint semantics. This pass does not itself compute
   per-level directives; it only emits the atoms a later stage will read.
*/
absl::Status rewrite_weak_constraints(Program& prog) {
  auto new_statements =
      std::make_unique<std::vector<std::unique_ptr<Statement>>>();
  for (auto& statement : *prog.statements) {
    if (!statement->weight) {
      new_statements->push_back(std::move(statement));
      continue;
    }

    auto args = std::make_unique<std::vector<std::unique_ptr<Term>>>();
    args->push_back(statement->weight->level ? statement->weight->level->clone()
                                             : std::make_unique<Number>(0));
    args->push_back(statement->weight->weight->clone());
    if (statement->weight->terms) {
      for (const auto& term : *statement->weight->terms) {
        args->push_back(term->clone());
      }
    }

    auto rule = std::make_unique<Statement>();
    rule->head = single_atom_head(kViolationPredicate, std::move(args));
    rule->body = std::move(statement->body);
    new_statements->push_back(std::move(rule));
  }
  prog.statements = std::move(new_statements);
  return absl::OkStatus();
}

/* Replaces every interval in `slot` with a variable of its own, adding to
   `added` the comparison that gives the variable its values:

     p(1..3, X)   becomes   p(_R0, X)   plus   _R0 = 1..3

   The bounds are lifted first, so an interval built out of intervals, the
   '(1..2)..3' that '1..2..3' parses as, comes out as '_R1 = _R0..3' with
   '_R0 = 1..2' beside it. The outer interval runs from each value the inner
   one takes.
*/
void lift_intervals(std::unique_ptr<Term>& slot, Added& added, size_t& next) {
  collect::for_each_subterm(slot, [&](std::unique_ptr<Term>& term) {
    if (term->kind != Term::IntervalKind) return;
    std::string name = absl::StrCat(kIntervalVariablePrefix, next++);
    auto comparison = std::make_unique<BuiltinAtom>();
    comparison->op = BinopType::kEQUAL;
    comparison->left = std::make_unique<Variable>(name);
    comparison->right = std::move(term);
    auto naf = std::make_unique<NafLiteral>();
    naf->literal = std::move(comparison);
    added.push_back(std::move(naf));

    term = std::make_unique<Variable>(name);
  });
}

/* Lift every interval in the program out to a comparison of its own, leaving
   an interval in one place only: the right-hand side of '_R = lo..hi'.

   An interval in an aggregate or choice element joins that element's own
   condition, so the variable it binds is local to the element and the element
   is counted once per value: '#count{ 1..3 }' is three tuples. Every other
   interval joins the statement's body.
*/
absl::Status lift_intervals(Program& prog) {
  size_t next = 0;
  for (auto& statement : *prog.statements) {
    for_each_scoped_term(*statement,
                         [&](std::unique_ptr<Term>& slot, Added& added) {
                           lift_intervals(slot, added, next);
                         });
  }

  // A query has no body to hold the comparison, and asking whether p holds for
  // some argument in a range is a question about several queries anyway. What
  // the lift leaves behind does not matter: the program is rejected.
  if (prog.query && prog.query->lit->args) {
    Added added;
    for (auto& arg : *prog.query->lit->args) {
      lift_intervals(arg, added, next);
    }
    if (!added.empty()) {
      return absl::InvalidArgumentError(
          "an interval cannot appear in a query: a query asks about one atom");
    }
  }
  return absl::OkStatus();
}

/* Eliminate classical (strong) negation by renaming each classically negated
   literal '-p(args)' to a fresh positive predicate '_neg_p(args)', then, for
   every predicate p/n that occurred negated, appending an integrity constraint
   forbidding p and its negation from both holding on the same argument tuple:

     :- p(X1, ..., Xn), _neg_p(X1, ..., Xn).
*/
absl::Status remove_classical_negation(Program& prog) {
  // Predicate name / arity pairs seen classically negated. Ordered so the
  // appended constraints are emitted deterministically.
  absl::btree_set<std::pair<std::string, size_t>> negated;
  auto rewrite = [&](ClassicalLiteral& literal) {
    if (!literal.negated) return;
    size_t arity = literal.args ? literal.args->size() : 0;
    negated.emplace(literal.id, arity);
    literal.id = absl::StrCat(kClassicalNegationPrefix, literal.id);
    literal.negated = false;
  };

  for (auto& statement : *prog.statements) {
    if (statement->head) {
      collect::for_each_classical_literal(*statement->head, rewrite);
    }
    if (statement->body) {
      collect::for_each_classical_literal(*statement->body, rewrite);
    }
  }
  if (prog.query && prog.query->lit) rewrite(*prog.query->lit);

  for (const auto& [name, arity] : negated) {
    auto items = std::make_unique<std::vector<std::unique_ptr<BodyItem>>>();
    items->push_back(positive_atom_item(name, fresh_variable_args(arity)));
    items->push_back(
        positive_atom_item(absl::StrCat(kClassicalNegationPrefix, name),
                           fresh_variable_args(arity)));
    auto constraint = std::make_unique<Statement>();
    constraint->body = std::make_unique<Body>();
    constraint->body->items = std::move(items);
    prog.statements->push_back(std::move(constraint));
  }
  return absl::OkStatus();
}

// Deep-copies the items of `body` (possibly null) into a fresh item vector.
std::unique_ptr<std::vector<std::unique_ptr<BodyItem>>> clone_body_items(
    const Body* body) {
  auto items = std::make_unique<std::vector<std::unique_ptr<BodyItem>>>();
  if (body && body->items) {
    for (const auto& item : *body->items) items->push_back(item->clone());
  }
  return items;
}

/* The auxiliary atom that says `atom` was passed over: '_ch_p(1)' for 'p(1)'.
   Naming it after the atom rather than after the choice element it came from
   makes the same atom offered by two elements one atom to choose and one tuple
   to count, so '{ a : q ; a : r } <= 1' counts a once.

   Grounding hides '_' predicates, and the lexer rejects a leading '_' in an
   identifier, so no user predicate collides with one.
*/
std::unique_ptr<ClassicalLiteral> choice_aux_atom(
    const ClassicalLiteral& atom) {
  auto aux = atom.clone();
  aux->id = absl::StrCat("_ch_", atom.id);
  return aux;
}

// The same auxiliary as a term, which is the shape the counting aggregate
// collects it in.
std::unique_ptr<Term> choice_aux_term(const ClassicalLiteral& atom) {
  auto aux = choice_aux_atom(atom);
  return std::make_unique<Atom>(aux->id, std::move(aux->args));
}

/* Transform each choice rule of the form:

   { p1: a1, a2 ; p2: b1 ; p3 } < 2 :- x, y

   into rules like:

   p1 | _ch_p1 :- a1, a2, x, y.
   p2 | _ch_p2 :- b1, x, y.
   p3 | _ch_p3 :- x, y.
   :- x, y, not #count{ _ch_p1 : p1, a1, a2 ; _ch_p2 : p2, b1 ; _ch_p3 } < 2.

   A tuple in that count names the atom chosen, so a bound counts atoms however
   many elements happen to offer the same one.
*/
absl::Status normalize_choice_rules(Program& prog) {
  auto new_statements =
      std::make_unique<std::vector<std::unique_ptr<Statement>>>();
  for (auto& statement : *(prog.statements)) {
    if (statement->head == nullptr ||
        statement->head->kind != Head::ChoiceKind) {
      new_statements->push_back(std::move(statement));
      continue;
    }
    const Choice& choice = static_cast<const Choice&>(*statement->head);
    const Body* body = statement->body.get();
    const auto& elements = *choice.elements;

    // One disjunctive rule per element: 'pN | _ch_pN :- conditions, body.'
    for (size_t e = 0; e < elements.size(); ++e) {
      const ChoiceElement& element = *elements[e];

      auto disjunction = std::make_unique<Disjunction>();
      disjunction->literals.push_back(element.literal->clone());
      disjunction->literals.push_back(choice_aux_atom(*element.literal));

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
    // ':- body, not #count{ _ch_p1 : p1, conditions ; ... } <bounds>.'
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
      terms->push_back(choice_aux_term(*element.literal));

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
  // '#minimize' first, so that everything below sees the weak constraints it
  // stands for rather than a statement shape of its own. '#const' next, since
  // a constant can name the term any of the rewrites below work on.
  rewrite_minimize_statements(prog);
  RETURN_IF_ERROR(resolve_constants(prog));
  // '#show' goes first so that a '-p' in a shown term's condition is left for
  // remove_classical_negation below, along with every other rule body.
  rewrite_show_statements(prog);
  // Intervals come out before the rewrites below build rules out of the terms
  // holding them, so that each one lands in the condition it belongs to while
  // the elements it was written in are still there to say which that is.
  RETURN_IF_ERROR(lift_intervals(prog));
  RETURN_IF_ERROR(rewrite_weak_constraints(prog));
  RETURN_IF_ERROR(remove_classical_negation(prog));
  RETURN_IF_ERROR(normalize_choice_rules(prog));
  return absl::OkStatus();
}
