#include "ground.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "collect.h"
#include "graph.h"
#include "macros.h"

namespace {

// A ground value: a number, a constant like abc, a quoted string like "abc",
// or a function term like f(1, abc) whose arguments are themselves values.
// The kind order matches the ASP-Core-2 spec, which orders integers <
// symbolic constants < string constants < functional terms.
struct Value {
  enum Kind { kNumber, kConstant, kString, kFunction };

  Kind kind;
  int64_t number = 0;        // set only when kind == kNumber
  std::string text;          // the constant's, string's, or function's name
  std::vector<Value> args;   // set only when kind == kFunction, never empty

  static Value make_number(int64_t n) { return {kNumber, n, "", {}}; }
  static Value make_constant(std::string name) {
    return {kConstant, 0, std::move(name), {}};
  }
  static Value make_string(std::string contents) {
    return {kString, 0, std::move(contents), {}};
  }
  static Value make_function(std::string name, std::vector<Value> args) {
    return {kFunction, 0, std::move(name), std::move(args)};
  }

  // The value as it prints in an answer set: 42, abc, "abc", or f(1,abc).
  std::string printed() const {
    if (kind == kNumber) return absl::StrCat(number);
    if (kind == kString) return absl::StrCat("\"", text, "\"");
    if (kind == kConstant) return text;
    auto print_arg = [](std::string* out, const Value& value) {
      out->append(value.printed());
    };
    return absl::StrCat(text, "(", absl::StrJoin(args, ",", print_arg), ")");
  }

  bool operator==(const Value&) const = default;

  template <typename H>
  friend H AbslHashValue(H h, const Value& v) {
    return H::combine(std::move(h), v.kind, v.number, v.text, v.args);
  }
};

// Orders ground values per the ASP-Core-2 spec (see the comment on Value
// above): numbers numerically, constants and strings lexicographically, and
// function terms by arity first, then name, then argument by argument.
int compare_values(const Value& a, const Value& b) {
  if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
  if (a.kind == Value::kNumber) {
    return a.number < b.number ? -1 : a.number > b.number ? 1 : 0;
  }
  if (a.kind == Value::kFunction && a.args.size() != b.args.size()) {
    return a.args.size() < b.args.size() ? -1 : 1;
  }
  if (a.text != b.text) return a.text < b.text ? -1 : 1;
  for (size_t k = 0; k < a.args.size(); ++k) {
    int c = compare_values(a.args[k], b.args[k]);
    if (c != 0) return c;
  }
  return 0;
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

absl::StatusOr<std::optional<Value>> eval_term(const Term& term,
                                               const Binding& binding);

// Evaluates a term that has to come out as a number, e.g. either side of a
// '+'. Returns nullopt if it doesn't, e.g. for the 'a + 1' that 'X + 1'
// becomes under the binding {X: a}: arithmetic is only defined on integers,
// so that binding is ill-formed and its rule instance does not exist.
absl::StatusOr<std::optional<int64_t>> eval_number(const Term& term,
                                                   const Binding& binding) {
  ASSIGN_OR_RETURN(std::optional<Value> value, eval_term(term, binding));
  if (!value.has_value() || value->kind != Value::kNumber) return std::nullopt;
  return value->number;
}

// Evaluates a term to its ground value, e.g. 'X' evaluates to 1 under the
// binding {X: 1}. Every variable must already be bound.
//
// Returns nullopt when the binding leaves an arithmetic term undefined, e.g.
// 'X / 0' or 'a + 1'. ASP-Core-2 calls such a binding ill-formed and builds
// no ground instance from it, so callers drop the instance rather than
// failing the whole grounding.
absl::StatusOr<std::optional<Value>> eval_term(const Term& term,
                                               const Binding& binding) {
  switch (term.kind) {
    case Term::NumberKind:
      return Value::make_number(
          static_cast<int64_t>(static_cast<const Number&>(term).value));
    case Term::StringKind:
      return Value::make_string(static_cast<const String&>(term).value);
    case Term::AtomKind: {
      const Atom& atom = static_cast<const Atom&>(term);
      if (atom.args == nullptr) return Value::make_constant(atom.name);
      std::vector<Value> args;
      args.reserve(atom.args->size());
      for (const auto& arg : *atom.args) {
        ASSIGN_OR_RETURN(std::optional<Value> value, eval_term(*arg, binding));
        if (!value.has_value()) return std::nullopt;
        args.push_back(std::move(*value));
      }
      return Value::make_function(atom.name, std::move(args));
    }
    case Term::VariableKind: {
      const std::string& name = static_cast<const Variable&>(term).name;
      auto it = binding.find(name);
      if (it == binding.end()) {
        return absl::InvalidArgumentError(
            absl::StrCat("variable '", name, "' is not bound by the rule body"));
      }
      return it->second;
    }
    case Term::AnonymousVariableKind:
      return absl::InvalidArgumentError(
          "'_' cannot appear in a position that must be ground");
    case Term::NegatedTermKind: {
      const NegatedTerm& negated = static_cast<const NegatedTerm&>(term);
      ASSIGN_OR_RETURN(std::optional<int64_t> value,
                       eval_number(*negated.term, binding));
      if (!value.has_value()) return std::nullopt;
      return Value::make_number(-*value);
    }
    case Term::TermOperationKind: {
      const TermOperation& operation = static_cast<const TermOperation&>(term);
      ASSIGN_OR_RETURN(std::optional<int64_t> left,
                       eval_number(*operation.left, binding));
      ASSIGN_OR_RETURN(std::optional<int64_t> right,
                       eval_number(*operation.right, binding));
      if (!left.has_value() || !right.has_value()) return std::nullopt;
      switch (operation.op) {
        case OperationType::kPLUS:
          return Value::make_number(*left + *right);
        case OperationType::kMINUS:
          return Value::make_number(*left - *right);
        case OperationType::kTIMES:
          return Value::make_number(*left * *right);
        case OperationType::kDIV:
          // Division by zero has no value, so the binding is ill-formed.
          if (*right == 0) return std::nullopt;
          return Value::make_number(*left / *right);
      }
      return absl::InternalError("unknown arithmetic operator");
    }
  }
  return absl::InternalError("unknown term kind");
}

// Evaluates a list of terms under `binding`, e.g. the 'X, 2' of 'p(X, 2)'
// evaluates to {1, 2} under the binding {X: 1}. Returns nullopt if any term
// is ill-formed, e.g. the 'X / 0' of 'p(X / 0)'.
absl::StatusOr<std::optional<Tuple>> eval_terms(const Terms& terms,
                                                const Binding& binding) {
  Tuple tuple;
  if (terms == nullptr) return tuple;
  tuple.reserve(terms->size());
  for (const auto& term : *terms) {
    ASSIGN_OR_RETURN(std::optional<Value> value, eval_term(*term, binding));
    if (!value.has_value()) return std::nullopt;
    tuple.push_back(std::move(*value));
  }
  return tuple;
}

// Evaluates each argument of a ground instance of `literal` under `binding`,
// e.g. 'p(X, 2)' evaluates to {1, 2} under the binding {X: 1}.
absl::StatusOr<std::optional<Tuple>> eval_args(const ClassicalLiteral& literal,
                                               const Binding& binding) {
  return eval_terms(literal.args, binding);
}

// Tries to match one argument term against one stored value, extending
// `binding` with whatever variables the match binds. Returns false on a
// mismatch, in which case `binding` may already hold bindings from the part
// that did match, so the caller must discard it.
//
// A function term matches value by value against a stored function of the
// same name and arity, e.g. 'f(X, b)' matches a stored f(1, b) and binds
// X to 1.
absl::StatusOr<bool> match_term(const Term& arg, const Value& value,
                                Binding& binding) {
  if (arg.kind == Term::VariableKind) {
    const std::string& name = static_cast<const Variable&>(arg).name;
    auto [it, inserted] = binding.try_emplace(name, value);
    return inserted || it->second == value;
  }
  if (arg.kind == Term::AnonymousVariableKind) return true;
  if (arg.kind == Term::AtomKind) {
    const Atom& atom = static_cast<const Atom&>(arg);
    if (atom.args != nullptr) {
      if (value.kind != Value::kFunction || value.text != atom.name ||
          value.args.size() != atom.args->size()) {
        return false;
      }
      for (size_t k = 0; k < value.args.size(); ++k) {
        ASSIGN_OR_RETURN(bool ok,
                         match_term(*(*atom.args)[k], value.args[k], binding));
        if (!ok) return false;
      }
      return true;
    }
  }
  // An ill-formed term, e.g. 'X / 0', has no value at all, so it matches
  // nothing.
  ASSIGN_OR_RETURN(std::optional<Value> evaluated, eval_term(arg, binding));
  return evaluated.has_value() && *evaluated == value;
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
    ASSIGN_OR_RETURN(bool ok,
                     match_term(*(*literal.args)[k], tuple[k], extended));
    if (!ok) return std::nullopt;
  }
  return extended;
}

// One rule body, split into the kinds of items grounding treats differently.
struct BodyParts {
  // Positive classical literals: matched against the store to bind
  // variables, e.g. 'edge(X, Y)' in "reachable(X, Y) :- edge(X, Y)."
  std::vector<const ClassicalLiteral*> positive;
  // Builtin atoms, e.g. 'X < 2' (possibly under 'not') or 'Y = X + 1'. An
  // equality with an unbound variable on one side binds that variable; every
  // other comparison is a test decided once the body is bound.
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

// A comparison read as an assignment: the variable it binds and the term
// whose value the variable takes, e.g. Y and 'X + 1' for 'Y = X + 1'.
struct Assignment {
  const std::string* name;
  const Term* value;
};

// Reads `naf` as an assignment, e.g. 'Y = X + 1' assigns to Y. Only an
// un-negated '=' with a still-unbound variable on one side assigns; anything
// else, including 'Y = X + 1' once Y is bound, is a test and gets nullopt.
std::optional<Assignment> assignment_of(const NafLiteral& naf,
                                        const Binding& binding) {
  const auto& builtin = static_cast<const BuiltinAtom&>(*naf.literal);
  if (naf.naf || builtin.op != BinopType::kEQUAL) return std::nullopt;
  if (builtin.left->kind == Term::VariableKind) {
    const std::string& name = static_cast<const Variable&>(*builtin.left).name;
    if (!binding.contains(name)) {
      return Assignment{.name = &name, .value = builtin.right.get()};
    }
  }
  if (builtin.right->kind == Term::VariableKind) {
    const std::string& name = static_cast<const Variable&>(*builtin.right).name;
    if (!binding.contains(name)) {
      return Assignment{.name = &name, .value = builtin.left.get()};
    }
  }
  return std::nullopt;
}

// Whether every variable in `term` already has a value, i.e. whether the term
// can be evaluated at all.
bool is_bound(const Term& term, const Binding& binding) {
  absl::flat_hash_set<std::string_view> vars;
  collect::collect_variables(term, vars);
  for (std::string_view var : vars) {
    if (!binding.contains(var)) return false;
  }
  return true;
}

// Extends `binding` with the variables the body's assignments bind, e.g. 'Y =
// X + 1' adds {Y: 2} to the binding {X: 1}, and then returns whether every
// remaining comparison holds, e.g. whether 'X < 2' holds given {X: 1}.
//
// Returns false, meaning the binding builds no rule instance, when a
// comparison has an ill-formed side, e.g. 'X < 1 / 0', or when an assignment's
// value is ill-formed, e.g. the 'Y = 1 / 0' that 'Y = 1 / X' becomes under
// {X: 0}.
absl::StatusOr<bool> apply_comparisons(const BodyParts& parts,
                                       Binding& binding) {
  std::vector<bool> assigns(parts.comparisons.size(), false);
  // One assignment can bind a variable another one needs, e.g. 'Y = X + 1, Z =
  // Y + 1', so passes repeat until a pass binds nothing new.
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t k = 0; k < parts.comparisons.size(); ++k) {
      if (assigns[k]) continue;
      std::optional<Assignment> assignment =
          assignment_of(*parts.comparisons[k], binding);
      if (!assignment.has_value()) continue;
      if (!is_bound(*assignment->value, binding)) continue;
      ASSIGN_OR_RETURN(std::optional<Value> value,
                       eval_term(*assignment->value, binding));
      if (!value.has_value()) return false;
      binding.emplace(*assignment->name, *std::move(value));
      assigns[k] = true;
      changed = true;
    }
  }

  // An assignment holds by construction, so only the tests are left to check.
  for (size_t k = 0; k < parts.comparisons.size(); ++k) {
    if (assigns[k]) continue;
    const NafLiteral& item = *parts.comparisons[k];
    const auto& builtin = static_cast<const BuiltinAtom&>(*item.literal);
    ASSIGN_OR_RETURN(std::optional<Value> left,
                     eval_term(*builtin.left, binding));
    ASSIGN_OR_RETURN(std::optional<Value> right,
                     eval_term(*builtin.right, binding));
    if (!left.has_value() || !right.has_value()) return false;
    bool holds = builtin_holds(builtin.op, *left, *right);
    if (item.naf) holds = !holds;
    if (!holds) return false;
  }
  return true;
}

// Finds every way to satisfy the body with the atoms currently in the store.
// Works through the positive literals left to right, extending each partial
// instance with every stored tuple that matches, then keeps the instances
// whose assignments and comparisons work out. 'not' literals never filter
// here; the caller decides what to do with them.
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
    ASSIGN_OR_RETURN(bool ok, apply_comparisons(parts, instance.binding));
    if (ok) passing.push_back(std::move(instance));
  }
  return passing;
}

// Evaluates each 'not p(...)' literal under `binding` and returns the
// negated literal for each one that is still derivable in `store`. A literal
// whose predicate was never derived at all can never be true, so it is
// dropped as trivially satisfied instead of being negated.
//
// Returns nullopt if one of the literals is ill-formed, e.g. 'not p(X / 0)':
// that makes the whole rule instance nonexistent, not just this literal.
absl::StatusOr<std::optional<std::vector<aspif::Lit>>> negative_lits(
    const std::vector<const ClassicalLiteral*>& negative,
    const Binding& binding, const Store& store) {
  std::vector<aspif::Lit> lits;
  for (const ClassicalLiteral* literal : negative) {
    ASSIGN_OR_RETURN(std::optional<Tuple> tuple, eval_args(*literal, binding));
    if (!tuple.has_value()) return std::nullopt;
    const PredData* data = store.find(pred_key(*literal));
    if (data == nullptr) continue;
    const GroundAtom* atom = data->find(*tuple);
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
      // An element whose terms are ill-formed under this local binding
      // contributes no tuple to the set.
      ASSIGN_OR_RETURN(std::optional<Tuple> maybe_tuple,
                       eval_terms(element.terms, instance.binding));
      if (!maybe_tuple.has_value()) continue;
      Tuple tuple = *std::move(maybe_tuple);
      int64_t weight = 1;
      if (agg.function == AggregateFunctionType::kAGGREGATE_SUM) {
        // #sum adds up the tuples whose first term is an integer and ignores
        // the others, e.g. '#sum{ 1 : p; a : q }' is just 1. A tuple that
        // adds nothing needs no literal in the weight body at all. (#count
        // does count such a tuple, which is why this only applies to #sum.)
        if (tuple.empty() || tuple[0].kind != Value::kNumber) continue;
        weight = tuple[0].number;
      }

      auto [it, inserted] = tuple_atoms.try_emplace(tuple, 0);
      if (inserted) {
        it->second = result.new_atom();
        weighted.push_back({.lit = it->second, .weight = weight});
      }

      ASSIGN_OR_RETURN(std::optional<std::vector<aspif::Lit>> neg,
                       negative_lits(parts.negative, instance.binding, store));
      if (!neg.has_value()) continue;
      aspif::Rule support;
      support.head = {it->second};
      support.body = instance.matched;
      support.body.insert(support.body.end(), neg->begin(), neg->end());
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
//
// ASPIF weights must be positive, but a #sum element can weigh a negative
// number, e.g. '#sum{ -1 : p }'. A negative weight w on literal l is rewritten
// using w * [l] = w + (-w) * [not l], which flips the literal, negates its
// weight, and leaves the constant w to fold into the bound. A weight of 0
// never changes the sum, so its literal is left out.
aspif::Atom at_least(int64_t bound,
                     const std::vector<aspif::WeightedLit>& weighted,
                     aspif::Program& result) {
  std::vector<aspif::WeightedLit> positive;
  positive.reserve(weighted.size());
  int64_t lower = bound;
  for (const aspif::WeightedLit& wl : weighted) {
    if (wl.weight > 0) {
      positive.push_back(wl);
    } else if (wl.weight < 0) {
      positive.push_back({.lit = -wl.lit, .weight = -wl.weight});
      lower -= wl.weight;
    }
  }

  aspif::Atom atom = result.new_atom();
  aspif::Rule rule;
  rule.head = {atom};
  // Positive weights can only sum to 0 or more, so a bound of 0 or less holds
  // no matter which literals are true: the atom is a fact instead.
  if (lower > 0) {
    rule.body_type = aspif::Rule::BodyType::kWeight;
    rule.lower_bound = lower;
    rule.weighted_body = std::move(positive);
  }
  result.rules.push_back(std::move(rule));
  return atom;
}

// Evaluates one side of an aggregate's bound, e.g. the '3' in
// '3 <= #count{...}'.
absl::StatusOr<std::optional<int64_t>> eval_bound(const Term& term,
                                                  const Binding& binding) {
  // A still-unbound variable here is the '#count{...} = X' form, which binds X
  // to the aggregate's value. Grounding can't do that: an ASPIF weight body
  // only compares an aggregate against a number it already knows, so there is
  // no value to hand back.
  if (term.kind == Term::VariableKind &&
      !binding.contains(static_cast<const Variable&>(term).name)) {
    return absl::UnimplementedError(
        absl::StrCat("variable '", static_cast<const Variable&>(term).name,
                     "' is bound to an aggregate's value, which is not "
                     "supported yet"));
  }
  ASSIGN_OR_RETURN(std::optional<Value> value, eval_term(term, binding));
  if (!value.has_value()) return std::nullopt;
  if (value->kind != Value::kNumber) {
    return absl::InvalidArgumentError("an aggregate bound must be a number");
  }
  return value->number;
}

// Grounds one Aggregate body item into the literals that must be appended to
// the enclosing rule's body for the aggregate to hold, e.g. '#count{X :
// p(X)} >= 2' grounds to a single literal referencing a fresh atom defined
// by an ASPIF weight-body rule. Only #count and #sum are supported: they map
// directly onto ASPIF's weight body, whereas #min/#max would need a
// different (guess-and-check) encoding.
// Returns nullopt if one of the aggregate's bounds is ill-formed, which
// makes the whole enclosing rule instance nonexistent.
absl::StatusOr<std::optional<std::vector<aspif::Lit>>> ground_aggregate(
    const Aggregate& agg, const Binding& outer_binding, const Store& store,
    aspif::Program& result) {
  if (agg.function == AggregateFunctionType::kAGGREGATE_MAX ||
      agg.function == AggregateFunctionType::kAGGREGATE_MIN) {
    return absl::UnimplementedError(
        "#min and #max aggregates are not supported yet");
  }

  AggBounds bounds;
  if (agg.lb_term != nullptr) {
    ASSIGN_OR_RETURN(std::optional<int64_t> k,
                     eval_bound(*agg.lb_term, outer_binding));
    if (!k.has_value()) return std::nullopt;
    apply_lower_bound(*k, agg.lb_op, bounds);
  }
  if (agg.ub_term != nullptr) {
    ASSIGN_OR_RETURN(std::optional<int64_t> k,
                     eval_bound(*agg.ub_term, outer_binding));
    if (!k.has_value()) return std::nullopt;
    apply_upper_bound(*k, agg.ub_op, bounds);
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

// Whether the body parts derive_atoms() otherwise ignores are well-formed
// under `binding`: the arguments of every 'not' literal and each aggregate's
// bounds. Ill-formed means the rule has no ground instance under this
// binding, so its head atom must not be derived either, e.g. "q(X) :- p(X),
// not r(4 / X)." derives no q(0), because 'not r(4 / 0)' cannot be ground.
absl::StatusOr<bool> ignored_parts_are_well_formed(const BodyParts& parts,
                                                   const Binding& binding) {
  for (const ClassicalLiteral* literal : parts.negative) {
    ASSIGN_OR_RETURN(std::optional<Tuple> tuple, eval_args(*literal, binding));
    if (!tuple.has_value()) return false;
  }
  for (const Aggregate* aggregate : parts.aggregates) {
    if (aggregate->lb_term != nullptr) {
      ASSIGN_OR_RETURN(std::optional<int64_t> k,
                       eval_bound(*aggregate->lb_term, binding));
      if (!k.has_value()) return false;
    }
    if (aggregate->ub_term != nullptr) {
      ASSIGN_OR_RETURN(std::optional<int64_t> k,
                       eval_bound(*aggregate->ub_term, binding));
      if (!k.has_value()) return false;
    }
  }
  return true;
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
        ASSIGN_OR_RETURN(
            bool well_formed,
            ignored_parts_are_well_formed(rule.parts, instance.binding));
        if (!well_formed) continue;
        ASSIGN_OR_RETURN(std::optional<Tuple> tuple,
                         eval_args(*rule.head, instance.binding));
        if (!tuple.has_value()) continue;
        if (store.insert(pred_key(*rule.head), *std::move(tuple), aspif_prog)) {
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
      // The head is looked up last, once every reason to drop the instance
      // has been ruled out: derive_atoms() dropped the same instances, so a
      // head atom looked up for one of them would be missing from the store.
      aspif::Rule aspif_rule;
      aspif_rule.body = instance.matched;
      ASSIGN_OR_RETURN(
          std::optional<std::vector<aspif::Lit>> neg,
          negative_lits(rule.parts.negative, instance.binding, store));
      if (!neg.has_value()) continue;
      aspif_rule.body.insert(aspif_rule.body.end(), neg->begin(), neg->end());

      bool well_formed = true;
      for (const Aggregate* aggregate : rule.parts.aggregates) {
        ASSIGN_OR_RETURN(
            std::optional<std::vector<aspif::Lit>> extra,
            ground_aggregate(*aggregate, instance.binding, store, result));
        if (!extra.has_value()) {
          well_formed = false;
          break;
        }
        aspif_rule.body.insert(aspif_rule.body.end(), extra->begin(),
                               extra->end());
      }
      if (!well_formed) continue;

      if (rule.head != nullptr) {
        ASSIGN_OR_RETURN(std::optional<Tuple> tuple,
                         eval_args(*rule.head, instance.binding));
        // An ill-formed head, e.g. the 'q(X / 0)' of "q(X / 0) :- p(X).",
        // means this instance has no ground rule. derive_atoms() skipped it
        // for the same reason, so nothing is missing from the store.
        if (!tuple.has_value()) continue;
        // derive_atoms() added every derivable head atom, so this lookup
        // succeeds.
        aspif_rule.head.push_back(
            store.find(pred_key(*rule.head))->find(*tuple)->id);
      }
      result.rules.push_back(std::move(aspif_rule));
    }
  }
  return absl::OkStatus();
}

// Turns the ground _viol atoms into one ASPIF minimize statement per
// priority level.
//
// normalize() rewrote each weak constraint ':~ body. [w@l, t1, ..., tn]' into
// the ordinary rule '_viol(l, w, t1, ..., tn) :- body.', so the _viol atoms
// in the store are already grounded and numbered like any others. Each one a
// solver makes true is one violation costing w at level l, and a minimize
// statement asks for exactly that: the sum of the weights of its true
// literals, minimized. Weak-constraint set semantics comes along for free,
// because two groundings agreeing on (l, w, t1, ..., tn) are the same ground
// atom and so appear once.
//
// A level's literals are collected across every _viol arity in the program:
// weak constraints with different numbers of terms give _viol different
// arities, which are distinct predicates in the store but the same level.
//
// A violation whose weight or level is not an integer, e.g. ':~ p(X). [X@0]'
// where X binds to a constant, costs nothing at any level, so it is left out.
void emit_minimize(const Store& store, aspif::Program& result) {
  absl::btree_map<int64_t, std::vector<aspif::WeightedLit>> by_level;
  for (const PredKey& key : store.order) {
    if (key.name != "_viol") continue;
    for (const GroundAtom& atom : store.find(key)->atoms) {
      // normalize() always puts the level and weight first, in that order.
      const Value& level = atom.args[0];
      const Value& weight = atom.args[1];
      if (level.kind != Value::kNumber || weight.kind != Value::kNumber) {
        continue;
      }
      by_level[level.number].push_back(
          {.lit = atom.id, .weight = weight.number});
    }
  }
  for (auto& [level, lits] : by_level) {
    result.minimize.push_back(
        aspif::Minimize{.priority = level, .lits = std::move(lits)});
  }
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
  emit_minimize(store, result);
  name_outputs(store, result);
  return result;
}
