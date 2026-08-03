#ifndef ENCODE_H_
#define ENCODE_H_

#include <cvc5/cvc5.h>

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "aspif.h"

/* The translation of a ground program into SMT, along the lines of Niemela,
   "Stable models and difference logic": the Clark completion of the program,
   plus a level ranking that rules out an atom supported by nothing but a
   positive cycle.

   build_encoding() is the translation itself, which solve.cc hands to cvc5 and
   searches. encode_smtlib() is the same translation as text, for a reader or
   another solver.
*/

// cvc5 rejects AND, OR, and ADD with fewer than two arguments, and every empty
// case comes up in practice. A rule with an empty body is a fact, so its body
// formula is true. An atom no rule derives can never hold, so its completion is
// false. A weight body with no literals adds up to zero.
cvc5::Term conjunction(cvc5::TermManager& tm,
                       const std::vector<cvc5::Term>& conjuncts);
cvc5::Term disjunction(cvc5::TermManager& tm,
                       const std::vector<cvc5::Term>& disjuncts);
cvc5::Term sum(cvc5::TermManager& tm, const std::vector<cvc5::Term>& addends);

// The formula for one aspif literal: the atom's variable, or its negation for a
// default-negated literal.
cvc5::Term literal_term(cvc5::TermManager& tm,
                        const std::vector<cvc5::Term>& atom_var,
                        aspif::Lit lit);

// The integer term adding up the weights of the true literals of `lits`. Each
// literal contributes its weight when it holds and zero when it does not. A
// weight body, a minimize statement, and the cost of a priority level are all
// this sum.
cvc5::Term weighted_sum(cvc5::TermManager& tm,
                        const std::vector<cvc5::Term>& atom_var,
                        const std::vector<aspif::WeightedLit>& lits);

// Whether at least `bound` of `lits` hold, as a formula about the literals
// rather than a sum of them.
//
// Saying it as a sum would be shorter, but it would also be the only arithmetic
// in an otherwise Boolean program, and it would cost the whole program QF_LIA.
// cvc5 then answers a question about counting Booleans by running simplex over
// terms lifted out of them, splitting on disequalities all the way. Measured on
// one ASP Competition graceful-graphs instance, the sum spent 59s where this
// spends 12s.
//
// This is the totalizer encoding: the literals are halved, each half counted,
// and the two counts added by cases. Counting stops at `bound`, since a count
// past it answers the same question, which keeps the formula O(n * bound)
// rather than O(n^2). The halves share their sub-counts, and cvc5 hash-conses
// terms, so a shared count is built once however often it is named.
cvc5::Term at_least(cvc5::TermManager& tm, const std::vector<cvc5::Term>& lits,
                    std::int64_t bound);

// One group of assertions and what it is called. They all have to hold, so the
// grouping changes nothing for the solver. It gives the script headings a
// reader can navigate by.
struct Section {
  const char* title = nullptr;
  std::vector<cvc5::Term> assertions;
};

// The whole translation of a ground program: the variables it declares and the
// formulas that hold of them.
struct Encoding {
  // The SMT logic the formulas fit in, QF_IDL or the wider QF_LIA.
  const char* logic = nullptr;
  // One Bool per atom, indexed by atom id. Slot 0 is a null Term, 0 being no
  // atom.
  std::vector<cvc5::Term> atom_var;
  // One integer per atom that can lie on a positive cycle, indexed the same
  // way. A null level variable is how to ask whether an atom needs ranking.
  std::vector<cvc5::Term> level_var;
  // What each atom variable is called, which is the symbol the program prints
  // for the atom where there is one.
  std::vector<std::string> atom_name;
  std::vector<Section> sections;
  // One formula per atom the program's query matched, empty where it matched
  // none. These are questions rather than assertions, so they are kept out of
  // the sections above. Asking one takes a search for an answer set that
  // falsifies it, which solve.cc runs.
  std::vector<cvc5::Term> query;
  // Whether a model of the assertions is only a candidate answer set, which is
  // what a head cycle leaves behind. Where this is false the assertions
  // describe the answer sets exactly.
  bool needs_reduct_check = false;
  // Whether each atom, indexed by atom id, sits in a head-cyclic component.
  // Those are the only atoms a minimality check has to vary: everywhere else
  // the level ranking already rules an unfounded atom out.
  std::vector<bool> in_head_cycle;
};

// Returns an UnimplementedError for a choice rule head. Nothing produces one:
// normalization rewrites choice rules into disjunctive ones.
absl::StatusOr<Encoding> build_encoding(cvc5::TermManager& tm,
                                        const aspif::Program& prog);

// The translation as an SMT-LIB script, asking '(check-sat)' and '(get-model)'
// at the end. Its models are the answer sets of `prog`, one model each, and a
// constant is named after the symbol the program prints for its atom, so that a
// model reads back as an answer set.
//
// A program with a query asserts the query negated, so its script asks the
// opposite question. A model of it is an answer set the query fails in, and
// 'unsat' means the query holds. A query no atom matched asserts nothing: no
// answer set can satisfy it, so 'unsat' there means the program has no answer
// set, which is the one way such a query holds.
//
// Three programs take more than the plain check-sat above:
//   - A head cycle, which asserts the minimality check as well. That assertion
//     quantifies over subsets, so the script is in the ALL logic.
//   - A query matching several atoms, which is one check-sat per atom, each
//     under a negation of its own in a push and pop scope, in place of the one
//     at the end.
//   - Weak constraints, whose optimum is known only after several runs under a
//     falling bound. The script names the cost of each priority level and the
//     steps that walk it down.
//
// Returns an UnimplementedError only for what build_encoding refuses.
absl::StatusOr<std::string> encode_smtlib(const aspif::Program& prog);

#endif  // ENCODE_H_
