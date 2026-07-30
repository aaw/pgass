#include "solve.h"

#include <cvc5/cvc5.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "aspif.h"
#include "graph.h"
#include "macros.h"

namespace {

// The SMT logic to solve `prog` in. cvc5 takes the logic once, before the first
// assertion, and offers no way to widen it later, so this scans the whole
// program up front instead of deciding while translating.
//
// Every level ranking constraint looks like 'lvl(a) - lvl(b) >= 1': two
// variables, no coefficients, a constant on the right. That is exactly what a
// difference logic atom can say, and QF_IDL lets cvc5 decide such constraints
// by looking for a negative cycle in a graph rather than running simplex.
//
// A weight body or a minimize statement adds up the weights of many literals at
// once, which no difference logic atom can say, and costs the whole program the
// more general QF_LIA. Every QF_IDL formula is also a QF_LIA formula, so
// answering QF_LIA is never wrong, only slower.
const char* logic_for(const aspif::Program& prog) {
  // A weighted sum is a weighted sum whether it comes from an aggregate or a
  // weak constraint.
  if (!prog.minimize.empty()) return "QF_LIA";
  for (const aspif::Rule& rule : prog.rules) {
    if (rule.body_type == aspif::Rule::BodyType::kWeight) return "QF_LIA";
  }
  return "QF_IDL";
}

// Rejects the parts of aspif that solve() does not handle, so that such a
// program gets an error rather than a wrong answer.
absl::Status check_supported(const aspif::Program& prog) {
  for (const aspif::Rule& rule : prog.rules) {
    // A choice head leaves its atoms free rather than deriving them, so the
    // completion below would have to stop forcing them. Nothing produces one:
    // normalization rewrites choice rules away.
    if (rule.head_type == aspif::Rule::HeadType::kChoice) {
      return absl::UnimplementedError("choice rule heads are not supported");
    }
    // Deciding a disjunctive program is complete for the second level of the
    // polynomial hierarchy, while one QF_IDL or QF_LIA query only decides an NP
    // problem, so no translation like the one below can exist unless that
    // hierarchy collapses. Disjunction needs a different shape of solving: a
    // saturation encoding, or a second solver checking each candidate answer
    // set for minimality.
    if (rule.head.size() > 1) {
      return absl::UnimplementedError(
          "disjunctive rule heads are not supported");
    }
  }
  return absl::OkStatus();
}

// The un-negated body atoms of `rule`, whichever body form it uses. These are
// the atoms the rule depends on positively, which is what both the dependency
// graph and the level ranking care about.
std::vector<aspif::Atom> positive_body_atoms(const aspif::Rule& rule) {
  std::vector<aspif::Atom> atoms;
  if (rule.body_type == aspif::Rule::BodyType::kNormal) {
    for (aspif::Lit lit : rule.body) {
      if (lit > 0) atoms.push_back(lit);
    }
  } else {
    for (const aspif::WeightedLit& weighted : rule.weighted_body) {
      if (weighted.lit > 0) atoms.push_back(weighted.lit);
    }
  }
  return atoms;
}

// The positive dependency graph of the ground program: an edge from each
// un-negated body atom to the head atom of the rule it appears in. Atom ids
// index the rows, so row 0 is present but always empty, 0 being no atom.
std::vector<std::vector<int>> positive_dependency_graph(
    const aspif::Program& prog) {
  std::vector<std::vector<int>> succ(prog.next_atom);
  for (const aspif::Rule& rule : prog.rules) {
    const std::vector<aspif::Atom> body_atoms = positive_body_atoms(rule);
    for (aspif::Atom head : rule.head) {
      DCHECK_LT(head, prog.next_atom);
      for (aspif::Atom body_atom : body_atoms) {
        DCHECK_LT(body_atom, prog.next_atom);
        succ[body_atom].push_back(head);
      }
    }
  }
  return succ;
}

// What the level ranking needs to know about the positive dependency graph.
struct Ranking {
  // The strongly connected component of each atom, indexed by atom id. Two
  // atoms can lie on a common positive cycle exactly when their components are
  // equal.
  std::vector<int> component;
  // Whether each atom can lie on a positive cycle at all, and so needs a level
  // variable. False for every atom of a stratified program, which leaves the
  // translation as plain completion.
  std::vector<bool> needs_level;
};

Ranking build_ranking(const aspif::Program& prog) {
  const std::vector<std::vector<int>> succ = positive_dependency_graph(prog);

  Ranking ranking;
  ranking.component = strongly_connected_components(succ);
  ranking.needs_level.assign(prog.next_atom, false);

  std::vector<int> component_size(ranking.component.size(), 0);
  for (int component : ranking.component) ++component_size[component];

  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    if (component_size[ranking.component[atom]] > 1) {
      ranking.needs_level[atom] = true;
      continue;
    }
    // A component of one atom is still a cycle when that atom depends on
    // itself, as in 'a :- a, p.', so a self-edge counts too.
    for (int successor : succ[atom]) {
      if (successor == atom) {
        ranking.needs_level[atom] = true;
        break;
      }
    }
  }
  return ranking;
}

// cvc5 rejects AND, OR, and ADD with fewer than two arguments, and every empty
// case below comes up in practice: a rule with an empty body is a fact, so its
// body formula is true; an atom no rule derives can never hold, so its
// completion is false; and a weight body with no literals adds up to zero.
cvc5::Term conjunction(cvc5::TermManager& tm,
                       const std::vector<cvc5::Term>& conjuncts) {
  if (conjuncts.empty()) return tm.mkTrue();
  if (conjuncts.size() == 1) return conjuncts.front();
  return tm.mkTerm(cvc5::Kind::AND, conjuncts);
}

cvc5::Term disjunction(cvc5::TermManager& tm,
                       const std::vector<cvc5::Term>& disjuncts) {
  if (disjuncts.empty()) return tm.mkFalse();
  if (disjuncts.size() == 1) return disjuncts.front();
  return tm.mkTerm(cvc5::Kind::OR, disjuncts);
}

cvc5::Term sum(cvc5::TermManager& tm, const std::vector<cvc5::Term>& addends) {
  if (addends.empty()) return tm.mkInteger(0);
  if (addends.size() == 1) return addends.front();
  return tm.mkTerm(cvc5::Kind::ADD, addends);
}

// One Bool per atom, indexed by atom id so that atom_var[a] is a's variable.
// Slot 0 stays a null Term, 0 being no atom.
//
// The variables are named after the atom id rather than the aspif::Output name,
// because not every atom has a name: the predicates normalization invents are
// kept out of the output.
std::vector<cvc5::Term> declare_atoms(cvc5::TermManager& tm,
                                      const aspif::Program& prog) {
  std::vector<cvc5::Term> atom_var(prog.next_atom);
  const cvc5::Sort bool_sort = tm.getBooleanSort();
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    atom_var[atom] = tm.mkConst(bool_sort, absl::StrCat("a", atom));
  }
  return atom_var;
}

// One integer level variable per atom that can lie on a positive cycle. Every
// other slot stays a null Term, so a null level variable is how the rest of
// this file asks whether an atom needs ranking at all.
std::vector<cvc5::Term> declare_levels(cvc5::TermManager& tm,
                                       const aspif::Program& prog,
                                       const Ranking& ranking) {
  std::vector<cvc5::Term> level_var(prog.next_atom);
  const cvc5::Sort int_sort = tm.getIntegerSort();
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    if (!ranking.needs_level[atom]) continue;
    level_var[atom] = tm.mkConst(int_sort, absl::StrCat("lvl", atom));
  }
  return level_var;
}

// The formula for one aspif literal: the atom's variable, or its negation for a
// default-negated literal.
cvc5::Term literal_term(cvc5::TermManager& tm,
                        const std::vector<cvc5::Term>& atom_var,
                        aspif::Lit lit) {
  if (lit > 0) return atom_var[lit];
  return tm.mkTerm(cvc5::Kind::NOT, {atom_var[-lit]});
}

// The integer term adding up the weights of the true literals of `lits`. Each
// literal contributes its weight when it holds and zero when it does not. Both
// a weight body and a minimize statement are that sum. One is compared against
// a bound, the other minimized.
cvc5::Term weighted_sum(cvc5::TermManager& tm,
                        const std::vector<cvc5::Term>& atom_var,
                        const std::vector<aspif::WeightedLit>& lits) {
  const cvc5::Term zero = tm.mkInteger(0);
  std::vector<cvc5::Term> addends;
  addends.reserve(lits.size());
  for (const aspif::WeightedLit& weighted : lits) {
    addends.push_back(
        tm.mkTerm(cvc5::Kind::ITE, {literal_term(tm, atom_var, weighted.lit),
                                    tm.mkInteger(weighted.weight), zero}));
  }
  return sum(tm, addends);
}

// The formula for one rule body.
cvc5::Term body_term(cvc5::TermManager& tm,
                     const std::vector<cvc5::Term>& atom_var,
                     const aspif::Rule& rule) {
  if (rule.body_type == aspif::Rule::BodyType::kNormal) {
    std::vector<cvc5::Term> conjuncts;
    conjuncts.reserve(rule.body.size());
    for (aspif::Lit lit : rule.body) {
      conjuncts.push_back(literal_term(tm, atom_var, lit));
    }
    return conjunction(tm, conjuncts);
  }
  // A weight body holds when the weights of its true literals reach
  // lower_bound.
  return tm.mkTerm(cvc5::Kind::GEQ,
                   {weighted_sum(tm, atom_var, rule.weighted_body),
                    tm.mkInteger(rule.lower_bound)});
}

// One rule deriving one atom.
struct Support {
  // The formula for the rule's body.
  cvc5::Term body;
  // The un-negated body atoms sharing the head's component, which the level
  // ranking has to order strictly below the head. A positive dependency that
  // crosses components cannot lie on a cycle, so it is left out and costs no
  // constraint.
  std::vector<aspif::Atom> ranked_below;
};

// The un-negated body atoms of `rule` that need a rank relative to `head`.
//
// A weight body contributes none of these in practice: ASP-Core-2 forbids
// recursive aggregates, so a weight body's atoms always sit in a lower
// component than the head. Were that to change, asking every literal of a
// weight body to rank below the head would be too strong, since reaching the
// bound only needs some of them to hold, and this would reject real answer
// sets.
std::vector<aspif::Atom> ranked_body_atoms(const aspif::Rule& rule,
                                           aspif::Atom head,
                                           const Ranking& ranking) {
  std::vector<aspif::Atom> ranked_below;
  if (!ranking.needs_level[head]) return ranked_below;
  for (aspif::Atom body_atom : positive_body_atoms(rule)) {
    if (ranking.component[body_atom] == ranking.component[head]) {
      ranked_below.push_back(body_atom);
    }
  }
  return ranked_below;
}

// Groups the rules by the atom they derive. Rules with an empty head derive
// nothing and are left out; assert_constraints handles those.
std::vector<std::vector<Support>> collect_supports(
    cvc5::TermManager& tm, const aspif::Program& prog,
    const std::vector<cvc5::Term>& atom_var, const Ranking& ranking) {
  std::vector<std::vector<Support>> supports(prog.next_atom);
  for (const aspif::Rule& rule : prog.rules) {
    if (rule.head.empty()) continue;
    cvc5::Term body = body_term(tm, atom_var, rule);
    for (aspif::Atom head : rule.head) {
      supports[head].push_back(
          Support{.body = body,
                  .ranked_below = ranked_body_atoms(rule, head, ranking)});
    }
  }
  return supports;
}

// Forbids the body of every integrity constraint, an aspif rule with an empty
// head, from holding.
void assert_constraints(cvc5::TermManager& tm, cvc5::Solver& solver,
                        const aspif::Program& prog,
                        const std::vector<cvc5::Term>& atom_var) {
  for (const aspif::Rule& rule : prog.rules) {
    if (!rule.head.empty()) continue;
    solver.assertFormula(
        tm.mkTerm(cvc5::Kind::NOT, {body_term(tm, atom_var, rule)}));
  }
}

// Asserts the Clark completion: an atom holds exactly when one of the bodies
// deriving it holds. An atom no rule derives has no bodies, so its completion
// says it is false since nothing can ever derive it.
//
// The rank conditions of the level ranking stay out of these formulas. The
// completion has to say "this body holds, therefore its head holds" without
// qualification; the ranking is a separate restriction on which supports count.
// Folding the two together would let the solver choose levels that falsify a
// support and so drop an atom the rules force to hold.
void assert_completion(cvc5::TermManager& tm, cvc5::Solver& solver,
                       const aspif::Program& prog,
                       const std::vector<cvc5::Term>& atom_var,
                       const std::vector<std::vector<Support>>& supports) {
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    std::vector<cvc5::Term> bodies;
    bodies.reserve(supports[atom].size());
    for (const Support& support : supports[atom]) {
      bodies.push_back(support.body);
    }
    solver.assertFormula(tm.mkTerm(cvc5::Kind::EQUAL,
                                   {atom_var[atom], disjunction(tm, bodies)}));
  }
}

// Asserts the level ranking: a true atom needs some rule whose body holds and
// whose same-component positive body atoms all rank strictly below it.
// Alongside the completion this rules out an atom whose only support is a
// positive cycle, because no assignment of levels can put every atom of a cycle
// below the next.
//
// Atoms with no level variable are skipped: they cannot lie on a cycle, so
// there is nothing about them to rank.
void assert_ranking(cvc5::TermManager& tm, cvc5::Solver& solver,
                    const aspif::Program& prog,
                    const std::vector<cvc5::Term>& atom_var,
                    const std::vector<cvc5::Term>& level_var,
                    const std::vector<std::vector<Support>>& supports) {
  const cvc5::Term one = tm.mkInteger(1);
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    if (level_var[atom].isNull()) continue;
    std::vector<cvc5::Term> justified;
    justified.reserve(supports[atom].size());
    for (const Support& support : supports[atom]) {
      std::vector<cvc5::Term> conjuncts = {support.body};
      for (aspif::Atom below : support.ranked_below) {
        DCHECK(!level_var[below].isNull());
        // lvl(atom) - lvl(below) >= 1. A rule deriving an atom from itself, as
        // in 'a :- a, p.', asks for lvl(a) - lvl(a) >= 1 and so can justify
        // nothing, which is the right answer.
        conjuncts.push_back(tm.mkTerm(
            cvc5::Kind::GEQ,
            {tm.mkTerm(cvc5::Kind::SUB, {level_var[atom], level_var[below]}),
             one}));
      }
      justified.push_back(conjunction(tm, conjuncts));
    }
    solver.assertFormula(tm.mkTerm(
        cvc5::Kind::IMPLIES, {atom_var[atom], disjunction(tm, justified)}));
  }
}

// One priority level to minimize.
struct LevelCost {
  // The weighted sum of the level's literals.
  cvc5::Term cost;
  // A cost no answer set can go below, which is where bisecting starts. Only a
  // negative weight can push the sum under zero, so this is the negative
  // weights added up.
  std::int64_t lower_bound = 0;
};

// One entry per priority level, the most important level first. That is the
// order the levels have to be settled in. A weak constraint at level 2 outranks
// every weak constraint at level 1, however many of the latter there are.
//
// Two minimize statements sharing a priority are one level, since the semantics
// is the total cost at each level and not the cost of each statement. Grounding
// already emits one statement per level, but aspif permits several.
std::vector<LevelCost> collect_level_costs(
    cvc5::TermManager& tm, const aspif::Program& prog,
    const std::vector<cvc5::Term>& atom_var) {
  absl::btree_map<std::int64_t, std::vector<aspif::WeightedLit>> by_priority;
  for (const aspif::Minimize& minimize : prog.minimize) {
    std::vector<aspif::WeightedLit>& lits = by_priority[minimize.priority];
    lits.insert(lits.end(), minimize.lits.begin(), minimize.lits.end());
  }

  std::vector<LevelCost> level_costs;
  level_costs.reserve(by_priority.size());
  // btree_map orders its keys ascending, so walking it backwards puts the
  // highest priority first.
  for (auto it = by_priority.rbegin(); it != by_priority.rend(); ++it) {
    std::int64_t lower_bound = 0;
    for (const aspif::WeightedLit& weighted : it->second) {
      if (weighted.weight < 0) lower_bound += weighted.weight;
    }
    level_costs.push_back(
        LevelCost{.cost = weighted_sum(tm, atom_var, it->second),
                  .lower_bound = lower_bound});
  }
  return level_costs;
}

// Reads an integer the solver assigned. cvc5 works in unbounded integers, so a
// program whose weights add up past 2^63 has a cost no std::int64_t can hold.
// Better to say so than to report a wrapped-around number.
absl::StatusOr<std::int64_t> int64_value(const cvc5::Term& value) {
  if (!value.isInt64Value()) {
    return absl::OutOfRangeError(absl::StrCat(
        "cost ", value.getIntegerValue(), " does not fit in a 64-bit integer"));
  }
  return value.getInt64Value();
}

absl::Status undecided(const cvc5::Result& result) {
  return absl::InternalError(
      absl::StrCat("cvc5 returned '", result.toString(),
                   "' rather than deciding the program"));
}

// The least cost a level can take, or nullopt when the program has no answer
// set. Both searches below return that, and leave the solver holding whatever
// bounds they proved along the way.
using LeastCost = absl::StatusOr<std::optional<std::int64_t>>;

// Finds any answer set, works out its cost, then asks for a strictly lower one.
// Repeats until that comes back unsat. The last cost seen is the least
// reachable one.
//
// The bounds only ever shrink, so each one can be asserted on top of the last
// with no bookkeeping. They all go in one push() scope. The final bound is
// unsatisfiable by construction, so it has to be popped before any later query.
LeastCost minimize_by_stepping_down(cvc5::TermManager& tm, cvc5::Solver& solver,
                                    const LevelCost& level) {
  std::optional<std::int64_t> best;
  solver.push();
  while (true) {
    const cvc5::Result result = solver.checkSat();
    if (result.isUnsat()) break;
    if (!result.isSat()) return undecided(result);
    ASSIGN_OR_RETURN(best, int64_value(solver.getValue(level.cost)));
    solver.assertFormula(
        tm.mkTerm(cvc5::Kind::LT, {level.cost, tm.mkInteger(*best)}));
  }
  solver.pop();
  return best;
}

// Halves the range the least cost is known to lie in until it holds one value.
//
// `high` is always a cost some answer set reaches and `low` one that none
// beats. So the least cost stays in [low, high] and the two meet on it. Each
// probe either pulls `high` down to the cost of the answer set it found or
// lifts `low` past the bound it refuted. Both of those are permanent. The
// probed bound itself is not, so it goes in as an assumption of the one query
// rather than an assertion.
LeastCost minimize_by_bisecting(cvc5::TermManager& tm, cvc5::Solver& solver,
                                const LevelCost& level) {
  // One answer set to start the range off. Its absence is how an unsatisfiable
  // program shows up.
  const cvc5::Result first = solver.checkSat();
  if (first.isUnsat()) return std::nullopt;
  if (!first.isSat()) return undecided(first);
  ASSIGN_OR_RETURN(std::int64_t high, int64_value(solver.getValue(level.cost)));
  solver.assertFormula(
      tm.mkTerm(cvc5::Kind::LEQ, {level.cost, tm.mkInteger(high)}));

  std::int64_t low = level.lower_bound;
  while (low < high) {
    // Rounds down, so the midpoint stays below `high` and the range really
    // shrinks.
    const std::int64_t middle = low + (high - low) / 2;
    const cvc5::Result probe = solver.checkSatAssuming(
        tm.mkTerm(cvc5::Kind::LEQ, {level.cost, tm.mkInteger(middle)}));
    if (probe.isSat()) {
      // The answer set found may cost well under what the probe asked for, so
      // take its cost rather than the midpoint.
      ASSIGN_OR_RETURN(high, int64_value(solver.getValue(level.cost)));
      solver.assertFormula(
          tm.mkTerm(cvc5::Kind::LEQ, {level.cost, tm.mkInteger(high)}));
    } else if (probe.isUnsat()) {
      low = middle + 1;
      solver.assertFormula(
          tm.mkTerm(cvc5::Kind::GEQ, {level.cost, tm.mkInteger(low)}));
    } else {
      return undecided(probe);
    }
  }
  return high;
}

LeastCost minimize_level(cvc5::TermManager& tm, cvc5::Solver& solver,
                         const LevelCost& level,
                         SolveOptions::Optimizer optimizer) {
  switch (optimizer) {
    case SolveOptions::Optimizer::kLinear:
      return minimize_by_stepping_down(tm, solver, level);
    case SolveOptions::Optimizer::kBisect:
      return minimize_by_bisecting(tm, solver, level);
  }
}

// Settles every level in turn and leaves `solver` asserting that each level's
// cost equals the least value it can take. Returns the least cost of each
// level, or no costs at all when the program has no answer set.
//
// cvc5 has no optimization API, so this is branch and bound by hand, in one of
// the two shapes above. The hardest instances to solve are the unsat ones with
// a bound near the sat/unsat threshold. Stepping down one model at a time only
// ever solves one of those, the last one, which is why it is the default.
// Bisecting the range makes fewer solver calls overall, but many of them are
// unsat, so it is often slower.
//
// Fixing a settled level with an equality rather than an inequality is what
// makes the levels lexicographic. Later levels can then be minimized freely,
// because no choice they make can spend an earlier level's budget.
//
// On return the solver is satisfiable exactly when the program has an answer
// set, and every answer set it still admits is an optimal one.
absl::StatusOr<std::vector<std::int64_t>> optimize(
    cvc5::TermManager& tm, cvc5::Solver& solver,
    const std::vector<LevelCost>& level_costs,
    SolveOptions::Optimizer optimizer) {
  std::vector<std::int64_t> costs;
  costs.reserve(level_costs.size());
  for (const LevelCost& level : level_costs) {
    ASSIGN_OR_RETURN(const std::optional<std::int64_t> least,
                     minimize_level(tm, solver, level, optimizer));
    // No cost at all means the program is unsat, which the caller finds out for
    // itself when it looks for an answer set.
    if (!least.has_value()) return std::vector<std::int64_t>();
    costs.push_back(*least);
    solver.assertFormula(
        tm.mkTerm(cvc5::Kind::EQUAL, {level.cost, tm.mkInteger(*least)}));
  }
  return costs;
}

}  // namespace

absl::StatusOr<std::vector<AnswerSet>> solve(const aspif::Program& prog,
                                             const SolveOptions& options) {
  if (absl::Status supported = check_supported(prog); !supported.ok()) {
    return supported;
  }
  if (options.max_answer_sets < 0) {
    return absl::InvalidArgumentError(
        "max_answer_sets cannot be negative; 0 asks for all answer sets");
  }

  const Ranking ranking = build_ranking(prog);

  cvc5::TermManager tm;
  cvc5::Solver solver(tm);
  solver.setLogic(logic_for(prog));
  solver.setOption("produce-models", "true");
  solver.setOption("incremental", "true");

  const std::vector<cvc5::Term> atom_var = declare_atoms(tm, prog);
  const std::vector<cvc5::Term> level_var = declare_levels(tm, prog, ranking);
  const std::vector<std::vector<Support>> supports =
      collect_supports(tm, prog, atom_var, ranking);

  assert_constraints(tm, solver, prog, atom_var);
  assert_completion(tm, solver, prog, atom_var, supports);
  assert_ranking(tm, solver, prog, atom_var, level_var, supports);

  // The ground query: literals every answer set has to satisfy. Asserted
  // outright rather than passed to checkSatAssuming(), because they hold for
  // every answer set of this call and nothing here retracts them.
  for (aspif::Lit lit : prog.assumptions) {
    solver.assertFormula(literal_term(tm, atom_var, lit));
  }

  // The cost every answer set below will have, optimizing having pinned each
  // level to its least value. A program with no minimize statements has no
  // levels, so it comes back with no cost and no solver call made.
  ASSIGN_OR_RETURN(const std::vector<std::int64_t> costs,
                   optimize(tm, solver, collect_level_costs(tm, prog, atom_var),
                            options.optimizer));

  // What to block an answer set on: the atoms alone, never the level variables.
  // One answer set admits many level rankings, so blocking a whole model would
  // keep handing the same answer set back under a different ranking.
  std::vector<cvc5::Term> atoms_only;
  atoms_only.reserve(prog.next_atom - 1);
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    atoms_only.push_back(atom_var[atom]);
  }

  std::vector<AnswerSet> answer_sets;
  while (options.max_answer_sets == 0 ||
         answer_sets.size() < static_cast<size_t>(options.max_answer_sets)) {
    const cvc5::Result result = solver.checkSat();
    if (result.isUnsat()) break;
    if (!result.isSat()) return undecided(result);

    AnswerSet answer_set;
    answer_set.costs = costs;
    for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
      if (solver.getValue(atom_var[atom]).getBooleanValue()) {
        answer_set.atoms.push_back(atom);
      }
    }
    answer_sets.push_back(std::move(answer_set));

    // A program with no atoms has the empty answer set and no other, and
    // blockModelValues() rejects an empty term list, so stop here.
    if (atoms_only.empty()) break;
    solver.blockModelValues(atoms_only);
  }
  return answer_sets;
}
