#include "solve.h"

#include <cvc5/cvc5.h>

#include <cstddef>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "aspif.h"
#include "graph.h"

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
  // check_supported rejects minimize statements, so this branch never fires
  // today. It states the rule anyway, because a weighted sum is a weighted sum
  // whether it comes from an aggregate or a weak constraint.
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
  // cvc5 has no optimization API, so a minimize statement cannot be handed to
  // it. Supporting one means a branch-and-bound loop here: find any answer set,
  // work out its cost, assert that the cost is strictly lower, and repeat until
  // the result is unsat, whereupon the last answer set found is the optimal
  // one. With several priority levels that runs once per level, each settled
  // level frozen with an equality before moving on to the next.
  if (!prog.minimize.empty()) {
    return absl::UnimplementedError(
        "weak constraints are not supported: optimization is not implemented");
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
  // lower_bound, so each literal adds its weight or zero.
  std::vector<cvc5::Term> addends;
  addends.reserve(rule.weighted_body.size());
  for (const aspif::WeightedLit& weighted : rule.weighted_body) {
    addends.push_back(tm.mkTerm(
        cvc5::Kind::ITE, {literal_term(tm, atom_var, weighted.lit),
                          tm.mkInteger(weighted.weight), tm.mkInteger(0)}));
  }
  return tm.mkTerm(cvc5::Kind::GEQ,
                   {sum(tm, addends), tm.mkInteger(rule.lower_bound)});
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
    if (!result.isSat()) {
      return absl::InternalError(
          absl::StrCat("cvc5 returned '", result.toString(),
                       "' rather than deciding the program"));
    }

    AnswerSet answer_set;
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
