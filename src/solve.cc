#include "solve.h"

#include <cvc5/cvc5.h>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "aspif.h"
#include "encode.h"
#include "macros.h"

namespace {

// Whether a solver call found a model, or an error where cvc5 decided neither
// way.
absl::StatusOr<bool> decided(const cvc5::Result& result) {
  if (result.isSat()) return true;
  if (result.isUnsat()) return false;
  return absl::InternalError(
      absl::StrCat("cvc5 returned '", result.toString(),
                   "' rather than deciding the program"));
}

// The options every solver here runs with.
void configure(cvc5::Solver& solver, const char* logic) {
  solver.setLogic(logic);
  solver.setOption("produce-models", "true");
  solver.setOption("sat-solver", "cadical");
}

// One search over one ground program. Every model find() returns is an answer
// set, and an optimal one once settle_costs() has bounded the levels. For most
// programs the encoding says so outright. A positive cycle leaves it to the
// minimality check, which find() runs before handing a model back.
//
// Every member holds terms of `tm`, so `tm` is declared first and destroyed
// last.
struct Search {
  cvc5::TermManager tm;
  const aspif::Program* prog = nullptr;
  Encoding encoding;
  std::optional<cvc5::Solver> solver;
  // The minimality check of encode.h, asked of every model the solver hands
  // back. Left unbuilt where the encoding already describes the answer sets.
  std::optional<cvc5::Solver> checker;

  // Builds the encoding of `program`. Nothing is asked of cvc5 until find().
  absl::Status start(const aspif::Program& program);

  // Whether the program has an answer set nothing asserted so far rules out.
  // `question`, where given, holds for this call alone.
  //
  // A program with a positive cycle checks each model for minimality first.
  // One that fails leaves behind a loop nogood, so the next round asks a
  // solver that knows more. The loop ends at a model that passes, or at a
  // solver with nothing left to try.
  absl::StatusOr<bool> find(
      const std::optional<cvc5::Term>& question = std::nullopt);

  // An unfounded set of the model the solver holds, empty where that model is
  // an answer set. Only a program with a positive cycle has one to find.
  absl::StatusOr<std::vector<aspif::Atom>> unfounded_set();

  // The atoms true in the model the solver holds.
  std::vector<aspif::Atom> model_atoms() const;

  // What that model cost at each priority level, most important level first.
  // Empty for a program with no weak constraints.
  std::vector<BigInt> model_costs() const;

  // Brings every priority level down to its least cost and asserts a bound
  // holding it there, so every model found afterwards is an optimal answer
  // set. Does nothing for a program with no weak constraints, or one with no
  // answer set.
  //
  // A level comes down a model at a time. Ask for an answer set costing less
  // than the one in hand, then take its cost as the new one to beat. When
  // nothing cheaper is left, the cost in hand is the least. The most
  // important level settles first and is bounded before the next one starts,
  // which is what makes the levels lexicographic.
  //
  // Bisecting would aim most of its calls just under the least cost. There
  // cvc5 has to prove nothing exists rather than find something, and those
  // calls are the slow ones. Walking pays for one.
  absl::Status settle_costs();

  // Rules the model out of every later find().
  void block(const std::vector<aspif::Atom>& atoms);

  // A solver holding the encoding.
  void load();

  // A second solver holding the minimality check, and the term lists below. A
  // check names all of them every round, so they are built once.
  void load_checker();

  // Everything one round asks the solver for, in one list so it takes one call:
  // MinimalityCheck::read first, then the droppable atoms.
  std::vector<cvc5::Term> probe;
  // MinimalityCheck::read negated, in the same order as the front of `probe`.
  std::vector<cvc5::Term> read_false;
  // The subset variable of each droppable atom and that variable negated, both
  // in the order of MinimalityCheck::droppable.
  std::vector<cvc5::Term> droppable_subset;
  std::vector<cvc5::Term> droppable_dropped;
  // Scratch refilled every round.
  std::vector<cvc5::Term> assumptions;
  std::vector<cvc5::Term> droppable_held;
  std::vector<bool> holds_droppable;
};

absl::Status Search::start(const aspif::Program& program) {
  prog = &program;
  ASSIGN_OR_RETURN(encoding, build_encoding(tm, program));
  return absl::OkStatus();
}

void Search::load() {
  solver.emplace(tm);
  configure(*solver, encoding.logic);
  for (const Section& section : encoding.sections) {
    for (const cvc5::Term& assertion : section.assertions) {
      solver->assertFormula(assertion);
    }
  }
}

void Search::load_checker() {
  checker.emplace(tm);
  configure(*checker, encoding.check.logic);
  for (const cvc5::Term& assertion : encoding.check.assertions) {
    checker->assertFormula(assertion);
  }

  const MinimalityCheck& check = encoding.check;
  probe.reserve(check.read.size() + check.droppable.size());
  read_false.reserve(check.read.size());
  for (aspif::Atom atom : check.read) {
    probe.push_back(encoding.atom_var[atom]);
    read_false.push_back(literal_term(tm, encoding.atom_var, -atom));
  }
  droppable_subset.reserve(check.droppable.size());
  droppable_dropped.reserve(check.droppable.size());
  for (aspif::Atom atom : check.droppable) {
    probe.push_back(encoding.atom_var[atom]);
    droppable_subset.push_back(check.subset_var[atom]);
    droppable_dropped.push_back(
        tm.mkTerm(cvc5::Kind::NOT, {check.subset_var[atom]}));
  }
  assumptions.reserve(probe.size() + 1);
  droppable_held.reserve(check.droppable.size());
  holds_droppable.resize(check.droppable.size());
}

absl::StatusOr<std::vector<aspif::Atom>> Search::unfounded_set() {
  const MinimalityCheck& check = encoding.check;
  if (!checker.has_value()) load_checker();

  // The model under test goes in as assumptions rather than assertions, so one
  // checker answers about every model and keeps what it learned about the
  // reduct from one to the next.
  //
  // One call reads every value. Asking cvc5 one at a time costs more than
  // deciding the check does, there being a check per round.
  const std::vector<cvc5::Term> model = solver->getValue(probe);
  assumptions.clear();
  for (size_t i = 0; i < check.read.size(); ++i) {
    assumptions.push_back(model[i].getBooleanValue() ? probe[i]
                                                     : read_false[i]);
  }

  // A smaller model has to leave out every atom the model leaves out, and drop
  // at least one it holds. Both are about the subset variables alone, so the
  // model's own variables stay out of the assumptions, which is what keeps a
  // check from costing one assumption per atom of the component.
  droppable_held.clear();
  for (size_t i = 0; i < check.droppable.size(); ++i) {
    holds_droppable[i] = model[check.read.size() + i].getBooleanValue();
    if (holds_droppable[i]) {
      droppable_held.push_back(droppable_dropped[i]);
    } else {
      assumptions.push_back(droppable_dropped[i]);
    }
  }
  // With nothing to drop the model is as small as the reduct allows.
  std::vector<aspif::Atom> unfounded;
  if (droppable_held.empty()) return unfounded;
  assumptions.push_back(disjunction(tm, droppable_held));

  ASSIGN_OR_RETURN(const bool smaller,
                   decided(checker->checkSatAssuming(assumptions)));
  if (!smaller) return unfounded;

  // The atoms the smaller model drops are the unfounded set. Nothing the model
  // holds derives them except by going round the positive cycle they sit on.
  const std::vector<cvc5::Term> kept = checker->getValue(droppable_subset);
  for (size_t i = 0; i < check.droppable.size(); ++i) {
    if (holds_droppable[i] && !kept[i].getBooleanValue()) {
      unfounded.push_back(check.droppable[i]);
    }
  }
  return unfounded;
}

absl::StatusOr<bool> Search::find(const std::optional<cvc5::Term>& question) {
  if (!solver.has_value()) load();
  const bool needs_check = !encoding.check.droppable.empty();
  while (true) {
    // A question holds for this call alone, so it goes in as an assumption
    // rather than an assertion.
    ASSIGN_OR_RETURN(const bool found,
                     question.has_value()
                         ? decided(solver->checkSatAssuming(*question))
                         : decided(solver->checkSat()));
    if (!found) return false;
    if (!needs_check) return true;

    ASSIGN_OR_RETURN(const std::vector<aspif::Atom> unfounded, unfounded_set());
    if (unfounded.empty()) return true;
    // The nogood is true of every answer set, so it stays asserted. It rules
    // out this model and every other that leaves the same set unsupported.
    solver->assertFormula(loop_nogood(tm, *prog, encoding, unfounded));
  }
}

std::vector<aspif::Atom> Search::model_atoms() const {
  std::vector<aspif::Atom> atoms;
  for (aspif::Atom atom = 1; atom < prog->next_atom; ++atom) {
    if (solver->getValue(encoding.atom_var[atom]).getBooleanValue()) {
      atoms.push_back(atom);
    }
  }
  return atoms;
}

// Adds up the weights of the true literals, level by level. Level::cost says
// the same thing as a term, but reading that back would mean parsing the
// decimal cvc5 prints for it.
std::vector<BigInt> Search::model_costs() const {
  std::vector<BigInt> costs;
  costs.reserve(encoding.levels.size());
  for (const Level& level : encoding.levels) {
    BigInt total;
    for (size_t i = 0; i < level.lits.size(); ++i) {
      if (solver->getValue(level.lit_terms[i]).getBooleanValue()) {
        total += level.lits[i].weight;
      }
    }
    costs.push_back(std::move(total));
  }
  return costs;
}

absl::Status Search::settle_costs() {
  if (encoding.levels.empty()) return absl::OkStatus();
  ASSIGN_OR_RETURN(const bool found, find());
  if (!found) return absl::OkStatus();

  std::vector<BigInt> costs = model_costs();
  for (size_t i = 0; i < encoding.levels.size(); ++i) {
    const Level& level = encoding.levels[i];
    // A negative weight pays to be violated, so a level bottoms out at all of
    // its negative weights at once, not at zero. Stopping there keeps the walk
    // from asking about a cost nothing can reach.
    BigInt least;
    for (const aspif::WeightedLit& weighted : level.lits) {
      if (weighted.weight < BigInt(0)) least += weighted.weight;
    }

    while (costs[i] > least) {
      ASSIGN_OR_RETURN(const bool cheaper,
                       find(cost_at_most(tm, level, costs[i] - 1)));
      if (!cheaper) break;
      costs = model_costs();
    }
    // Holds the level at its least cost, so the levels after it settle under
    // it and every answer set reported still costs that.
    solver->assertFormula(cost_at_most(tm, level, costs[i]));
  }
  return absl::OkStatus();
}

void Search::block(const std::vector<aspif::Atom>& atoms) {
  // A program with no atoms has one model, the empty one, and no clause can say
  // anything about it. Callers stop before asking for a second.
  if (prog->next_atom <= 1) return;

  const absl::flat_hash_set<aspif::Atom> is_true(atoms.begin(), atoms.end());
  // The clause asks for some atom to differ from this model. Only the atoms are
  // named, never the level variables: one answer set admits many rankings, so
  // blocking a whole model would keep handing the same answer set back under a
  // different ranking.
  std::vector<cvc5::Term> literals;
  literals.reserve(prog->next_atom - 1);
  for (aspif::Atom atom = 1; atom < prog->next_atom; ++atom) {
    literals.push_back(
        is_true.contains(atom)
            ? tm.mkTerm(cvc5::Kind::NOT, {encoding.atom_var[atom]})
            : encoding.atom_var[atom]);
  }
  solver->assertFormula(disjunction(tm, literals));
}

}  // namespace

absl::StatusOr<SolveResult> solve(const aspif::Program& prog,
                                  const SolveOptions& options) {
  if (options.max_answer_sets < 0) {
    return absl::InvalidArgumentError(
        "max_answer_sets cannot be negative; 0 asks for all answer sets");
  }

  Search search;
  RETURN_IF_ERROR(search.start(prog));
  // Settling first is what leaves only optimal answer sets below.
  RETURN_IF_ERROR(search.settle_costs());

  SolveResult result;
  while (options.max_answer_sets == 0 ||
         result.answer_sets.size() <
             static_cast<size_t>(options.max_answer_sets)) {
    ASSIGN_OR_RETURN(const bool found, search.find());
    if (!found) {
      // Nothing left to find, so the search covered the whole space.
      result.exhausted = true;
      break;
    }

    AnswerSet answer_set;
    answer_set.costs = search.model_costs();
    answer_set.atoms = search.model_atoms();
    search.block(answer_set.atoms);
    result.answer_sets.push_back(std::move(answer_set));

    // A program with no atoms has the empty answer set and no other, and there
    // is no clause to block it with, so stop before asking again. That one
    // answer set is all of them.
    if (prog.next_atom <= 1) {
      result.exhausted = true;
      break;
    }
  }
  return result;
}

absl::StatusOr<QueryResult> answer_query(const aspif::Program& prog) {
  Search search;
  RETURN_IF_ERROR(search.start(prog));
  // The query asks about the optimal answer sets, so the costs settle first.
  RETURN_IF_ERROR(search.settle_costs());

  // A program with no answer set holds every query. Looking for an answer set
  // first is also what tells that case apart from a query that holds, since
  // both leave the searches below with nothing to find.
  ASSIGN_OR_RETURN(const bool coherent, search.find());
  if (!coherent) {
    return QueryResult{.answer = QueryAnswer::kYes, .no_answer_set = true};
  }

  // An atom this answer set leaves out is already answered no, so only the ones
  // it holds are worth a search. That is what makes a query over a large
  // predicate affordable, since one answer set usually leaves few standing. The
  // formula and the atom it stands for are paired up here because the searches
  // below take the formula and the answer names the atom. Encoding::query runs
  // parallel to aspif::Program::query.
  std::vector<std::pair<cvc5::Term, aspif::Lit>> standing;
  for (size_t i = 0; i < search.encoding.query.size(); ++i) {
    const cvc5::Term& formula = search.encoding.query[i];
    if (search.solver->getValue(formula).getBooleanValue()) {
      standing.emplace_back(formula, (*prog.query)[i]);
    }
  }

  QueryResult result;
  for (const auto& [formula, atom] : standing) {
    // Asking about an atom means looking for an answer set without it, a
    // question that belongs to this find() alone.
    const cvc5::Term without = search.tm.mkTerm(cvc5::Kind::NOT, {formula});
    ASSIGN_OR_RETURN(const bool refuted, search.find(without));
    // Every atom that holds is an answer, so the search runs to the end rather
    // than stopping at the first.
    if (!refuted) result.holds.push_back(atom);
  }
  result.answer = result.holds.empty() ? QueryAnswer::kNo : QueryAnswer::kYes;
  return result;
}
