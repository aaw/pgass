#include "ground.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "graph.h"
#include "macros.h"

namespace {

// A ground value: a number, a constant like abc, or a quoted string like
// "abc". Values are atomic (function terms are not supported yet). The kind
// order matches the ASP-Core-2 spec, which orders integers < symbolic
// constants < string constants < functional terms.
struct Value {
  enum Kind { kNumber, kConstant, kString };

  Kind kind;
  uint64_t number = 0;  // set only when kind == kNumber
  std::string text;     // the constant's name or the string's contents

  static Value make_number(uint64_t n) { return {kNumber, n, ""}; }
  static Value make_constant(std::string name) {
    return {kConstant, 0, std::move(name)};
  }
  static Value make_string(std::string contents) {
    return {kString, 0, std::move(contents)};
  }

  // The value as it prints in an answer set: 42, abc, or "abc".
  std::string printed() const {
    if (kind == kNumber) return absl::StrCat(number);
    if (kind == kString) return absl::StrCat("\"", text, "\"");
    return text;
  }

  bool operator==(const Value&) const = default;

  template <typename H>
  friend H AbslHashValue(H h, const Value& v) {
    return H::combine(std::move(h), v.kind, v.number, v.text);
  }
};

// Orders ground values per the ASP-Core-2 spec (see the comment on Value
// above): numbers numerically, constants and strings lexicographically.
int compare_values(const Value& a, const Value& b) {
  if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
  if (a.kind == Value::kNumber) {
    return a.number < b.number ? -1 : a.number > b.number ? 1 : 0;
  }
  return a.text < b.text ? -1 : a.text > b.text ? 1 : 0;
}

// The argument values of one ground atom, e.g. {1, abc} for p(1, abc).
using Tuple = std::vector<Value>;
// Variable name -> value, for one candidate rule instance, e.g. {X: 1, Y:
// abc} while matching the body of "p(X, Y) :- q(X), r(Y)." against q(1) and
// r(abc).
using Binding = absl::flat_hash_map<std::string, Value>;

// One ground atom: its argument tuple plus the ASPIF atom number assigned to
// it, e.g. {1, abc} and 7 for p(1, abc) numbered 7.
struct GroundAtom {
  Tuple args;
  aspif::Atom id;
};

// One predicate's ground atoms found so far, e.g. the two GroundAtoms for
// edge(a, b) and edge(b, c) once both have been derived for edge/2.
struct PredData {
  std::vector<GroundAtom> atoms;             // in first-derived order
  absl::flat_hash_map<Tuple, size_t> index;  // args -> position in `atoms`

  const GroundAtom* find(const Tuple& args) const {
    auto it = index.find(args);
    return it == index.end() ? nullptr : &atoms[it->second];
  }
};

// Every ground atom derived so far, grouped by predicate. This is the
// grounder's working state: derive_atoms() fills it by running the rules to
// a fixpoint, and emit_rules() looks atoms up in it to build the ground
// rules.
struct Store {
  absl::flat_hash_map<PredKey, PredData> preds;
  std::vector<PredKey> order;  // predicates in first-seen order

  const PredData* find(const PredKey& key) const {
    auto it = preds.find(key);
    return it == preds.end() ? nullptr : &it->second;
  }

  // Adds the tuple if it has not been seen before, giving it the next ASPIF
  // atom number from `aspif_prog`. Returns true if the tuple was new.
  bool insert(const PredKey& key, Tuple tuple, aspif::Program& aspif_prog) {
    auto [pit, new_pred] = preds.try_emplace(key);
    if (new_pred) order.push_back(key);
    PredData& data = pit->second;
    bool is_new = data.index.try_emplace(tuple, data.atoms.size()).second;
    if (!is_new) return false;
    data.atoms.push_back(GroundAtom{std::move(tuple), aspif_prog.new_atom()});
    return true;
  }
};

// Evaluates a term to its ground value, e.g. 'X' evaluates to 1 under the
// binding {X: 1}. Every variable must already be bound.
absl::StatusOr<Value> eval_term(const Term& term, const Binding& binding) {
  switch (term.kind) {
    case Term::NumberKind:
      return Value::make_number(static_cast<const Number&>(term).value);
    case Term::StringKind:
      return Value::make_string(static_cast<const String&>(term).value);
    case Term::AtomKind: {
      const Atom& atom = static_cast<const Atom&>(term);
      if (atom.args != nullptr) {
        return absl::UnimplementedError(absl::StrCat(
            "function terms are not supported yet: '", atom.name, "(...)'"));
      }
      return Value::make_constant(atom.name);
    }
    case Term::VariableKind: {
      const std::string& name = static_cast<const Variable&>(term).name;
      auto it = binding.find(name);
      if (it == binding.end()) {
        return absl::UnimplementedError(absl::StrCat(
            "variable '", name,
            "' is not bound by a positive body literal (binding through "
            "assignment is not supported yet)"));
      }
      return it->second;
    }
    case Term::AnonymousVariableKind:
      return absl::InvalidArgumentError(
          "'_' cannot appear in a position that must be ground");
    default:  // NegatedTermKind, TermOperationKind
      return absl::UnimplementedError("arithmetic is not supported yet");
  }
}

// Evaluates each argument of a ground instance of `literal` under `binding`,
// e.g. 'p(X, 2)' evaluates to {1, 2} under the binding {X: 1}.
absl::StatusOr<Tuple> eval_args(const ClassicalLiteral& literal,
                                const Binding& binding) {
  Tuple tuple;
  if (literal.args == nullptr) return tuple;
  tuple.reserve(literal.args->size());
  for (const auto& arg : *literal.args) {
    ASSIGN_OR_RETURN(Value value, eval_term(*arg, binding));
    tuple.push_back(std::move(value));
  }
  return tuple;
}

// Tries to match `literal`'s arguments against a stored tuple. On a match,
// returns a copy of `binding` extended with any variables the match binds;
// on a mismatch, returns nullopt. `tuple` has one value per argument: the
// store groups atoms by name and arity, so every tuple stored under the
// literal's predicate has the literal's arity.
absl::StatusOr<std::optional<Binding>> match_args(
    const ClassicalLiteral& literal, const Tuple& tuple,
    const Binding& binding) {
  Binding extended = binding;
  size_t n = literal.args ? literal.args->size() : 0;
  for (size_t k = 0; k < n; ++k) {
    const Term& arg = *(*literal.args)[k];
    if (arg.kind == Term::VariableKind) {
      const std::string& name = static_cast<const Variable&>(arg).name;
      auto [it, inserted] = extended.try_emplace(name, tuple[k]);
      if (!inserted && it->second != tuple[k]) return std::nullopt;
    } else if (arg.kind == Term::AnonymousVariableKind) {
      continue;
    } else {
      ASSIGN_OR_RETURN(Value value, eval_term(arg, extended));
      if (value != tuple[k]) return std::nullopt;
    }
  }
  return extended;
}

// One rule body, split into the kinds of items grounding treats differently.
struct BodyParts {
  // Positive classical literals: matched against the store to bind
  // variables, e.g. 'edge(X, Y)' in "reachable(X, Y) :- edge(X, Y)."
  std::vector<const ClassicalLiteral*> positive;
  // Comparisons like 'X < 2' (possibly under 'not'): decided once bound.
  std::vector<const NafLiteral*> comparisons;
  // 'not p(...)' literals: kept in the emitted ground rule.
  std::vector<const ClassicalLiteral*> negative;
  // Aggregates, e.g. '#count{ X : p(X) } >= 2' in "q :- #count{ X : p(X) }
  // >= 2.". An aggregate's own 'not' lives on the Aggregate node itself, not
  // on a wrapping NafLiteral, so it isn't split into positive/negative here.
  std::vector<const Aggregate*> aggregates;
};

// Sorts one naf_literal into the positive/comparisons/negative bucket of
// `parts` it belongs in. Shared by rule bodies and aggregate element
// conditions, which are both flat lists of naf_literals.
void split_naf_literal(const NafLiteral& naf, BodyParts& parts) {
  if (naf.literal->kind == Literal::BuiltinAtomKind) {
    parts.comparisons.push_back(&naf);
  } else if (naf.naf) {
    parts.negative.push_back(
        static_cast<const ClassicalLiteral*>(naf.literal.get()));
  } else {
    parts.positive.push_back(
        static_cast<const ClassicalLiteral*>(naf.literal.get()));
  }
}

BodyParts split_naf_literals(const NafLiterals& literals) {
  BodyParts parts;
  if (literals != nullptr) {
    for (const auto& item : *literals) split_naf_literal(*item, parts);
  }
  return parts;
}

BodyParts split_body(const Body* body) {
  BodyParts parts;
  if (body == nullptr || body->items == nullptr) return parts;
  for (const auto& item : *body->items) {
    if (item->kind == BodyItem::AggregateKind) {
      parts.aggregates.push_back(static_cast<const Aggregate*>(item.get()));
      continue;
    }
    split_naf_literal(static_cast<const NafLiteral&>(*item), parts);
  }
  return parts;
}

// A normalized rule as the grounder works with it: the single head literal
// (null for a constraint) and the body split into its parts, e.g. head =
// 'reachable(X, Z)' and parts.positive = {'reachable(X, Y)', 'edge(Y, Z)'}
// for "reachable(X, Z) :- reachable(X, Y), edge(Y, Z)."
struct RuleView {
  const ClassicalLiteral* head = nullptr;
  BodyParts parts;
};

// Checks that the program is a normalized program the grounder can handle
// and splits each statement into a RuleView.
absl::StatusOr<std::vector<RuleView>> make_rule_views(const Program& prog) {
  if (prog.query != nullptr) {
    return absl::UnimplementedError("queries are not supported yet");
  }
  std::vector<RuleView> rules;
  rules.reserve(prog.statements->size());
  for (const auto& statement : *prog.statements) {
    if (statement->weight != nullptr) {
      return absl::InvalidArgumentError(
          "ground() expects a normalized program, but found a weak "
          "constraint");
    }
    RuleView rule;
    if (statement->head != nullptr) {
      absl::Status not_normalized = absl::InvalidArgumentError(
          "ground() expects a normalized program, but found a choice or "
          "disjunctive head");
      if (statement->head->kind != Head::DisjunctionKind) {
        return not_normalized;
      }
      const auto& head = static_cast<const Disjunction&>(*statement->head);
      if (head.literals.size() != 1) return not_normalized;
      rule.head = head.literals[0].get();
      if (rule.head->id == "_viol") {
        return absl::UnimplementedError(
            "weak constraints are not supported yet");
      }
    }
    rule.parts = split_body(statement->body.get());
    rules.push_back(std::move(rule));
  }
  return rules;
}

// Buckets rules by component[id_of[head predicate]]. Constraints (no head)
// are skipped; they stay only in the flat `rules` list for emit_rules.
std::vector<std::vector<RuleView>> bucket_rule_views(
    const PredGraph& graph, const std::vector<int>& component,
    const std::vector<RuleView>& rules) {
  // `component` is empty when the program mentions no predicate at all, e.g.
  // ":- 1 < 2."
  int num_components =
      component.empty()
          ? 0
          : *std::max_element(component.begin(), component.end()) + 1;
  std::vector<std::vector<RuleView>> bucket(num_components);
  for (const RuleView& rv : rules) {
    if (rv.head == nullptr) continue;
    bucket[component[graph.id_of.at(pred_key(*rv.head))]].push_back(rv);
  }
  return bucket;
}

// One way to satisfy a rule body with the atoms in the store: a value for
// each of the body's variables, plus the ASPIF atom each positive literal
// matched, e.g. binding = {X: a, Y: b} and matched = {3} for 'edge(X, Y)'
// matching a stored edge(a, b) numbered atom 3.
struct Instance {
  Binding binding;
  std::vector<aspif::Lit> matched;
};

bool builtin_holds(BinopType op, const Value& left, const Value& right) {
  int c = compare_values(left, right);
  switch (op) {
    case BinopType::kEQUAL:
      return c == 0;
    case BinopType::kUNEQUAL:
      return c != 0;
    case BinopType::kLESS:
      return c < 0;
    case BinopType::kGREATER:
      return c > 0;
    case BinopType::kLESS_OR_EQ:
      return c <= 0;
    case BinopType::kGREATER_OR_EQ:
      return c >= 0;
  }
  return false;  // unreachable
}

// Returns whether every comparison in the body holds under `binding`, e.g.
// whether 'X < 2' holds given the binding {X: 1}.
absl::StatusOr<bool> comparisons_hold(const BodyParts& parts,
                                      const Binding& binding) {
  for (const NafLiteral* item : parts.comparisons) {
    const auto& builtin = static_cast<const BuiltinAtom&>(*item->literal);
    ASSIGN_OR_RETURN(Value left, eval_term(*builtin.left, binding));
    ASSIGN_OR_RETURN(Value right, eval_term(*builtin.right, binding));
    bool holds = builtin_holds(builtin.op, left, right);
    if (item->naf) holds = !holds;
    if (!holds) return false;
  }
  return true;
}

// Finds every way to satisfy the body with the atoms currently in the store.
// Works through the positive literals left to right, extending each partial
// instance with every stored tuple that matches, then keeps the instances
// whose comparisons all hold. 'not' literals never filter here; the caller
// decides what to do with them.
//
// `seed_binding` lets a caller start from variables already bound by an
// enclosing scope, e.g. grounding an aggregate element's condition under the
// enclosing rule instance's binding.
absl::StatusOr<std::vector<Instance>> find_instances(
    const BodyParts& parts, const Store& store, Binding seed_binding = {}) {
  std::vector<Instance> instances;
  instances.push_back(Instance{std::move(seed_binding), {}});
  for (const ClassicalLiteral* literal : parts.positive) {
    const PredData* data = store.find(pred_key(*literal));
    // A predicate with no atoms at all means the body cannot be satisfied.
    if (data == nullptr) return std::vector<Instance>();
    std::vector<Instance> extended;
    for (const Instance& partial : instances) {
      for (const GroundAtom& atom : data->atoms) {
        ASSIGN_OR_RETURN(std::optional<Binding> bound,
                         match_args(*literal, atom.args, partial.binding));
        if (!bound.has_value()) continue;
        Instance next{std::move(*bound), partial.matched};
        next.matched.push_back(atom.id);
        extended.push_back(std::move(next));
      }
    }
    instances = std::move(extended);
  }

  std::vector<Instance> passing;
  for (Instance& instance : instances) {
    ASSIGN_OR_RETURN(bool ok, comparisons_hold(parts, instance.binding));
    if (ok) passing.push_back(std::move(instance));
  }
  return passing;
}

// Evaluates each 'not p(...)' literal under `binding` and returns the
// negated literal for each one that is still derivable in `store`. A literal
// whose predicate was never derived at all can never be true, so it is
// dropped as trivially satisfied instead of being negated.
absl::StatusOr<std::vector<aspif::Lit>> negative_lits(
    const std::vector<const ClassicalLiteral*>& negative,
    const Binding& binding, const Store& store) {
  std::vector<aspif::Lit> lits;
  for (const ClassicalLiteral* literal : negative) {
    ASSIGN_OR_RETURN(Tuple tuple, eval_args(*literal, binding));
    const PredData* data = store.find(pred_key(*literal));
    if (data == nullptr) continue;
    const GroundAtom* atom = data->find(tuple);
    if (atom == nullptr) continue;
    lits.push_back(-atom->id);
  }
  return lits;
}

// Grounds one aggregate element into weighted literals, one per distinct
// evaluated `terms` tuple (ASP-Core-2 aggregates range over a *set* of
// tuples, so two elements or two groundings of the same element that
// produce equal tuples must contribute only once). Each distinct tuple gets
// a fresh auxiliary atom, supported by one plain rule per grounding that
// produced it -- multiple such rules give the atom OR semantics for free,
// exactly modeling "this tuple is in the set if any grounding satisfies it".
absl::StatusOr<std::vector<aspif::WeightedLit>> ground_agg_elements(
    const Aggregate& agg, const Binding& outer_binding, const Store& store,
    aspif::Program& result) {
  std::vector<aspif::WeightedLit> weighted;
  if (agg.elements == nullptr) return weighted;

  absl::flat_hash_map<Tuple, aspif::Atom> tuple_atoms;
  for (const auto& element_ptr : *agg.elements) {
    const AggregateElement& element = *element_ptr;
    BodyParts parts = split_naf_literals(element.literals);
    ASSIGN_OR_RETURN(
        std::vector<Instance> instances,
        find_instances(parts, store, outer_binding));
    for (const Instance& instance : instances) {
      Tuple tuple;
      if (element.terms != nullptr) {
        tuple.reserve(element.terms->size());
        for (const auto& term : *element.terms) {
          ASSIGN_OR_RETURN(Value value, eval_term(*term, instance.binding));
          tuple.push_back(std::move(value));
        }
      }
      int64_t weight = 1;
      if (agg.function == AggregateFunctionType::kAGGREGATE_SUM) {
        if (tuple.empty() || tuple[0].kind != Value::kNumber) {
          return absl::InvalidArgumentError(
              "a #sum aggregate element must give a number as its first "
              "term");
        }
        weight = static_cast<int64_t>(tuple[0].number);
      }

      auto [it, inserted] = tuple_atoms.try_emplace(tuple, 0);
      if (inserted) {
        it->second = result.new_atom();
        weighted.push_back({.lit = it->second, .weight = weight});
      }

      aspif::Rule support;
      support.head = {it->second};
      support.body = instance.matched;
      ASSIGN_OR_RETURN(std::vector<aspif::Lit> neg,
                       negative_lits(parts.negative, instance.binding, store));
      support.body.insert(support.body.end(), neg.begin(), neg.end());
      result.rules.push_back(std::move(support));
    }
  }
  return weighted;
}

// The aggregate bound requirements accumulated from its (up to two)
// 'term binop' sides, e.g. "3 <= #count{...} < 7" contributes lower = 3 (from
// '3 <=') and upper = 6 (from '< 7').
struct AggBounds {
  std::optional<int64_t> lower;    // the aggregate's value must be >= this.
  std::optional<int64_t> upper;    // the aggregate's value must be <= this.
  std::vector<int64_t> not_equal;  // the aggregate's value must differ from
                                   // each of these.

  void apply_lower(int64_t k) {
    lower = lower.has_value() ? std::max(*lower, k) : k;
  }
  void apply_upper(int64_t k) {
    upper = upper.has_value() ? std::min(*upper, k) : k;
  }
};

// Folds the upper-bound side ('AGG op k', e.g. 'AGG <= 7') into `bounds`.
void apply_upper_bound(int64_t k, BinopType op, AggBounds& bounds) {
  switch (op) {
    case BinopType::kLESS_OR_EQ:
      bounds.apply_upper(k);
      break;
    case BinopType::kLESS:
      bounds.apply_upper(k - 1);
      break;
    case BinopType::kGREATER_OR_EQ:
      bounds.apply_lower(k);
      break;
    case BinopType::kGREATER:
      bounds.apply_lower(k + 1);
      break;
    case BinopType::kEQUAL:
      bounds.apply_lower(k);
      bounds.apply_upper(k);
      break;
    case BinopType::kUNEQUAL:
      bounds.not_equal.push_back(k);
      break;
  }
}

// Folds the lower-bound side ('k op AGG', e.g. '3 <= AGG') into `bounds`.
void apply_lower_bound(int64_t k, BinopType op, AggBounds& bounds) {
  switch (op) {
    case BinopType::kLESS_OR_EQ:
      bounds.apply_lower(k);
      break;
    case BinopType::kLESS:
      bounds.apply_lower(k + 1);
      break;
    case BinopType::kGREATER_OR_EQ:
      bounds.apply_upper(k);
      break;
    case BinopType::kGREATER:
      bounds.apply_upper(k - 1);
      break;
    case BinopType::kEQUAL:
      bounds.apply_lower(k);
      bounds.apply_upper(k);
      break;
    case BinopType::kUNEQUAL:
      bounds.not_equal.push_back(k);
      break;
  }
}

// Negates a conjunction of literals into a single literal: if there's at
// most one literal, negating it directly is equivalent; otherwise a fresh
// atom is defined as their conjunction so its negation can stand for "not
// all of these hold".
aspif::Lit negate_conjunction(const std::vector<aspif::Lit>& lits,
                              aspif::Program& result) {
  if (lits.empty()) {
    // Negating an empty (vacuously true) conjunction is always false: a
    // fresh atom with no supporting rule can never be derived.
    return result.new_atom();
  }
  if (lits.size() == 1) return -lits[0];
  aspif::Atom conj = result.new_atom();
  aspif::Rule rule;
  rule.head = {conj};
  rule.body = lits;
  result.rules.push_back(std::move(rule));
  return -conj;
}

// Defines a fresh atom that holds exactly when the weights of the true
// literals in `weighted` sum to at least `bound`, and returns it. ASPIF's
// weight body is the only construct that can compare an aggregate against a
// number, so every bound an aggregate can carry is expressed in terms of it.
aspif::Atom at_least(int64_t bound,
                     const std::vector<aspif::WeightedLit>& weighted,
                     aspif::Program& result) {
  aspif::Atom atom = result.new_atom();
  aspif::Rule rule;
  rule.head = {atom};
  rule.body_type = aspif::Rule::BodyType::kWeight;
  rule.lower_bound = bound;
  rule.weighted_body = weighted;
  result.rules.push_back(std::move(rule));
  return atom;
}

// Evaluates one side of an aggregate's bound, e.g. the '3' in
// '3 <= #count{...}'.
absl::StatusOr<int64_t> eval_bound(const Term& term, const Binding& binding) {
  ASSIGN_OR_RETURN(Value value, eval_term(term, binding));
  if (value.kind != Value::kNumber) {
    return absl::InvalidArgumentError("an aggregate bound must be a number");
  }
  return static_cast<int64_t>(value.number);
}

// Grounds one Aggregate body item into the literals that must be appended to
// the enclosing rule's body for the aggregate to hold, e.g. '#count{X :
// p(X)} >= 2' grounds to a single literal referencing a fresh atom defined
// by an ASPIF weight-body rule. Only #count and #sum are supported: they map
// directly onto ASPIF's weight body, whereas #min/#max would need a
// different (guess-and-check) encoding.
absl::StatusOr<std::vector<aspif::Lit>> ground_aggregate(
    const Aggregate& agg, const Binding& outer_binding, const Store& store,
    aspif::Program& result) {
  if (agg.function == AggregateFunctionType::kAGGREGATE_MAX ||
      agg.function == AggregateFunctionType::kAGGREGATE_MIN) {
    return absl::UnimplementedError(
        "#min and #max aggregates are not supported yet");
  }

  AggBounds bounds;
  if (agg.lb_term != nullptr) {
    ASSIGN_OR_RETURN(int64_t k, eval_bound(*agg.lb_term, outer_binding));
    apply_lower_bound(k, agg.lb_op, bounds);
  }
  if (agg.ub_term != nullptr) {
    ASSIGN_OR_RETURN(int64_t k, eval_bound(*agg.ub_term, outer_binding));
    apply_upper_bound(k, agg.ub_op, bounds);
  }

  ASSIGN_OR_RETURN(std::vector<aspif::WeightedLit> weighted,
                   ground_agg_elements(agg, outer_binding, store, result));

  // The conjunction of literals that together mean "the bound holds". Only
  // "at least" is available, so the other comparisons are phrased in terms of
  // it: the value is at most 7 exactly when it isn't at least 8.
  std::vector<aspif::Lit> extra;
  if (bounds.lower.has_value()) {
    extra.push_back(at_least(*bounds.lower, weighted, result));
  }
  if (bounds.upper.has_value()) {
    extra.push_back(-at_least(*bounds.upper + 1, weighted, result));
  }
  for (int64_t k : bounds.not_equal) {
    // The value equals k exactly when it is at least k but not at least
    // k + 1, so requiring it to differ from k negates that conjunction.
    aspif::Lit k_or_more = at_least(k, weighted, result);
    aspif::Lit more_than_k = at_least(k + 1, weighted, result);
    extra.push_back(negate_conjunction({k_or_more, -more_than_k}, result));
  }

  if (!agg.naf) return extra;
  return std::vector<aspif::Lit>{negate_conjunction(extra, result)};
}

// Fills `store` with every atom that could appear in an answer set, by
// running each rule against the atoms collected so far and repeating until a
// full pass adds nothing new. Each new atom gets its ASPIF number from
// `aspif_prog`.
//
// 'not' literals are ignored here. Given "p :- q, not r." with q and r both
// collected, p is collected too. That is deliberate: this phase only decides
// which atoms can exist at all; emit_rules() emits the rule with the 'not r'
// still in it, and the solver decides whether p is true.
//
// Aggregates are ignored the same way: a rule's aggregates are never checked
// here, so a rule like 'p :- dom(X), #count{Y : q(X,Y)} >= 2.' derives p(x)
// for every x in dom regardless of the count. safety.cc guarantees an
// aggregate's own predicates can't be recursive with the rule's head, so by
// the time this rule's component is emitted, the store for those predicates
// is already complete and the real weight-body encoding constrains the
// solver correctly.
//
// Atoms a rule derives are inserted into `store` right away, so a rule later
// in the same pass already sees them. A rule never sees atoms from its own
// current run, though, only from previous ones. So a rule that feeds on its
// own head, like 'reachable(X, Z) :- reachable(X, Y), edge(Y, Z).', extends
// the reachable chain by one hop per pass. That's why repeating passes to a
// fixpoint is necessary in the first place.
absl::Status derive_atoms(const std::vector<RuleView>& rules, Store& store,
                          aspif::Program& aspif_prog) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (const RuleView& rule : rules) {
      if (rule.head == nullptr) continue;
      ASSIGN_OR_RETURN(std::vector<Instance> instances,
                       find_instances(rule.parts, store));
      for (const Instance& instance : instances) {
        ASSIGN_OR_RETURN(Tuple tuple, eval_args(*rule.head, instance.binding));
        if (store.insert(pred_key(*rule.head), std::move(tuple), aspif_prog)) {
          changed = true;
        }
      }
    }
  }
  return absl::OkStatus();
}

// Emits one ASPIF rule per rule instance. A 'not q' whose atom q was never
// derived by derive_atoms() can never be true, so the literal is dropped as
// satisfied; otherwise it stays in the rule body, negated.
absl::Status emit_rules(const std::vector<RuleView>& rules, const Store& store,
                        aspif::Program& result) {
  for (const RuleView& rule : rules) {
    ASSIGN_OR_RETURN(std::vector<Instance> instances,
                     find_instances(rule.parts, store));
    for (const Instance& instance : instances) {
      aspif::Rule aspif_rule;
      if (rule.head != nullptr) {
        ASSIGN_OR_RETURN(Tuple tuple, eval_args(*rule.head, instance.binding));
        // derive_atoms() added every derivable head atom, so this lookup
        // succeeds.
        aspif_rule.head.push_back(
            store.find(pred_key(*rule.head))->find(tuple)->id);
      }
      aspif_rule.body = instance.matched;
      ASSIGN_OR_RETURN(
          std::vector<aspif::Lit> neg,
          negative_lits(rule.parts.negative, instance.binding, store));
      aspif_rule.body.insert(aspif_rule.body.end(), neg.begin(), neg.end());
      for (const Aggregate* aggregate : rule.parts.aggregates) {
        ASSIGN_OR_RETURN(
            std::vector<aspif::Lit> extra,
            ground_aggregate(*aggregate, instance.binding, store, result));
        aspif_rule.body.insert(aspif_rule.body.end(), extra.begin(),
                               extra.end());
      }
      result.rules.push_back(std::move(aspif_rule));
    }
  }
  return absl::OkStatus();
}

// Names every atom of a user-visible predicate ('_' prefixes are internal)
// so answer sets print symbolically.
void name_outputs(const Store& store, aspif::Program& result) {
  for (const PredKey& key : store.order) {
    if (!key.name.empty() && key.name[0] == '_') continue;
    const PredData& data = *store.find(key);
    for (const GroundAtom& atom : data.atoms) {
      std::string name = key.name;
      if (key.arity > 0) {
        auto print_arg = [](std::string* out, const Value& value) {
          out->append(value.printed());
        };
        absl::StrAppend(&name, "(", absl::StrJoin(atom.args, ",", print_arg),
                        ")");
      }
      result.outputs.push_back(
          aspif::Output{.name = std::move(name), .condition = {atom.id}});
    }
  }
}

}  // namespace

absl::StatusOr<aspif::Program> ground(const Program& prog) {
  const PredGraph graph = build_pred_graph(prog);
  const std::vector<int> component =
      strongly_connected_components(graph.pos_succ);
  ASSIGN_OR_RETURN(std::vector<RuleView> rules, make_rule_views(prog));
  std::vector<std::vector<RuleView>> rules_by_component =
      bucket_rule_views(graph, component, rules);
  std::vector<RuleView> headless_rules;
  for (const auto& rule : rules) {
    if (rule.head == nullptr) headless_rules.push_back(rule);
  }

  // Two passes: derive every component, then emit every component. A
  // component's positive body literals depend only on earlier components,
  // so deriving in ascending order gets those right. Negation is different:
  // a 'not q' can point at a later component, since negation edges don't
  // constrain component order. So by the time any component is emitted, q's
  // final atom set already exists, and emit_rules can correctly decide
  // whether q is derivable.
  aspif::Program result;
  Store store;
  for (const auto& component_rules : rules_by_component) {
    RETURN_IF_ERROR(derive_atoms(component_rules, store, result));
  }
  for (const auto& component_rules : rules_by_component) {
    RETURN_IF_ERROR(emit_rules(component_rules, store, result));
  }
  RETURN_IF_ERROR(emit_rules(headless_rules, store, result));
  name_outputs(store, result);
  return result;
}
