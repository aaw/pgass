#include "solve.h"

#include <cvc5/cvc5.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "aspif.h"
#include "encode.h"
#include "macros.h"

namespace {

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

absl::Status undecided(const cvc5::Result& result) {
  return absl::InternalError(
      absl::StrCat("cvc5 returned '", result.toString(),
                   "' rather than deciding the program"));
}

// One rule of the reduct, as a formula over `subset_var`. A null Term means the
// rule is not in the reduct at all, because no subset of the candidate can
// satisfy its body.
//
// A normal body drops its default-negated literals, which the candidate has
// already settled. 'not q' where the candidate holds q is false, and takes the
// rule with it. 'not q' where it does not hold q stays true however small the
// subset, so it says nothing and goes. A positive atom outside the candidate is
// false in every subset, and takes the rule too.
//
// A weight body keeps its shape, since which literals hold decides whether it
// reaches its bound. Weights are positive, so an atom outside the subset only
// lowers the sum. A default-negated literal contributes the constant weight the
// candidate gives it.
cvc5::Term reduct_body(cvc5::TermManager& tm, const aspif::Rule& rule,
                       const absl::flat_hash_set<aspif::Atom>& candidate,
                       const std::vector<cvc5::Term>& subset_var) {
  if (rule.body_type == aspif::Rule::BodyType::kNormal) {
    std::vector<cvc5::Term> conjuncts;
    conjuncts.reserve(rule.body.size());
    for (aspif::Lit lit : rule.body) {
      if (lit < 0 && candidate.contains(-lit)) return cvc5::Term();
      if (lit > 0 && !candidate.contains(lit)) return cvc5::Term();
      if (lit > 0) conjuncts.push_back(subset_var[lit]);
    }
    return conjunction(tm, conjuncts);
  }

  const cvc5::Term zero = tm.mkInteger(0);
  std::vector<cvc5::Term> addends;
  addends.reserve(rule.weighted_body.size());
  for (const aspif::WeightedLit& weighted : rule.weighted_body) {
    const cvc5::Term weight = tm.mkInteger(weighted.weight);
    if (weighted.lit < 0 && !candidate.contains(-weighted.lit)) {
      addends.push_back(weight);
    } else if (weighted.lit > 0 && candidate.contains(weighted.lit)) {
      addends.push_back(
          tm.mkTerm(cvc5::Kind::ITE, {subset_var[weighted.lit], weight, zero}));
    }
  }
  return tm.mkTerm(cvc5::Kind::GEQ,
                   {sum(tm, addends), tm.mkInteger(rule.lower_bound)});
}

// Whether some proper subset of `candidate` models the reduct of `prog` with
// respect to `candidate`. That is how a candidate model fails to be an answer
// set. ASP-Core-2 defines an answer set the same way: a model I of P such that
// no proper subset of I models the reduct of P with respect to I.
//
// One Boolean per candidate atom stands for that subset. Atoms outside the
// candidate are false in every subset of it, so they need no variable.
//
// This is the second solver call a head cycle costs. It asks for the absence
// of a smaller model, which no search for a model can answer on its own.
absl::StatusOr<bool> subset_models_reduct(
    cvc5::TermManager& tm, const aspif::Program& prog, const char* logic,
    const absl::flat_hash_set<aspif::Atom>& candidate) {
  // The empty candidate has no proper subset.
  if (candidate.empty()) return false;

  cvc5::Solver checker(tm);
  checker.setLogic(logic);

  std::vector<cvc5::Term> subset_var(prog.next_atom);
  const cvc5::Sort bool_sort = tm.getBooleanSort();
  std::vector<cvc5::Term> dropped;
  dropped.reserve(candidate.size());
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    if (!candidate.contains(atom)) continue;
    subset_var[atom] = tm.mkConst(bool_sort, absl::StrCat("s", atom));
    dropped.push_back(tm.mkTerm(cvc5::Kind::NOT, {subset_var[atom]}));
  }
  // Proper: the subset leaves at least one candidate atom out.
  checker.assertFormula(disjunction(tm, dropped));

  for (const aspif::Rule& rule : prog.rules) {
    const cvc5::Term body = reduct_body(tm, rule, candidate, subset_var);
    if (body.isNull()) continue;
    // Only head atoms the candidate holds can satisfy the rule, a subset of it
    // holding no others. An empty head leaves an integrity constraint, which
    // rules no subset out: its body is false under the candidate, which is a
    // model, and stays false under anything smaller.
    std::vector<cvc5::Term> heads;
    for (aspif::Atom head : rule.head) {
      if (candidate.contains(head)) heads.push_back(subset_var[head]);
    }
    checker.assertFormula(
        tm.mkTerm(cvc5::Kind::IMPLIES, {body, disjunction(tm, heads)}));
  }

  const cvc5::Result result = checker.checkSat();
  if (result.isSat()) return true;
  if (result.isUnsat()) return false;
  return undecided(result);
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

// The solver, and what looking for answer sets rather than models takes.
//
// Every search goes through find(). Where a component is head-cyclic, a model
// of the assertions can fail to be an answer set. find() checks each model it is
// handed and rules out the ones that fail, until it holds an answer set or the
// solver runs out of models. Where no component is head-cyclic, the assertions
// describe the answer sets exactly and find() is one checkSat().
struct Search {
  cvc5::TermManager& tm;
  cvc5::Solver& solver;
  const aspif::Program& prog;
  const std::vector<cvc5::Term>& atom_var;
  // Whether a model has to pass the minimality check to count as an answer set.
  bool check_reduct = false;
  // The logic to check the reduct in, which is the one the program is solved in.
  const char* logic = nullptr;

  // Clauses ruling out the models already handed back, the answer sets and the
  // failed candidates alike. Both are permanent. A model that is not an answer
  // set never becomes one, and an answer set is reported once. pop() drops what
  // its scope asserted, so they are kept here and asserted again on the way out.
  std::vector<cvc5::Term> blocked;
  // Where in `blocked` each open push() scope began.
  std::vector<size_t> scopes;

  // Whether the solver holds an answer set. `assumption`, where given, holds for
  // this search alone.
  absl::StatusOr<bool> find(
      const std::optional<cvc5::Term>& assumption = std::nullopt);

  // The atoms true in the model the solver holds.
  std::vector<aspif::Atom> model_atoms() const;

  // Rules a model out of every later search.
  void block(const std::vector<aspif::Atom>& atoms);

  void push();
  void pop();
};

std::vector<aspif::Atom> Search::model_atoms() const {
  std::vector<aspif::Atom> atoms;
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    if (solver.getValue(atom_var[atom]).getBooleanValue()) {
      atoms.push_back(atom);
    }
  }
  return atoms;
}

void Search::block(const std::vector<aspif::Atom>& atoms) {
  // A program with no atoms has one model, the empty one, and no clause can say
  // anything about it. Callers stop before asking for a second.
  if (prog.next_atom <= 1) return;

  const absl::flat_hash_set<aspif::Atom> is_true(atoms.begin(), atoms.end());
  // The clause asks for some atom to differ from this model. Only the atoms are
  // named, never the level variables: one answer set admits many rankings, so
  // blocking a whole model would keep handing the same answer set back under a
  // different ranking.
  std::vector<cvc5::Term> literals;
  literals.reserve(prog.next_atom - 1);
  for (aspif::Atom atom = 1; atom < prog.next_atom; ++atom) {
    literals.push_back(is_true.contains(atom)
                           ? tm.mkTerm(cvc5::Kind::NOT, {atom_var[atom]})
                           : atom_var[atom]);
  }
  const cvc5::Term clause = disjunction(tm, literals);
  blocked.push_back(clause);
  solver.assertFormula(clause);
}

void Search::push() {
  scopes.push_back(blocked.size());
  solver.push();
}

void Search::pop() {
  solver.pop();
  // The scope took the clauses asserted inside it with it, so they go back in.
  for (size_t i = scopes.back(); i < blocked.size(); ++i) {
    solver.assertFormula(blocked[i]);
  }
  scopes.pop_back();
}

absl::StatusOr<bool> Search::find(
    const std::optional<cvc5::Term>& assumption) {
  while (true) {
    const cvc5::Result result = assumption.has_value()
                                    ? solver.checkSatAssuming(*assumption)
                                    : solver.checkSat();
    if (result.isUnsat()) return false;
    if (!result.isSat()) return undecided(result);
    if (!check_reduct) return true;

    const std::vector<aspif::Atom> atoms = model_atoms();
    ASSIGN_OR_RETURN(
        const bool smaller,
        subset_models_reduct(
            tm, prog, logic,
            absl::flat_hash_set<aspif::Atom>(atoms.begin(), atoms.end())));
    if (!smaller) return true;
    // A model of the rules that a smaller model of its reduct undercuts is no
    // answer set, whatever cost bound it was found under, so it is out for good.
    block(atoms);
  }
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
// unsatisfiable by construction, so it has to be popped before anything else
// is asked.
LeastCost minimize_by_stepping_down(Search& search, const LevelCost& level) {
  cvc5::TermManager& tm = search.tm;
  std::optional<std::int64_t> best;
  search.push();
  while (true) {
    ASSIGN_OR_RETURN(const bool found, search.find());
    if (!found) break;
    ASSIGN_OR_RETURN(best, int64_value(search.solver.getValue(level.cost)));
    search.solver.assertFormula(
        tm.mkTerm(cvc5::Kind::LT, {level.cost, tm.mkInteger(*best)}));
  }
  search.pop();
  return best;
}

// Halves the range the least cost is known to lie in until it holds one value.
//
// `high` is always a cost some answer set reaches and `low` one that none
// beats. So the least cost stays in [low, high] and the two meet on it. Each
// probe either pulls `high` down to the cost of the answer set it found or
// lifts `low` past the bound it refuted. Both of those are permanent. The
// probed bound itself is not, so it goes in as an assumption of the one solver
// call rather than an assertion.
LeastCost minimize_by_bisecting(Search& search, const LevelCost& level) {
  cvc5::TermManager& tm = search.tm;
  // One answer set to start the range off. Its absence is how an unsatisfiable
  // program shows up.
  ASSIGN_OR_RETURN(const bool found, search.find());
  if (!found) return std::nullopt;
  ASSIGN_OR_RETURN(std::int64_t high,
                   int64_value(search.solver.getValue(level.cost)));
  search.solver.assertFormula(
      tm.mkTerm(cvc5::Kind::LEQ, {level.cost, tm.mkInteger(high)}));

  std::int64_t low = level.lower_bound;
  while (low < high) {
    // Rounds down, so the midpoint stays below `high` and the range really
    // shrinks.
    const std::int64_t middle = low + (high - low) / 2;
    ASSIGN_OR_RETURN(
        const bool under_bound,
        search.find(tm.mkTerm(cvc5::Kind::LEQ,
                              {level.cost, tm.mkInteger(middle)})));
    if (under_bound) {
      // The answer set found may cost well under what the probe asked for, so
      // take its cost rather than the midpoint.
      ASSIGN_OR_RETURN(high, int64_value(search.solver.getValue(level.cost)));
      search.solver.assertFormula(
          tm.mkTerm(cvc5::Kind::LEQ, {level.cost, tm.mkInteger(high)}));
    } else {
      low = middle + 1;
      search.solver.assertFormula(
          tm.mkTerm(cvc5::Kind::GEQ, {level.cost, tm.mkInteger(low)}));
    }
  }
  return high;
}

LeastCost minimize_level(Search& search, const LevelCost& level,
                         SolveOptions::Optimizer optimizer) {
  switch (optimizer) {
    case SolveOptions::Optimizer::kLinear:
      return minimize_by_stepping_down(search, level);
    case SolveOptions::Optimizer::kBisect:
      return minimize_by_bisecting(search, level);
  }
  return std::nullopt;  // unreachable
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
    Search& search, const std::vector<LevelCost>& level_costs,
    SolveOptions::Optimizer optimizer) {
  std::vector<std::int64_t> costs;
  costs.reserve(level_costs.size());
  for (const LevelCost& level : level_costs) {
    ASSIGN_OR_RETURN(const std::optional<std::int64_t> least,
                     minimize_level(search, level, optimizer));
    // No cost at all means the program is unsat, which the caller finds out for
    // itself when it looks for an answer set.
    if (!least.has_value()) return std::vector<std::int64_t>();
    costs.push_back(*least);
    search.solver.assertFormula(search.tm.mkTerm(
        cvc5::Kind::EQUAL, {level.cost, search.tm.mkInteger(*least)}));
  }
  return costs;
}

// What it takes to search one program: the encoding, the solver holding it, and
// the Search over that solver. They live together because a Search holds
// references to the other two, and because building them has to happen in this
// order. They all hold terms of `tm`, so `tm` is declared first and destroyed
// last.
struct Session {
  cvc5::TermManager tm;
  Encoding encoding;
  std::optional<cvc5::Solver> solver;
  std::optional<Search> search;
  // The cost of each priority level, pinned to its least value. Empty for a
  // program with no minimize statements, and for one with no answer set.
  std::vector<std::int64_t> costs;

  // Builds the encoding, loads it into a solver, and settles every weak
  // constraint level. On return the solver is satisfiable exactly when the
  // program has an answer set, and every answer set it still admits is optimal.
  absl::Status start(const aspif::Program& prog, const SolveOptions& options);
};

absl::Status Session::start(const aspif::Program& prog,
                            const SolveOptions& options) {
  ASSIGN_OR_RETURN(encoding, build_encoding(tm, prog));

  solver.emplace(tm);
  solver->setLogic(encoding.logic);
  solver->setOption("produce-models", "true");
  solver->setOption("incremental", "true");
  for (const Section& section : encoding.sections) {
    for (const cvc5::Term& assertion : section.assertions) {
      solver->assertFormula(assertion);
    }
  }

  // The encoding settles everything but a head cycle, which is what turns the
  // minimality check on.
  search.emplace(Search{.tm = tm,
                        .solver = *solver,
                        .prog = prog,
                        .atom_var = encoding.atom_var,
                        .check_reduct = encoding.needs_reduct_check,
                        .logic = encoding.logic});

  // A program with no minimize statements has no levels, so this comes back
  // with no cost and no solver call made.
  ASSIGN_OR_RETURN(
      costs, optimize(*search, collect_level_costs(tm, prog, encoding.atom_var),
                      options.optimizer));
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<std::vector<AnswerSet>> solve(const aspif::Program& prog,
                                             const SolveOptions& options) {
  if (options.max_answer_sets < 0) {
    return absl::InvalidArgumentError(
        "max_answer_sets cannot be negative; 0 asks for all answer sets");
  }

  Session session;
  RETURN_IF_ERROR(session.start(prog, options));
  Search& search = *session.search;

  std::vector<AnswerSet> answer_sets;
  while (options.max_answer_sets == 0 ||
         answer_sets.size() < static_cast<size_t>(options.max_answer_sets)) {
    ASSIGN_OR_RETURN(const bool found, search.find());
    if (!found) break;

    AnswerSet answer_set;
    answer_set.costs = session.costs;
    answer_set.atoms = search.model_atoms();
    search.block(answer_set.atoms);
    answer_sets.push_back(std::move(answer_set));

    // A program with no atoms has the empty answer set and no other, and there
    // is no clause to block it with, so stop before asking again.
    if (prog.next_atom <= 1) break;
  }
  return answer_sets;
}

absl::StatusOr<QueryAnswer> answer_query(const aspif::Program& prog,
                                         const SolveOptions& options) {
  Session session;
  RETURN_IF_ERROR(session.start(prog, options));
  Search& search = *session.search;

  // A program with no answer set is asked nothing at all. Looking for an answer
  // set first is also what tells that case apart from a query that holds. Both
  // leave the searches below with nothing to find.
  ASSIGN_OR_RETURN(const bool coherent, search.find());
  if (!coherent) return QueryAnswer::kNoAnswerSet;

  // An atom this answer set leaves out is already answered no, so only the ones
  // it holds are worth a search. That is what makes a query over a large
  // predicate affordable, since one answer set usually leaves few standing.
  std::vector<cvc5::Term> standing;
  for (const cvc5::Term& atom : session.encoding.query) {
    if (search.solver.getValue(atom).getBooleanValue()) {
      standing.push_back(atom);
    }
  }

  for (const cvc5::Term& atom : standing) {
    // Asking about an atom means looking for an answer set without it. That
    // goes in a scope of its own rather than as an assumption, because find()
    // can make several solver calls and an assumption holds for one.
    search.push();
    search.solver.assertFormula(session.tm.mkTerm(cvc5::Kind::NOT, {atom}));
    ASSIGN_OR_RETURN(const bool refuted, search.find());
    search.pop();
    if (!refuted) return QueryAnswer::kYes;
  }
  return QueryAnswer::kNo;
}
