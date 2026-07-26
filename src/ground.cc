#include "ground.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "collect.h"
#include "graph.h"
#include "macros.h"
#include "normalize.h"

namespace {

// --------------------------------------------------------------------------
// Ground values and the store of derived atoms
//
// The values a variable can take, the atoms built out of them, and the
// bindings that map a rule's variables to values while it is being ground.
// --------------------------------------------------------------------------

// A ground value: a number, a constant like abc, a quoted string like "abc",
// or a function term like f(1, abc) whose arguments are themselves values.
// The kind order matches the ASP-Core-2 spec, which orders integers <
// symbolic constants < string constants < functional terms.
struct Value {
  enum Kind { kNumber, kConstant, kString, kFunction };

  Kind kind;
  int64_t number = 0;       // set only when kind == kNumber
  std::string text;         // the constant's, string's, or function's name
  std::vector<Value> args;  // set only when kind == kFunction, never empty

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
  std::string printed() const;

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

// One ground atom as it prints, e.g. p(1,abc) for the predicate p/2 and the
// tuple {1, abc}.
std::string printed_atom(std::string_view name, const Tuple& args) {
  if (args.empty()) return std::string(name);
  auto print_arg = [](std::string* out, const Value& value) {
    out->append(value.printed());
  };
  return absl::StrCat(name, "(", absl::StrJoin(args, ",", print_arg), ")");
}

// A function term prints exactly like an atom of the same name and arguments,
// e.g. the f(1,abc) that is a value and the f(1,abc) that is an atom.
std::string Value::printed() const {
  if (kind == kNumber) return absl::StrCat(number);
  if (kind == kString) return absl::StrCat("\"", text, "\"");
  if (kind == kConstant) return text;
  return printed_atom(text, args);
}

// A slot number for each variable occurrence in one rule, e.g. both X's of
// "p(X) :- q(X)." get slot 0 and Y gets slot 1. Occurrences of the same name
// share a slot, which is what makes matching q(1) bind the head's X too.
//
// Grounding looks a variable up by the address of its AST node, so a lookup
// hashes a pointer rather than the variable's name, and what it returns is an
// index into a plain vector of values.
struct VarSlots {
  absl::flat_hash_map<const Variable*, size_t> of_node;
  size_t count = 0;

  // Gives `var` the slot its name already has, or a fresh one if this is the
  // first occurrence of the name.
  void add(const Variable& var,
           absl::flat_hash_map<std::string_view, size_t>& by_name) {
    auto [it, inserted] = by_name.try_emplace(var.name, count);
    if (inserted) ++count;
    of_node[&var] = it->second;
  }
};

// The values one candidate rule instance gives the rule's variables, e.g. {X:
// 1, Y: abc} while matching the body of "p(X, Y) :- q(X), r(Y)." against q(1)
// and r(abc). A variable with no value yet has an empty slot.
class Binding {
 public:
  explicit Binding(const VarSlots& slots)
      : slots_(&slots), values_(slots.count) {}

  // The slot `var` reads and writes. Every variable of the rule the binding
  // belongs to has one.
  size_t slot_of(const Variable& var) const { return slots_->of_node.at(&var); }

  // The value of `var`, or null if it has none yet. A variable from outside
  // this binding's rule, which cannot happen for a well-formed rule, also
  // reads as having none.
  const Value* find(const Variable& var) const {
    auto it = slots_->of_node.find(&var);
    if (it == slots_->of_node.end()) return nullptr;
    return at(it->second);
  }
  bool contains(const Variable& var) const { return find(var) != nullptr; }

  const Value* at(size_t slot) const {
    const std::optional<Value>& value = values_[slot];
    return value.has_value() ? &*value : nullptr;
  }
  void set(size_t slot, Value value) { values_[slot] = std::move(value); }
  void clear(size_t slot) { values_[slot].reset(); }

 private:
  const VarSlots* slots_;
  std::vector<std::optional<Value>> values_;
};

// Undoes the slots filled while it is alive, so a join can try the next
// candidate atom from the binding it started the last one with. Matching runs
// far more often than it succeeds, and a failed match leaves behind whatever
// its matching prefix bound, so every step of the search brackets itself with
// one of these instead of working on a copy of the binding.
class BindingTrail {
 public:
  explicit BindingTrail(Binding& binding) : binding_(binding) {}
  BindingTrail(const BindingTrail&) = delete;
  BindingTrail& operator=(const BindingTrail&) = delete;
  ~BindingTrail() {
    for (size_t slot : filled_) binding_.clear(slot);
  }

  void record(size_t slot) { filled_.push_back(slot); }

  // Keeps the values filled so far instead of undoing them, for a caller that
  // is building a binding to hold on to rather than searching.
  void keep() { filled_.clear(); }

 private:
  Binding& binding_;
  absl::InlinedVector<size_t, 8> filled_;
};

// One ground atom: its argument tuple plus the ASPIF atom number assigned to
// it, e.g. {1, abc} and 7 for p(1, abc) numbered 7.
struct GroundAtom {
  Tuple args;
  aspif::Atom id;
};

// One predicate's ground atoms found so far, e.g. the two GroundAtoms for
// edge(a, b) and edge(b, c) once both have been derived for edge/2.
//
// `atoms` is a deque because a join hands out pointers into it and then derives
// new atoms while it is still reading: a deque keeps the atoms already in it
// where they are when it grows, and a vector would move them out from under
// those pointers.
struct PredData {
  std::deque<GroundAtom> atoms;              // in first-derived order
  absl::flat_hash_map<Tuple, size_t> index;  // args -> position in `atoms`

  // How many atoms the predicate had when the current derivation pass started,
  // and when the previous one did. Atoms are only ever appended, so the ones in
  // between are what the previous pass derived.
  size_t size_before_pass = 0;
  size_t size_before_prev_pass = 0;

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

  // Starts a derivation pass, so that what the last one derived becomes the
  // delta the next round of joins reads.
  void begin_pass() {
    for (auto& [key, data] : preds) {
      data.size_before_prev_pass = data.size_before_pass;
      data.size_before_pass = data.atoms.size();
    }
  }
};

// --------------------------------------------------------------------------
// Evaluating and matching terms
//
// Evaluating turns a term into its value under a binding, e.g. 'X + 1' into
// 2 under {X: 1}. Matching is the other direction: it compares a term
// against a stored value and binds whatever variables that takes, e.g.
// matching 'f(X, b)' against a stored f(1, b) binds X to 1.
// --------------------------------------------------------------------------

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
      // TODO: wraps on this cast and on overflow below; TODO.md tracks moving
      // to unlimited precision integers.
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
      const Variable& variable = static_cast<const Variable&>(term);
      const Value* value = binding.find(variable);
      if (value == nullptr) {
        return absl::InvalidArgumentError(absl::StrCat(
            "variable '", variable.name, "' is not bound by the rule body"));
      }
      return *value;
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

// Tries to match one argument term against one stored value, extending
// `binding` with whatever variables the match binds and recording each of them
// in `trail`. Returns false on a mismatch, in which case `binding` still holds
// whatever the part that did match bound, so the caller must let the trail undo
// it before trying anything else.
//
// A function term matches value by value against a stored function of the
// same name and arity, e.g. 'f(X, b)' matches a stored f(1, b) and binds
// X to 1.
absl::StatusOr<bool> match_term(const Term& arg, const Value& value,
                                Binding& binding, BindingTrail& trail) {
  if (arg.kind == Term::VariableKind) {
    size_t slot = binding.slot_of(static_cast<const Variable&>(arg));
    const Value* bound = binding.at(slot);
    if (bound != nullptr) return *bound == value;
    binding.set(slot, value);
    trail.record(slot);
    return true;
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
        ASSIGN_OR_RETURN(bool ok, match_term(*(*atom.args)[k], value.args[k],
                                             binding, trail));
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

// Whether `term` can match anything at all under `binding`: the question
// match_term answers, asked without a value to compare against. A '_' matches
// whatever sits opposite it, so it always can, but 'X / 0' has no value at all,
// so it never can.
absl::StatusOr<bool> can_match(const Term& term, const Binding& binding) {
  if (term.kind == Term::AnonymousVariableKind) return true;
  if (term.kind == Term::AtomKind) {
    const Atom& atom = static_cast<const Atom&>(term);
    if (atom.args != nullptr) {
      for (const auto& arg : *atom.args) {
        ASSIGN_OR_RETURN(bool ok, can_match(*arg, binding));
        if (!ok) return false;
      }
      return true;
    }
  }
  ASSIGN_OR_RETURN(std::optional<Value> value, eval_term(term, binding));
  return value.has_value();
}

// Whether every argument of `literal` can match, e.g. false for the 'r(4 / 0)'
// that 'r(4 / X)' becomes under {X: 0}. Such a literal has no ground instance
// at all, which is different from having one that no stored atom matches.
absl::StatusOr<bool> args_can_match(const ClassicalLiteral& literal,
                                    const Binding& binding) {
  if (literal.args == nullptr) return true;
  for (const auto& arg : *literal.args) {
    ASSIGN_OR_RETURN(bool ok, can_match(*arg, binding));
    if (!ok) return false;
  }
  return true;
}

// Tries to match `literal`'s arguments against a stored tuple, extending
// `binding` with any variables the match binds and recording them in `trail`.
// `tuple` has one value per argument: the store groups atoms by name and arity,
// so every tuple stored under the literal's predicate has the literal's arity.
absl::StatusOr<bool> match_args(const ClassicalLiteral& literal,
                                const Tuple& tuple, Binding& binding,
                                BindingTrail& trail) {
  size_t n = literal.args ? literal.args->size() : 0;
  for (size_t k = 0; k < n; ++k) {
    ASSIGN_OR_RETURN(bool ok,
                     match_term(*(*literal.args)[k], tuple[k], binding, trail));
    if (!ok) return false;
  }
  return true;
}

// --------------------------------------------------------------------------
// Rules as the grounder sees them
//
// A statement's body is sorted into the kinds of item grounding handles
// differently, and every variable the rule mentions is given a slot number,
// so that grounding it can work with slots and vectors rather than names.
// --------------------------------------------------------------------------

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
  // Numbers every variable the rule mentions, head and body alike, including
  // the ones inside its aggregates. A rule's bindings all index by these.
  VarSlots slots;
};

// Numbers every variable in the rule, so that grounding it can look variables
// up by slot. An aggregate's variables are numbered here too, and by name like
// all the others, so an element condition mentioning the enclosing rule's X
// reads the value the rule bound.
VarSlots make_var_slots(const RuleView& rule) {
  VarSlots slots;
  absl::flat_hash_map<std::string_view, size_t> by_name;
  // Takes a literal or an aggregate, whichever collect::for_each_variable
  // overload fits the node.
  auto add_from = [&](const auto& node) {
    collect::for_each_variable(
        node, [&](const Variable& var) { slots.add(var, by_name); });
  };

  if (rule.head != nullptr) add_from(*rule.head);
  for (const ClassicalLiteral* literal : rule.parts.positive)
    add_from(*literal);
  for (const ClassicalLiteral* literal : rule.parts.negative)
    add_from(*literal);
  for (const NafLiteral* naf : rule.parts.comparisons) add_from(*naf->literal);
  for (const Aggregate* agg : rule.parts.aggregates) add_from(*agg);
  return slots;
}

// Checks that the program is a normalized program the grounder can handle
// and splits each statement into a RuleView.
absl::StatusOr<std::vector<RuleView>> make_rule_views(const Program& prog) {
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
    rule.slots = make_var_slots(rule);
    rules.push_back(std::move(rule));
  }
  return rules;
}

// Buckets rules by component[id_of[head predicate]], with the constraints,
// which have no head and so no component, in one extra bucket at the end.
// `rules` owns the RuleViews and must outlive the buckets.
std::vector<std::vector<const RuleView*>> bucket_rule_views(
    const PredGraph& graph, const std::vector<int>& component,
    const std::vector<RuleView>& rules) {
  // `component` is empty when the program mentions no predicate at all, e.g.
  // ":- 1 < 2."
  int num_components =
      component.empty()
          ? 0
          : *std::max_element(component.begin(), component.end()) + 1;
  std::vector<std::vector<const RuleView*>> bucket(num_components + 1);
  std::vector<const RuleView*>& constraints = bucket.back();
  for (const RuleView& rv : rules) {
    if (rv.head == nullptr) {
      constraints.push_back(&rv);
      continue;
    }
    bucket[component[graph.id_of.at(pred_key(*rv.head))]].push_back(&rv);
  }
  return bucket;
}

// --------------------------------------------------------------------------
// Satisfying a body: joins over the store
//
// find_instances() hands its caller every way to satisfy a body with the
// atoms derived so far. It matches the positive literals against the store
// one at a time, backtracking on a mismatch, then keeps the results whose
// assignments, aggregates, and comparisons work out.
// --------------------------------------------------------------------------

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
  const Variable* variable;
  const Term* value;
};

// The variable on one side of a comparison that still needs a value, e.g. the
// Y of 'Y = X + 1'. A side that is not a plain variable, is not an equality,
// or holds an already-bound variable gives null.
const Variable* unbound_var(const Term* term, BinopType op,
                            const Binding& binding) {
  if (term == nullptr || op != BinopType::kEQUAL ||
      term->kind != Term::VariableKind) {
    return nullptr;
  }
  const Variable& variable = static_cast<const Variable&>(*term);
  if (binding.contains(variable)) return nullptr;
  return &variable;
}

// Reads `naf` as an assignment, e.g. 'Y = X + 1' assigns to Y. Only an
// un-negated '=' with a still-unbound variable on one side assigns; anything
// else, including 'Y = X + 1' once Y is bound, is a test and gets nullopt.
std::optional<Assignment> assignment_of(const NafLiteral& naf,
                                        const Binding& binding) {
  const auto& builtin = static_cast<const BuiltinAtom&>(*naf.literal);
  if (naf.naf) return std::nullopt;
  const Variable* left = unbound_var(builtin.left.get(), builtin.op, binding);
  if (left != nullptr) {
    return Assignment{.variable = left, .value = builtin.right.get()};
  }
  const Variable* right = unbound_var(builtin.right.get(), builtin.op, binding);
  if (right != nullptr) {
    return Assignment{.variable = right, .value = builtin.left.get()};
  }
  return std::nullopt;
}

// Whether every variable in `term` already has a value, i.e. whether the term
// can be evaluated at all.
bool is_bound(const Term& term, const Binding& binding) {
  bool bound = true;
  collect::for_each_variable(term, [&](const Variable& var) {
    if (!binding.contains(var)) bound = false;
  });
  return bound;
}

// Extends `binding` with the variables the body's assignments bind, e.g. 'Y =
// X + 1' adds {Y: 2} to {X: 1}. An assignment whose value still holds an
// unbound variable is left for a later call.
//
// Returns false when an assignment's value is ill-formed, e.g. the 'Y = 1 / 0'
// that 'Y = 1 / X' becomes under {X: 0}: the binding builds no rule instance.
absl::StatusOr<bool> bind_assignments(const BodyParts& parts, Binding& binding,
                                      BindingTrail& trail) {
  // One assignment can bind a variable another needs, e.g. 'Y = X + 1, Z = Y +
  // 1', so passes repeat until one binds nothing new. A pass skips the
  // assignments already made: their variables are bound.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const NafLiteral* item : parts.comparisons) {
      std::optional<Assignment> assignment = assignment_of(*item, binding);
      if (!assignment.has_value()) continue;
      if (!is_bound(*assignment->value, binding)) continue;
      ASSIGN_OR_RETURN(std::optional<Value> value,
                       eval_term(*assignment->value, binding));
      if (!value.has_value()) return false;
      size_t slot = binding.slot_of(*assignment->variable);
      binding.set(slot, *std::move(value));
      trail.record(slot);
      changed = true;
    }
  }
  return true;
}

// Returns whether every comparison in the body holds under `binding`, e.g.
// whether 'X < 2' holds given the binding {X: 1}. A comparison with an
// ill-formed side, e.g. 'X < 1 / 0', counts as not holding: the binding builds
// no rule instance either way.
//
// An assignment holds by construction: its variable holds exactly what the
// other side evaluates to.
absl::StatusOr<bool> comparisons_hold(const BodyParts& parts,
                                      const Binding& binding) {
  for (const NafLiteral* item : parts.comparisons) {
    const auto& builtin = static_cast<const BuiltinAtom&>(*item->literal);
    ASSIGN_OR_RETURN(std::optional<Value> left,
                     eval_term(*builtin.left, binding));
    ASSIGN_OR_RETURN(std::optional<Value> right,
                     eval_term(*builtin.right, binding));
    if (!left.has_value() || !right.has_value()) return false;
    bool holds = builtin_holds(builtin.op, *left, *right);
    if (item->naf) holds = !holds;
    if (!holds) return false;
  }
  return true;
}

// The stretch of one predicate's atoms a join step reads, as positions into
// PredData::atoms.
struct AtomRange {
  size_t begin = 0;
  size_t end = 0;
};

// Which atoms a join step reads at the positive literal in `position`. Without
// a delta position it reads all of them. With one it reads what the previous
// pass derived at that literal and what existed before this pass at the others,
// so it skips the instances an earlier pass already found.
AtomRange scan_range(const PredData& data, std::optional<size_t> delta_position,
                     size_t position) {
  if (!delta_position.has_value()) return {0, data.atoms.size()};
  if (position == *delta_position) {
    return {data.size_before_prev_pass, data.size_before_pass};
  }
  return {0, data.size_before_pass};
}

// The order a join visits a body's positive literals in. The delta literal goes
// first: it reads only what the previous pass derived, which is usually a small
// fraction of the store, so starting there keeps the partial instances the join
// carries around few. Left to right, the join would instead start by building
// one partial instance per atom of the first literal and only then discover
// that the delta has nothing to join them with.
std::vector<size_t> join_order(size_t count,
                               std::optional<size_t> delta_position) {
  std::vector<size_t> order;
  order.reserve(count);
  if (delta_position.has_value()) order.push_back(*delta_position);
  for (size_t k = 0; k < count; ++k) {
    if (k != delta_position) order.push_back(k);
  }
  return order;
}

// Whether `term` holds a '_' anywhere, e.g. the 'f(_)' of 'p(f(_))'. Such a
// term has no value of its own: it takes whatever the atom holds there.
bool holds_anonymous_variable(const Term& term) {
  switch (term.kind) {
    case Term::AnonymousVariableKind:
      return true;
    case Term::AtomKind: {
      const Atom& atom = static_cast<const Atom&>(term);
      if (atom.args == nullptr) return false;
      for (const auto& arg : *atom.args) {
        if (holds_anonymous_variable(*arg)) return true;
      }
      return false;
    }
    case Term::NegatedTermKind:
      return holds_anonymous_variable(
          *static_cast<const NegatedTerm&>(term).term);
    case Term::TermOperationKind: {
      const TermOperation& operation = static_cast<const TermOperation&>(term);
      return holds_anonymous_variable(*operation.left) ||
             holds_anonymous_variable(*operation.right);
    }
    case Term::NumberKind:
    case Term::StringKind:
    case Term::VariableKind:
      return false;
  }
  return false;
}

// Whether `arg` has a value of its own under `binding`, e.g. the 'Y' of
// 'edge(Y, Z)' once Y is bound. An unbound variable or a '_' takes whatever
// the atom holds in that position instead, so it has none.
bool arg_is_ground(const Term& arg, const Binding& binding) {
  return is_bound(arg, binding) && !holds_anonymous_variable(arg);
}

// Whether every argument of `literal` has a value under `binding`, e.g. true
// for the 'r(X, 2)' of a binding holding X and false for 'r(X, _)'. Such a
// literal names one atom, which the store can be looked up for; the others
// stand for a set of atoms and have to be matched against it.
bool args_are_ground(const ClassicalLiteral& literal, const Binding& binding) {
  if (literal.args == nullptr) return true;
  for (const auto& arg : *literal.args) {
    if (!arg_is_ground(*arg, binding)) return false;
  }
  return true;
}

// The argument positions of `literal` whose terms have a value to look up, e.g.
// {0} for the 'edge(Y, Z)' of "reachable(X, Z) :- reachable(X, Y), edge(Y, Z)."
// once matching the first literal has bound Y.
std::vector<size_t> probeable_positions(const ClassicalLiteral& literal,
                                        const Binding& binding) {
  std::vector<size_t> positions;
  if (literal.args == nullptr) return positions;
  for (size_t k = 0; k < literal.args->size(); ++k) {
    if (arg_is_ground(*(*literal.args)[k], binding)) positions.push_back(k);
  }
  return positions;
}

// Groups `range`'s atoms by the values they hold at `positions`, e.g. the
// edge/2 atoms by their first argument, so a join step can find the atoms
// matching an instance. An empty `positions` puts every atom in one bucket,
// which is what a literal with nothing bound yet needs.
absl::flat_hash_map<Tuple, std::vector<const GroundAtom*>> index_atoms(
    const PredData& data, const AtomRange& range,
    const std::vector<size_t>& positions) {
  absl::flat_hash_map<Tuple, std::vector<const GroundAtom*>> index;
  for (size_t k = range.begin; k < range.end; ++k) {
    const GroundAtom& atom = data.atoms[k];
    Tuple key;
    key.reserve(positions.size());
    for (size_t position : positions) key.push_back(atom.args[position]);
    index[std::move(key)].push_back(&atom);
  }
  return index;
}

// The values one partial instance needs at `positions`, e.g. {b} for the
// 'edge(Y, Z)' above under {Y: b}. Looking these up in the index_atoms() index
// gives the atoms that can extend the instance.
//
// Returns nullopt when one of the terms is ill-formed, e.g. the 'edge(1 / 0,
// Z)' that 'edge(X / 0, Z)' becomes under {X: 0}: no atom matches it.
absl::StatusOr<std::optional<Tuple>> probe_key(
    const ClassicalLiteral& literal, const std::vector<size_t>& positions,
    const Binding& binding) {
  Tuple key;
  key.reserve(positions.size());
  for (size_t position : positions) {
    ASSIGN_OR_RETURN(std::optional<Value> value,
                     eval_term(*(*literal.args)[position], binding));
    if (!value.has_value()) return std::nullopt;
    key.push_back(*std::move(value));
  }
  return key;
}

// What a completed rule instance is handed to. Returning a non-ok status stops
// the search.
//
// The instance is the search's own live state, so it stays valid only for the
// duration of the call. Every caller uses it and moves on, which is what lets
// the join get away with never copying a binding.
using InstanceFn = absl::FunctionRef<absl::Status(const Instance&)>;

// One step of a join: the positive literal it matches and the store's atoms for
// that predicate, grouped by the argument positions whose values are known by
// the time the step runs.
//
// A step is set up the first time the search reaches it rather than up front,
// because which positions it can probe by depends on what the earlier steps
// have bound. That answer is the same every time the search arrives here: the
// steps run in a fixed order and each one binds exactly the variables its
// literal mentions, whatever values it matched. So the index is built once and
// reused for every partial instance that reaches this step.
struct JoinStep {
  const ClassicalLiteral* literal = nullptr;
  std::vector<size_t> positions;
  absl::flat_hash_map<Tuple, std::vector<const GroundAtom*>> index;
  bool ready = false;
  // Set when the predicate has no atoms at all, which means no instance can get
  // past this step.
  bool dead = false;
};

// One join in progress: the body it is satisfying, the store it reads, and the
// steps it runs in order.
struct Join {
  const BodyParts& parts;
  const Store& store;
  std::vector<size_t> order;  // positive literal positions, delta first
  std::optional<size_t> delta_position;
  std::vector<JoinStep> steps;  // one per entry of `order`, in that order
};

// find_instances() and bind_agg_outputs() call each other: grounding an
// aggregate's elements needs find_instances() for the element conditions.
// Aggregates cannot nest, so the recursion stops one level down.
absl::StatusOr<std::vector<Instance>> bind_agg_outputs(
    const BodyParts& parts, const Store& store,
    std::vector<Instance> instances);

absl::Status extend(Join& join, size_t depth, Instance& instance,
                    const InstanceFn& emit);

// Hands `emit` the instances that survive the parts of the body that are
// decided once every positive literal has matched: the assignments, the
// aggregates, and the comparisons.
absl::Status finish(const Join& join, Instance& instance,
                    const InstanceFn& emit) {
  BindingTrail trail(instance.binding);
  ASSIGN_OR_RETURN(bool ok,
                   bind_assignments(join.parts, instance.binding, trail));
  if (!ok) return absl::OkStatus();

  if (join.parts.aggregates.empty()) {
    // Every variable the body binds has a value by now, so all the comparisons
    // are decidable.
    ASSIGN_OR_RETURN(bool holds,
                     comparisons_hold(join.parts, instance.binding));
    if (!holds) return absl::OkStatus();
    return emit(instance);
  }

  // An aggregate that binds a variable to its value, e.g. '#count{X : p(X)} =
  // S', splits the instance into one per value the aggregate can take, so this
  // is the one place the search works on copies.
  ASSIGN_OR_RETURN(std::vector<Instance> expanded,
                   bind_agg_outputs(join.parts, join.store,
                                    std::vector<Instance>{instance}));
  for (const Instance& next : expanded) {
    ASSIGN_OR_RETURN(bool holds, comparisons_hold(join.parts, next.binding));
    if (holds) RETURN_IF_ERROR(emit(next));
  }
  return absl::OkStatus();
}

// Sets up the step at `depth` against the variables `binding` has bound by the
// time the search first reaches it.
void prepare_step(Join& join, size_t depth, const Binding& binding) {
  JoinStep& step = join.steps[depth];
  step.ready = true;
  size_t position = join.order[depth];
  const ClassicalLiteral& literal = *join.parts.positive[position];
  const PredData* data = join.store.find(pred_key(literal));
  if (data == nullptr) {
    step.dead = true;
    return;
  }
  step.literal = &literal;
  step.positions = probeable_positions(literal, binding);
  step.index = index_atoms(
      *data, scan_range(*data, join.delta_position, position), step.positions);
}

// Extends `instance` with every stored atom the step at `depth` can match, and
// recurses into the step after it.
absl::Status extend(Join& join, size_t depth, Instance& instance,
                    const InstanceFn& emit) {
  if (depth == join.order.size()) return finish(join, instance, emit);

  if (!join.steps[depth].ready) prepare_step(join, depth, instance.binding);
  const JoinStep& step = join.steps[depth];
  if (step.dead) return absl::OkStatus();

  ASSIGN_OR_RETURN(std::optional<Tuple> key,
                   probe_key(*step.literal, step.positions, instance.binding));
  if (!key.has_value()) return absl::OkStatus();
  auto it = step.index.find(*key);
  if (it == step.index.end()) return absl::OkStatus();

  // The candidates already agree on the probed positions, so match_args is here
  // to bind the variables in the positions still open.
  for (const GroundAtom* atom : it->second) {
    BindingTrail trail(instance.binding);
    ASSIGN_OR_RETURN(bool ok, match_args(*step.literal, atom->args,
                                         instance.binding, trail));
    if (!ok) continue;
    instance.matched.push_back(atom->id);
    absl::Status status = extend(join, depth + 1, instance, emit);
    instance.matched.pop_back();
    RETURN_IF_ERROR(status);
  }
  return absl::OkStatus();
}

// Hands `emit` every way to satisfy the body with the atoms currently in the
// store. Works through the positive literals one at a time, matching each
// against the store and backtracking, then keeps the instances whose
// assignments, aggregates, and comparisons work out. 'not' literals never
// filter here; the caller decides what to do with them.
//
// `seed` is the binding to start from: an empty one for a rule, or, for an
// aggregate element's condition, the enclosing rule instance's binding, so that
// the condition sees the variables the rule already bound.
//
// `delta_position` makes the join semi-naive: see scan_range. Only
// derive_atoms() passes one; every other caller wants every instance the store
// supports.
absl::Status find_instances(
    const BodyParts& parts, const Store& store, Binding seed,
    const InstanceFn& emit,
    std::optional<size_t> delta_position = std::nullopt) {
  Join join{.parts = parts,
            .store = store,
            .order = join_order(parts.positive.size(), delta_position),
            .delta_position = delta_position};
  join.steps.resize(join.order.size());
  Instance instance{std::move(seed), {}};
  return extend(join, 0, instance, emit);
}

// The stored atoms `literal` stands for under `binding`: the one atom it names,
// e.g. r(1, 2) for 'r(X, 2)' under {X: 1}, or, when an argument has no value of
// its own, every stored atom it matches, e.g. both r(1, 2) and r(3, 2) for
// 'r(_, 2)'. A literal naming an atom the store does not hold comes back with
// none.
absl::StatusOr<std::vector<aspif::Atom>> matching_atoms(
    const ClassicalLiteral& literal, const Binding& binding,
    const Store& store) {
  std::vector<aspif::Atom> matched;
  const PredData* data = store.find(pred_key(literal));
  if (data == nullptr) return matched;
  if (args_are_ground(literal, binding)) {
    ASSIGN_OR_RETURN(std::optional<Tuple> tuple,
                     eval_terms(literal.args, binding));
    if (!tuple.has_value()) return matched;
    const GroundAtom* atom = data->find(*tuple);
    if (atom != nullptr) matched.push_back(atom->id);
    return matched;
  }
  // Matching binds the open arguments' variables, e.g. the X of 'r(X, _)' when
  // X has no value yet, which this literal's own scope has no use for, so it
  // happens on a scratch copy.
  Binding scratch = binding;
  for (const GroundAtom& atom : data->atoms) {
    BindingTrail trail(scratch);
    ASSIGN_OR_RETURN(bool ok, match_args(literal, atom.args, scratch, trail));
    if (ok) matched.push_back(atom.id);
  }
  return matched;
}

// Negates each 'not p(...)' literal under `binding` into the literals the
// emitted rule body needs. An atom the store never derived can never be true,
// so it is dropped as trivially satisfied instead of being negated.
//
// A 'not' over a '_' rules out a set of atoms at once: 'not r(_, 2)' holds only
// when no stored r with 2 in its second argument is true, so all of them are
// negated.
//
// Returns nullopt if one of the literals cannot match, e.g. 'not p(X / 0)':
// that makes the whole rule instance nonexistent, not just this literal.
absl::StatusOr<std::optional<std::vector<aspif::Lit>>> negative_lits(
    const std::vector<const ClassicalLiteral*>& negative,
    const Binding& binding, const Store& store) {
  std::vector<aspif::Lit> lits;
  for (const ClassicalLiteral* literal : negative) {
    ASSIGN_OR_RETURN(bool can_match, args_can_match(*literal, binding));
    if (!can_match) return std::nullopt;
    ASSIGN_OR_RETURN(std::vector<aspif::Atom> matched,
                     matching_atoms(*literal, binding, store));
    for (aspif::Atom atom : matched) lits.push_back(-atom);
  }
  return lits;
}

// --------------------------------------------------------------------------
// Aggregates
//
// ASPIF has no aggregate statement, so a '#count{...} >= 2' becomes a fresh
// atom defined by a weight-body rule over one auxiliary atom per tuple the
// elements can produce. An aggregate whose value binds a variable, e.g.
// '#count{...} = S', is handled before that, by splitting the rule instance
// into one per value the aggregate can take.
// --------------------------------------------------------------------------

// One distinct tuple an aggregate's elements can produce, e.g. the [1] that
// both elements of '#count{ X : p(X) ; X : r(X) }' produce once p(1) and r(1)
// are derived.
struct AggTuple {
  Tuple tuple;
  int64_t weight;  // what the tuple adds to the aggregate's value
  // One body per grounding that puts the tuple in the set. Empty when every
  // grounding has an ill-formed 'not' literal.
  std::vector<std::vector<aspif::Lit>> supports;
};

// Grounds an aggregate's elements against the store: the distinct tuples they
// can produce, in first-produced order. ASP-Core-2 aggregates range over a
// *set* of tuples, so two elements, or two groundings of one element, that
// produce equal tuples give one AggTuple with two supports.
//
// Emits nothing, so a caller that only needs the values the aggregate can
// take (see possible_values) adds no atoms or rules to the program.
absl::StatusOr<std::vector<AggTuple>> collect_agg_tuples(
    const Aggregate& agg, const Binding& outer_binding, const Store& store) {
  std::vector<AggTuple> tuples;
  if (agg.elements == nullptr) return tuples;

  absl::flat_hash_map<Tuple, size_t> seen;  // tuple -> index into `tuples`
  for (const auto& element_ptr : *agg.elements) {
    const AggregateElement& element = *element_ptr;
    BodyParts parts = split_naf_literals(element.literals);
    RETURN_IF_ERROR(find_instances(
        parts, store, outer_binding,
        [&](const Instance& instance) -> absl::Status {
          // An element whose terms are ill-formed under this local binding
          // contributes no tuple to the set.
          ASSIGN_OR_RETURN(std::optional<Tuple> maybe_tuple,
                           eval_terms(element.terms, instance.binding));
          if (!maybe_tuple.has_value()) return absl::OkStatus();
          Tuple tuple = *std::move(maybe_tuple);
          int64_t weight = 1;
          if (agg.function == AggregateFunctionType::kAGGREGATE_SUM) {
            // #sum adds up the tuples whose first term is an integer and
            // ignores the others, e.g. '#sum{ 1 : p; a : q }' is just 1. A
            // tuple that adds nothing needs no literal in the weight body at
            // all. (#count does count such a tuple, which is why this only
            // applies to #sum.)
            if (tuple.empty() || tuple[0].kind != Value::kNumber) {
              return absl::OkStatus();
            }
            weight = tuple[0].number;
          }

          auto [it, inserted] = seen.try_emplace(tuple, tuples.size());
          if (inserted) {
            tuples.push_back(
                AggTuple{.tuple = std::move(tuple), .weight = weight});
          }

          ASSIGN_OR_RETURN(
              std::optional<std::vector<aspif::Lit>> neg,
              negative_lits(parts.negative, instance.binding, store));
          if (!neg.has_value()) return absl::OkStatus();
          std::vector<aspif::Lit> support = instance.matched;
          support.insert(support.end(), neg->begin(), neg->end());
          tuples[it->second].supports.push_back(std::move(support));
          return absl::OkStatus();
        }));
  }
  return tuples;
}

// A cap on how many values an aggregate may bind a variable to: each value
// costs one ground instance of the rule.
constexpr size_t kMaxAggregateValues = 4096;

// The values an aggregate can take, in ascending order. Which tuples are in
// the set is up to the solver, so the value is the sum of the weights of some
// subset of them, e.g. {0, 1, 2} for a #count over two tuples and {0, 3, 5, 8}
// for a #sum over tuples weighing 3 and 5.
//
// A value no answer set reaches, e.g. the 0 of a #count over a tuple backed
// by a fact, is harmless: its rule instance carries the literals that check
// for that value, which no answer set satisfies.
absl::StatusOr<std::vector<int64_t>> possible_values(
    const std::vector<AggTuple>& tuples) {
  absl::btree_set<int64_t> reachable = {0};
  for (const AggTuple& tuple : tuples) {
    absl::btree_set<int64_t> extended = reachable;
    for (int64_t sum : reachable) extended.insert(sum + tuple.weight);
    if (extended.size() > kMaxAggregateValues) {
      return absl::ResourceExhaustedError(absl::StrCat(
          "an aggregate whose value binds a variable can take more than ",
          kMaxAggregateValues, " different values"));
    }
    reachable = std::move(extended);
  }
  return std::vector<int64_t>(reachable.begin(), reachable.end());
}

// The variables an aggregate binds to its own value, e.g. the S of
// '#sum{...} = S'. Either side can hold one (see unbound_var). A 'not'
// aggregate binds nothing: it only says the value differs from the bound.
std::vector<size_t> agg_output_slots(const Aggregate& agg,
                                     const Binding& binding) {
  std::vector<size_t> slots;
  if (agg.naf) return slots;
  const Variable* lower = unbound_var(agg.lb_term.get(), agg.lb_op, binding);
  if (lower != nullptr) slots.push_back(binding.slot_of(*lower));
  const Variable* upper = unbound_var(agg.ub_term.get(), agg.ub_op, binding);
  if (upper != nullptr) slots.push_back(binding.slot_of(*upper));
  return slots;
}

// The slots of every variable occurring anywhere in an aggregate: in its
// bounds, in its element terms, and in its element conditions.
absl::flat_hash_set<size_t> agg_variable_slots(const Aggregate& agg,
                                               const Binding& binding) {
  absl::flat_hash_set<size_t> slots;
  collect::for_each_variable(
      agg, [&](const Variable& var) { slots.insert(binding.slot_of(var)); });
  return slots;
}

// Whether one of `pending` binds a variable `agg` mentions, e.g. the X of
// 'q(C) :- #count{Y : e(X, Y)} = C, X = #sum{Z : n(Z)}.' The #count waits for
// the #sum there: to the #count, an unbound X reads as a local variable.
bool waits_for_another(const Aggregate& agg,
                       const std::vector<const Aggregate*>& pending,
                       const Binding& binding) {
  absl::flat_hash_set<size_t> slots = agg_variable_slots(agg, binding);
  for (const Aggregate* other : pending) {
    if (other == &agg) continue;
    for (size_t slot : agg_output_slots(*other, binding)) {
      if (slots.contains(slot)) return true;
    }
  }
  return false;
}

// Replaces each instance with one per value `agg` can take, binding `outputs`
// to that value, e.g. 'q(S) :- #count{X : p(X)} = S.' with two derivable p
// atoms turns one instance into three, binding S to 0, 1, and 2 in turn.
absl::StatusOr<std::vector<Instance>> expand_over_values(
    const Aggregate& agg, const std::vector<size_t>& outputs,
    const BodyParts& parts, const Store& store,
    const std::vector<Instance>& instances) {
  std::vector<Instance> expanded;
  for (const Instance& instance : instances) {
    ASSIGN_OR_RETURN(std::vector<AggTuple> tuples,
                     collect_agg_tuples(agg, instance.binding, store));
    ASSIGN_OR_RETURN(std::vector<int64_t> values, possible_values(tuples));
    for (int64_t value : values) {
      Instance next = instance;
      for (size_t slot : outputs)
        next.binding.set(slot, Value::make_number(value));
      // The value can complete an assignment, e.g. the 'T = S + 1' of
      // 'q(T) :- #count{X : p(X)} = S, T = S + 1.'
      BindingTrail trail(next.binding);
      ASSIGN_OR_RETURN(bool ok, bind_assignments(parts, next.binding, trail));
      if (!ok) continue;
      trail.keep();
      expanded.push_back(std::move(next));
    }
  }
  return expanded;
}

// Binds the variables the body's aggregates take their values from, splitting
// each instance into one per value.
//
// An expanded instance carries the aggregate with its variable bound, so
// emit_rules grounds it as an ordinary '= value' check. That check holds only
// when the aggregate takes that value, so an answer set satisfies exactly one
// of the instances an aggregate expands into.
absl::StatusOr<std::vector<Instance>> bind_agg_outputs(
    const BodyParts& parts, const Store& store,
    std::vector<Instance> instances) {
  std::vector<const Aggregate*> pending = parts.aggregates;
  while (!pending.empty() && !instances.empty()) {
    std::vector<const Aggregate*> waiting;
    for (const Aggregate* agg : pending) {
      if (instances.empty()) break;
      // Every instance binds the same variables, since they all come out of
      // the same body, so the first one decides which are still unbound.
      // Expanding an aggregate replaces the list, so this reads the current
      // one each time around.
      const Binding& sample = instances.front().binding;
      std::vector<size_t> outputs = agg_output_slots(*agg, sample);
      // An aggregate that binds nothing is a plain check on its value, which
      // emit_rules handles.
      if (outputs.empty()) continue;
      if (waits_for_another(*agg, pending, sample)) {
        waiting.push_back(agg);
        continue;
      }
      ASSIGN_OR_RETURN(instances, expand_over_values(*agg, outputs, parts,
                                                     store, instances));
    }
    // Aggregates left waiting on each other bind their variables in a cycle
    // and can never be ground. verify_safe() rejects such a rule; stopping
    // here leaves the variables unbound, which reports it as well.
    if (waiting.size() == pending.size()) break;
    pending = std::move(waiting);
  }
  return instances;
}

// Grounds an aggregate's elements into the weighted literals its value sums
// over. Each distinct tuple gets a fresh auxiliary atom, supported by one
// plain rule per grounding that produced it. Multiple such rules give the atom
// OR semantics for free, exactly modeling "this tuple is in the set if any
// grounding satisfies it".
absl::StatusOr<std::vector<aspif::WeightedLit>> ground_agg_elements(
    const Aggregate& agg, const Binding& outer_binding, const Store& store,
    aspif::Program& result) {
  ASSIGN_OR_RETURN(std::vector<AggTuple> tuples,
                   collect_agg_tuples(agg, outer_binding, store));
  std::vector<aspif::WeightedLit> weighted;
  weighted.reserve(tuples.size());
  for (const AggTuple& tuple : tuples) {
    aspif::Atom atom = result.new_atom();
    weighted.push_back({.lit = atom, .weight = tuple.weight});
    for (const std::vector<aspif::Lit>& support : tuple.supports) {
      aspif::Rule rule;
      rule.head = {atom};
      rule.body = support;
      result.rules.push_back(std::move(rule));
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
  // In a '#count{...} = S', S holds the value this rule instance checks for:
  // find_instances() split the instance over the values the aggregate can
  // take.
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

// --------------------------------------------------------------------------
// Building the ground program
//
// Two phases. derive_atoms() runs the rules to a fixpoint to find every atom
// that could appear in an answer set and numbers it; emit_rules() then walks
// the rules again and emits one ASPIF rule per instance, in terms of those
// numbers. The rest turns the store into the program's minimize statements,
// output names, and query assumption.
// --------------------------------------------------------------------------

// Whether the body parts derive_atoms() otherwise ignores are well-formed
// under `binding`: the arguments of every 'not' literal and each aggregate's
// bounds. Ill-formed means the rule has no ground instance under this
// binding, so its head atom must not be derived either, e.g. "q(X) :- p(X),
// not r(4 / X)." derives no q(0), because 'not r(4 / 0)' cannot be ground.
absl::StatusOr<bool> ignored_parts_are_well_formed(const BodyParts& parts,
                                                   const Binding& binding) {
  for (const ClassicalLiteral* literal : parts.negative) {
    ASSIGN_OR_RETURN(bool can_match, args_can_match(*literal, binding));
    if (!can_match) return false;
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

// Runs one rule against the store and adds the head atom of every instance it
// finds, setting `changed` if any of them was new. `delta_position` picks the
// positive literal to read the previous pass's atoms from (see
// find_instances), or nullopt to read the whole store.
absl::Status derive_from_rule(const RuleView& rule,
                              std::optional<size_t> delta_position,
                              Store& store, aspif::Program& aspif_prog,
                              bool& changed) {
  return find_instances(
      rule.parts, store, Binding(rule.slots),
      [&](const Instance& instance) -> absl::Status {
        ASSIGN_OR_RETURN(bool well_formed, ignored_parts_are_well_formed(
                                               rule.parts, instance.binding));
        if (!well_formed) return absl::OkStatus();
        ASSIGN_OR_RETURN(std::optional<Tuple> tuple,
                         eval_terms(rule.head->args, instance.binding));
        if (!tuple.has_value()) return absl::OkStatus();
        if (store.insert(pred_key(*rule.head), *std::move(tuple), aspif_prog)) {
          changed = true;
        }
        return absl::OkStatus();
      },
      delta_position);
}

// Fills `store` with every atom that could appear in an answer set, by
// running each rule against the atoms collected so far and repeating until a
// pass adds nothing new. Each new atom gets its ASPIF number from
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
// A rule only sees atoms from passes before the current one, so a rule that
// feeds on its own head, like 'reachable(X, Z) :- reachable(X, Y), edge(Y,
// Z).', extends the reachable chain by one hop per pass. That's why repeating
// passes to a fixpoint is necessary in the first place.
//
// After the first pass the passes are semi-naive: a rule runs once per positive
// literal, reading only the atoms the previous pass derived at that literal.
// Every instance this skips is built entirely from atoms an earlier pass
// already had, so an earlier pass already found it.
//
// A rule with no positive literals therefore fires only in the first pass,
// which is enough because nothing it reads can still change: it reaches the
// store only through an aggregate binding a variable to its value, and those
// predicates sit in an earlier component, as noted above.
absl::Status derive_atoms(const std::vector<const RuleView*>& rules,
                          Store& store, aspif::Program& aspif_prog) {
  // The first pass reads the whole store. It is the only one that fires the
  // rules with no positive literals, and it derives the delta the passes below
  // start from.
  bool changed = false;
  for (const RuleView* rule : rules) {
    if (rule->head == nullptr) continue;
    RETURN_IF_ERROR(
        derive_from_rule(*rule, std::nullopt, store, aspif_prog, changed));
  }

  do {
    changed = false;
    store.begin_pass();
    for (const RuleView* rule : rules) {
      if (rule->head == nullptr) continue;
      for (size_t position = 0; position < rule->parts.positive.size();
           ++position) {
        RETURN_IF_ERROR(
            derive_from_rule(*rule, position, store, aspif_prog, changed));
      }
    }
  } while (changed);
  return absl::OkStatus();
}

// Emits one ASPIF rule per rule instance. A 'not q' whose atom q was never
// derived by derive_atoms() can never be true, so the literal is dropped as
// satisfied; otherwise it stays in the rule body, negated.
absl::Status emit_rules(const std::vector<const RuleView*>& rules,
                        const Store& store, aspif::Program& result) {
  for (const RuleView* rule_ptr : rules) {
    const RuleView& rule = *rule_ptr;
    RETURN_IF_ERROR(find_instances(
        rule.parts, store, Binding(rule.slots),
        [&](const Instance& instance) -> absl::Status {
          // The head is looked up last, once every reason to drop the instance
          // has been ruled out: derive_atoms() dropped the same instances, so a
          // head atom looked up for one of them would be missing from the
          // store.
          aspif::Rule aspif_rule;
          aspif_rule.body = instance.matched;
          ASSIGN_OR_RETURN(
              std::optional<std::vector<aspif::Lit>> neg,
              negative_lits(rule.parts.negative, instance.binding, store));
          if (!neg.has_value()) return absl::OkStatus();
          aspif_rule.body.insert(aspif_rule.body.end(), neg->begin(),
                                 neg->end());

          for (const Aggregate* aggregate : rule.parts.aggregates) {
            ASSIGN_OR_RETURN(
                std::optional<std::vector<aspif::Lit>> extra,
                ground_aggregate(*aggregate, instance.binding, store, result));
            if (!extra.has_value()) return absl::OkStatus();
            aspif_rule.body.insert(aspif_rule.body.end(), extra->begin(),
                                   extra->end());
          }

          if (rule.head != nullptr) {
            ASSIGN_OR_RETURN(std::optional<Tuple> tuple,
                             eval_terms(rule.head->args, instance.binding));
            // An ill-formed head, e.g. the 'q(X / 0)' of "q(X / 0) :- p(X).",
            // means this instance has no ground rule. derive_atoms() skipped it
            // for the same reason, so nothing is missing from the store.
            if (!tuple.has_value()) return absl::OkStatus();
            // derive_atoms() added every derivable head atom, so a miss means
            // the program never passed verify_safe().
            const PredData* data = store.find(pred_key(*rule.head));
            const GroundAtom* head =
                data == nullptr ? nullptr : data->find(*tuple);
            if (head == nullptr) {
              return absl::InternalError(
                  absl::StrCat("grounding derived no atom for the head '",
                               printed_atom(rule.head->id, *tuple),
                               "'; was the program checked by verify_safe()?"));
            }
            aspif_rule.head.push_back(head->id);
          }
          result.rules.push_back(std::move(aspif_rule));
          return absl::OkStatus();
        }));
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

// Names every atom of a user-visible predicate so answer sets print
// symbolically. '_' predicates are internal and stay hidden, apart from a
// '_neg_p' standing for a classically negated '-p', which prints as '-p'.
void name_outputs(const Store& store, aspif::Program& result) {
  for (const PredKey& key : store.order) {
    std::string predicate;
    if (key.name.starts_with(kClassicalNegationPrefix)) {
      predicate =
          absl::StrCat("-", key.name.substr(kClassicalNegationPrefix.size()));
    } else if (key.name.starts_with('_')) {
      continue;
    } else {
      predicate = key.name;
    }
    for (const GroundAtom& atom : store.find(key)->atoms) {
      result.outputs.push_back(aspif::Output{
          .name = printed_atom(predicate, atom.args), .condition = {atom.id}});
    }
  }
}

// Turns the program's query into an ASPIF assumption: a literal every answer
// set must satisfy.
//
// A query's variables are existential, so that 'p(X, a)?' asks whether an
// answer set holds p(x, a) for some x. The assumption therefore has to mean "at
// least one of the matching ground atoms is true". A fresh atom with one rule
// per matching atom says exactly that: any one of them derives it.
//
// A query that matches nothing gets that atom with no rule at all, so it can
// never be true and the program has no answer set. That is the right answer:
// nothing satisfies the query.
absl::Status emit_query(const ClassicalLiteral& query, const Store& store,
                        aspif::Program& result) {
  // A query is its own scope, so its variables are numbered on their own rather
  // than sharing a rule's slots, and they start out unbound: the query's atoms
  // are whichever ones the store matches.
  VarSlots slots;
  absl::flat_hash_map<std::string_view, size_t> by_name;
  collect::for_each_variable(
      query, [&](const Variable& var) { slots.add(var, by_name); });
  ASSIGN_OR_RETURN(std::vector<aspif::Atom> matched,
                   matching_atoms(query, Binding(slots), store));
  // One match needs no atom of its own: assuming that atom means the same
  // thing, e.g. for a query with no variables in it at all.
  if (matched.size() == 1) {
    result.assumptions.push_back(matched[0]);
    return absl::OkStatus();
  }
  aspif::Atom holds = result.new_atom();
  for (aspif::Lit lit : matched) {
    aspif::Rule rule;
    rule.head = {holds};
    rule.body = {lit};
    result.rules.push_back(std::move(rule));
  }
  result.assumptions.push_back(holds);
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<aspif::Program> ground(const Program& prog) {
  const PredGraph graph = build_pred_graph(prog);
  const std::vector<int> component =
      strongly_connected_components(graph.pos_succ);
  ASSIGN_OR_RETURN(std::vector<RuleView> rules, make_rule_views(prog));
  std::vector<std::vector<const RuleView*>> rules_by_component =
      bucket_rule_views(graph, component, rules);

  // Two passes: derive every component, then emit every component. A
  // component's positive body literals depend only on earlier components,
  // so deriving in ascending order gets those right. Negation is different:
  // a 'not q' can point at a later component, since negation edges don't
  // constrain component order. So by the time any component is emitted, q's
  // final atom set already exists, and emit_rules can correctly decide
  // whether q is derivable.
  //
  // The last bucket holds the constraints, which derive nothing and so are
  // emitted after every atom exists.
  aspif::Program result;
  Store store;
  for (const auto& component_rules : rules_by_component) {
    RETURN_IF_ERROR(derive_atoms(component_rules, store, result));
  }
  for (const auto& component_rules : rules_by_component) {
    RETURN_IF_ERROR(emit_rules(component_rules, store, result));
  }
  emit_minimize(store, result);
  name_outputs(store, result);
  if (prog.query != nullptr && prog.query->lit != nullptr) {
    RETURN_IF_ERROR(emit_query(*prog.query->lit, store, result));
  }
  return result;
}
