#include "ground.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/container/node_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "collect.h"
#include "format.h"
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

// The stretch of one predicate's atoms a join step reads, as positions into
// PredData::atoms.
struct AtomRange {
  size_t begin = 0;
  size_t end = 0;

  bool operator==(const AtomRange&) const = default;
};

// Where one key's atoms sit in GroupedIndex::members.
struct Run {
  std::uint32_t begin = 0;
  std::uint32_t count = 0;
};

// The atoms a join step reads, with each probe key's atoms kept together as a
// run of `members`, so that a key costs no allocation of its own. A step that
// probes no argument reads every atom in `members` and leaves `runs` empty.
// grouped_index() builds these.
struct GroupedIndex {
  std::vector<const GroundAtom*> members;
  absl::flat_hash_map<Tuple, Run> runs;
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

  // The grouping a join step last built over this predicate, one per set of
  // probed argument positions. See grouped_index() for what is kept and why.
  struct CachedIndex {
    AtomRange range;
    std::shared_ptr<const GroupedIndex> index;
  };
  mutable absl::flat_hash_map<std::vector<size_t>, CachedIndex> groupings;

  const GroundAtom* find(const Tuple& args) const {
    auto it = index.find(args);
    return it == index.end() ? nullptr : &atoms[it->second];
  }

  // The same, among the atoms in [begin, end). A join step reads a stretch of
  // the deque rather than all of it.
  const GroundAtom* find_within(const Tuple& args, size_t begin,
                                size_t end) const {
    auto it = index.find(args);
    if (it == index.end() || it->second < begin || it->second >= end) {
      return nullptr;
    }
    return &atoms[it->second];
  }

  // How many different values the atoms hold at each argument position, e.g.
  // {2, 3} for a p/2 holding p(a,1), p(a,2), p(b,3). join_order() reads these
  // to guess how many atoms a step matches.
  //
  // Atoms are only ever appended, so each call counts only the ones added
  // since the last.
  const std::vector<size_t>& distinct_per_position() const {
    if (counted_at == atoms.size()) return distinct;
    if (seen.empty() && !atoms.empty()) seen.resize(atoms.front().args.size());
    for (; counted_at < atoms.size(); ++counted_at) {
      const Tuple& args = atoms[counted_at].args;
      for (size_t k = 0; k < seen.size(); ++k) seen[k].insert(args[k]);
    }
    distinct.clear();
    distinct.reserve(seen.size());
    for (const absl::flat_hash_set<Sym>& values : seen) {
      distinct.push_back(values.size());
    }
    return distinct;
  }

 private:
  mutable std::vector<absl::flat_hash_set<Sym>> seen;  // values, by position
  mutable std::vector<size_t> distinct;                // their counts
  mutable size_t counted_at = 0;
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
  // Node-based because a join step holds a PredData while it runs, and the
  // instances it hands on derive atoms of their own. A predicate seen for the
  // first time mid-join would grow a flat map and move every PredData in it.
  absl::node_hash_map<PredKey, PredData> preds;
  std::vector<PredKey> order;  // predicates in first-seen order
  // How many atoms the store holds, and the most it may hold. 0 means no
  // limit. See check_size().
  size_t num_atoms = 0;
  size_t max_atoms = 0;
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
    ++num_atoms;
    return Inserted{.atom = data.atoms.back().id, .is_new = true};
  }

  // Fails once the store holds more than `max_atoms` atoms, naming `key` as the
  // predicate whose atom took it there.
  //
  // Some programs have no finite grounding at all. 'p(1). p(X+1) :- p(X).'
  // derives a new p forever, because every atom it derives gives the rule one
  // more atom to read. So does 'p(a). p(f(X)) :- p(X).', and so does an
  // aggregate feeding its own value back in, as ground.h describes. ASP-Core-2
  // calls such a program inadmissible but leaves it to the system to notice.
  // Deriving to a fixpoint never returns on one, so the limit is what turns a
  // hang into an error naming the predicate that ran away.
  absl::Status check_size(const PredKey& key) const {
    if (max_atoms == 0 || num_atoms <= max_atoms) return absl::OkStatus();
    return absl::ResourceExhaustedError(absl::StrCat(
        "grounding gave up at ", max_atoms, " ground atoms, while deriving ",
        key.name, "/", key.arity,
        ". A predicate whose terms keep growing has no finite grounding. Raise "
        "--max_ground_atoms if this program really is that big"));
  }

  // Starts a derivation pass, so that what the last one derived becomes the
  // delta the next round of joins reads.
  //
  // The groupings go too. New boundaries mean no step asks for the old
  // stretches again, and each grouping holds a pointer per atom it covers.
  void begin_pass() {
    for (auto& [key, data] : preds) {
      data.size_before_prev_pass = data.size_before_pass;
      data.size_before_pass = data.atoms.size();
      data.groupings.clear();
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

// What 'a + 1' and '1 / 0' come back as. Nothing else in grounding uses this
// code.
//
// ASP-Core-2 builds no ground instance from such a binding, so this is not an
// error in the program. It is a status so that every step between the term and
// the join propagates it for free. Join::lost_instance() catches it.
inline constexpr absl::StatusCode kNoValue = absl::StatusCode::kOutOfRange;

absl::Status no_value(const Term& term, std::string_view why) {
  return absl::Status(kNoValue, absl::StrCat("'", format(term), "' ", why));
}

bool has_no_value(const absl::Status& status) {
  return status.code() == kNoValue;
}

absl::StatusOr<Sym> eval_number_sym(const Term& term, const Binding& binding,
                                    Symbols& syms);

// What evaluating an interval comes back as.
absl::Status no_interval_value(const Term& term) {
  return absl::InternalError(
      absl::StrCat("'", format(term),
                   "' stands for every value in it, so it has no one value to "
                   "evaluate"));
}

// Evaluates a term to its ground value, e.g. 'X' evaluates to 1 under the
// binding {X: 1}. Every variable must already be bound.
//
// Comes back kNoValue for a term like 'X / 0' or 'a + 1'.
absl::StatusOr<Sym> eval_term(const Term& term, const Binding& binding,
                              Symbols& syms) {
  switch (term.kind) {
    case Term::NumberKind:
      return syms.number(static_cast<const Number&>(term).value);
    case Term::StringKind:
      return syms.string(static_cast<const String&>(term).value);
    case Term::AtomKind: {
      const Atom& atom = static_cast<const Atom&>(term);
      if (atom.args == nullptr) return syms.constant(atom.name);
      std::vector<Sym> args;
      args.reserve(atom.args->size());
      for (const auto& arg : *atom.args) {
        ASSIGN_OR_RETURN(Sym value, eval_term(*arg, binding, syms));
        args.push_back(value);
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
      ASSIGN_OR_RETURN(Sym value,
                       eval_number_sym(*negated.term, binding, syms));
      if (is_inline_number(value)) return syms.number(-inline_number(value));
      return syms.number(-syms.number_of(value));
    }
    case Term::TermOperationKind: {
      const TermOperation& operation = static_cast<const TermOperation&>(term);
      ASSIGN_OR_RETURN(Sym left_sym,
                       eval_number_sym(*operation.left, binding, syms));
      ASSIGN_OR_RETURN(Sym right_sym,
                       eval_number_sym(*operation.right, binding, syms));
      if (operation.op == OperationType::kDIV) {
        const BigInt right = syms.number_of(right_sym);
        if (right.is_zero()) return no_value(term, "divides by zero");
        return syms.number(syms.number_of(left_sym) / right);
      }
      // Two handles carrying their own integers add, subtract and multiply in
      // 64 bits, with no BigInt built. This is most of the arithmetic a
      // grounding run does.
      if (is_inline_number(left_sym) && is_inline_number(right_sym)) {
        const std::int64_t left = inline_number(left_sym);
        const std::int64_t right = inline_number(right_sym);
        // An inlined integer is under 2^30, so a product of two stays well
        // inside 64 bits.
        static_assert(kInlineNumberBias <= (std::int64_t{1} << 31),
                      "inlined integers must square inside an int64");
        switch (operation.op) {
          case OperationType::kPLUS:
            return syms.number(left + right);
          case OperationType::kMINUS:
            return syms.number(left - right);
          case OperationType::kTIMES:
            return syms.number(left * right);
          case OperationType::kDIV:
            break;
        }
      }
      const BigInt left = syms.number_of(left_sym);
      const BigInt right = syms.number_of(right_sym);
      switch (operation.op) {
        case OperationType::kPLUS:
          return syms.number(left + right);
        case OperationType::kMINUS:
          return syms.number(left - right);
        case OperationType::kTIMES:
          return syms.number(left * right);
        case OperationType::kDIV:
          break;
      }
      return absl::InternalError("unknown arithmetic operator");
    }
    case Term::IntervalKind:
      // An interval is several values. expand_intervals() is where they are
      // read, one at a time.
      return no_interval_value(term);
  }
  return absl::InternalError("unknown term kind");
}

// Evaluates a term that has to come out as a number, e.g. either side of a
// '+'. Arithmetic works on integers only, so anything else comes back kNoValue,
// e.g. the 'a' that 'X' becomes under {X: a}.
absl::StatusOr<Sym> eval_number_sym(const Term& term, const Binding& binding,
                                    Symbols& syms) {
  ASSIGN_OR_RETURN(Sym value, eval_term(term, binding, syms));
  if (!syms.is_number(value)) return no_value(term, "is not an integer");
  return value;
}

// Evaluates a list of terms under `binding`, e.g. the 'X, 2' of 'p(X, 2)'
// evaluates to {1, 2} under the binding {X: 1}.
absl::StatusOr<Tuple> eval_terms(const Terms& terms, const Binding& binding,
                                 Symbols& syms) {
  Tuple tuple;
  if (terms != nullptr) {
    tuple.reserve(terms->size());
    for (const auto& term : *terms) {
      ASSIGN_OR_RETURN(Sym value, eval_term(*term, binding, syms));
      tuple.push_back(value);
    }
  }
  return tuple;
}

/* The two sides of a comparison against an interval, e.g. the '1..3' and the
   '_R0' of '_R0 = 1..3'. A comparison against no interval has neither.

   normalize() writes every interval a program holds into a comparison of this
   shape, so this is the only shape grounding meets. The term opposite the
   interval either takes its values from the interval, or has one already and
   is tested against it. See expand_intervals().
*/
struct IntervalSides {
  const Interval* interval = nullptr;
  const Term* other = nullptr;
};

IntervalSides interval_sides(const BuiltinAtom& builtin) {
  if (builtin.right != nullptr && builtin.right->kind == Term::IntervalKind) {
    return {.interval = static_cast<const Interval*>(builtin.right.get()),
            .other = builtin.left.get()};
  }
  if (builtin.left != nullptr && builtin.left->kind == Term::IntervalKind) {
    return {.interval = static_cast<const Interval*>(builtin.left.get()),
            .other = builtin.right.get()};
  }
  return {};
}

// Whether `other` is one of the values `interval` holds, e.g. whether the X of
// 'X = 1..3' is 1, 2 or 3.
//
// An end that is not an integer, the 'a' of 'X = a..3', comes back kNoValue.
// There is no interval to be in, so the binding builds no rule instance. A
// value that is not an integer is simply not in one.
absl::StatusOr<bool> interval_holds(const IntervalSides& sides,
                                    const Binding& binding, Symbols& syms) {
  ASSIGN_OR_RETURN(Sym lower,
                   eval_number_sym(*sides.interval->lower, binding, syms));
  ASSIGN_OR_RETURN(Sym upper,
                   eval_number_sym(*sides.interval->upper, binding, syms));
  ASSIGN_OR_RETURN(Sym value, eval_term(*sides.other, binding, syms));
  if (!syms.is_number(value)) return false;
  return syms.compare(lower, value) <= 0 && syms.compare(value, upper) <= 0;
}

// An argument read backwards, from the value it matched to the variable in it.
// 'N + 1' matched against 4 gives N of 3.
struct Inverse {
  // What to do to the value the argument matched to get the variable's. 'N + c'
  // and 'c + N' both take the constant off, so they are one case.
  enum class Kind { kTakeConstantOff, kAddConstant, kTakeFromConstant };

  const Variable* variable = nullptr;
  const BigInt* constant = nullptr;
  // The constant as an int64 where it fits. Two small integers work out in 64
  // bits, which is what keeps the common case from building a BigInt.
  std::optional<std::int64_t> small_constant;
  Kind kind = Kind::kTakeConstantOff;

  // The variable's value, given the value the argument matched. That value has
  // to be a number, which match_term() checks first.
  Sym solve(Sym value, Symbols& syms) const {
    if (small_constant.has_value() && is_inline_number(value)) {
      const std::int64_t got = inline_number(value);
      switch (kind) {
        case Kind::kTakeConstantOff: return syms.number(got - *small_constant);
        case Kind::kAddConstant: return syms.number(got + *small_constant);
        case Kind::kTakeFromConstant: return syms.number(*small_constant - got);
      }
    }
    const BigInt got = syms.number_of(value);
    switch (kind) {
      case Kind::kTakeConstantOff: return syms.number(got - *constant);
      case Kind::kAddConstant: return syms.number(got + *constant);
      case Kind::kTakeFromConstant: return syms.number(*constant - got);
    }
    return kNoSym;
  }
};

// Reads `term` as an argument that can be worked backwards: a variable on one
// side of a '+' or a '-', a number on the other.
//
// This is what lets 'p(N+1) :- p(N), q(N)' look p up by the value it matched
// instead of waiting for N. Without it that literal cannot go first, and a pass
// that should read only what the pass before derived rescans the whole
// predicate. Multiplying and dividing do not come back this way over the
// integers.
//
// match_term() does the arithmetic and collect_literal_vars() tells the join
// order that such an argument binds its variable rather than needing it. Both
// read this, so both mean the same thing by it.
std::optional<Inverse> invertible(const Term& term) {
  if (term.kind != Term::TermOperationKind) return std::nullopt;
  const TermOperation& operation = static_cast<const TermOperation&>(term);
  const bool plus = operation.op == OperationType::kPLUS;
  if (!plus && operation.op != OperationType::kMINUS) return std::nullopt;
  if (operation.left->kind == Term::VariableKind &&
      operation.right->kind == Term::NumberKind) {
    const BigInt& constant = static_cast<const Number&>(*operation.right).value;
    return Inverse{
        .variable = static_cast<const Variable*>(operation.left.get()),
        .constant = &constant,
        .small_constant = constant.to_int64(),
        // 'N + c' takes the constant off the value, 'N - c' adds it back.
        .kind = plus ? Inverse::Kind::kTakeConstantOff
                     : Inverse::Kind::kAddConstant};
  }
  if (operation.right->kind == Term::VariableKind &&
      operation.left->kind == Term::NumberKind) {
    const BigInt& constant = static_cast<const Number&>(*operation.left).value;
    return Inverse{
        .variable = static_cast<const Variable*>(operation.right.get()),
        .constant = &constant,
        .small_constant = constant.to_int64(),
        // 'c + N' takes the constant off too, 'c - N' takes the value off it.
        .kind = plus ? Inverse::Kind::kTakeConstantOff
                     : Inverse::Kind::kTakeFromConstant};
  }
  return std::nullopt;
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
  // An unbound variable takes the value the inverse works out. A bound one
  // falls through and is compared like any other arithmetic.
  if (arg.kind == Term::TermOperationKind) {
    if (const std::optional<Inverse> inverse = invertible(arg);
        inverse.has_value()) {
      const size_t slot = binding.slot_of(*inverse->variable);
      if (binding.at(slot) == kNoSym) {
        if (!syms.is_number(value)) return false;
        binding.set(slot, inverse->solve(value, syms));
        trail.record(slot);
        return true;
      }
    }
  }
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
  ASSIGN_OR_RETURN(Sym evaluated, eval_term(arg, binding, syms));
  return evaluated == value;
}

// Checks that `term` can match anything at all under `binding`: the question
// match_term answers, asked without a value to compare against. A '_' matches
// whatever sits opposite it, so it always can. 'X / 0' never can, and comes
// back kNoValue.
absl::Status can_match(const Term& term, const Binding& binding,
                       Symbols& syms) {
  if (term.kind == Term::AnonymousVariableKind) return absl::OkStatus();
  if (term.kind == Term::AtomKind) {
    const Atom& atom = static_cast<const Atom&>(term);
    if (atom.args != nullptr) {
      for (const auto& arg : *atom.args) {
        RETURN_IF_ERROR(can_match(*arg, binding, syms));
      }
      return absl::OkStatus();
    }
  }
  return eval_term(term, binding, syms).status();
}

// Checks that every argument of `literal` can match, e.g. kNoValue for the
// 'r(4 / 0)' that 'r(4 / X)' becomes under {X: 0}. Such a literal has no ground
// instance at all, which is different from having one that no stored atom
// matches.
absl::Status args_can_match(const ClassicalLiteral& literal,
                            const Binding& binding, Symbols& syms) {
  if (literal.args == nullptr) return absl::OkStatus();
  for (const auto& arg : *literal.args) {
    RETURN_IF_ERROR(can_match(*arg, binding, syms));
  }
  return absl::OkStatus();
}

// Whether `term` holds arithmetic anywhere, e.g. the 'f(X+1)' of 'p(f(X+1))'.
// Matching such a term evaluates that part rather than binding it.
bool holds_arithmetic(const Term& term) {
  switch (term.kind) {
    case Term::TermOperationKind:
    case Term::NegatedTermKind:
    case Term::IntervalKind:
      return true;
    case Term::AtomKind: {
      const Atom& atom = static_cast<const Atom&>(term);
      if (atom.args == nullptr) return false;
      for (const auto& arg : *atom.args) {
        if (holds_arithmetic(*arg)) return true;
      }
      return false;
    }
    case Term::AnonymousVariableKind:
    case Term::VariableKind:
    case Term::NumberKind:
    case Term::StringKind:
      return false;
  }
  return false;
}

// Which arguments of a literal a match works through, and in what order.
//
// The arguments that bind go first, whatever order they stand in, because the
// arithmetic ones can only be evaluated once the variables they read have
// values: matching 'q(X+1, X)' against a stored q(4, 3) takes the X from the
// second argument and only then works out whether X + 1 is the 4.
//
// `skip` names the positions to leave out, which is how a join step drops the
// ones it probed the store by.
std::vector<size_t> match_plan(const ClassicalLiteral& literal,
                               const std::vector<size_t>& skip) {
  const size_t n = literal.args ? literal.args->size() : 0;
  std::vector<size_t> plan;
  plan.reserve(n);
  for (bool arithmetic : {false, true}) {
    for (size_t k = 0; k < n; ++k) {
      if (absl::c_linear_search(skip, k)) continue;
      if (holds_arithmetic(*(*literal.args)[k]) != arithmetic) continue;
      plan.push_back(k);
    }
  }
  return plan;
}

// Tries to match `literal`'s arguments against a stored tuple, extending
// `binding` with any variables the match binds and recording them in `trail`.
// `tuple` has one value per argument: the store groups atoms by name and arity,
// so every tuple stored under the literal's predicate has the literal's arity.
//
// `plan` is what match_plan() worked out for the literal.
absl::StatusOr<bool> match_args(const ClassicalLiteral& literal,
                                const std::vector<size_t>& plan,
                                const Tuple& tuple, Binding& binding,
                                BindingTrail& trail, Symbols& syms) {
  for (size_t k : plan) {
    ASSIGN_OR_RETURN(
        bool ok,
        match_term(*(*literal.args)[k], tuple[k], binding, trail, syms));
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
  // The comparisons against an interval, e.g. '_R0 = 1..3'. An interval is not
  // one value, so these are kept apart for expand_intervals() to read.
  std::vector<const NafLiteral*> intervals;
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
    if (interval_sides(static_cast<const BuiltinAtom&>(*naf.literal)).interval !=
        nullptr) {
      parts.intervals.push_back(&naf);
    } else {
      parts.comparisons.push_back(&naf);
    }
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
  // component, which only a disjunctive head can bring about. See
  // bucket_rule_views(). Deriving atoms settles no aggregate of
  // such a rule, neither to drop an instance nor to mark a fact, because
  // settling an aggregate needs a store that is complete for what the aggregate
  // reads, and its own component is by definition still being derived.
  bool aggregate_in_own_component = false;
  // Whether every 'not' literal of the body reads an earlier component, so
  // that an atom missing from the store is missing for good. Such a rule can
  // derive facts. See bucket_rule_views().
  bool negation_is_settled = true;
  // Numbers every variable the rule mentions, head and body alike, including
  // the ones inside its aggregates. A rule's bindings all index by these.
  VarSlots slots;
  // Kept so that a warning can print the rule it is about.
  const Statement* statement = nullptr;
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
  for (const NafLiteral* naf : rule.parts.intervals)
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
    rule.statement = statement.get();
    rules.push_back(std::move(rule));
  }
  return rules;
}

// The predicate dependency graph with the predicates of each disjunctive head
// joined into a cycle, so that they all land in one strongly connected
// component.
//
// Deriving runs one component at a time, in order, and one instance of
// 'p(X) | q(X) :- dom(X).' derives an atom for p and one for q. Were p and q in
// different components, the rule would run in the later one and add atoms to a
// component already finished. The rules reading those atoms would never see
// them.
//
// Negative edges count too, so a 'not q' reads a q that is already complete.
// That is what orders last/1 before s/1 in 's(X + 1) :- s(X), not last(X).',
// which needs a complete last/1 to stop counting upwards. Unstratified
// negation makes a cycle of these edges, putting the predicates in one
// component that no order could complete first.
std::vector<std::vector<int>> derivation_succ(const PredGraph& graph) {
  std::vector<std::vector<int>> succ = graph.pos_succ;
  for (size_t i = 0; i < graph.neg_succ.size(); ++i) {
    succ[i].insert(succ[i].end(), graph.neg_succ[i].begin(),
                   graph.neg_succ[i].end());
  }
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
          if (it != graph.id_of.end() &&
              component[it->second] == component_id) {
            reads = true;
          }
        });
  }
  return reads;
}

// The component a rule derives into. Every literal of a disjunctive head sits
// in the one component, so the first speaks for all. See derivation_succ().
int head_component(const PredGraph& graph, const std::vector<int>& component,
                   const RuleView& rule) {
  return component[graph.id_of.at(pred_key(*rule.head[0]))];
}

// Buckets rules by the component they derive into, with the constraints, which
// have no head and so no component, in one extra bucket at the end. Along the
// way, since both also start from a rule's own component, marks
// aggregate_in_own_component and negation_is_settled on each rule (see
// RuleView). `rules` owns the RuleViews and must outlive the buckets.
//
// aggregate_in_own_component: whether one of the rule's aggregates reads a
// predicate from the rule's own component, which only a disjunctive head can
// bring about, and which stops deriving from settling that aggregate at all.
// verify_safe() keeps an aggregate's predicates out of a positive component
// with the rule's head, but a disjunctive head can still join them, e.g. 'p |
// q(1) :- r.' next to 'p :- #count{ X : q(X) } <= 0.' puts q/1 in p's
// component. Deriving reads the store as it stands, so the count above would
// settle at 0 before q(1) exists, wrongly making p a fact and losing the
// answer set {r, q(1)}. The aggregate is left alone instead: its rule's head
// atoms derive without it, and it reaches the solver through emit_rules(),
// which runs once every component is complete.
//
// negation_is_settled: whether every 'not' literal of the body reads an
// earlier component, so a missing atom is missing for good and the rule can
// derive facts. Unstratified negation is the exception, putting the 'not' in
// the rule's own component, where a later pass can still add the atom.
std::vector<std::vector<const RuleView*>> bucket_rule_views(
    const PredGraph& graph, const std::vector<int>& component,
    std::vector<RuleView>& rules) {
  // `component` is empty when the program mentions no predicate at all, e.g.
  // ":- 1 < 2."
  int num_components =
      component.empty()
          ? 0
          : *std::max_element(component.begin(), component.end()) + 1;
  std::vector<std::vector<const RuleView*>> bucket(num_components + 1);
  std::vector<const RuleView*>& constraints = bucket.back();
  for (RuleView& rule : rules) {
    if (rule.head.empty()) {
      constraints.push_back(&rule);
      continue;
    }
    const int own = head_component(graph, component, rule);
    for (const ClassicalLiteral* literal : rule.head) {
      DCHECK_EQ(component[graph.id_of.at(pred_key(*literal))], own);
    }
    for (const Aggregate* aggregate : rule.parts.aggregates) {
      if (reads_component(*aggregate, graph, component, own)) {
        rule.aggregate_in_own_component = true;
        break;
      }
    }
    for (const ClassicalLiteral* literal : rule.parts.negative) {
      if (component[graph.id_of.at(pred_key(*literal))] >= own) {
        rule.negation_is_settled = false;
        break;
      }
    }
    bucket[own].push_back(&rule);
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
  // Which of parts.comparisons bind_assignments() has already turned into an
  // assignment, indexed the same way. comparisons_hold() skips these: an
  // assignment holds by construction, so re-evaluating it is wasted work.
  std::vector<bool> settled;
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
// Comes back kNoValue when an assignment's value is ill-formed, e.g. the
// 'Y = 1 / 0' that 'Y = 1 / X' becomes under {X: 0}: the binding builds no rule
// instance.
absl::Status bind_assignments(const BodyParts& parts, Binding& binding,
                              BindingTrail& trail, Symbols& syms,
                              std::vector<bool>& settled) {
  // One assignment can bind a variable another needs, e.g. 'Y = X + 1, Z = Y +
  // 1', so passes repeat until one binds nothing new. A pass skips the
  // assignments already made: their variables are bound.
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < parts.comparisons.size(); ++i) {
      const NafLiteral& item = *parts.comparisons[i];
      std::optional<Assignment> assignment = assignment_of(item, binding);
      if (!assignment.has_value()) continue;
      if (!is_bound(*assignment->value, binding)) continue;
      ASSIGN_OR_RETURN(Sym value, eval_term(*assignment->value, binding, syms));
      size_t slot = binding.slot_of(*assignment->variable);
      binding.set(slot, value);
      trail.record(slot);
      settled[i] = true;
      changed = true;
    }
  }
  return absl::OkStatus();
}

// What each value an interval holds is handed to.
using ValueFn = absl::FunctionRef<absl::Status(Sym)>;

// Hands `visit` each value `interval` holds, e.g. 1, 2 and 3 for '1..3'.
absl::Status for_each_interval_value(const Interval& interval,
                                     const Binding& binding, Symbols& syms,
                                     const ValueFn& visit) {
  ASSIGN_OR_RETURN(Sym lower, eval_number_sym(*interval.lower, binding, syms));
  ASSIGN_OR_RETURN(Sym upper, eval_number_sym(*interval.upper, binding, syms));

  const std::optional<std::int64_t> low = syms.number_of(lower).to_int64();
  const std::optional<std::int64_t> high = syms.number_of(upper).to_int64();
  if (low.has_value() && high.has_value()) {
    for (std::int64_t value = *low; value <= *high; ++value) {
      RETURN_IF_ERROR(visit(syms.number(value)));
    }
    return absl::OkStatus();
  }
  // Both ends are out past an int64, which a narrow interval far from zero
  // can be, e.g. the four values from 2^64 to 2^64 + 3.
  for (BigInt value = syms.number_of(lower); value <= syms.number_of(upper);
       value += 1) {
    RETURN_IF_ERROR(visit(syms.number(value)));
  }
  return absl::OkStatus();
}

// Returns whether every comparison in the body holds under `binding`, e.g.
// whether 'X < 2' holds given the binding {X: 1}. A comparison with an
// ill-formed side, e.g. 'X < 1 / 0', comes back kNoValue: the binding builds
// no rule instance at all.
//
// The comparisons against an interval are not among these. expand_intervals()
// settles those.
//
// An assignment holds by construction: its variable holds exactly what the
// other side evaluates to. `settled` is bind_assignments()'s record of which
// ones those were, so this skips re-evaluating them.
absl::StatusOr<bool> comparisons_hold(const BodyParts& parts,
                                      const Binding& binding, Symbols& syms,
                                      const std::vector<bool>& settled) {
  for (size_t i = 0; i < parts.comparisons.size(); ++i) {
    if (settled[i]) continue;
    const NafLiteral& item = *parts.comparisons[i];
    const auto& builtin = static_cast<const BuiltinAtom&>(*item.literal);
    ASSIGN_OR_RETURN(Sym left, eval_term(*builtin.left, binding, syms));
    ASSIGN_OR_RETURN(Sym right, eval_term(*builtin.right, binding, syms));
    bool holds = builtin_holds(builtin.op, left, right, syms);
    if (item.naf) holds = !holds;
    if (!holds) return false;
  }
  return true;
}

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

// The variables of one positive literal, split by what matching does with
// them. Matching binds a variable standing as a whole argument or inside a
// function term, e.g. the X and Y of 'q(X, f(Y))'. A variable under arithmetic
// is read instead of bound: matching 'q(X+1)' evaluates X + 1 and compares the
// result, so something else has to bind X before the literal runs.
struct LiteralVars {
  absl::flat_hash_set<size_t> binds;
  absl::flat_hash_set<size_t> needs;
};

void collect_literal_vars(const Term& term, bool under_arithmetic,
                          const Binding& binding, LiteralVars& out) {
  switch (term.kind) {
    case Term::VariableKind: {
      const size_t slot = binding.slot_of(static_cast<const Variable&>(term));
      if (under_arithmetic) {
        out.needs.insert(slot);
      } else {
        out.binds.insert(slot);
      }
      return;
    }
    case Term::AtomKind: {
      const Atom& atom = static_cast<const Atom&>(term);
      if (atom.args == nullptr) return;
      for (const auto& arg : *atom.args) {
        collect_literal_vars(*arg, under_arithmetic, binding, out);
      }
      return;
    }
    case Term::NegatedTermKind:
      collect_literal_vars(*static_cast<const NegatedTerm&>(term).term,
                           /*under_arithmetic=*/true, binding, out);
      return;
    case Term::TermOperationKind: {
      // An argument that works backwards binds its variable rather than
      // needing it, which is what lets the literal take its turn first.
      if (!under_arithmetic) {
        if (const std::optional<Inverse> inverse = invertible(term);
            inverse.has_value()) {
          out.binds.insert(binding.slot_of(*inverse->variable));
          return;
        }
      }
      const TermOperation& operation = static_cast<const TermOperation&>(term);
      collect_literal_vars(*operation.left, /*under_arithmetic=*/true, binding,
                           out);
      collect_literal_vars(*operation.right, /*under_arithmetic=*/true, binding,
                           out);
      return;
    }
    case Term::IntervalKind: {
      // An interval reads its bounds the way arithmetic reads its operands: a
      // variable in one has to be bound before the interval says anything.
      const Interval& interval = static_cast<const Interval&>(term);
      collect_literal_vars(*interval.lower, /*under_arithmetic=*/true, binding,
                           out);
      collect_literal_vars(*interval.upper, /*under_arithmetic=*/true, binding,
                           out);
      return;
    }
    case Term::AnonymousVariableKind:
    case Term::NumberKind:
    case Term::StringKind:
      return;
  }
}

LiteralVars literal_vars(const ClassicalLiteral& literal,
                         const Binding& binding) {
  LiteralVars vars;
  if (literal.args == nullptr) return vars;
  for (const auto& arg : *literal.args) {
    collect_literal_vars(*arg, /*under_arithmetic=*/false, binding, vars);
  }
  return vars;
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
    case Term::IntervalKind: {
      const Interval& interval = static_cast<const Interval&>(term);
      return holds_anonymous_variable(*interval.lower) ||
             holds_anonymous_variable(*interval.upper);
    }
    case Term::NumberKind:
    case Term::StringKind:
    case Term::VariableKind:
      return false;
  }
  return false;
}

// What ordering needs to know about one positive literal. An argument whose
// slots are all bound and which holds no '_' has a value by the time the
// literal runs, so the step can probe the store by it. probeable_positions()
// answers that once the join is running. Ordering answers it from slots alone,
// before any value exists.
struct LiteralStats {
  struct Arg {
    absl::InlinedVector<size_t, 4> slots;
    bool anonymous = false;
  };
  const PredData* data = nullptr;
  absl::InlinedVector<Arg, 4> args;
};

LiteralStats literal_stats(const ClassicalLiteral& literal,
                           const Binding& binding, const Store& store) {
  LiteralStats stats;
  stats.data = store.find(pred_key(literal));
  if (literal.args == nullptr) return stats;
  for (const auto& arg : *literal.args) {
    LiteralStats::Arg& out = stats.args.emplace_back();
    out.anonymous = holds_anonymous_variable(*arg);
    collect::for_each_variable(*arg, [&](const Variable& var) {
      out.slots.push_back(binding.slot_of(var));
    });
  }
  return stats;
}

// Roughly how many atoms a step at this literal matches once `bound` holds the
// slots the steps before it fill.
//
// A step with nothing to probe by reads the whole predicate. Each position it
// can probe by cuts that down by however many different values the predicate
// holds there. The atoms are taken to be spread evenly over those values, so
// naming one keeps a 1/values share. For a p/2 of a thousand atoms with a
// hundred different first arguments, 'p(X, Y)' with X bound is guessed to
// match ten.
double match_estimate(const LiteralStats& stats,
                      const absl::flat_hash_set<size_t>& bound) {
  if (stats.data == nullptr) return 0;
  double estimate = static_cast<double>(stats.data->atoms.size());
  const std::vector<size_t>& distinct = stats.data->distinct_per_position();
  for (size_t k = 0; k < stats.args.size() && k < distinct.size(); ++k) {
    if (stats.args[k].anonymous || distinct[k] == 0) continue;
    const bool probeable =
        absl::c_all_of(stats.args[k].slots,
                       [&](size_t slot) { return bound.contains(slot); });
    if (probeable) estimate /= static_cast<double>(distinct[k]);
  }
  return estimate;
}

// The order a join visits a body's positive literals in.
//
// A literal waits for the variables its arithmetic reads, so
// "p(X) :- q(X+1), r(X)." matches r(X) first and only then evaluates X + 1.
// Source order would reach q(X+1) with X still unbound, which is not a term
// grounding can evaluate.
//
// Among the literals whose turn it could be, the delta literal goes first: it
// reads only what the previous pass derived, which is usually a small fraction
// of the store, so starting there keeps the partial instances the join carries
// around few. Left to right, the join would instead build one partial instance
// per atom of the first literal and only then discover that the delta has
// nothing to join them with.
//
// The rest keep the order the body wrote them in unless another order reads
// far fewer atoms, as match_estimate() counts them. Which order is picked
// matters most on a body whose literals do not all share variables, e.g.
//
//   true(conj(S), N) :- elem(conj(S)), true(P), setn(S, N, pos(P)).
//
// Source order matches elem, then pairs every S it found with every atom of
// true, and only then throws nearly all of those pairs away at setn. Taking
// setn second instead probes it by the S in hand, which binds N and P, and
// leaves true(P) a single lookup.
//
// `bound` holds only what the literals bind. extend() also binds an assignment
// once the steps before it have what it reads, so a step probed by such a
// variable is scored here at full predicate size. Counting those slots was
// measured and dropped. It picks a different order on PartnerUnits, and
// renumbering the atoms costs that benchmark ten times its solving time.
std::vector<size_t> join_order(const BodyParts& parts, const Binding& binding,
                               const Store& store,
                               std::optional<size_t> delta_position) {
  const size_t count = parts.positive.size();
  std::vector<LiteralVars> vars;
  std::vector<LiteralStats> stats;
  vars.reserve(count);
  stats.reserve(count);
  absl::flat_hash_set<size_t> bound;
  for (const ClassicalLiteral* literal : parts.positive) {
    vars.push_back(literal_vars(*literal, binding));
    stats.push_back(literal_stats(*literal, binding, store));
    for (size_t slot : vars.back().binds) {
      if (binding.at(slot) != kNoSym) bound.insert(slot);
    }
    for (size_t slot : vars.back().needs) {
      if (binding.at(slot) != kNoSym) bound.insert(slot);
    }
  }

  // A literal's turn has come once every variable its arithmetic reads is
  // bound, either by an earlier literal or by the literal itself, e.g. the X
  // of 'q(X+1, X)', which match_args binds from the argument that can bind it.
  auto is_ready = [&](size_t k) {
    for (size_t slot : vars[k].needs) {
      if (!bound.contains(slot) && !vars[k].binds.contains(slot)) return false;
    }
    return true;
  };

  // An order costs what its steps read. Each step reads match_estimate() atoms
  // for every partial instance that reaches it, so the order costs the sum of
  // the running products.
  //
  // The walk takes the lowest-numbered ready literal at every step, so the
  // first order it completes is the body's own. That one is the incumbent.
  std::vector<size_t> best_order;
  double best_cost = 0;  // what an order has to come in under
  std::vector<size_t> order;
  order.reserve(count);
  std::vector<bool> taken(count, false);

  // How many orders the search may look at beyond the first. Trying them all
  // is factorial in the number of positive literals, so a wide body needs a
  // cutoff to keep ordering cheaper than running.
  int budget = 4000;

  // The estimate is crude, so it decides nothing it says is close. Keeping the
  // body's own order where the win is small also keeps the order atoms are
  // derived in, which is the order they are numbered in.
  constexpr double kWorthReordering = 4.0;
  auto record_order = [&](const std::vector<size_t>& candidate, double cost) {
    const bool incumbent = best_order.empty();
    best_order = candidate;
    best_cost = incumbent ? cost / kWorthReordering : cost;
  };

  auto search = [&](auto& self, double rows, double cost) -> void {
    if (!best_order.empty() && (cost >= best_cost || --budget < 0)) return;
    if (order.size() == count) {
      record_order(order, cost);
      return;
    }
    bool any_ready = false;
    for (size_t k = 0; k < count; ++k) {
      if (taken[k] || !is_ready(k)) continue;
      // The delta literal reads only what the last pass derived, so it takes
      // its turn as soon as it is ready. Any other literal ahead of it reads
      // the whole store to find the instances that pass already found.
      if (delta_position.has_value() && !taken[*delta_position] &&
          is_ready(*delta_position) && k != *delta_position) {
        continue;
      }
      any_ready = true;
      const double estimate = match_estimate(stats[k], bound);
      absl::InlinedVector<size_t, 8> fresh;
      for (size_t slot : vars[k].binds) {
        if (bound.insert(slot).second) fresh.push_back(slot);
      }
      taken[k] = true;
      order.push_back(k);
      self(self, rows * estimate, cost + rows * estimate);
      order.pop_back();
      taken[k] = false;
      for (size_t slot : fresh) bound.erase(slot);
    }
    // Nothing left is ready, which means a variable is bound by something the
    // steps do not bind: an assignment, or an interval or aggregate the join
    // waits on. The rest go in as they come, and the step that has what one of
    // those reads binds it. See extend() and plan_items().
    if (!any_ready) {
      std::vector<size_t> rest = order;
      for (size_t k = 0; k < count; ++k) {
        if (!taken[k]) rest.push_back(k);
      }
      record_order(rest, cost);
    }
  };
  search(search, 1.0, 0.0);
  return best_order;
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

/* The atoms a join step can match, arranged so that probing by the values the
   step knows takes a lookup rather than a scan.

   Three shapes, because the two ends need no arranging at all. A step that
   probes every argument names one atom, which the store's own index already
   finds. A step that probes none matches every atom it reads. Only a step
   between the two needs its atoms grouped.
*/
struct StepIndex {
  enum class Kind { kEvery, kOne, kGrouped };
  Kind kind = Kind::kEvery;
  // kEvery and kGrouped: the atoms the step reads, shared with the predicate
  // that built them. See grouped_index().
  std::shared_ptr<const GroupedIndex> grouped;
  // kOne: the store's own index maps a full argument tuple straight to its
  // atom, so the step reads from there and checks the atom is one it reads.
  const PredData* data = nullptr;
  AtomRange range;
};

// Groups `range`'s atoms by their values at `positions`, e.g. the edge/2 atoms
// by their first argument.
GroupedIndex group_atoms(const PredData& data, const AtomRange& range,
                         const std::vector<size_t>& positions) {
  GroupedIndex index;
  index.members.reserve(range.end - range.begin);
  if (positions.empty()) {
    for (size_t k = range.begin; k < range.end; ++k) {
      index.members.push_back(&data.atoms[k]);
    }
    return index;
  }

  // Sizing the map up front saves most of the rehashing. The atoms can share
  // far fewer keys than there are of them, so the guess is capped.
  constexpr size_t kMostToReserve = size_t{1} << 16;
  index.runs.reserve(std::min(range.end - range.begin, kMostToReserve));
  // Each atom's key is worked out once, here. The group it lands in is kept, so
  // that filling `members` below needs no second lookup. A run's `begin` stands
  // for its group until the offsets are known.
  std::vector<std::uint32_t> group_of;
  std::vector<std::uint32_t> counts;
  group_of.reserve(range.end - range.begin);
  Tuple key;
  for (size_t k = range.begin; k < range.end; ++k) {
    const GroundAtom& atom = data.atoms[k];
    key.clear();
    for (size_t position : positions) key.push_back(atom.args[position]);
    auto [it, added] = index.runs.try_emplace(key, Run{});
    if (added) {
      it->second.begin = static_cast<std::uint32_t>(counts.size());
      counts.push_back(0);
    }
    group_of.push_back(it->second.begin);
    ++counts[it->second.begin];
  }

  // The groups take the block in the order they were first seen, so a run is
  // its group's count added to everything before it.
  std::vector<std::uint32_t> offsets(counts.size(), 0);
  std::uint32_t next = 0;
  for (size_t group = 0; group < counts.size(); ++group) {
    offsets[group] = next;
    next += counts[group];
  }
  for (auto& entry : index.runs) {
    const std::uint32_t group = entry.second.begin;
    entry.second.begin = offsets[group];
    entry.second.count = counts[group];
  }

  index.members.resize(next);
  std::vector<std::uint32_t> cursor = std::move(offsets);
  for (size_t k = range.begin; k < range.end; ++k) {
    index.members[cursor[group_of[k - range.begin]]++] = &data.atoms[k];
  }
  return index;
}

/* The grouping for `positions` over `range`, built once and kept on the
   predicate for the next step that wants the same one.

   Grounding an aggregate's elements is a join of its own, run once per instance
   of the rule holding it. The '#count{ X : pred(C,X) }' of the MaxSAT encoding
   runs once per clause, and grouping every pred atom afresh each time is
   quadratic in the number of clauses for an answer that does not change.

   A predicate only ever gains atoms, so a grouping stays right for its own
   stretch of them. One grouping per set of positions is kept, and a step
   reading a different stretch replaces it. Steps hold a share of what they
   built rather than the map's copy, so replacing it leaves a running join with
   the atoms it started on.
*/
std::shared_ptr<const GroupedIndex> grouped_index(
    const PredData& data, const AtomRange& range,
    const std::vector<size_t>& positions) {
  auto [it, added] = data.groupings.try_emplace(positions);
  PredData::CachedIndex& cached = it->second;
  if (!added && cached.range == range) return cached.index;
  cached.range = range;
  cached.index =
      std::make_shared<const GroupedIndex>(group_atoms(data, range, positions));
  return cached.index;
}

// How the step at `positions` reads `range`'s atoms.
StepIndex build_step_index(const PredData& data, const AtomRange& range,
                           const std::vector<size_t>& positions, size_t arity) {
  StepIndex index;
  if (positions.size() == arity && arity > 0) {
    index.kind = StepIndex::Kind::kOne;
    index.data = &data;
    index.range = range;
    return index;
  }
  if (!positions.empty()) index.kind = StepIndex::Kind::kGrouped;
  index.grouped = grouped_index(data, range, positions);
  return index;
}

// The values one partial instance needs at `positions`, e.g. {b} for the
// 'edge(Y, Z)' above under {Y: b}. Looking these up in the step's index gives
// the atoms that can extend the instance.
//
// Comes back kNoValue when one of the terms is ill-formed, e.g. the
// 'edge(1 / 0, Z)' that 'edge(X / 0, Z)' becomes under {X: 0}: no atom matches
// it.
absl::StatusOr<Tuple> probe_key(const ClassicalLiteral& literal,
                                const std::vector<size_t>& positions,
                                const Binding& binding, Symbols& syms) {
  Tuple key;
  key.reserve(positions.size());
  for (size_t position : positions) {
    ASSIGN_OR_RETURN(Sym value,
                     eval_term(*(*literal.args)[position], binding, syms));
    key.push_back(value);
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
// have bound. That answer is the same every time the search arrives here. The
// steps run in a fixed order, each binds the variables its literal mentions,
// and extend() binds whichever assignments those make ready. None of it turns
// on the values matched. So the index is built once and reused for every
// partial instance that reaches this step.
// One interval or aggregate a step runs before it reads the store, and the
// slots it binds: an interval's variable, or an aggregate's value. One of
// `interval` and `aggregate` is set.
struct JoinItem {
  const NafLiteral* interval = nullptr;
  const Aggregate* aggregate = nullptr;
  absl::InlinedVector<size_t, 2> slots;
};

struct JoinStep {
  const ClassicalLiteral* literal = nullptr;
  std::vector<size_t> positions;
  // The arguments matching still has to work through. See match_plan().
  std::vector<size_t> plan;
  StepIndex index;
  bool ready = false;
  // Set when the predicate has no atoms at all, which means no instance can get
  // past this step.
  bool dead = false;
  // What the step runs before it reads the store, in the order plan_items()
  // put them in. Empty for nearly every step.
  std::vector<JoinItem> items;
};

// One join in progress: the body it is satisfying, the store it reads, and the
// steps it runs in order.
struct Join {
  const BodyParts& parts;
  const Store& store;
  Symbols& syms;
  // Why the body first dropped a ground instance, e.g. "'a' is not an
  // integer".
  std::optional<std::string> no_value_seen;
  std::vector<size_t> order;  // positive literal positions, delta first
  std::optional<size_t> delta_position;
  // The intervals and aggregates left for finish() to run. plan_items() takes
  // out the ones it gave to a step.
  std::vector<const NafLiteral*> intervals;
  std::vector<const Aggregate*> aggregates;
  std::vector<JoinStep> steps;  // one per entry of `order`, in that order

  // True when the status is a term with no value, which drops the instance it
  // came from. The first reason is kept for the warning.
  bool lost_instance(const absl::Status& status) {
    if (!has_no_value(status)) return false;
    if (!no_value_seen.has_value()) {
      no_value_seen = std::string(status.message());
    }
    return true;
  }
};

// One distinct tuple an aggregate's elements can produce, e.g. the [1] that
// both elements of '#count{ X : p(X) ; X : r(X) }' produce once p(1) and r(1)
// are derived.
struct AggTuple {
  Tuple tuple;
  BigInt weight;  // what the tuple adds to the aggregate's value
  // One body per grounding that puts the tuple in the set. Empty when every
  // grounding has an ill-formed 'not' literal.
  std::vector<std::vector<aspif::Lit>> supports;
};

// find_instances() and bind_agg_outputs() call each other: grounding an
// aggregate's elements needs find_instances() for the element conditions.
// Aggregates cannot nest, so the recursion stops one level down.
absl::StatusOr<std::vector<Instance>> bind_agg_outputs(
    Join& join, std::vector<Instance> instances);

// Defined with the rest of the aggregate code, below. The join reads them to
// run an aggregate whose value a literal waits for. See plan_items().
std::vector<size_t> agg_output_slots(const Aggregate& agg,
                                     const Binding& binding);
std::vector<size_t> agg_variable_slots(const Aggregate& agg,
                                       const Binding& binding);
absl::StatusOr<std::vector<AggTuple>> collect_agg_tuples(
    const Aggregate& agg, const Binding& outer_binding, const Store& store,
    Symbols& syms);
absl::StatusOr<std::vector<Sym>> possible_values(
    const Aggregate& agg, const std::vector<AggTuple>& tuples, Symbols& syms);

absl::Status extend(Join& join, size_t depth, Instance& instance,
                    const InstanceFn& emit);

/* Replaces each instance with one per value the interval of `item` holds, e.g.
   'p(X) :- X = 1..3.' turns one instance into three, binding X to 1, 2 and 3
   in turn.

   An interval whose term already has a value tests it instead. The X of
   "q :- p(1..3)." took its value from a stored p atom, so the instances whose
   X the interval does not hold are dropped.
*/
absl::StatusOr<std::vector<Instance>> expand_over_interval(
    const NafLiteral& item, Join& join,
    const std::vector<Instance>& instances) {
  const auto& builtin = static_cast<const BuiltinAtom&>(*item.literal);
  const IntervalSides sides = interval_sides(builtin);
  // Every instance binds the same variables, since they all come out of the
  // same body, so the first one answers for all of them.
  const Binding& sample = instances.front().binding;
  const bool generates =
      sides.other->kind == Term::VariableKind &&
      !sample.contains(static_cast<const Variable&>(*sides.other));

  std::vector<Instance> expanded;
  if (!generates) {
    for (const Instance& instance : instances) {
      absl::StatusOr<bool> holds =
          interval_holds(sides, instance.binding, join.syms);
      if (join.lost_instance(holds.status())) continue;
      RETURN_IF_ERROR(holds.status());
      if (*holds) expanded.push_back(instance);
    }
    return expanded;
  }

  const size_t slot =
      sample.slot_of(static_cast<const Variable&>(*sides.other));
  for (const Instance& instance : instances) {
    absl::Status listed = for_each_interval_value(
        *sides.interval, instance.binding, join.syms,
        [&](Sym value) -> absl::Status {
          Instance next = instance;
          next.binding.set(slot, value);
          // The value can complete an assignment, e.g. the 'Y = X + 1' of
          // 'p(X, Y) :- X = 1..3, Y = X + 1.'
          BindingTrail trail(next.binding);
          absl::Status bound = bind_assignments(join.parts, next.binding, trail,
                                                join.syms, next.settled);
          if (join.lost_instance(bound)) return absl::OkStatus();
          RETURN_IF_ERROR(bound);
          trail.keep();
          expanded.push_back(std::move(next));
          return absl::OkStatus();
        });
    // An end with no value, the 'a..3' of 'X = a..N' under {N: a}, leaves this
    // instance no values. The others keep theirs.
    if (join.lost_instance(listed)) continue;
    RETURN_IF_ERROR(listed);
  }
  return expanded;
}

// Runs every interval the join left, each one either binding its variable to
// one value at a time or testing the value something else bound.
absl::StatusOr<std::vector<Instance>> expand_intervals(
    Join& join, std::vector<Instance> instances) {
  std::vector<const NafLiteral*> pending = join.intervals;
  while (!pending.empty() && !instances.empty()) {
    // An interval's turn has come once its ends have values. Running another
    // one can be what settles them: the N of '_R1 = 1..N' can be the _R0 of
    // '_R0 = 1..3'. The first instance answers for all of them, since they all
    // come out of the same body.
    const Binding& sample = instances.front().binding;
    auto ready = [&](const NafLiteral* item) {
      const IntervalSides sides =
          interval_sides(static_cast<const BuiltinAtom&>(*item->literal));
      return is_bound(*sides.interval->lower, sample) &&
             is_bound(*sides.interval->upper, sample);
    };
    auto it = absl::c_find_if(pending, ready);
    // Intervals whose ends read each other can never run. verify_safe()
    // rejects such a rule. Stopping here leaves the variables unbound, which
    // reports it as well.
    if (it == pending.end()) break;

    const NafLiteral& item = **it;
    pending.erase(it);
    ASSIGN_OR_RETURN(instances, expand_over_interval(item, join, instances));
  }
  return instances;
}

/* Hands `emit` the instances that survive the parts of the body that are
   decided once every positive literal has matched: the intervals, the
   aggregates, and the comparisons. extend() has made the assignments, and the
   steps have run whatever the join itself waited on.

   An interval stands for several values, and an aggregate that binds a
   variable to its value, e.g. '#count{X : p(X)} = S', for several answers. So
   each of them splits the instance into one per value, which is the one part
   of the search that works on copies.
*/
absl::Status finish(Join& join, Instance& instance, const InstanceFn& emit) {
  if (join.aggregates.empty() && join.intervals.empty()) {
    // Every variable the body binds has a value by now, so all the comparisons
    // are decidable.
    absl::StatusOr<bool> holds = comparisons_hold(
        join.parts, instance.binding, join.syms, instance.settled);
    if (join.lost_instance(holds.status())) return absl::OkStatus();
    RETURN_IF_ERROR(holds.status());
    if (!*holds) return absl::OkStatus();
    absl::Status emitted = emit(instance);
    if (join.lost_instance(emitted)) return absl::OkStatus();
    return emitted;
  }

  // An expansion that loses a term's value loses this instance, not the whole
  // grounding run, so neither of these propagates its status.
  std::vector<Instance> instances{instance};
  if (!join.intervals.empty()) {
    // An interval runs before the aggregates, since an aggregate inside a rule
    // holding one is a different aggregate under each of its values.
    absl::StatusOr<std::vector<Instance>> expanded =
        expand_intervals(join, std::move(instances));
    if (join.lost_instance(expanded.status())) return absl::OkStatus();
    RETURN_IF_ERROR(expanded.status());
    instances = *std::move(expanded);
  }
  if (!join.aggregates.empty()) {
    absl::StatusOr<std::vector<Instance>> expanded =
        bind_agg_outputs(join, std::move(instances));
    if (join.lost_instance(expanded.status())) return absl::OkStatus();
    RETURN_IF_ERROR(expanded.status());
    instances = *std::move(expanded);
  }

  for (const Instance& next : instances) {
    absl::StatusOr<bool> holds =
        comparisons_hold(join.parts, next.binding, join.syms, next.settled);
    if (join.lost_instance(holds.status())) continue;
    RETURN_IF_ERROR(holds.status());
    if (!*holds) continue;
    absl::Status emitted = emit(next);
    if (join.lost_instance(emitted)) continue;
    RETURN_IF_ERROR(emitted);
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
  step.plan = match_plan(literal, step.positions);
  step.index = build_step_index(
      *data, scan_range(*data, join.delta_position, position), step.positions,
      literal.args == nullptr ? 0 : literal.args->size());
}

// The atoms `index` can match under `key`. An index naming one atom has
// nowhere to keep it, so `one` holds it for the length of the call.
absl::Span<const GroundAtom* const> candidates(const StepIndex& index,
                                               const Tuple& key,
                                               const GroundAtom** one) {
  if (index.kind == StepIndex::Kind::kOne) {
    *one = index.data->find_within(key, index.range.begin, index.range.end);
    if (*one == nullptr) return {};
    return absl::MakeConstSpan(one, 1);
  }
  const GroupedIndex& grouped = *index.grouped;
  if (index.kind == StepIndex::Kind::kEvery) {
    return absl::MakeConstSpan(grouped.members);
  }
  auto it = grouped.runs.find(key);
  if (it == grouped.runs.end()) return {};
  return absl::MakeConstSpan(grouped.members)
      .subspan(it->second.begin, it->second.count);
}

absl::Status run_step(Join& join, size_t depth, Instance& instance,
                      const InstanceFn& emit);

absl::Status run_items(Join& join, size_t depth, size_t k, Instance& instance,
                       const InstanceFn& emit);

// Runs the interval `item` stands for, one value at a time, and the items
// after it under each of those.
absl::Status run_interval_item(Join& join, size_t depth, size_t k,
                               const JoinItem& item, Instance& instance,
                               const InstanceFn& emit) {
  const IntervalSides sides =
      interval_sides(static_cast<const BuiltinAtom&>(*item.interval->literal));
  const size_t slot = item.slots.front();
  absl::Status listed = for_each_interval_value(
      *sides.interval, instance.binding, join.syms,
      [&](Sym value) -> absl::Status {
        BindingTrail trail(instance.binding);
        instance.binding.set(slot, value);
        trail.record(slot);
        // The value can complete an assignment, e.g. the 'X = _R0' normalize()
        // writes 'X = 1..3' as.
        absl::Status bound = bind_assignments(
            join.parts, instance.binding, trail, join.syms, instance.settled);
        if (join.lost_instance(bound)) return absl::OkStatus();
        RETURN_IF_ERROR(bound);
        return run_items(join, depth, k + 1, instance, emit);
      });
  // An end with no value, the 'a..3' of 'X = a..N' under {N: a}, leaves this
  // instance no values at all.
  if (join.lost_instance(listed)) return absl::OkStatus();
  return listed;
}

// Runs the aggregate `item` stands for, one of the values it can take at a
// time. Each value is a rule instance of its own, the one an answer set giving
// the aggregate that value satisfies.
absl::Status run_aggregate_item(Join& join, size_t depth, size_t k,
                                const JoinItem& item, Instance& instance,
                                const InstanceFn& emit) {
  // An assignment can have bound the value by now, which leaves the aggregate
  // a plain check for emit_rules to make.
  absl::InlinedVector<size_t, 2> outputs;
  for (size_t slot : item.slots) {
    if (instance.binding.at(slot) == kNoSym) outputs.push_back(slot);
  }
  if (outputs.empty()) return run_items(join, depth, k + 1, instance, emit);

  absl::StatusOr<std::vector<AggTuple>> tuples = collect_agg_tuples(
      *item.aggregate, instance.binding, join.store, join.syms);
  if (join.lost_instance(tuples.status())) return absl::OkStatus();
  RETURN_IF_ERROR(tuples.status());
  absl::StatusOr<std::vector<Sym>> values =
      possible_values(*item.aggregate, *tuples, join.syms);
  if (join.lost_instance(values.status())) return absl::OkStatus();
  RETURN_IF_ERROR(values.status());

  for (Sym value : *values) {
    BindingTrail trail(instance.binding);
    for (size_t slot : outputs) {
      instance.binding.set(slot, value);
      trail.record(slot);
    }
    // The value can complete an assignment, e.g. the 'T = S + 1' of
    // 'q(T) :- #count{X : p(X)} = S, T = S + 1.' A value the assignment cannot
    // use, e.g. a #min that comes out a constant, drops that value alone.
    absl::Status bound = bind_assignments(
        join.parts, instance.binding, trail, join.syms, instance.settled);
    if (join.lost_instance(bound)) continue;
    RETURN_IF_ERROR(bound);
    RETURN_IF_ERROR(run_items(join, depth, k + 1, instance, emit));
  }
  return absl::OkStatus();
}

// Runs the items of the step at `depth` from `k` on, and the step itself once
// each of them has bound its variable.
absl::Status run_items(Join& join, size_t depth, size_t k, Instance& instance,
                       const InstanceFn& emit) {
  const std::vector<JoinItem>& items = join.steps[depth].items;
  if (k == items.size()) return run_step(join, depth, instance, emit);
  if (items[k].interval != nullptr) {
    return run_interval_item(join, depth, k, items[k], instance, emit);
  }
  return run_aggregate_item(join, depth, k, items[k], instance, emit);
}

// Extends `instance` with every stored atom the step at `depth` can match, and
// recurses into the step after it.
absl::Status extend(Join& join, size_t depth, Instance& instance,
                    const InstanceFn& emit) {
  // The steps so far can have given an assignment its value, e.g. the 'Y = -X'
  // of "pred(C,Y) :- inClause(C,X), nVar(Y), Y = -X." once inClause has bound
  // X. Binding it here lets the step below look nVar up by Y rather than read
  // every atom it holds. The trail undoes it on the way back out.
  BindingTrail trail(instance.binding);
  if (!join.parts.comparisons.empty()) {
    absl::Status bound = bind_assignments(
        join.parts, instance.binding, trail, join.syms, instance.settled);
    if (join.lost_instance(bound)) return absl::OkStatus();
    RETURN_IF_ERROR(bound);
  }

  if (depth == join.order.size()) return finish(join, instance, emit);

  // The intervals and aggregates the step waits on come before it: the literal
  // it matches can be reading a variable one of them binds.
  if (!join.steps[depth].items.empty()) {
    return run_items(join, depth, 0, instance, emit);
  }
  return run_step(join, depth, instance, emit);
}

// Matches the step at `depth` against the store and recurses into the step
// after it, with every variable it reads bound.
absl::Status run_step(Join& join, size_t depth, Instance& instance,
                      const InstanceFn& emit) {
  if (!join.steps[depth].ready) prepare_step(join, depth, instance.binding);
  const JoinStep& step = join.steps[depth];
  if (step.dead) return absl::OkStatus();

  absl::StatusOr<Tuple> key =
      probe_key(*step.literal, step.positions, instance.binding, join.syms);
  // A probed argument with no value, e.g. the 'X / 0' of 'r(X / 0, Y)', names
  // no atom, so nothing gets past this step.
  if (join.lost_instance(key.status())) return absl::OkStatus();
  RETURN_IF_ERROR(key.status());
  const GroundAtom* one = nullptr;
  // The candidates already agree on the probed positions, which is why the
  // step's plan leaves those out: matching is here to bind the variables in the
  // positions still open.
  for (const GroundAtom* atom : candidates(step.index, *key, &one)) {
    BindingTrail trail(instance.binding);
    absl::StatusOr<bool> ok = match_args(*step.literal, step.plan, atom->args,
                                         instance.binding, trail, join.syms);
    // An argument with no value matches no atom at all, so the step is over,
    // not just this candidate.
    if (join.lost_instance(ok.status())) return absl::OkStatus();
    RETURN_IF_ERROR(ok.status());
    if (!*ok) continue;
    instance.matched.push_back(atom->id);
    absl::Status status = extend(join, depth + 1, instance, emit);
    instance.matched.pop_back();
    RETURN_IF_ERROR(status);
  }
  return absl::OkStatus();
}

/* One thing in the body that gives a variable a value, and what it reads to
   do it. An assignment binds its variable and reads the other side of the '='.
   An interval binds its variable and reads its ends. An aggregate binds its
   value and reads what it counts over.

   `interval` and `aggregate` name the ones a step can run. An assignment has
   neither, since bind_assignments() makes those as the join goes.
*/
struct Provider {
  const NafLiteral* interval = nullptr;
  const Aggregate* aggregate = nullptr;
  absl::InlinedVector<size_t, 2> binds;
  absl::InlinedVector<size_t, 4> reads;
};

std::vector<Provider> body_providers(const BodyParts& parts,
                                     const Binding& binding) {
  std::vector<Provider> providers;
  auto read = [&](Provider& provider, const Term& term) {
    collect::for_each_variable(term, [&](const Variable& var) {
      provider.reads.push_back(binding.slot_of(var));
    });
  };
  for (const NafLiteral* item : parts.comparisons) {
    const std::optional<Assignment> assignment = assignment_of(*item, binding);
    if (!assignment.has_value()) continue;
    Provider& provider = providers.emplace_back();
    provider.binds.push_back(binding.slot_of(*assignment->variable));
    read(provider, *assignment->value);
  }
  for (const NafLiteral* item : parts.intervals) {
    const IntervalSides sides =
        interval_sides(static_cast<const BuiltinAtom&>(*item->literal));
    if (sides.other->kind != Term::VariableKind) continue;
    Provider& provider = providers.emplace_back();
    provider.interval = item;
    provider.binds.push_back(
        binding.slot_of(static_cast<const Variable&>(*sides.other)));
    read(provider, *sides.interval);
  }
  for (const Aggregate* agg : parts.aggregates) {
    const std::vector<size_t> outputs = agg_output_slots(*agg, binding);
    if (outputs.empty()) continue;
    Provider& provider = providers.emplace_back();
    provider.aggregate = agg;
    provider.binds.assign(outputs.begin(), outputs.end());
    for (size_t slot : agg_variable_slots(*agg, binding)) {
      if (!absl::c_linear_search(outputs, slot)) provider.reads.push_back(slot);
    }
  }
  return providers;
}

/* Works out what each step of the join runs before it reads the store.

   An interval and an aggregate each bind their variable one value at a time,
   which is why finish() runs them once the join is done. A literal reading
   such a variable under arithmetic has nothing to read there, so
   "p(X) :- X = 1..3, q(X*2)." leaves X unbound at q under a plain join.

   The ones the join waits on move into it, each to the first step whose
   matching gives it everything it reads. That step binds it one value at a
   time before reading the store, so 'q(X*2)' probes by a value it has.

   The rest stay with finish(). An interval over a variable the join binds
   itself, the 'X = 1..3' of "p(X) :- q(X), X = 1..3.", is a test on the value
   q matched, and running it first would fan the join out over values q never
   holds. So is an item no step makes ready, which takes a body binding its
   variables in a cycle. verify_safe() rejects those.
*/
void plan_items(Join& join, const Binding& seed) {
  const BodyParts& parts = join.parts;
  if (parts.intervals.empty() && parts.aggregates.empty()) return;

  // What the literals bind, and what they read under arithmetic and cannot
  // bind. Only an interval or an aggregate can give the second kind a value.
  std::vector<LiteralVars> vars;
  vars.reserve(parts.positive.size());
  absl::flat_hash_set<size_t> literal_binds;
  for (const ClassicalLiteral* literal : parts.positive) {
    vars.push_back(literal_vars(*literal, seed));
    for (size_t slot : vars.back().binds) {
      if (seed.at(slot) == kNoSym) literal_binds.insert(slot);
    }
  }
  absl::flat_hash_set<size_t> wanted;
  for (const LiteralVars& literal : vars) {
    for (size_t slot : literal.needs) {
      if (seed.at(slot) == kNoSym && !literal_binds.contains(slot)) {
        wanted.insert(slot);
      }
    }
  }
  if (wanted.empty()) return;

  std::vector<Provider> providers = body_providers(parts, seed);
  absl::flat_hash_set<size_t> provided;
  for (const Provider& provider : providers) {
    provided.insert(provider.binds.begin(), provider.binds.end());
  }
  // A variable nothing in the body binds is one an aggregate's element keeps
  // to itself, e.g. the Y of '#count{Y : m(X, Y)}'. Waiting for it would wait
  // forever, so it is no part of what a provider reads.
  for (Provider& provider : providers) {
    absl::InlinedVector<size_t, 4> kept;
    for (size_t slot : provider.reads) {
      if (seed.at(slot) != kNoSym) continue;
      if (provided.contains(slot) || literal_binds.contains(slot)) {
        kept.push_back(slot);
      }
    }
    provider.reads = std::move(kept);
  }
  // What a wanted variable is bound from is wanted in turn. That is the _R0
  // of the 'X = _R0' normalize() writes 'X = 1..3' as, the N of an 'X = 1..N'
  // whose end is a #count, and the X a wanted '#count{Y : m(X, Y)}' counts
  // under.
  for (bool grew = true; grew;) {
    grew = false;
    for (const Provider& provider : providers) {
      if (!absl::c_any_of(provider.binds, [&](size_t slot) {
            return wanted.contains(slot);
          })) {
        continue;
      }
      // A slot a literal binds is not wanted: the join gives it a value, and
      // whatever reads it runs on that value.
      for (size_t slot : provider.reads) {
        if (literal_binds.contains(slot)) continue;
        if (wanted.insert(slot).second) grew = true;
      }
    }
  }

  std::vector<const Provider*> pending;
  for (const Provider& provider : providers) {
    if (provider.interval == nullptr && provider.aggregate == nullptr) continue;
    if (absl::c_any_of(provider.binds,
                       [&](size_t slot) { return wanted.contains(slot); })) {
      pending.push_back(&provider);
    }
  }
  if (pending.empty()) return;

  // The steps run in a fixed order and bind the same slots every time, so
  // which step an item lands on is settled here, before the search starts.
  absl::flat_hash_set<size_t> bound;
  auto has_value = [&](size_t slot) {
    return seed.at(slot) != kNoSym || bound.contains(slot);
  };
  // An item's value reaches the rest of the body through an assignment, the
  // 'X = _R0' an interval is written as. The walk makes those where the join
  // makes them.
  auto settle_assignments = [&]() {
    for (bool grew = true; grew;) {
      grew = false;
      for (const Provider& provider : providers) {
        if (provider.interval != nullptr || provider.aggregate != nullptr) {
          continue;
        }
        if (has_value(provider.binds.front())) continue;
        if (!absl::c_all_of(provider.reads, has_value)) continue;
        bound.insert(provider.binds.front());
        grew = true;
      }
    }
  };
  settle_assignments();
  for (size_t depth = 0; depth < join.order.size() && !pending.empty();
       ++depth) {
    // One item's value can make the next one ready, so this takes them until
    // nothing more is.
    for (bool took = true; took;) {
      took = false;
      for (size_t k = 0; k < pending.size(); ++k) {
        const Provider& item = *pending[k];
        if (!absl::c_all_of(item.reads, has_value)) continue;
        join.steps[depth].items.push_back(JoinItem{.interval = item.interval,
                                                   .aggregate = item.aggregate,
                                                   .slots = item.binds});
        if (item.interval != nullptr) {
          std::erase(join.intervals, item.interval);
        } else {
          std::erase(join.aggregates, item.aggregate);
        }
        bound.insert(item.binds.begin(), item.binds.end());
        settle_assignments();
        pending.erase(pending.begin() + k);
        took = true;
        break;
      }
    }
    for (size_t slot : vars[join.order[depth]].binds) bound.insert(slot);
    settle_assignments();
  }
}

// Hands `emit` every way to satisfy the body with the atoms currently in the
// store. Works through the positive literals one at a time, matching each
// against the store and backtracking, then keeps the instances whose
// assignments, aggregates, and comparisons work out. 'not' literals never
// filter here; the caller decides what to do with them.
//
// An interval or aggregate the join reads but cannot bind runs inside it, at
// the step that makes it ready. See plan_items().
//
// `seed` is the binding to start from: an empty one for a rule, or, for an
// aggregate element's condition, the enclosing rule instance's binding, so that
// the condition sees the variables the rule already bound.
//
// `delta_position` makes the join semi-naive: see scan_range. Only
// derive_atoms() passes one; every other caller wants every instance the store
// supports.
//
// Comes back with why the body dropped a ground instance, for the caller to
// warn about.
absl::StatusOr<std::optional<std::string>> find_instances(
    const BodyParts& parts, const Store& store, Symbols& syms, Binding seed,
    const InstanceFn& emit,
    std::optional<size_t> delta_position = std::nullopt) {
  Instance instance{.binding = std::move(seed),
                    .matched = {},
                    .settled = std::vector<bool>(parts.comparisons.size())};

  // The assignments that stand on their own, e.g. the 'X = 2' of
  // "p(X) :- q(X+1), X = 2.", are made before the join, so that a literal
  // reading X has it. The ones waiting on a variable the join binds are left
  // to finish(). An assignment made here holds for every instance the join
  // finds, so nothing undoes it.
  BindingTrail trail(instance.binding);
  absl::Status bound = bind_assignments(parts, instance.binding, trail, syms,
                                        instance.settled);
  if (has_no_value(bound)) return std::optional(std::string(bound.message()));
  RETURN_IF_ERROR(bound);
  trail.keep();

  Join join{.parts = parts,
            .store = store,
            .syms = syms,
            .delta_position = delta_position,
            .intervals = parts.intervals,
            .aggregates = parts.aggregates};
  join.order = join_order(parts, instance.binding, store, delta_position);
  join.steps.resize(join.order.size());
  plan_items(join, instance.binding);
  RETURN_IF_ERROR(extend(join, 0, instance, emit));
  return join.no_value_seen;
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
    ASSIGN_OR_RETURN(Tuple tuple, eval_terms(literal.args, binding, syms));
    const GroundAtom* atom = data->find(tuple);
    if (atom != nullptr) matched.push_back(atom->id);
    return matched;
  }
  // Matching binds the open arguments' variables, e.g. the X of 'r(X, _)' when
  // X has no value yet, which this literal's own scope has no use for, so it
  // happens on a scratch copy.
  Binding scratch = binding;
  const std::vector<size_t> positions = probeable_positions(literal, scratch);
  const size_t arity = literal.args == nullptr ? 0 : literal.args->size();
  const StepIndex index = build_step_index(
      *data, AtomRange{.begin = 0, .end = data->atoms.size()}, positions,
      arity);
  const std::vector<size_t> plan = match_plan(literal, positions);
  ASSIGN_OR_RETURN(Tuple key, probe_key(literal, positions, scratch, syms));
  const GroundAtom* one = nullptr;
  for (const GroundAtom* atom : candidates(index, key, &one)) {
    BindingTrail trail(scratch);
    ASSIGN_OR_RETURN(bool ok,
                     match_args(literal, plan, atom->args, scratch, trail, syms));
    if (ok) matched.push_back(atom->id);
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

// What one group of body items comes to against the store as it stands. The
// 'not' literals of a body have one of these and its aggregates another.
enum class BodyValue {
  // No answer set satisfies them, so neither does the body.
  kFalse,
  // The solver has the say.
  kOpen,
  // They hold in every answer set, so they ask nothing of the body.
  kTrue,
};

// Reads the 'not' literals of a body, which is what gives them their say in
// which atoms can exist. `lits`, when given, collects the negation of every
// atom still open, which is what an emitted rule body needs. A null `lits`
// asks for the verdict alone and stops at the first fact.
//
// kFalse is a 'not' over a fact. An atom stays a fact once it is one, so a
// later pass never takes that answer back. kTrue is a 'not' matching no atom
// the store holds, which is final only for some rules: see
// RuleView::negation_is_settled.
//
// A literal that cannot match, e.g. 'not p(X / 0)', comes back a no-value
// status: the rule instance does not exist at all, rather than existing
// without it.
absl::StatusOr<BodyValue> negation_value(
    const std::vector<const ClassicalLiteral*>& negative,
    const Binding& binding, const Store& store, Symbols& syms,
    std::vector<aspif::Lit>* lits = nullptr) {
  BodyValue value = BodyValue::kTrue;
  for (const ClassicalLiteral* literal : negative) {
    RETURN_IF_ERROR(args_can_match(*literal, binding, syms));
    ASSIGN_OR_RETURN(std::vector<aspif::Atom> matched,
                     matching_atoms(*literal, binding, store, syms));
    for (aspif::Atom atom : matched) {
      if (store.is_fact(atom)) return BodyValue::kFalse;
      value = BodyValue::kOpen;
      if (lits != nullptr) lits->push_back(-atom);
    }
  }
  return value;
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
// set, which is a 'not' over a fact.
absl::StatusOr<std::optional<std::vector<aspif::Lit>>> negative_lits(
    const std::vector<const ClassicalLiteral*>& negative,
    const Binding& binding, const Store& store, Symbols& syms) {
  std::vector<aspif::Lit> lits;
  ASSIGN_OR_RETURN(const BodyValue value,
                   negation_value(negative, binding, store, syms, &lits));
  if (value == BodyValue::kFalse) return std::nullopt;
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

// Whether `function` is #min or #max, the two aggregates whose value is a term
// rather than a number.
bool is_minmax(AggregateFunctionType function) {
  return function == AggregateFunctionType::kAGGREGATE_MIN ||
         function == AggregateFunctionType::kAGGREGATE_MAX;
}

// What a #min or #max evaluates to: one of the terms its tuples produced, or
// the infinity an empty set gives. ASP-Core-2 puts -infinity below every term
// and +infinity above every one. Neither equals a term.
struct MinMaxValue {
  enum Kind { kNegInf, kTerm, kPosInf };

  Kind kind;
  Sym term = kNoSym;  // set only when kind == kTerm
};

// Whether 'value op guard' holds, e.g. true for the 'a >= 1' of a #max over
// the terms 1 and a.
bool minmax_holds(BinopType op, const MinMaxValue& value, Sym guard,
                  const Symbols& syms) {
  switch (value.kind) {
    case MinMaxValue::kTerm:
      return builtin_holds(op, value.term, guard, syms);
    case MinMaxValue::kPosInf:
      return op == BinopType::kGREATER || op == BinopType::kGREATER_OR_EQ ||
             op == BinopType::kUNEQUAL;
    case MinMaxValue::kNegInf:
      return op == BinopType::kLESS || op == BinopType::kLESS_OR_EQ ||
             op == BinopType::kUNEQUAL;
  }
  return false;  // unreachable
}

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
    // A term with no value drops an element's tuple, not the enclosing rule's
    // instance, so there is nothing to warn about here.
    RETURN_IF_ERROR(
        find_instances(
            parts, store, syms, outer_binding,
            [&](const Instance& instance) -> absl::Status {
              // An element whose terms have no value under this local binding
              // contributes no tuple to the set.
              ASSIGN_OR_RETURN(Tuple tuple, eval_terms(element.terms,
                                                       instance.binding, syms));
              BigInt weight = 1;
              if (agg.function == AggregateFunctionType::kAGGREGATE_SUM) {
                // #sum adds up the tuples whose first term is an integer and
                // ignores the others, e.g. '#sum{ 1 : p; a : q }' is just 1. A
                // tuple that adds nothing needs no literal in the weight body
                // at all. (#count does count such a tuple, which is why this
                // only applies to #sum.)
                if (tuple.empty() || !syms.is_number(tuple[0])) {
                  return absl::OkStatus();
                }
                weight = syms.number_of(tuple[0]);
              } else if (is_minmax(agg.function) && tuple.empty()) {
                // #min and #max read a tuple's first term, so a tuple with
                // no terms is in neither. An element that is all condition,
                // like the '#min{ : p }' counting whether p holds, produces
                // one.
                return absl::OkStatus();
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
            })
            .status());
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

// Whether the set an aggregate ranges over is the same in every answer set. A
// tuple is settled when one of its supports came out empty: every literal in
// that support was a fact, so the tuple is in the set whatever the solver
// decides. One tuple whose supports all carry a literal is enough to leave the
// set open.
//
// Element conditions must be free of 'not' for any of this to hold. safety.cc
// keeps an aggregate's un-negated predicates out of the rule's own component,
// so their atoms and facts are settled before this rule is ever ground. A
// negated one can still share that component, which unstratified negation
// brings about, and would read here as a support that nothing can take away.
bool set_is_settled(const Aggregate& agg, const std::vector<AggTuple>& tuples) {
  if (elements_use_negation(agg)) return false;
  for (const AggTuple& tuple : tuples) {
    bool settled = false;
    for (const std::vector<aspif::Lit>& support : tuple.supports) {
      if (support.empty()) {
        settled = true;
        break;
      }
    }
    if (!settled) return false;
  }
  return true;
}

// The value a #count or #sum takes in every answer set, when grounding can work
// that out on its own, e.g. the 2 of '#count{ X : p(X) }' over the facts p(1)
// and p(2). Nullopt means the solver still has a say.
std::optional<BigInt> settled_agg_value(const Aggregate& agg,
                                        const std::vector<AggTuple>& tuples) {
  if (!set_is_settled(agg, tuples)) return std::nullopt;
  BigInt value;
  for (const AggTuple& tuple : tuples) value += tuple.weight;
  return value;
}

// The value a #min or #max takes in every answer set, worked out the same way,
// over the term order instead of a sum. An empty set is an infinity.
std::optional<MinMaxValue> settled_minmax_value(
    const Aggregate& agg, const std::vector<AggTuple>& tuples,
    const Symbols& syms) {
  if (!set_is_settled(agg, tuples)) return std::nullopt;
  const bool want_min = agg.function == AggregateFunctionType::kAGGREGATE_MIN;
  MinMaxValue value{.kind =
                        want_min ? MinMaxValue::kPosInf : MinMaxValue::kNegInf};
  for (const AggTuple& tuple : tuples) {
    const Sym first = tuple.tuple.front();
    if (value.kind != MinMaxValue::kTerm) {
      value = MinMaxValue{.kind = MinMaxValue::kTerm, .term = first};
      continue;
    }
    const int c = syms.compare(first, value.term);
    if (want_min ? c < 0 : c > 0) value.term = first;
  }
  return value;
}

// A cap on how many values an aggregate may bind a variable to: each value
// costs one ground instance of the rule.
constexpr size_t kMaxAggregateValues = 4096;

// The terms a #min or #max can take, in ascending order. Its value is the first
// term of one of its tuples.
//
// An empty set gives an infinity, which is no term of the program and equal to
// none, so it binds nothing: 'q(S) :- #min{ X : p(X) } = S.' derives no q when
// nothing supports p.
absl::StatusOr<std::vector<Sym>> minmax_values(
    const Aggregate& agg, const std::vector<AggTuple>& tuples, Symbols& syms) {
  std::optional<MinMaxValue> settled = settled_minmax_value(agg, tuples, syms);
  if (settled.has_value()) {
    if (settled->kind != MinMaxValue::kTerm) return std::vector<Sym>{};
    return std::vector<Sym>{settled->term};
  }

  std::vector<Sym> values;
  for (const AggTuple& tuple : tuples) values.push_back(tuple.tuple.front());
  std::sort(values.begin(), values.end(),
            [&syms](Sym a, Sym b) { return syms.compare(a, b) < 0; });
  values.erase(std::unique(values.begin(), values.end()), values.end());
  if (values.size() > kMaxAggregateValues) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "an aggregate whose value binds a variable can take more than ",
        kMaxAggregateValues, " different values"));
  }
  return values;
}

// The values an aggregate can take, in ascending order. Which tuples are in
// the set is up to the solver, so the value is the sum of the weights of some
// subset of them, e.g. {0, 1, 2} for a #count over two tuples and {0, 3, 5, 8}
// for a #sum over tuples weighing 3 and 5.
//
// A value no answer set reaches, e.g. the 0 of a #count over a tuple backed
// by a fact, is harmless: its rule instance carries the literals that check
// for that value, which no answer set satisfies.
absl::StatusOr<std::vector<Sym>> possible_values(
    const Aggregate& agg, const std::vector<AggTuple>& tuples, Symbols& syms) {
  if (is_minmax(agg.function)) return minmax_values(agg, tuples, syms);

  // An aggregate grounding has settled takes one value and no other, e.g. the
  // 2 that 'q(S) :- #count{ X : p(X) } = S.' binds S to over two p facts.
  std::optional<BigInt> settled = settled_agg_value(agg, tuples);
  if (settled.has_value()) return std::vector<Sym>{syms.number(*settled)};
  absl::btree_set<BigInt> reachable = {BigInt()};
  for (const AggTuple& tuple : tuples) {
    absl::btree_set<BigInt> extended = reachable;
    for (const BigInt& sum : reachable) extended.insert(sum + tuple.weight);
    if (extended.size() > kMaxAggregateValues) {
      return absl::ResourceExhaustedError(absl::StrCat(
          "an aggregate whose value binds a variable can take more than ",
          kMaxAggregateValues, " different values"));
    }
    reachable = std::move(extended);
  }
  std::vector<Sym> values;
  values.reserve(reachable.size());
  for (const BigInt& value : reachable) values.push_back(syms.number(value));
  return values;
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
    const Aggregate& agg, const std::vector<size_t>& outputs, Join& join,
    const std::vector<Instance>& instances) {
  const Store& store = join.store;
  Symbols& syms = join.syms;
  std::vector<Instance> expanded;
  for (const Instance& instance : instances) {
    ASSIGN_OR_RETURN(std::vector<AggTuple> tuples,
                     collect_agg_tuples(agg, instance.binding, store, syms));
    ASSIGN_OR_RETURN(std::vector<Sym> values,
                     possible_values(agg, tuples, syms));
    for (Sym value : values) {
      Instance next = instance;
      for (size_t slot : outputs) next.binding.set(slot, value);
      // The value can complete an assignment, e.g. the 'T = S + 1' of
      // 'q(T) :- #count{X : p(X)} = S, T = S + 1.'
      BindingTrail trail(next.binding);
      // A value the assignment cannot use, e.g. a #min that comes out a
      // constant in 'T = S + 1', drops this value's instance. The other values
      // keep theirs.
      absl::Status bound = bind_assignments(join.parts, next.binding, trail,
                                            syms, next.settled);
      if (join.lost_instance(bound)) continue;
      RETURN_IF_ERROR(bound);
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
    Join& join, std::vector<Instance> instances) {
  std::vector<const Aggregate*> pending = join.aggregates;
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
      ASSIGN_OR_RETURN(instances,
                       expand_over_values(*agg, outputs, join, instances));
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

// The literal meaning "some tuple whose first term `wanted` accepts is in the
// set". A fresh atom with one rule per accepted tuple, which is what gives it
// the "some".
//
// No accepted tuple leaves the atom with no rule at all, so it is false. That
// is where an empty set's infinities come from: '#min{ } <= u' is this literal
// and so fails, and '#min{ } >= u' negates it and so holds.
aspif::Lit some_tuple_in_set(const std::vector<AggTuple>& tuples,
                             const std::vector<aspif::WeightedLit>& lits,
                             absl::FunctionRef<bool(Sym)> wanted,
                             aspif::Program& result) {
  std::vector<aspif::Lit> accepted;
  for (size_t i = 0; i < tuples.size(); ++i) {
    if (wanted(tuples[i].tuple.front())) accepted.push_back(lits[i].lit);
  }
  // One accepted tuple needs no atom of its own: its literal already says
  // exactly "this tuple is in the set", and it is the only one that could be.
  if (accepted.size() == 1) return accepted.front();

  aspif::Atom atom = result.new_atom();
  for (aspif::Lit lit : accepted) {
    aspif::Rule rule;
    rule.head = {atom};
    rule.body = {lit};
    result.rules.push_back(std::move(rule));
  }
  return atom;
}

// The comparison that holds exactly when `op` fails, e.g. '<' for '>='.
BinopType negated(BinopType op) {
  switch (op) {
    case BinopType::kLESS:
      return BinopType::kGREATER_OR_EQ;
    case BinopType::kLESS_OR_EQ:
      return BinopType::kGREATER;
    case BinopType::kGREATER:
      return BinopType::kLESS_OR_EQ;
    case BinopType::kGREATER_OR_EQ:
      return BinopType::kLESS;
    case BinopType::kEQUAL:
      return BinopType::kUNEQUAL;
    case BinopType::kUNEQUAL:
      return BinopType::kEQUAL;
  }
  return op;  // unreachable
}

// The literal standing for one comparison of a #min or #max value against
// `guard`, e.g. the '> 3' of '#min{ X : p(X) } > 3'. Only the four ordering
// comparisons come here. '=' and '!=' are built out of two of these.
//
// A #min is at or below `guard` exactly when some tuple is, and above it
// exactly when no tuple is at or below it, so one search over the tuples
// answers either way round. A #max mirrors both.
aspif::Lit minmax_compare(bool is_min, BinopType op, Sym guard,
                          const std::vector<AggTuple>& tuples,
                          const std::vector<aspif::WeightedLit>& lits,
                          const Symbols& syms, aspif::Program& result) {
  const bool wants_larger =
      op == BinopType::kGREATER || op == BinopType::kGREATER_OR_EQ;
  // A #min searches for the tuples that break a lower guard, and a #max for
  // those that break an upper one. Both search for the opposite comparison.
  const bool negate = is_min == wants_larger;
  const BinopType search = negate ? negated(op) : op;
  const aspif::Lit some = some_tuple_in_set(
      tuples, lits,
      [&](Sym value) { return builtin_holds(search, value, guard, syms); },
      result);
  return negate ? -some : some;
}

// One comparison a #min or #max value must pass, read as 'value op term'. The
// lower side of a '3 <= #min{...}' is swapped round into '>= 3', so both of an
// aggregate's guards read the same way.
struct Guard {
  BinopType op;
  Sym term;
};

// The same comparison read from the other side, e.g. '>=' for '<='.
BinopType with_sides_swapped(BinopType op) {
  switch (op) {
    case BinopType::kLESS:
      return BinopType::kGREATER;
    case BinopType::kLESS_OR_EQ:
      return BinopType::kGREATER_OR_EQ;
    case BinopType::kGREATER:
      return BinopType::kLESS;
    case BinopType::kGREATER_OR_EQ:
      return BinopType::kLESS_OR_EQ;
    case BinopType::kEQUAL:
    case BinopType::kUNEQUAL:
      return op;
  }
  return op;  // unreachable
}

// The aggregate bound requirements accumulated from its (up to two)
// 'term binop' sides, e.g. "3 <= #count{...} < 7" contributes lower = 3 (from
// '3 <=') and upper = 6 (from '< 7').
struct AggBounds {
  std::optional<BigInt> lower;    // the aggregate's value must be >= this.
  std::optional<BigInt> upper;    // the aggregate's value must be <= this.
  std::vector<BigInt> not_equal;  // the aggregate's value must differ from
                                  // each of these.
  // Whether a bound rules every value out, which only a bound that is not a
  // number can do. See apply_non_numeric_bound().
  bool unsatisfiable = false;

  void apply_lower(const BigInt& k) {
    lower = lower.has_value() ? std::max(*lower, k) : k;
  }
  void apply_upper(const BigInt& k) {
    upper = upper.has_value() ? std::min(*upper, k) : k;
  }

  // Whether a value meets every bound collected here.
  bool hold_for(const BigInt& value) const {
    if (unsatisfiable) return false;
    if (lower.has_value() && value < *lower) return false;
    if (upper.has_value() && value > *upper) return false;
    for (const BigInt& k : not_equal) {
      if (value == k) return false;
    }
    return true;
  }
};

// Folds the upper-bound side ('AGG op k', e.g. 'AGG <= 7') into `bounds`.
void apply_upper_bound(const BigInt& k, BinopType op, AggBounds& bounds) {
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
// Reading it from the other side turns it into an upper-bound comparison.
void apply_lower_bound(const BigInt& k, BinopType op, AggBounds& bounds) {
  apply_upper_bound(k, with_sides_swapped(op), bounds);
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
aspif::Atom at_least(const BigInt& bound,
                     const std::vector<aspif::WeightedLit>& weighted,
                     aspif::Program& result) {
  std::vector<aspif::WeightedLit> positive;
  positive.reserve(weighted.size());
  BigInt lower = bound;
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

// Folds a bound that is not a number into `bounds`, e.g. the 'a' of
// '#count{...} >= a'. A #count or #sum is always an integer, and ASP-Core-2
// puts every integer below every other kind of term, so which integer it turns
// out to be makes no difference. Any number stands in for it here.
void apply_non_numeric_bound(Sym bound, BinopType op, bool bound_is_upper,
                             AggBounds& bounds, Symbols& syms) {
  const Sym any_number = syms.number(0);
  const bool holds = bound_is_upper
                         ? builtin_holds(op, any_number, bound, syms)
                         : builtin_holds(op, bound, any_number, syms);
  if (!holds) bounds.unsatisfiable = true;
}

// Reads the (up to two) bound sides of a #count or #sum under `binding`.
// Nullopt means a bound is ill-formed, e.g. the '4 / 0' of '#count{...} >= 4/X'
// under X = 0, which makes the enclosing rule instance nonexistent.
//
// In a '#count{...} = S', S holds the value this rule instance checks for:
// find_instances() split the instance over the values the aggregate can take.
absl::StatusOr<std::optional<AggBounds>> eval_agg_bounds(const Aggregate& agg,
                                                         const Binding& binding,
                                                         Symbols& syms) {
  AggBounds bounds;
  if (agg.lb_term != nullptr) {
    ASSIGN_OR_RETURN(std::optional<Sym> k,
                     eval_term(*agg.lb_term, binding, syms));
    if (!k.has_value()) return std::nullopt;
    if (syms.is_number(*k)) {
      apply_lower_bound(syms.number_of(*k), agg.lb_op, bounds);
    } else {
      apply_non_numeric_bound(*k, agg.lb_op, /*bound_is_upper=*/false, bounds,
                              syms);
    }
  }
  if (agg.ub_term != nullptr) {
    ASSIGN_OR_RETURN(std::optional<Sym> k,
                     eval_term(*agg.ub_term, binding, syms));
    if (!k.has_value()) return std::nullopt;
    if (syms.is_number(*k)) {
      apply_upper_bound(syms.number_of(*k), agg.ub_op, bounds);
    } else {
      apply_non_numeric_bound(*k, agg.ub_op, /*bound_is_upper=*/true, bounds,
                              syms);
    }
  }
  return bounds;
}

// Reads the (up to two) guards of a #min or #max under `binding`. A guard is
// any term, since the value it is compared against is one too. Nullopt means
// the same as it does for eval_agg_bounds().
absl::StatusOr<std::optional<std::vector<Guard>>> eval_minmax_guards(
    const Aggregate& agg, const Binding& binding, Symbols& syms) {
  std::vector<Guard> guards;
  if (agg.lb_term != nullptr) {
    ASSIGN_OR_RETURN(std::optional<Sym> term,
                     eval_term(*agg.lb_term, binding, syms));
    if (!term.has_value()) return std::nullopt;
    guards.push_back({.op = with_sides_swapped(agg.lb_op), .term = *term});
  }
  if (agg.ub_term != nullptr) {
    ASSIGN_OR_RETURN(std::optional<Sym> term,
                     eval_term(*agg.ub_term, binding, syms));
    if (!term.has_value()) return std::nullopt;
    guards.push_back({.op = agg.ub_op, .term = *term});
  }
  return guards;
}

// Whether an aggregate whose value grounding has settled holds, e.g. true for
// the '#count{ X : p(X) } >= 1' of a rule over the fact p(1). Nullopt when the
// value is not settled, which leaves the aggregate for the solver.
std::optional<bool> settled_agg_holds(const Aggregate& agg,
                                      const AggBounds& bounds,
                                      const std::vector<AggTuple>& tuples) {
  // A bound no integer meets, e.g. the 'a' of '#count{...} >= a', settles the
  // aggregate whatever the solver does with its value.
  if (bounds.unsatisfiable) return agg.naf;
  std::optional<BigInt> value = settled_agg_value(agg, tuples);
  if (!value.has_value()) return std::nullopt;
  const bool holds = bounds.hold_for(*value);
  return agg.naf ? !holds : holds;
}

// The same for a #min or #max, over the term order.
std::optional<bool> settled_minmax_holds(const Aggregate& agg,
                                         const std::vector<Guard>& guards,
                                         const std::vector<AggTuple>& tuples,
                                         const Symbols& syms) {
  std::optional<MinMaxValue> value = settled_minmax_value(agg, tuples, syms);
  if (!value.has_value()) return std::nullopt;
  bool holds = true;
  for (const Guard& guard : guards) {
    if (!minmax_holds(guard.op, *value, guard.term, syms)) holds = false;
  }
  return agg.naf ? !holds : holds;
}

// Whether grounding settles `agg` under `binding`, and if so whether it holds,
// asked while atoms are still being derived. Nullopt means the solver decides,
// which is also the answer for an aggregate this phase is too early to judge.
//
// The caller, aggregates_value(), only asks this when
// rule.aggregate_in_own_component is false, so every predicate `agg` reads,
// negated or not, sits in a component that has already fully derived.
absl::StatusOr<std::optional<bool>> settle_aggregate(const Aggregate& agg,
                                                     const Binding& binding,
                                                     const Store& store,
                                                     Symbols& syms) {
  // A 'not' inside an element points at a predicate the same way; see
  // settled_agg_value().
  if (elements_use_negation(agg)) return std::nullopt;
  if (is_minmax(agg.function)) {
    ASSIGN_OR_RETURN(std::optional<std::vector<Guard>> guards,
                     eval_minmax_guards(agg, binding, syms));
    if (!guards.has_value()) return std::nullopt;
    ASSIGN_OR_RETURN(std::vector<AggTuple> tuples,
                     collect_agg_tuples(agg, binding, store, syms));
    return settled_minmax_holds(agg, *guards, tuples, syms);
  }
  ASSIGN_OR_RETURN(std::optional<AggBounds> bounds,
                   eval_agg_bounds(agg, binding, syms));
  if (!bounds.has_value()) return std::nullopt;
  ASSIGN_OR_RETURN(std::vector<AggTuple> tuples,
                   collect_agg_tuples(agg, binding, store, syms));
  return settled_agg_holds(agg, *bounds, tuples);
}

// The literals one guard on a #min or #max contributes to the enclosing rule's
// body. The four ordering comparisons give one literal each. '=' asks for two,
// since a value equals the guard when it is neither above nor below it, and
// '!=' negates that pair.
std::vector<aspif::Lit> minmax_guard_lits(
    bool is_min, const Guard& guard, const std::vector<AggTuple>& tuples,
    const std::vector<aspif::WeightedLit>& lits, const Symbols& syms,
    aspif::Program& result) {
  if (guard.op != BinopType::kEQUAL && guard.op != BinopType::kUNEQUAL) {
    return {minmax_compare(is_min, guard.op, guard.term, tuples, lits, syms,
                           result)};
  }
  aspif::Lit not_above = minmax_compare(is_min, BinopType::kLESS_OR_EQ,
                                        guard.term, tuples, lits, syms, result);
  aspif::Lit not_below = minmax_compare(is_min, BinopType::kGREATER_OR_EQ,
                                        guard.term, tuples, lits, syms, result);
  if (guard.op == BinopType::kEQUAL) return {not_above, not_below};
  return {negate_conjunction({not_above, not_below}, result)};
}

// Grounds one #min or #max body item into the literals its guards ask of the
// enclosing rule's body, as ground_aggregate() does for the other two.
absl::StatusOr<std::optional<std::vector<aspif::Lit>>> ground_minmax(
    const Aggregate& agg, const Binding& outer_binding, const Store& store,
    Symbols& syms, aspif::Program& result) {
  ASSIGN_OR_RETURN(std::optional<std::vector<Guard>> maybe_guards,
                   eval_minmax_guards(agg, outer_binding, syms));
  if (!maybe_guards.has_value()) return std::nullopt;
  const std::vector<Guard>& guards = *maybe_guards;

  ASSIGN_OR_RETURN(std::vector<AggTuple> tuples,
                   collect_agg_tuples(agg, outer_binding, store, syms));
  std::optional<bool> settled = settled_minmax_holds(agg, guards, tuples, syms);
  if (settled.has_value()) {
    if (!*settled) return std::nullopt;
    return std::vector<aspif::Lit>{};
  }

  const std::vector<aspif::WeightedLit> lits =
      ground_agg_elements(tuples, result);
  const bool is_min = agg.function == AggregateFunctionType::kAGGREGATE_MIN;
  std::vector<aspif::Lit> extra;
  for (const Guard& guard : guards) {
    std::vector<aspif::Lit> lits_for_guard =
        minmax_guard_lits(is_min, guard, tuples, lits, syms, result);
    extra.insert(extra.end(), lits_for_guard.begin(), lits_for_guard.end());
  }

  if (!agg.naf) return extra;
  return std::vector<aspif::Lit>{negate_conjunction(extra, result)};
}

// Grounds one Aggregate body item into the literals that must be appended to
// the enclosing rule's body for the aggregate to hold, e.g. '#count{X :
// p(X)} >= 2' grounds to a single literal referencing a fresh atom defined
// by an ASPIF weight-body rule. #count and #sum map onto that weight body;
// #min and #max range over the term order instead, and ground_minmax() encodes
// those.
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
  if (is_minmax(agg.function)) {
    return ground_minmax(agg, outer_binding, store, syms, result);
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
  for (const BigInt& k : bounds.not_equal) {
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
  // A disjunctive head can join an aggregate's predicate to the component of
  // the rule reading it, which would leave that store incomplete. The rules
  // this happens to never ask, so no answer taken from a partial store is
  // cached here; see bucket_rule_views().
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
    for (size_t slot : slots_for(agg, binding)) {
      key.values.push_back(binding.at(slot));
    }
    return key;
  }

  // agg_variable_slots() depends only on `agg`'s own variables and the
  // rule's fixed slot layout, not on what `binding` currently holds, so the
  // same aggregate gives the same slots on every call. Caching it here saves
  // re-walking the aggregate's AST on every key_for() call, including hits.
  const std::vector<size_t>& slots_for(const Aggregate& agg,
                                       const Binding& binding) {
    auto [it, inserted] = slots_.try_emplace(&agg);
    if (inserted) it->second = agg_variable_slots(agg, binding);
    return it->second;
  }

  absl::flat_hash_map<const Aggregate*, std::vector<size_t>> slots_;
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

// Checks that every aggregate bound has a value under `binding`, e.g. kNoValue
// for the '>= 4 / 0' that '>= 4 / X' becomes under {X: 0}. Such a bound leaves
// the aggregate nothing to compare against, so the rule has no instance here.
absl::Status agg_bounds_have_values(
    const std::vector<const Aggregate*>& aggregates, const Binding& binding,
    Symbols& syms) {
  for (const Aggregate* aggregate : aggregates) {
    if (aggregate->lb_term != nullptr) {
      RETURN_IF_ERROR(eval_term(*aggregate->lb_term, binding, syms).status());
    }
    if (aggregate->ub_term != nullptr) {
      RETURN_IF_ERROR(eval_term(*aggregate->ub_term, binding, syms).status());
    }
  }
  return absl::OkStatus();
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

// Reads the aggregates of a body, which is what lets grounding stop at a
// recursion an aggregate cuts off. kOpen is an aggregate whose value grounding
// cannot work out on its own. See AggCache::settle() for why the other two
// answers hold for the rest of the run.
absl::StatusOr<BodyValue> aggregates_value(
    const std::vector<const Aggregate*>& aggregates, const Binding& binding,
    const Store& store, Symbols& syms, AggCache& agg_cache) {
  BodyValue value = BodyValue::kTrue;
  for (const Aggregate* aggregate : aggregates) {
    ASSIGN_OR_RETURN(std::optional<bool> holds,
                     agg_cache.settle(*aggregate, binding, store, syms));
    if (!holds.has_value()) {
      value = BodyValue::kOpen;
    } else if (!*holds) {
      return BodyValue::kFalse;
    }
  }
  return value;
}

// Runs one rule against the store and adds the head atoms of every instance it
// finds, recording in `changes` what that did. `delta_position` picks the
// positive literal to read the previous pass's atoms from (see
// find_instances), or nullopt to read the whole store.
//
// An aggregate grounding settles false takes the body with it, so the instance
// derives nothing. That is what ends the recursion in "s(X+1) :- s(X), #count{
// N : num(N), N > X } >= 1.": the count is 0 once X reaches the largest num.
//
// An instance whose positive literals all matched facts derives its head atom
// as a fact in turn: every one of those atoms holds in every answer set, so the
// body does, so the head does. That is only sound for a rule whose body has
// nothing else in it that can fail. A 'not q' is such a thing unless grounding
// settles it, so an instance derives a fact only when its negation came out
// kTrue on a rule where that answer is final. An aggregate is one unless
// grounding settles it true.
//
// A disjunctive head derives no fact either, however solid its body: 'a | b.'
// says one of a and b holds, and neither of them holds in every answer set. Its
// atoms are only possible ones, which is exactly what this phase collects.
//
// A rule whose aggregate reads its own component settles none of its aggregates
// here, neither to drop an instance nor to derive a fact. Settling one would
// read a store still being filled. See bucket_rule_views().
//
// This is where 'p(1).' becomes a fact, and where a rule over facts alone, like
// the second rule of "edge(a, b). reachable(X, Y) :- edge(X, Y).", passes
// factness on.
absl::Status derive_from_rule(const RuleView& rule,
                              std::optional<size_t> delta_position,
                              Store& store, Symbols& syms, AggCache& agg_cache,
                              aspif::Program& aspif_prog, Changes& changes) {
  const bool derives_facts = rule.negation_is_settled && rule.head.size() == 1;
  // Deriving runs a rule once per pass and per positive literal, so warning
  // here would repeat the same line. emit_rules() runs each rule once and
  // warns there.
  return find_instances(
             rule.parts, store, syms, Binding(rule.slots),
             [&](const Instance& instance) -> absl::Status {
               // The parts the join itself does not check. A term with no value
               // in either means the rule has no ground instance under this
               // binding, so its head atom is not derived either. That is why
               // "q(X) :- p(X), not r(4 / X)." derives no q(0).
               RETURN_IF_ERROR(agg_bounds_have_values(rule.parts.aggregates,
                                                      instance.binding, syms));
               ASSIGN_OR_RETURN(const BodyValue negation,
                                negation_value(rule.parts.negative,
                                               instance.binding, store, syms));
               if (negation == BodyValue::kFalse) return absl::OkStatus();

               BodyValue aggregates = BodyValue::kOpen;
               if (!rule.aggregate_in_own_component) {
                 ASSIGN_OR_RETURN(
                     aggregates,
                     aggregates_value(rule.parts.aggregates, instance.binding,
                                      store, syms, agg_cache));
                 if (aggregates == BodyValue::kFalse) return absl::OkStatus();
               }

               // A head term with no value means this instance has no ground
               // rule at all, so none of its head atoms is derived.
               // emit_rules() drops the same instances.
               std::vector<Tuple> tuples;
               tuples.reserve(rule.head.size());
               for (const ClassicalLiteral* literal : rule.head) {
                 ASSIGN_OR_RETURN(
                     Tuple tuple,
                     eval_terms(literal->args, instance.binding, syms));
                 tuples.push_back(std::move(tuple));
               }

               std::vector<Inserted> heads;
               heads.reserve(rule.head.size());
               for (size_t i = 0; i < rule.head.size(); ++i) {
                 const PredKey key = pred_key(*rule.head[i]);
                 heads.push_back(
                     store.insert(key, std::move(tuples[i]), aspif_prog));
                 if (!heads.back().is_new) continue;
                 changes.atoms = true;
                 RETURN_IF_ERROR(store.check_size(key));
               }

               if (derives_facts && negation == BodyValue::kTrue &&
                   aggregates == BodyValue::kTrue &&
                   matched_all_facts(instance.matched, store)) {
                 const bool newly_fact = store.mark_fact(heads.front().atom);
                 // Marking an atom the store already held is the one change a
                 // delta pass cannot carry; see derive_atoms().
                 if (newly_fact && !heads.front().is_new) changes.facts = true;
               }
               return absl::OkStatus();
             },
             delta_position)
      .status();
}

// Fills `store` with every atom that could appear in an answer set, by
// running each rule against the atoms collected so far and repeating until a
// pass adds nothing new. Each new atom gets its ASPIF number from
// `aspif_prog`.
//
// A 'not' literal only settles a body here when it is over a fact. See
// negation_value(). Given "p :- q, not r." with r collected but not a fact, p
// is collected too, since r is still open. That is deliberate. This phase only
// decides which atoms can exist at all. emit_rules() emits the rule with the
// 'not r' still in it, and the solver decides whether p is true.
//
// An aggregate settles a body here only when grounding can work its value out.
// Otherwise it is ignored, so 'p :- dom(X), #count{Y : q(X,Y)} >= 2.' derives
// p(x) for every x in dom while the solver still has a say in q. emit_rules()
// then gives the solver the real weight-body encoding, which rules out the p(x)
// whose count falls short.
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
// only draw support from a rule whose every other head atom is false, which
// this one's fact never is.
//
// A body literal for a fact goes the same way, dropped from the rules that do
// get emitted, since a body holds exactly when it holds without it.
absl::Status emit_rules(const std::vector<const RuleView*>& rules,
                        const Store& store, Symbols& syms,
                        absl::flat_hash_set<aspif::Atom>& emitted_facts,
                        AggCache& agg_cache, aspif::Program& result,
                        std::vector<std::string>* warnings) {
  for (const RuleView* rule_ptr : rules) {
    const RuleView& rule = *rule_ptr;
    ASSIGN_OR_RETURN(
        std::optional<std::string> no_value_seen,
        find_instances(
            rule.parts, store, syms, Binding(rule.slots),
            [&](const Instance& instance) -> absl::Status {
              // The head atoms are looked up here. An instance missing one is
              // only an error further down, once every reason to drop the
              // instance has been ruled out. derive_atoms() dropped the same
              // instances, so a head atom looked up for one of them would be
              // missing from the store.
              std::vector<Tuple> head_tuples;
              std::vector<const GroundAtom*> heads;
              head_tuples.reserve(rule.head.size());
              heads.reserve(rule.head.size());
              for (const ClassicalLiteral* literal : rule.head) {
                // A head with no value, e.g. the 'q(X / 0)' of "q(X / 0) :-
                // p(X).", means this instance has no ground rule.
                // derive_atoms() skipped it for the same reason, so nothing is
                // missing from the store.
                ASSIGN_OR_RETURN(
                    Tuple tuple,
                    eval_terms(literal->args, instance.binding, syms));
                head_tuples.push_back(std::move(tuple));
                const PredData* data = store.find(pred_key(*literal));
                heads.push_back(
                    data == nullptr ? nullptr : data->find(head_tuples.back()));
              }
              // One head atom, and it is a fact: the fact is all the solver
              // needs, once, however many rules derive it.
              if (heads.size() == 1 && heads[0] != nullptr &&
                  store.is_fact(heads[0]->id)) {
                if (emitted_facts.insert(heads[0]->id).second) {
                  result.rules.push_back(aspif::Rule{.head = {heads[0]->id}});
                }
                return absl::OkStatus();
              }
              // A fact among the atoms of a disjunctive head satisfies the
              // rule, so the instance says nothing and goes.
              for (const GroundAtom* head : heads) {
                if (head != nullptr && store.is_fact(head->id)) {
                  return absl::OkStatus();
                }
              }

              aspif::Rule aspif_rule;
              aspif_rule.body = without_facts(instance.matched, store);
              ASSIGN_OR_RETURN(std::optional<std::vector<aspif::Lit>> neg,
                               negative_lits(rule.parts.negative,
                                             instance.binding, store, syms));
              if (!neg.has_value()) return absl::OkStatus();
              aspif_rule.body.insert(aspif_rule.body.end(), neg->begin(),
                                     neg->end());

              // A bound with no value drops the instance and is worth a
              // warning. Just below, an aggregate the store settles false
              // drops it too, but that is ordinary and says nothing.
              RETURN_IF_ERROR(agg_bounds_have_values(rule.parts.aggregates,
                                                     instance.binding, syms));

              for (const Aggregate* aggregate : rule.parts.aggregates) {
                ASSIGN_OR_RETURN(std::optional<std::vector<aspif::Lit>> extra,
                                 agg_cache.ground(*aggregate, instance.binding,
                                                  store, syms, result));
                if (!extra.has_value()) return absl::OkStatus();
                aspif_rule.body.insert(aspif_rule.body.end(), extra->begin(),
                                       extra->end());
              }

              for (size_t i = 0; i < heads.size(); ++i) {
                // derive_atoms() added every derivable head atom, so a miss
                // means the program never passed verify_safe().
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
    if (no_value_seen.has_value() && warnings != nullptr) {
      warnings->push_back(absl::StrCat("dropped ground instances of '",
                                       format(*rule.statement), "' where ",
                                       *no_value_seen));
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
void emit_minimize(const Store& store, const Symbols& syms,
                   aspif::Program& result) {
  absl::btree_map<BigInt, std::vector<aspif::WeightedLit>> by_level;
  for (const PredKey& key : store.order) {
    if (key.name != kViolationPredicate) continue;
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

// Whether a predicate prints. The scan runs once per predicate, not once per
// atom, over the handful of signatures one program names.
bool shows_predicate(const ShowFilter& filter, bool negated,
                     std::string_view name, size_t arity) {
  if (!filter.has_value()) return true;
  for (const Show::Signature& signature : *filter) {
    if (signature.negated == negated && signature.arity == arity &&
        signature.name == name) {
      return true;
    }
  }
  return false;
}

// Names every atom an answer set prints so it reads symbolically. That is each
// atom of a user-visible predicate `filter` admits, plus each shown term.
// normalize() left a shown term behind as a '_show' atom holding the term as
// its one argument.
//
// The rest of the '_' predicates are internal and stay hidden, apart from a
// '_neg_p' standing for a classically negated '-p', which prints as '-p'.
void name_outputs(const ShowFilter& filter, const Store& store,
                  const Symbols& syms, aspif::Program& result) {
  for (const PredKey& key : store.order) {
    if (key.name == kShowPredicate) {
      for (const GroundAtom& atom : store.find(key)->atoms) {
        result.outputs.push_back(aspif::Output{
            .name = syms.printed(atom.args[0]), .condition = {atom.id}});
      }
      continue;
    }

    std::string_view name = key.name;
    const bool negated = name.starts_with(kClassicalNegationPrefix);
    if (negated) {
      name.remove_prefix(kClassicalNegationPrefix.size());
    } else if (name.starts_with('_')) {
      continue;
    }
    if (!shows_predicate(filter, negated, name, key.arity)) continue;

    const std::string predicate =
        negated ? absl::StrCat("-", name) : std::string(name);
    for (const GroundAtom& atom : store.find(key)->atoms) {
      result.outputs.push_back(
          aspif::Output{.name = syms.printed_call(predicate, atom.args),
                        .condition = {atom.id}});
    }
  }
}

// Collects the atoms the program's query matches, which solve.h then asks
// every answer set about.
//
// A query's variables stand for substitutions. 'p(X, a)?' asks of each x in
// the store whether p(x, a) holds in every answer set, so the matches are kept
// apart rather than folded into one atom. Which of them hold is the answer to
// a non-ground query.
//
// A query nothing matches collects no atom, so nothing can make it hold. That
// is the right answer, nothing in the program satisfying the query.
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
  result.query.emplace(matched.begin(), matched.end());
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<aspif::Program> ground(const Program& prog,
                                      std::vector<std::string>* warnings,
                                      size_t max_ground_atoms) {
  const PredGraph graph = build_pred_graph(prog);
  const std::vector<int> component =
      strongly_connected_components(derivation_succ(graph));
  ASSIGN_OR_RETURN(std::vector<RuleView> rules, make_rule_views(prog));
  std::vector<std::vector<const RuleView*>> rules_by_component =
      bucket_rule_views(graph, component, rules);

  // Two passes: derive every component, then emit every component. A
  // component's body literals, positive and negative alike, depend only on
  // earlier components, so deriving in ascending order gets those right.
  // Unstratified negation is the exception, pointing a 'not q' inside the
  // rule's own component. Emitting only once every component has derived
  // covers that too, since q's final atoms exist by then.
  //
  // The last bucket holds the constraints, which derive nothing and so are
  // emitted after every atom exists.
  aspif::Program result;
  // One symbol table for the whole run: every tuple in the store holds handles
  // into it, so it has to outlive them.
  Symbols syms;
  Store store;
  store.max_atoms = max_ground_atoms;
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
                               agg_cache, result, warnings));
  }
  emit_minimize(store, syms, result);
  name_outputs(prog.show_filter, store, syms, result);
  if (prog.query != nullptr && prog.query->lit != nullptr) {
    RETURN_IF_ERROR(emit_query(*prog.query->lit, store, syms, result));
    result.query_text = format(*prog.query);
  }
  return result;
}
