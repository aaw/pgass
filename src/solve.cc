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

// Reads an integer the solver assigned. cvc5 prints one in decimal and works in
// unbounded integers, as BigInt does, so no cost is too big to read back.
absl::StatusOr<BigInt> cost_value(const cvc5::Term& value) {
  const std::string text = value.getIntegerValue();
  std::optional<BigInt> cost = BigInt::from_decimal(text);
  if (!cost.has_value()) {
    return absl::InternalError(absl::StrCat("cvc5 gave a cost of '", text,
                                            "', which is not a number"));
  }
  return *std::move(cost);
}

// One search over one ground program. The encoding describes the answer sets
// exactly, so every model the solver returns is an answer set, and an optimal
// one where the program has weak constraints.
//
// Every member holds terms of `tm`, so `tm` is declared first and destroyed
// last.
struct Search {
  cvc5::TermManager tm;
  const aspif::Program* prog = nullptr;
  Encoding encoding;
  std::optional<cvc5::Solver> solver;
  // Clauses ruling out the answer sets already handed back, so that each is
  // reported once. A fresh solver takes them along with the encoding.
  std::vector<cvc5::Term> blocked;

  // Builds the encoding of `program`. Nothing is asked of cvc5 until find().
  absl::Status start(const aspif::Program& program);

  // Whether the program has an answer set none of `blocked` rules out.
  // `question`, where given, holds for this search alone.
  absl::StatusOr<bool> find(
      const std::optional<cvc5::Term>& question = std::nullopt);

  // The atoms true in the model the solver holds.
  std::vector<aspif::Atom> model_atoms() const;

  // What that model cost at each priority level, most important level first.
  // Empty for a program with no weak constraints.
  absl::StatusOr<std::vector<BigInt>> model_costs() const;

  // Rules the model out of every later find().
  void block(const std::vector<aspif::Atom>& atoms);

  // Whether each find() builds a solver of its own. The optimality assertion
  // is only affordable under cvc5's sygus-inst, 2s against 77s on a vertex
  // cover of 30 nodes, and cvc5 refuses that option alongside incremental
  // solving. Every other program keeps its solver and the work it has done,
  // which is what makes enumeration affordable: 200 answer sets of a
  // head-cyclic program take 0.08s that way and over 200s the other.
  bool one_solver_per_question() const {
    return encoding.optimality.has_value();
  }

  // A solver holding the encoding and everything blocked so far.
  void load();
};

absl::Status Search::start(const aspif::Program& program) {
  prog = &program;
  ASSIGN_OR_RETURN(encoding, build_encoding(tm, program));
  return absl::OkStatus();
}

void Search::load() {
  solver.emplace(tm);
  solver->setLogic(encoding.logic);
  solver->setOption("produce-models", "true");
  solver->setOption("sat-solver", "cadical");
  // cvc5 solves incrementally unless told otherwise, and refuses sygus-inst
  // there, so a solver that answers one question has to say so.
  solver->setOption("incremental",
                    one_solver_per_question() ? "false" : "true");
  if (one_solver_per_question()) solver->setOption("sygus-inst", "true");

  for (const Section& section : encoding.sections) {
    for (const cvc5::Term& assertion : section.assertions) {
      solver->assertFormula(assertion);
    }
  }
  // Asserting optimality is what leaves only optimal answer sets to find, so
  // nothing here searches for the least cost of a level.
  if (encoding.optimality.has_value()) {
    solver->assertFormula(*encoding.optimality);
  }
  for (const cvc5::Term& clause : blocked) solver->assertFormula(clause);
}

absl::StatusOr<bool> Search::find(const std::optional<cvc5::Term>& question) {
  if (!solver.has_value() || one_solver_per_question()) load();
  if (!question.has_value()) return decided(solver->checkSat());
  // A question holds for this find() alone. A solver that is thrown away after
  // it can simply assert it, and one that stays takes it as an assumption.
  if (one_solver_per_question()) {
    solver->assertFormula(*question);
    return decided(solver->checkSat());
  }
  return decided(solver->checkSatAssuming(*question));
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

absl::StatusOr<std::vector<BigInt>> Search::model_costs() const {
  std::vector<BigInt> costs;
  costs.reserve(encoding.level_cost.size());
  for (const cvc5::Term& cost : encoding.level_cost) {
    ASSIGN_OR_RETURN(BigInt value, cost_value(solver->getValue(cost)));
    costs.push_back(std::move(value));
  }
  return costs;
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
  blocked.push_back(disjunction(tm, literals));
  // The solver in hand keeps searching, so it hears about the clause now. A
  // fresh one picks it up from `blocked`.
  if (!one_solver_per_question()) solver->assertFormula(blocked.back());
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
    ASSIGN_OR_RETURN(answer_set.costs, search.model_costs());
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
