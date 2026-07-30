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
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "collect.h"
#include "graph.h"
#include "macros.h"
#include "normalize.h"
#include "symbols.h"

namespace {

// --------------------------------------------------------------------------
// Ground values and the store of derived atoms
//
// The values a variable can take, the atoms built out of them, and the
// bindings that map a rule's variables to values while it is being ground.
//
// A value is a Sym, the four-byte handle a Symbols table interns it to, so
// everything below carries and compares handles and asks the table whenever it
// needs to know what one stands for.
// --------------------------------------------------------------------------

// The argument values of one ground atom, e.g. {1, abc} for p(1, abc). Most
// predicates are small, so a tuple keeps up to four handles inside itself and
// only reaches for the heap beyond that.
using Tuple = absl::InlinedVector<Sym, 4>;

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
// and r(abc). A variable with no value yet reads as kNoSym.
class Binding {
 public:
  explicit Binding(const VarSlots& slots)
      : slots_(&slots), values_(slots.count, kNoSym) {}

  // The slot `var` reads and writes. Every variable of the rule the binding
  // belongs to has one.
  size_t slot_of(const Variable& var) const { return slots_->of_node.at(&var); }

  // The value of `var`, or kNoSym if it has none yet. A variable from outside
  // this binding's rule, which cannot happen for a well-formed rule, also
  // reads as having none.
  Sym find(const Variable& var) const {
    auto it = slots_->of_node.find(&var);
    if (it == slots_->of_node.end()) return kNoSym;
    return at(it->second);
  }
  bool contains(const Variable& var) const { return find(var) != kNoSym; }

  Sym at(size_t slot) const { return values_[slot]; }
  void set(size_t slot, Sym value) { values_[slot] = value; }
  void clear(size_t slot) { values_[slot] = kNoSym; }

 private:
  const VarSlots* slots_;
  std::vector<Sym> values_;
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

// What one call to Store::insert did: the ASPIF atom the tuple has, and
// whether this call is what gave it one.
struct Inserted {
  aspif::Atom atom;
  bool is_new;
};

// Every ground atom derived so far, grouped by predicate. This is the
// grounder's working state: derive_atoms() fills it by running the rules to
// a fixpoint, and emit_rules() looks atoms up in it to build the ground
// rules.
struct Store {
  absl::flat_hash_map<PredKey, PredData> preds;
  std::vector<PredKey> order;  // predicates in first-seen order
  // Which atoms are facts, indexed by ASPIF atom number. A fact holds in every
  // answer set: derive_from_rule() decides which atoms those are, and
  // emit_rules() states them as facts instead of through their rules.
  std::vector<bool> facts;

  const PredData* find(const PredKey& key) const {
    auto it = preds.find(key);
    return it == preds.end() ? nullptr : &it->second;
  }

  bool is_fact(aspif::Atom atom) const {
    return static_cast<size_t>(atom) < facts.size() && facts[atom];
  }

  // Records that `atom` is a fact, and returns true if that is news. Atom
  // numbers are handed out in order, so `facts` is only ever grown at the end.
  bool mark_fact(aspif::Atom atom) {
    if (static_cast<size_t>(atom) >= facts.size()) facts.resize(atom + 1);
    if (facts[atom]) return false;
    facts[atom] = true;
    return true;
  }

  // Adds the tuple if it has not been seen before, giving it the next ASPIF
  // atom number from `aspif_prog`.
  Inserted insert(const PredKey& key, Tuple tuple, aspif::Program& aspif_prog) {
    auto [pit, new_pred] = preds.try_emplace(key);
    if (new_pred) order.push_back(key);
    PredData& data = pit->second;
    auto [it, is_new] = data.index.try_emplace(tuple, data.atoms.size());
    if (!is_new) {
      return Inserted{.atom = data.atoms[it->second].id, .is_new = false};
    }
    data.atoms.push_back(GroundAtom{std::move(tuple), aspif_prog.new_atom()});
    return Inserted{.atom = data.atoms.back().id, .is_new = true};
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

absl::StatusOr<std::optional<int64_t>> eval_number(const Term& term,
                                                   const Binding& binding,
                                                   Symbols& syms);

// Evaluates a term to its ground value, e.g. 'X' evaluates to 1 under the
// binding {X: 1}. Every variable must already be bound.
//
// Returns nullopt when the binding leaves an arithmetic term undefined, e.g.
// 'X / 0' or 'a + 1'. ASP-Core-2 calls such a binding ill-formed and builds
// no ground instance from it, so callers drop the instance rather than
// failing the whole grounding.
absl::StatusOr<std::optional<Sym>> eval_term(const Term& term,
                                             const Binding& binding,
                                             Symbols& syms) {
  switch (term.kind) {
    case Term::NumberKind:
      // TODO: this cast and the arithmetic below wrap on overflow; move to
      // unlimited precision integers to fix.
      return syms.number(
          static_cast<int64_t>(static_cast<const Number&>(term).value));
    case Term::StringKind:
      return syms.string(static_cast<const String&>(term).value);
    case Term::AtomKind: {
      const Atom& atom = static_cast<const Atom&>(term);
      if (atom.args == nullptr) return syms.constant(atom.name);
      std::vector<Sym> args;
      args.reserve(atom.args->size());
      for (const auto& arg : *atom.args) {
        ASSIGN_OR_RETURN(std::optional<Sym> value,
                         eval_term(*arg, binding, syms));
        if (!value.has_value()) return std::nullopt;
        args.push_back(*value);
      }
      return syms.function(atom.name, std::move(args));
    }
    case Term::VariableKind: {
      const Variable& variable = static_cast<const Variable&>(term);
      const Sym value = binding.find(variable);
      if (value == kNoSym) {
        return absl::InvalidArgumentError(absl::StrCat(
            "variable '", variable.name, "' is not bound by the rule body"));
      }
      return value;
    }
    case Term::AnonymousVariableKind:
      return absl::InvalidArgumentError(
          "'_' cannot appear in a position that must be ground");
    case Term::NegatedTermKind: {
      const NegatedTerm& negated = static_cast<const NegatedTerm&>(term);
      ASSIGN_OR_RETURN(std::optional<int64_t> value,
                       eval_number(*negated.term, binding, syms));
      if (!value.has_value()) return std::nullopt;
      return syms.number(-*value);
    }
    case Term::TermOperationKind: {
      const TermOperation& operation = static_cast<const TermOperation&>(term);
      ASSIGN_OR_RETURN(std::optional<int64_t> left,
                       eval_number(*operation.left, binding, syms));
      ASSIGN_OR_RETURN(std::optional<int64_t> right,
                       eval_number(*operation.right, binding, syms));
      if (!left.has_value() || !right.has_value()) return std::nullopt;
      switch (operation.op) {
        case OperationType::kPLUS:
          return syms.number(*left + *right);
        case OperationType::kMINUS:
          return syms.number(*left - *right);
        case OperationType::kTIMES:
          return syms.number(*left * *right);
        case OperationType::kDIV:
          // Division by zero has no value, so the binding is ill-formed.
          if (*right == 0) return std::nullopt;
          return syms.number(*left / *right);
      }
      return absl::InternalError("unknown arithmetic operator");
    }
  }
  return absl::InternalError("unknown term kind");
}

// Evaluates a term that has to come out as a number, e.g. either side of a
// '+'. Returns nullopt if it doesn't, e.g. for the 'a + 1' that 'X + 1'
// becomes under the binding {X: a}: arithmetic is only defined on integers,
// so that binding is ill-formed and its rule instance does not exist.
absl::StatusOr<std::optional<int64_t>> eval_number(const Term& term,
                                                   const Binding& binding,
                                                   Symbols& syms) {
  ASSIGN_OR_RETURN(std::optional<Sym> value, eval_term(term, binding, syms));
  if (!value.has_value() || !syms.is_number(*value)) return std::nullopt;
  return syms.number_of(*value);
}

// Evaluates a list of terms under `binding`, e.g. the 'X, 2' of 'p(X, 2)'
// evaluates to {1, 2} under the binding {X: 1}. Returns nullopt if any term
// is ill-formed, e.g. the 'X / 0' of 'p(X / 0)'.
absl::StatusOr<std::optional<Tuple>> eval_terms(const Terms& terms,
                                                const Binding& binding,
                                                Symbols& syms) {
  Tuple tuple;
  if (terms != nullptr) {
    tuple.reserve(terms->size());
    for (const auto& term : *terms) {
      ASSIGN_OR_RETURN(std::optional<Sym> value,
                       eval_term(*term, binding, syms));
      if (!value.has_value()) return std::nullopt;
      tuple.push_back(*value);
    }
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
absl::StatusOr<bool> match_term(const Term& arg, Sym value, Binding& binding,
                                BindingTrail& trail, Symbols& syms) {
  if (arg.kind == Term::VariableKind) {
    size_t slot = binding.slot_of(static_cast<const Variable&>(arg));
    Sym bound = binding.at(slot);
    if (bound != kNoSym) return bound == value;
    binding.set(slot, value);
    trail.record(slot);
    return true;
  }
  if (arg.kind == Term::AnonymousVariableKind) return true;
  if (arg.kind == Term::AtomKind) {
    const Atom& atom = static_cast<const Atom&>(arg);
    if (atom.args != nullptr) {
      if (syms.kind_of(value) != SymEntry::kFunction) return false;
      const SymEntry& entry = syms.entry(value);
      if (entry.text != atom.name || entry.args.size() != atom.args->size()) {
        return false;
      }
      for (size_t k = 0; k < entry.args.size(); ++k) {
        ASSIGN_OR_RETURN(bool ok, match_term(*(*atom.args)[k], entry.args[k],
                                             binding, trail, syms));
        if (!ok) return false;
      }
      return true;
    }
  }
  // An ill-formed term, e.g. 'X / 0', has no value at all, so it matches
  // nothing.
  ASSIGN_OR_RETURN(std::optional<Sym> evaluated, eval_term(arg, binding, syms));
  return evaluated.has_value() && *evaluated == value;
}

// Whether `term` can match anything at all under `binding`: the question
// match_term answers, asked without a value to compare against. A '_' matches
// whatever sits opposite it, so it always can, but 'X / 0' has no value at all,
// so it never can.
absl::StatusOr<bool> can_match(const Term& term, const Binding& binding,
                               Symbols& syms) {
  if (term.kind == Term::AnonymousVariableKind) return true;
  if (term.kind == Term::AtomKind) {
    const Atom& atom = static_cast<const Atom&>(term);
    if (atom.args != nullptr) {
      for (const auto& arg : *atom.args) {
        ASSIGN_OR_RETURN(bool ok, can_match(*arg, binding, syms));
        if (!ok) return false;
      }
      return true;
    }
  }
  ASSIGN_OR_RETURN(std::optional<Sym> value, eval_term(term, binding, syms));
  return value.has_value();
}

// Whether every argument of `literal` can match, e.g. false for the 'r(4 / 0)'
// that 'r(4 / X)' becomes under {X: 0}. Such a literal has no ground instance
// at all, which is different from having one that no stored atom matches.
absl::StatusOr<bool> args_can_match(const ClassicalLiteral& literal,
                                    const Binding& binding, Symbols& syms) {
  if (literal.args == nullptr) return true;
  for (const auto& arg : *literal.args) {
    ASSIGN_OR_RETURN(bool ok, can_match(*arg, binding, syms));
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
                                BindingTrail& trail, Symbols& syms) {
  size_t n = literal.args ? literal.args->size() : 0;
  for (size_t k = 0; k < n; ++k) {
    ASSIGN_OR_RETURN(bool ok, match_term(*(*literal.args)[k], tuple[k], binding,
                                         trail, syms));
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

// A normalized rule as the grounder works with it: the literals of its head and
// the body split into its parts, e.g. head = {'reachable(X, Z)'} and
// parts.positive = {'reachable(X, Y)', 'edge(Y, Z)'} for
// "reachable(X, Z) :- reachable(X, Y), edge(Y, Z)."
struct RuleView {
  // One literal for an ordinary rule, several for a disjunctive one, none for a
  // constraint.
  std::vector<const ClassicalLiteral*> head;
  BodyParts parts;
  // Whether one of the rule's aggregates reads a predicate from the rule's own
  // component, which only a disjunctive head can bring about; see
  // mark_aggregates_in_own_component(). Such a rule derives no facts, because
  // settling an aggregate needs a store that is complete for what the aggregate
  // reads, and its own component is by definition still being derived.
  bool aggregate_in_own_component = false;
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
  auto record = [&](const Variable& var) { slots.add(var, by_name); };

  for (const ClassicalLiteral* literal : rule.head)
    collect::for_each_variable(*literal, record);
  for (const ClassicalLiteral* literal : rule.parts.positive)
    collect::for_each_variable(*literal, record);
  for (const ClassicalLiteral* literal : rule.parts.negative)
    collect::for_each_variable(*literal, record);
  for (const NafLiteral* naf : rule.parts.comparisons)
    collect::for_each_variable(*naf->literal, record);
  for (const Aggregate* agg : rule.parts.aggregates)
    collect::for_each_variable(*agg, record);
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
      if (statement->head->kind != Head::DisjunctionKind) {
        return absl::InvalidArgumentError(
            "ground() expects a normalized program, but found a choice head");
      }
      const auto& head = static_cast<const Disjunction&>(*statement->head);
      for (const auto& literal : head.literals) {
        rule.head.push_back(literal.get());
      }
    }
    rule.parts = split_body(statement->body.get());
    rule.slots = make_var_slots(rule);
    rules.push_back(std::move(rule));
  }
  return rules;
}

// The positive predicate dependency graph with the predicates of each
// disjunctive head joined into a cycle, so that they all land in one strongly
// connected component.
//
// Deriving runs one component at a time, in order, and one instance of
// 'p(X) | q(X) :- dom(X).' derives an atom for p and one for q. Were p and q in
// different components, the rule would run in the later one and add atoms to a
// component already finished. The rules reading those atoms would never see
// them.
std::vector<std::vector<int>> derivation_succ(const PredGraph& graph) {
  std::vector<std::vector<int>> succ = graph.pos_succ;
  for (const std::vector<int>& group : graph.head_groups) {
    for (size_t i = 0; i < group.size(); ++i) {
      succ[group[i]].push_back(group[(i + 1) % group.size()]);
    }
  }
  return succ;
}

// Whether any predicate `aggregate` reads sits in `component_id`.
bool reads_component(const Aggregate& aggregate, const PredGraph& graph,
                     const std::vector<int>& component, int component_id) {
  if (aggregate.elements == nullptr) return false;
  bool reads = false;
  for (const auto& element : *aggregate.elements) {
    if (element->literals == nullptr) continue;
    collect::for_each_classical_literal(
        *element->literals, /*negated_context=*/false,
        [&](ClassicalLiteral& literal, bool) {
          const auto it = graph.id_of.find(pred_key(literal));
          if (it != graph.id_of.end() && component[it->second] == component_id) {
            reads = true;
          }
        });
  }
  return reads;
}

// Marks the rules whose aggregates read a predicate from the rule's own
// component, which stops those rules from deriving facts.
//
// verify_safe() enforces the ASP-Core-2 rule that no predicate inside an
// aggregate shares a positive component with the head of the rule holding that
// aggregate. It asks about pos_succ, where 'p | q(1).' leaves p and q/1 apart.
// Joining them for derivation can put an aggregate's predicate in its own rule's
// component after all:
//
//   r.
//   p :- #count{ X : q(X) } <= 0.
//   p | q(1) :- r.
//
// Deriving facts is what that breaks. settled_agg_value() takes an aggregate's
// value from the store as it stands, to decide whether the atom the aggregate
// feeds holds in every answer set. A predicate with no atoms derived yet has no
// tuples, so the count above settles at 0 while q(1) does not exist. That would
// make a fact of p and lose the answer set {r, q(1)}.
//
// The rest of the rule needs nothing. Its head atoms are derived with the
// aggregate ignored, as every rule's are, and the aggregate reaches the solver
// through emit_rules(), which runs once every component has been derived and so
// reads a complete store.
void mark_aggregates_in_own_component(const PredGraph& graph,
                                      const std::vector<int>& component,
                                      std::vector<RuleView>& rules) {
  for (RuleView& rule : rules) {
    if (rule.head.empty()) continue;
    const int head_component =
        component[graph.id_of.at(pred_key(*rule.head[0]))];
    for (const Aggregate* aggregate : rule.parts.aggregates) {
      if (!reads_component(*aggregate, graph, component, head_component)) {
        continue;
      }
      rule.aggregate_in_own_component = true;
      break;
    }
  }
}

// Buckets rules by component[id_of[head predicate]], with the constraints,
// which have no head and so no component, in one extra bucket at the end.
// `rules` owns the RuleViews and must outlive the buckets.
//
// `component` has to come from derivation_succ(), which is what puts every
// predicate of a disjunctive head in the one component this picks by the head's
// first literal.
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
    if (rv.head.empty()) {
      constraints.push_back(&rv);
      continue;
    }
    const int head_component = component[graph.id_of.at(pred_key(*rv.head[0]))];
    for (const ClassicalLiteral* literal : rv.head) {
      DCHECK_EQ(component[graph.id_of.at(pred_key(*literal))], head_component);
    }
    bucket[head_component].push_back(&rv);
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

bool builtin_holds(BinopType op, Sym left, Sym right, const Symbols& syms) {
  int c = syms.compare(left, right);
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
  if (term == nullptr) return nullptr;
  if (op != BinopType::kEQUAL) return nullptr;
  if (term->kind != Term::VariableKind) return nullptr;
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
                                      BindingTrail& trail, Symbols& syms) {
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
      ASSIGN_OR_RETURN(std::optional<Sym> value,
                       eval_term(*assignment->value, binding, syms));
      if (!value.has_value()) return false;
      size_t slot = binding.slot_of(*assignment->variable);
      binding.set(slot, *value);
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
                                      const Binding& binding, Symbols& syms) {
  for (const NafLiteral* item : parts.comparisons) {
    const auto& builtin = static_cast<const BuiltinAtom&>(*item->literal);
    ASSIGN_OR_RETURN(std::optional<Sym> left,
                     eval_term(*builtin.left, binding, syms));
    ASSIGN_OR_RETURN(std::optional<Sym> right,
                     eval_term(*builtin.right, binding, syms));
    if (!left.has_value() || !right.has_value()) return false;
    bool holds = builtin_holds(builtin.op, *left, *right, syms);
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
  if (!delta_position.has_value()) {
    return {.begin = 0, .end = data.atoms.size()};
  }
  if (position == *delta_position) {
    return {.begin = data.size_before_prev_pass, .end = data.size_before_pass};
  }
  return {.begin = 0, .end = data.size_before_pass};
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
    if (!delta_position.has_value() || k != *delta_position) order.push_back(k);
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
    const Binding& binding, Symbols& syms) {
  Tuple key;
  key.reserve(positions.size());
  for (size_t position : positions) {
    ASSIGN_OR_RETURN(std::optional<Sym> value,
                     eval_term(*(*literal.args)[position], binding, syms));
    if (!value.has_value()) return std::nullopt;
    key.push_back(*value);
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
  Symbols& syms;
  std::vector<size_t> order;  // positive literal positions, delta first
  std::optional<size_t> delta_position;
  std::vector<JoinStep> steps;  // one per entry of `order`, in that order
};

// find_instances() and bind_agg_outputs() call each other: grounding an
// aggregate's elements needs find_instances() for the element conditions.
// Aggregates cannot nest, so the recursion stops one level down.
absl::StatusOr<std::vector<Instance>> bind_agg_outputs(
    const BodyParts& parts, const Store& store, Symbols& syms,
    std::vector<Instance> instances);

absl::Status extend(Join& join, size_t depth, Instance& instance,
                    const InstanceFn& emit);

// Hands `emit` the instances that survive the parts of the body that are
// decided once every positive literal has matched: the assignments, the
// aggregates, and the comparisons.
absl::Status finish(const Join& join, Instance& instance,
                    const InstanceFn& emit) {
  BindingTrail trail(instance.binding);
  ASSIGN_OR_RETURN(
      bool ok, bind_assignments(join.parts, instance.binding, trail, join.syms));
  if (!ok) return absl::OkStatus();

  if (join.parts.aggregates.empty()) {
    // Every variable the body binds has a value by now, so all the comparisons
    // are decidable.
    ASSIGN_OR_RETURN(bool holds,
                     comparisons_hold(join.parts, instance.binding, join.syms));
    if (!holds) return absl::OkStatus();
    return emit(instance);
  }

  // An aggregate that binds a variable to its value, e.g. '#count{X : p(X)} =
  // S', splits the instance into one per value the aggregate can take, so this
  // is the one place the search works on copies.
  ASSIGN_OR_RETURN(std::vector<Instance> expanded,
                   bind_agg_outputs(join.parts, join.store, join.syms,
                                    std::vector<Instance>{instance}));
  for (const Instance& next : expanded) {
    ASSIGN_OR_RETURN(bool holds,
                     comparisons_hold(join.parts, next.binding, join.syms));
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

  ASSIGN_OR_RETURN(
      std::optional<Tuple> key,
      probe_key(*step.literal, step.positions, instance.binding, join.syms));
  if (!key.has_value()) return absl::OkStatus();
  auto it = step.index.find(*key);
  if (it == step.index.end()) return absl::OkStatus();

  // The candidates already agree on the probed positions, so match_args is here
  // to bind the variables in the positions still open.
  for (const GroundAtom* atom : it->second) {
    BindingTrail trail(instance.binding);
    ASSIGN_OR_RETURN(bool ok, match_args(*step.literal, atom->args,
                                         instance.binding, trail, join.syms));
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
    const BodyParts& parts, const Store& store, Symbols& syms, Binding seed,
    const InstanceFn& emit,
    std::optional<size_t> delta_position = std::nullopt) {
  Join join{.parts = parts,
            .store = store,
            .syms = syms,
            .order = join_order(parts.positive.size(), delta_position),
            .delta_position = delta_position};
  join.steps.resize(join.order.size());
  Instance instance{.binding = std::move(seed), .matched = {}};
  return extend(join, 0, instance, emit);
}

// The stored atoms `literal` stands for under `binding`: the one atom it names,
// e.g. r(1, 2) for 'r(X, 2)' under {X: 1}, or, when an argument has no value of
// its own, every stored atom it matches, e.g. both r(1, 2) and r(3, 2) for
// 'r(_, 2)'. A literal naming an atom the store does not hold comes back with
// none.
absl::StatusOr<std::vector<aspif::Atom>> matching_atoms(
    const ClassicalLiteral& literal, const Binding& binding, const Store& store,
    Symbols& syms) {
  std::vector<aspif::Atom> matched;
  const PredData* data = store.find(pred_key(literal));
  if (data == nullptr) return matched;
  if (args_are_ground(literal, binding)) {
    ASSIGN_OR_RETURN(std::optional<Tuple> tuple,
                     eval_terms(literal.args, binding, syms));
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
    ASSIGN_OR_RETURN(bool ok,
                     match_args(literal, atom.args, scratch, trail, syms));
    if (ok) matched.push_back(atom.id);
  }
  return matched;
}

// Whether every atom an instance's positive literals matched is a fact, which
// makes the body of that instance true in every answer set.
bool matched_all_facts(const std::vector<aspif::Lit>& matched,
                       const Store& store) {
  for (aspif::Lit lit : matched) {
    if (!store.is_fact(lit)) return false;
  }
  return true;
}

// The positive literals an emitted body still needs. A literal for a fact is
// true in every answer set, so the body holds exactly when it holds without
// that literal, e.g. 'reachable(a, c) :- reachable(a, b), edge(b, c).' with
// edge(b, c) a fact becomes 'reachable(a, c) :- reachable(a, b).'
std::vector<aspif::Lit> without_facts(const std::vector<aspif::Lit>& matched,
                                      const Store& store) {
  std::vector<aspif::Lit> lits;
  for (aspif::Lit lit : matched) {
    if (!store.is_fact(lit)) lits.push_back(lit);
  }
  return lits;
}

// Negates each 'not p(...)' literal under `binding` into the literals the
// emitted rule body needs. An atom the store never derived can never be true,
// so it is dropped as trivially satisfied instead of being negated.
//
// A 'not' over a '_' rules out a set of atoms at once: 'not r(_, 2)' holds only
// when no stored r with 2 in its second argument is true, so all of them are
// negated.
//
// Returns nullopt when the body these literals belong to can hold in no answer
// set, which is either of:
//   - a literal that cannot match, e.g. 'not p(X / 0)': the rule instance does
//     not exist at all, rather than existing without this literal;
//   - a 'not' over a fact, which no answer set can satisfy.
absl::StatusOr<std::optional<std::vector<aspif::Lit>>> negative_lits(
    const std::vector<const ClassicalLiteral*>& negative,
    const Binding& binding, const Store& store, Symbols& syms) {
  std::vector<aspif::Lit> lits;
  for (const ClassicalLiteral* literal : negative) {
    ASSIGN_OR_RETURN(bool matchable, args_can_match(*literal, binding, syms));
    if (!matchable) return std::nullopt;
    ASSIGN_OR_RETURN(std::vector<aspif::Atom> matched,
                     matching_atoms(*literal, binding, store, syms));
    for (aspif::Atom atom : matched) {
      if (store.is_fact(atom)) return std::nullopt;
      lits.push_back(-atom);
    }
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
    const Aggregate& agg, const Binding& outer_binding, const Store& store,
    Symbols& syms) {
  std::vector<AggTuple> tuples;
  if (agg.elements == nullptr) return tuples;

  absl::flat_hash_map<Tuple, size_t> seen;  // tuple -> index into `tuples`
  for (const auto& element_ptr : *agg.elements) {
    const AggregateElement& element = *element_ptr;
    BodyParts parts = split_naf_literals(element.literals);
    RETURN_IF_ERROR(find_instances(
        parts, store, syms, outer_binding,
        [&](const Instance& instance) -> absl::Status {
          // An element whose terms are ill-formed under this local binding
          // contributes no tuple to the set.
          ASSIGN_OR_RETURN(std::optional<Tuple> maybe_tuple,
                           eval_terms(element.terms, instance.binding, syms));
          if (!maybe_tuple.has_value()) return absl::OkStatus();
          Tuple tuple = std::move(*maybe_tuple);
          int64_t weight = 1;
          if (agg.function == AggregateFunctionType::kAGGREGATE_SUM) {
            // #sum adds up the tuples whose first term is an integer and
            // ignores the others, e.g. '#sum{ 1 : p; a : q }' is just 1. A
            // tuple that adds nothing needs no literal in the weight body at
            // all. (#count does count such a tuple, which is why this only
            // applies to #sum.)
            if (tuple.empty() || !syms.is_number(tuple[0])) {
              return absl::OkStatus();
            }
            weight = syms.number_of(tuple[0]);
          }

          auto [it, inserted] = seen.try_emplace(tuple, tuples.size());
          if (inserted) {
            tuples.push_back(
                AggTuple{.tuple = std::move(tuple), .weight = weight});
          }

          ASSIGN_OR_RETURN(
              std::optional<std::vector<aspif::Lit>> neg,
              negative_lits(parts.negative, instance.binding, store, syms));
          if (!neg.has_value()) return absl::OkStatus();
          std::vector<aspif::Lit> support =
              without_facts(instance.matched, store);
          support.insert(support.end(), neg->begin(), neg->end());
          tuples[it->second].supports.push_back(std::move(support));
          return absl::OkStatus();
        }));
  }
  return tuples;
}

// Whether any element condition holds a 'not' over an atom, e.g. the
// '#count{ X : p(X), not q(X) }' of a rule counting the p's without a q.
// A comparison under 'not', like 'not X < 2', doesn't count: grounding decides
// that one itself.
bool elements_use_negation(const Aggregate& agg) {
  if (agg.elements == nullptr) return false;
  for (const auto& element : *agg.elements) {
    if (element->literals == nullptr) continue;
    for (const auto& naf : *element->literals) {
      if (naf->naf && naf->literal->kind != Literal::BuiltinAtomKind) {
        return true;
      }
    }
  }
  return false;
}

// The value the aggregate takes in every answer set, when grounding can work
// that out on its own, e.g. the 2 of '#count{ X : p(X) }' over the facts p(1)
// and p(2). Nullopt means the solver still has a say.
//
// Every tuple has to be settled for the value to be, and a tuple is settled
// when one of its supports came out empty: every literal in that support was a
// fact, so the tuple is in the set whatever the solver decides. One tuple whose
// supports all carry a literal is enough to leave the value open.
//
// Element conditions must be free of 'not' for any of this to hold. safety.cc
// keeps an aggregate's un-negated predicates out of the rule's own component,
// so their atoms and facts are settled before this rule is ever ground, but it
// leaves negated ones alone: 'not q(X)' inside an element can point at a
// predicate that has no atoms yet, which would read here as a support that
// nothing can take away.
std::optional<int64_t> settled_agg_value(const Aggregate& agg,
                                         const std::vector<AggTuple>& tuples) {
  // #min and #max are rejected further on; their value is not this sum.
  if (agg.function != AggregateFunctionType::kAGGREGATE_COUNT &&
      agg.function != AggregateFunctionType::kAGGREGATE_SUM) {
    return std::nullopt;
  }
  if (elements_use_negation(agg)) return std::nullopt;
  int64_t value = 0;
  for (const AggTuple& tuple : tuples) {
    bool settled = false;
    for (const std::vector<aspif::Lit>& support : tuple.supports) {
      if (support.empty()) {
        settled = true;
        break;
      }
    }
    if (!settled) return std::nullopt;
    value += tuple.weight;
  }
  return value;
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
    const Aggregate& agg, const std::vector<AggTuple>& tuples) {
  // An aggregate grounding has settled takes one value and no other, e.g. the
  // 2 that 'q(S) :- #count{ X : p(X) } = S.' binds S to over two p facts.
  std::optional<int64_t> settled = settled_agg_value(agg, tuples);
  if (settled.has_value()) return std::vector<int64_t>{*settled};
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
// bounds, in its element terms, and in its element conditions. Ascending and
// without repeats, so that waits_for_another() can search it and AggCache can
// read one aggregate's slots in the same order every time.
std::vector<size_t> agg_variable_slots(const Aggregate& agg,
                                       const Binding& binding) {
  std::vector<size_t> slots;
  collect::for_each_variable(
      agg, [&](const Variable& var) { slots.push_back(binding.slot_of(var)); });
  std::sort(slots.begin(), slots.end());
  slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
  return slots;
}

// Whether one of `pending` binds a variable `agg` mentions, e.g. the X of
// 'q(C) :- #count{Y : e(X, Y)} = C, X = #sum{Z : n(Z)}.' The #count waits for
// the #sum there: to the #count, an unbound X reads as a local variable.
bool waits_for_another(const Aggregate& agg,
                       const std::vector<const Aggregate*>& pending,
                       const Binding& binding) {
  const std::vector<size_t> slots = agg_variable_slots(agg, binding);
  for (const Aggregate* other : pending) {
    if (other == &agg) continue;
    for (size_t slot : agg_output_slots(*other, binding)) {
      if (std::binary_search(slots.begin(), slots.end(), slot)) return true;
    }
  }
  return false;
}

// Replaces each instance with one per value `agg` can take, binding `outputs`
// to that value, e.g. 'q(S) :- #count{X : p(X)} = S.' with two derivable p
// atoms turns one instance into three, binding S to 0, 1, and 2 in turn.
absl::StatusOr<std::vector<Instance>> expand_over_values(
    const Aggregate& agg, const std::vector<size_t>& outputs,
    const BodyParts& parts, const Store& store, Symbols& syms,
    const std::vector<Instance>& instances) {
  std::vector<Instance> expanded;
  for (const Instance& instance : instances) {
    ASSIGN_OR_RETURN(std::vector<AggTuple> tuples,
                     collect_agg_tuples(agg, instance.binding, store, syms));
    ASSIGN_OR_RETURN(std::vector<int64_t> values, possible_values(agg, tuples));
    for (int64_t value : values) {
      Instance next = instance;
      const Sym sym = syms.number(value);
      for (size_t slot : outputs) next.binding.set(slot, sym);
      // The value can complete an assignment, e.g. the 'T = S + 1' of
      // 'q(T) :- #count{X : p(X)} = S, T = S + 1.'
      BindingTrail trail(next.binding);
      ASSIGN_OR_RETURN(bool ok,
                       bind_assignments(parts, next.binding, trail, syms));
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
    const BodyParts& parts, const Store& store, Symbols& syms,
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
                                                     store, syms, instances));
    }
    // Aggregates left waiting on each other bind their variables in a cycle
    // and can never be ground. verify_safe() rejects such a rule; stopping
    // here leaves the variables unbound, which reports it as well.
    if (waiting.size() == pending.size()) break;
    pending = std::move(waiting);
  }
  return instances;
}

// Turns an aggregate's tuples into the weighted literals its value sums over.
// Each distinct tuple gets a fresh auxiliary atom, supported by one plain rule
// per grounding that produced it. Multiple such rules give the atom OR
// semantics for free, exactly modeling "this tuple is in the set if any
// grounding satisfies it".
std::vector<aspif::WeightedLit> ground_agg_elements(
    const std::vector<AggTuple>& tuples, aspif::Program& result) {
  std::vector<aspif::WeightedLit> weighted;
  weighted.reserve(tuples.size());
  for (const AggTuple& tuple : tuples) {
    // A tuple with one support holding one literal needs no atom of its own:
    // that literal already says exactly "this tuple is in the set". Two tuples
    // can land on the same literal, e.g. the '#count{ 1 : p ; 2 : p }' whose
    // value is 2 when p holds, and a weight body adds up its literals, so the
    // two entries keep counting as the two tuples they are.
    if (tuple.supports.size() == 1 && tuple.supports.front().size() == 1) {
      weighted.push_back(
          {.lit = tuple.supports.front().front(), .weight = tuple.weight});
      continue;
    }
    aspif::Atom atom = result.new_atom();
    weighted.push_back({.lit = atom, .weight = tuple.weight});
    for (const std::vector<aspif::Lit>& support : tuple.supports) {
      aspif::Rule rule;
      rule.head = {atom};
      rule.body = support;
      result.rules.push_back(std::move(rule));
      // A support left empty, because every literal in it was a fact, puts the
      // tuple in the set unconditionally. Whatever the remaining supports say,
      // they can only say it again.
      if (support.empty()) break;
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

  // Whether a value meets every bound collected here.
  bool hold_for(int64_t value) const {
    if (lower.has_value() && value < *lower) return false;
    if (upper.has_value() && value > *upper) return false;
    for (int64_t k : not_equal) {
      if (value == k) return false;
    }
    return true;
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
                                                  const Binding& binding,
                                                  Symbols& syms) {
  // In a '#count{...} = S', S holds the value this rule instance checks for:
  // find_instances() split the instance over the values the aggregate can
  // take.
  ASSIGN_OR_RETURN(std::optional<Sym> value, eval_term(term, binding, syms));
  if (!value.has_value()) return std::nullopt;
  if (!syms.is_number(*value)) {
    return absl::InvalidArgumentError("an aggregate bound must be a number");
  }
  return syms.number_of(*value);
}

// Reads the (up to two) bound sides of an aggregate under `binding`. Nullopt
// means a bound is ill-formed, e.g. the '4 / 0' of '#count{...} >= 4 / X'
// under X = 0, which makes the enclosing rule instance nonexistent.
absl::StatusOr<std::optional<AggBounds>> eval_agg_bounds(const Aggregate& agg,
                                                         const Binding& binding,
                                                         Symbols& syms) {
  AggBounds bounds;
  if (agg.lb_term != nullptr) {
    ASSIGN_OR_RETURN(std::optional<int64_t> k,
                     eval_bound(*agg.lb_term, binding, syms));
    if (!k.has_value()) return std::nullopt;
    apply_lower_bound(*k, agg.lb_op, bounds);
  }
  if (agg.ub_term != nullptr) {
    ASSIGN_OR_RETURN(std::optional<int64_t> k,
                     eval_bound(*agg.ub_term, binding, syms));
    if (!k.has_value()) return std::nullopt;
    apply_upper_bound(*k, agg.ub_op, bounds);
  }
  return bounds;
}

// Whether an aggregate whose value grounding has settled holds, e.g. true for
// the '#count{ X : p(X) } >= 1' of a rule over the fact p(1). Nullopt when the
// value is not settled, which leaves the aggregate for the solver.
std::optional<bool> settled_agg_holds(const Aggregate& agg,
                                      const AggBounds& bounds,
                                      const std::vector<AggTuple>& tuples) {
  std::optional<int64_t> value = settled_agg_value(agg, tuples);
  if (!value.has_value()) return std::nullopt;
  const bool holds = bounds.hold_for(*value);
  return agg.naf ? !holds : holds;
}

// Whether grounding settles `agg` under `binding`, and if so whether it holds,
// asked while atoms are still being derived. Nullopt means the solver decides,
// which is also the answer for an aggregate this phase is too early to judge.
absl::StatusOr<std::optional<bool>> settle_aggregate(const Aggregate& agg,
                                                     const Binding& binding,
                                                     const Store& store,
                                                     Symbols& syms) {
  // An aggregate under 'not' is the too-early case. Its element predicates
  // reach the rule's head through negative dependency edges only, and those
  // don't order components, so 'q :- not #count{ X : p(X) } >= 1.' can have q's
  // component derived before p has a single atom. Counting there would find an
  // empty set and read the negation as satisfied. emit_rules() runs once every
  // component has derived and settles these safely.
  if (agg.naf) return std::nullopt;
  // A 'not' inside an element points at a predicate the same way; see
  // settled_agg_value().
  if (elements_use_negation(agg)) return std::nullopt;
  ASSIGN_OR_RETURN(std::optional<AggBounds> bounds,
                   eval_agg_bounds(agg, binding, syms));
  if (!bounds.has_value()) return std::nullopt;
  ASSIGN_OR_RETURN(std::vector<AggTuple> tuples,
                   collect_agg_tuples(agg, binding, store, syms));
  return settled_agg_holds(agg, *bounds, tuples);
}

// Grounds one Aggregate body item into the literals that must be appended to
// the enclosing rule's body for the aggregate to hold, e.g. '#count{X :
// p(X)} >= 2' grounds to a single literal referencing a fresh atom defined
// by an ASPIF weight-body rule. Only #count and #sum are supported: they map
// directly onto ASPIF's weight body, whereas #min/#max would need a
// different (guess-and-check) encoding.
//
// An aggregate whose value grounding has settled needs no encoding at all: it
// either holds in every answer set, and so asks nothing of the rule's body, or
// in none, and takes the rule instance with it.
//
// Returns nullopt if the aggregate can hold in no answer set, either because
// one of its bounds is ill-formed or because its settled value misses them.
// Both make the whole enclosing rule instance nonexistent.
absl::StatusOr<std::optional<std::vector<aspif::Lit>>> ground_aggregate(
    const Aggregate& agg, const Binding& outer_binding, const Store& store,
    Symbols& syms, aspif::Program& result) {
  if (agg.function == AggregateFunctionType::kAGGREGATE_MAX ||
      agg.function == AggregateFunctionType::kAGGREGATE_MIN) {
    return absl::UnimplementedError(
        "#min and #max aggregates are not supported yet");
  }

  ASSIGN_OR_RETURN(std::optional<AggBounds> maybe_bounds,
                   eval_agg_bounds(agg, outer_binding, syms));
  if (!maybe_bounds.has_value()) return std::nullopt;
  const AggBounds& bounds = *maybe_bounds;

  ASSIGN_OR_RETURN(std::vector<AggTuple> tuples,
                   collect_agg_tuples(agg, outer_binding, store, syms));
  std::optional<bool> settled = settled_agg_holds(agg, bounds, tuples);
  if (settled.has_value()) {
    if (!*settled) return std::nullopt;
    return std::vector<aspif::Lit>{};
  }

  std::vector<aspif::WeightedLit> weighted =
      ground_agg_elements(tuples, result);

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

// Grounds each aggregate once per way of reading it, however many rule
// instances read it that way.
//
// What an aggregate grounds to depends on the aggregate and on the values its
// own variables have, and on nothing else, so instances that agree on those
// share one encoding, auxiliary atoms and all. Every instance of
// 'p(X) :- dom(X), #count{ Y : r(Y) } >= 2.' reads the #count the same way,
// because it mentions no variable the rest of the rule binds, so the whole
// rule needs the one encoding.
//
// Sharing an auxiliary atom between rules is sound because the atom means the
// same thing in each: the rules defining it are built from the same tuples.
class AggCache {
 public:
  // The literals `agg` contributes to a rule body under `binding`, as
  // ground_aggregate() gives them, and nullopt in the same cases.
  absl::StatusOr<std::optional<std::vector<aspif::Lit>>> ground(
      const Aggregate& agg, const Binding& binding, const Store& store,
      Symbols& syms, aspif::Program& result) {
    Key key = key_for(agg, binding);
    auto it = ground_.find(key);
    if (it != ground_.end()) return it->second;

    ASSIGN_OR_RETURN(std::optional<std::vector<aspif::Lit>> lits,
                     ground_aggregate(agg, binding, store, syms, result));
    ground_.emplace(std::move(key), lits);
    return lits;
  }

  // Whether grounding settles `agg` under `binding`, and if so whether it
  // holds, as settle_aggregate() works it out. Deriving atoms asks this once
  // per instance and over the same tuples, so the answer is worth keeping
  // here too. It stays true for the whole of grounding: safety.cc keeps an
  // aggregate's predicates in components that are complete before any rule
  // reading them is ground, so what its elements produce cannot change.
  //
  // A disjunctive head can join an aggregate's predicate to the component of the
  // rule reading it, which would leave that store incomplete. The rules this
  // happens to never ask, so no answer taken from a partial store is cached
  // here; see mark_aggregates_in_own_component().
  absl::StatusOr<std::optional<bool>> settle(const Aggregate& agg,
                                             const Binding& binding,
                                             const Store& store,
                                             Symbols& syms) {
    Key key = key_for(agg, binding);
    auto it = settled_.find(key);
    if (it != settled_.end()) return it->second;

    ASSIGN_OR_RETURN(std::optional<bool> holds,
                     settle_aggregate(agg, binding, store, syms));
    settled_.emplace(std::move(key), holds);
    return holds;
  }

 private:
  // One aggregate together with what its variables are bound to. A slot reading
  // kNoSym is a variable local to the aggregate, which every instance leaves
  // for the aggregate's own grounding to bind.
  struct Key {
    const Aggregate* agg;
    std::vector<Sym> values;

    bool operator==(const Key&) const = default;

    template <typename H>
    friend H AbslHashValue(H h, const Key& key) {
      return H::combine(std::move(h), key.agg, key.values);
    }
  };

  Key key_for(const Aggregate& agg, const Binding& binding) {
    Key key{.agg = &agg};
    for (size_t slot : agg_variable_slots(agg, binding)) {
      key.values.push_back(binding.at(slot));
    }
    return key;
  }

  absl::flat_hash_map<Key, std::optional<std::vector<aspif::Lit>>> ground_;
  absl::flat_hash_map<Key, std::optional<bool>> settled_;
};

// --------------------------------------------------------------------------
// Building the ground program
//
// Two phases. derive_atoms() runs the rules to a fixpoint to find every atom
// that could appear in an answer set, numbers it, and works out which of them
// are facts; emit_rules() then walks the rules again and emits one ASPIF rule
// per instance, in terms of those numbers, leaving out what the facts have
// already settled. The rest turns the store into the program's minimize
// statements, output names, and query assumption.
// --------------------------------------------------------------------------

// Whether the body parts derive_atoms() otherwise ignores are well-formed
// under `binding`: the arguments of every 'not' literal and each aggregate's
// bounds. Ill-formed means the rule has no ground instance under this
// binding, so its head atom must not be derived either, e.g. "q(X) :- p(X),
// not r(4 / X)." derives no q(0), because 'not r(4 / 0)' cannot be ground.
absl::StatusOr<bool> ignored_parts_are_well_formed(const BodyParts& parts,
                                                   const Binding& binding,
                                                   Symbols& syms) {
  for (const ClassicalLiteral* literal : parts.negative) {
    ASSIGN_OR_RETURN(bool matchable, args_can_match(*literal, binding, syms));
    if (!matchable) return false;
  }
  for (const Aggregate* aggregate : parts.aggregates) {
    if (aggregate->lb_term != nullptr) {
      ASSIGN_OR_RETURN(std::optional<int64_t> k,
                       eval_bound(*aggregate->lb_term, binding, syms));
      if (!k.has_value()) return false;
    }
    if (aggregate->ub_term != nullptr) {
      ASSIGN_OR_RETURN(std::optional<int64_t> k,
                       eval_bound(*aggregate->ub_term, binding, syms));
      if (!k.has_value()) return false;
    }
  }
  return true;
}

// What a derivation pass changed, which is what decides whether another pass
// is worth running and what it has to look at.
struct Changes {
  // An atom was derived for the first time.
  bool atoms = false;
  // An atom the store already held became a fact. Marking it added no atom, so
  // the next pass's delta would not revisit the rules that read it; see
  // derive_atoms().
  bool facts = false;
};

// Whether grounding settles every aggregate in a body and finds them all
// satisfied, which is what lets an instance carrying aggregates derive a fact.
absl::StatusOr<bool> aggregates_settle_true(
    const std::vector<const Aggregate*>& aggregates, const Binding& binding,
    const Store& store, Symbols& syms, AggCache& agg_cache) {
  for (const Aggregate* aggregate : aggregates) {
    ASSIGN_OR_RETURN(std::optional<bool> holds,
                     agg_cache.settle(*aggregate, binding, store, syms));
    if (!holds.has_value() || !*holds) return false;
  }
  return true;
}

// Runs one rule against the store and adds the head atoms of every instance it
// finds, recording in `changes` what that did. `delta_position` picks the
// positive literal to read the previous pass's atoms from (see
// find_instances), or nullopt to read the whole store.
//
// An instance whose positive literals all matched facts derives its head atom
// as a fact in turn: every one of those atoms holds in every answer set, so the
// body does, so the head does. That is only sound for a rule whose body has
// nothing else in it that can fail. A 'not q' is exactly such a thing, and this
// phase ignores it (see derive_atoms), so a rule carrying one derives no facts.
// An aggregate is one only sometimes, so AggCache::settle() is asked about it,
// last, once the cheap reasons not to bother have been ruled out.
//
// A disjunctive head derives no fact either, however solid its body: 'a | b.'
// says one of a and b holds, and neither of them holds in every answer set. Its
// atoms are only possible ones, which is exactly what this phase collects.
//
// Nor does a rule whose aggregate reads its own component. Settling such an
// aggregate would read a store still being filled. See
// mark_aggregates_in_own_component().
//
// This is where 'p(1).' becomes a fact, and where a rule over facts alone, like
// the second rule of "edge(a, b). reachable(X, Y) :- edge(X, Y).", passes
// factness on.
absl::Status derive_from_rule(const RuleView& rule,
                              std::optional<size_t> delta_position,
                              Store& store, Symbols& syms, AggCache& agg_cache,
                              aspif::Program& aspif_prog, Changes& changes) {
  const bool derives_facts = rule.parts.negative.empty() &&
                             rule.head.size() == 1 &&
                             !rule.aggregate_in_own_component;
  return find_instances(
      rule.parts, store, syms, Binding(rule.slots),
      [&](const Instance& instance) -> absl::Status {
        ASSIGN_OR_RETURN(
            bool well_formed,
            ignored_parts_are_well_formed(rule.parts, instance.binding, syms));
        if (!well_formed) return absl::OkStatus();

        // An ill-formed head term means this instance has no ground rule at all,
        // so none of its head atoms is derived. emit_rules() drops the same
        // instances.
        std::vector<Tuple> tuples;
        tuples.reserve(rule.head.size());
        for (const ClassicalLiteral* literal : rule.head) {
          ASSIGN_OR_RETURN(std::optional<Tuple> tuple,
                           eval_terms(literal->args, instance.binding, syms));
          if (!tuple.has_value()) return absl::OkStatus();
          tuples.push_back(*std::move(tuple));
        }

        std::vector<Inserted> heads;
        heads.reserve(rule.head.size());
        for (size_t i = 0; i < rule.head.size(); ++i) {
          heads.push_back(store.insert(pred_key(*rule.head[i]),
                                       std::move(tuples[i]), aspif_prog));
          if (heads.back().is_new) changes.atoms = true;
        }

        if (derives_facts && matched_all_facts(instance.matched, store)) {
          ASSIGN_OR_RETURN(
              bool aggregates_hold,
              aggregates_settle_true(rule.parts.aggregates, instance.binding,
                                     store, syms, agg_cache));
          if (!aggregates_hold) return absl::OkStatus();
          const bool newly_fact = store.mark_fact(heads.front().atom);
          // Marking an atom the store already held is the one change a delta
          // pass cannot carry; see derive_atoms().
          if (newly_fact && !heads.front().is_new) changes.facts = true;
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
//
// Which atoms are facts is settled here too, by the same repeated passes:
// marking an atom a fact can make the head of a rule that reads it a fact, so
// factness spreads until a pass marks nothing new, just as atoms do. A pass
// that only marked facts is the one case the delta cannot carry, because a
// rule reading that atom sees no new atom to re-run on, so the pass after it
// reads the whole store instead.
absl::Status derive_atoms(const std::vector<const RuleView*>& rules,
                          Store& store, Symbols& syms, AggCache& agg_cache,
                          aspif::Program& aspif_prog) {
  // The first pass reads the whole store. It is the only one that fires the
  // rules with no positive literals, and it derives the delta the passes below
  // start from. Later passes are semi-naive unless the only change was marking
  // facts, which a delta cannot carry, so the next pass reads the whole store.
  bool full_scan = true;
  Changes changes;
  while (full_scan || changes.atoms) {
    changes = Changes{};
    store.begin_pass();
    for (const RuleView* rule : rules) {
      if (rule->head.empty()) continue;
      if (full_scan) {
        RETURN_IF_ERROR(derive_from_rule(*rule, std::nullopt, store, syms,
                                         agg_cache, aspif_prog, changes));
        continue;
      }
      for (size_t position = 0; position < rule->parts.positive.size();
           ++position) {
        RETURN_IF_ERROR(derive_from_rule(*rule, position, store, syms,
                                         agg_cache, aspif_prog, changes));
      }
    }
    full_scan = changes.facts && !changes.atoms;
  }
  return absl::OkStatus();
}

// Emits one ASPIF rule per rule instance. A 'not q' whose atom q was never
// derived by derive_atoms() can never be true, so the literal is dropped as
// satisfied; otherwise it stays in the rule body, negated.
//
// An instance whose head is a fact needs no rule of its own. The atom holds in
// every answer set whatever this instance says, so all that has to reach the
// solver is the fact itself, once, however many rules derive it: that is what
// `emitted_facts` keeps track of, across the components emitted before this
// one. A transitive closure over a graph of facts is entirely facts this way,
// and reaches the solver with no rules at all.
//
// One fact among the atoms of a disjunctive head drops that instance too. The
// disjunction already holds, so it asks nothing of the other head atoms: they
// only draw support from a rule whose every other head atom is false, which this
// one's fact never is.
//
// A body literal for a fact goes the same way, dropped from the rules that do
// get emitted, since a body holds exactly when it holds without it.
absl::Status emit_rules(const std::vector<const RuleView*>& rules,
                        const Store& store, Symbols& syms,
                        absl::flat_hash_set<aspif::Atom>& emitted_facts,
                        AggCache& agg_cache, aspif::Program& result) {
  for (const RuleView* rule_ptr : rules) {
    const RuleView& rule = *rule_ptr;
    RETURN_IF_ERROR(find_instances(
        rule.parts, store, syms, Binding(rule.slots),
        [&](const Instance& instance) -> absl::Status {
          // The head atoms are looked up here. An instance missing one is only
          // an error further down, once every reason to drop the instance has
          // been ruled out. derive_atoms() dropped the same instances, so a head
          // atom looked up for one of them would be missing from the store.
          std::vector<Tuple> head_tuples;
          std::vector<const GroundAtom*> heads;
          head_tuples.reserve(rule.head.size());
          heads.reserve(rule.head.size());
          for (const ClassicalLiteral* literal : rule.head) {
            ASSIGN_OR_RETURN(std::optional<Tuple> tuple,
                             eval_terms(literal->args, instance.binding, syms));
            // An ill-formed head, e.g. the 'q(X / 0)' of "q(X / 0) :- p(X).",
            // means this instance has no ground rule. derive_atoms() skipped it
            // for the same reason, so nothing is missing from the store.
            if (!tuple.has_value()) return absl::OkStatus();
            head_tuples.push_back(*std::move(tuple));
            const PredData* data = store.find(pred_key(*literal));
            heads.push_back(data == nullptr ? nullptr
                                            : data->find(head_tuples.back()));
          }
          // One head atom, and it is a fact: the fact is all the solver needs,
          // once, however many rules derive it.
          if (heads.size() == 1 && heads[0] != nullptr &&
              store.is_fact(heads[0]->id)) {
            if (emitted_facts.insert(heads[0]->id).second) {
              result.rules.push_back(aspif::Rule{.head = {heads[0]->id}});
            }
            return absl::OkStatus();
          }
          // A fact among the atoms of a disjunctive head satisfies the rule, so
          // the instance says nothing and goes.
          for (const GroundAtom* head : heads) {
            if (head != nullptr && store.is_fact(head->id)) {
              return absl::OkStatus();
            }
          }

          aspif::Rule aspif_rule;
          aspif_rule.body = without_facts(instance.matched, store);
          ASSIGN_OR_RETURN(
              std::optional<std::vector<aspif::Lit>> neg,
              negative_lits(rule.parts.negative, instance.binding, store, syms));
          if (!neg.has_value()) return absl::OkStatus();
          aspif_rule.body.insert(aspif_rule.body.end(), neg->begin(),
                                 neg->end());

          for (const Aggregate* aggregate : rule.parts.aggregates) {
            ASSIGN_OR_RETURN(std::optional<std::vector<aspif::Lit>> extra,
                             agg_cache.ground(*aggregate, instance.binding,
                                              store, syms, result));
            if (!extra.has_value()) return absl::OkStatus();
            aspif_rule.body.insert(aspif_rule.body.end(), extra->begin(),
                                   extra->end());
          }

          for (size_t i = 0; i < heads.size(); ++i) {
            // derive_atoms() added every derivable head atom, so a miss means
            // the program never passed verify_safe().
            if (heads[i] == nullptr) {
              return absl::InternalError(absl::StrCat(
                  "grounding derived no atom for the head '",
                  syms.printed_call(rule.head[i]->id, head_tuples[i]),
                  "'; was the program checked by verify_safe()?"));
            }
            aspif_rule.head.push_back(heads[i]->id);
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
void emit_minimize(const Store& store, const Symbols& syms,
                   aspif::Program& result) {
  absl::btree_map<int64_t, std::vector<aspif::WeightedLit>> by_level;
  for (const PredKey& key : store.order) {
    if (key.name != "_viol") continue;
    for (const GroundAtom& atom : store.find(key)->atoms) {
      // normalize() always puts the level and weight first, in that order.
      const Sym level = atom.args[0];
      const Sym weight = atom.args[1];
      if (!syms.is_number(level) || !syms.is_number(weight)) continue;
      by_level[syms.number_of(level)].push_back(
          {.lit = atom.id, .weight = syms.number_of(weight)});
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
void name_outputs(const Store& store, const Symbols& syms,
                  aspif::Program& result) {
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
      result.outputs.push_back(
          aspif::Output{.name = syms.printed_call(predicate, atom.args),
                        .condition = {atom.id}});
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
                        Symbols& syms, aspif::Program& result) {
  // A query is its own scope, so its variables are numbered on their own rather
  // than sharing a rule's slots, and they start out unbound: the query's atoms
  // are whichever ones the store matches.
  VarSlots slots;
  absl::flat_hash_map<std::string_view, size_t> by_name;
  collect::for_each_variable(
      query, [&](const Variable& var) { slots.add(var, by_name); });
  ASSIGN_OR_RETURN(std::vector<aspif::Atom> matched,
                   matching_atoms(query, Binding(slots), store, syms));
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
      strongly_connected_components(derivation_succ(graph));
  ASSIGN_OR_RETURN(std::vector<RuleView> rules, make_rule_views(prog));
  mark_aggregates_in_own_component(graph, component, rules);
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
  // One symbol table for the whole run: every tuple in the store holds handles
  // into it, so it has to outlive them.
  Symbols syms;
  Store store;
  // One cache across both phases: they ask different questions about the same
  // aggregates, and what either answer rests on is complete before it is asked.
  AggCache agg_cache;
  absl::flat_hash_set<aspif::Atom> emitted_facts;
  for (const auto& component_rules : rules_by_component) {
    RETURN_IF_ERROR(
        derive_atoms(component_rules, store, syms, agg_cache, result));
  }
  for (const auto& component_rules : rules_by_component) {
    RETURN_IF_ERROR(emit_rules(component_rules, store, syms, emitted_facts,
                               agg_cache, result));
  }
  emit_minimize(store, syms, result);
  name_outputs(store, syms, result);
  if (prog.query != nullptr && prog.query->lit != nullptr) {
    RETURN_IF_ERROR(emit_query(*prog.query->lit, store, syms, result));
  }
  return result;
}
