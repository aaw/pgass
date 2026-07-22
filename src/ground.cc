#include "ground.h"

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

// Orders ground values per the ASP-Core-2 spec: (see the comment on Value
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

// One rule body, split into the three kinds of items grounding treats
// differently.
struct BodyParts {
  // Positive classical literals: matched against the store to bind
  // variables, e.g. 'edge(X, Y)' in "reachable(X, Y) :- edge(X, Y)."
  std::vector<const ClassicalLiteral*> positive;
  // Comparisons like 'X < 2' (possibly under 'not'): decided once bound.
  std::vector<const NafLiteral*> comparisons;
  // 'not p(...)' literals: kept in the emitted ground rule.
  std::vector<const ClassicalLiteral*> negative;
};

absl::StatusOr<BodyParts> split_body(const Body* body) {
  BodyParts parts;
  if (body == nullptr || body->items == nullptr) return parts;
  for (const auto& item : *body->items) {
    if (item->kind == BodyItem::AggregateKind) {
      return absl::UnimplementedError("aggregates are not supported yet");
    }
    const auto& naf = static_cast<const NafLiteral&>(*item);
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
    ASSIGN_OR_RETURN(rule.parts, split_body(statement->body.get()));
    rules.push_back(std::move(rule));
  }
  return rules;
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
absl::StatusOr<std::vector<Instance>> find_instances(const BodyParts& parts,
                                                     const Store& store) {
  // Start from a single empty instance: no variables bound, no atoms
  // matched. Each positive literal below multiplies it out.
  std::vector<Instance> instances;
  instances.push_back(Instance{});
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
      for (const ClassicalLiteral* negative : rule.parts.negative) {
        ASSIGN_OR_RETURN(Tuple tuple, eval_args(*negative, instance.binding));
        const PredData* data = store.find(pred_key(*negative));
        if (data == nullptr) continue;
        const GroundAtom* atom = data->find(tuple);
        if (atom == nullptr) continue;
        aspif_rule.body.push_back(-atom->id);
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
  ASSIGN_OR_RETURN(std::vector<RuleView> rules, make_rule_views(prog));

  aspif::Program result;
  Store store;
  RETURN_IF_ERROR(derive_atoms(rules, store, result));
  RETURN_IF_ERROR(emit_rules(rules, store, result));
  name_outputs(store, result);
  return result;
}
