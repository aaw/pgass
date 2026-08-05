#ifndef ENCODE_H_
#define ENCODE_H_

#include <cvc5/cvc5.h>

#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "aspif.h"
#include "bigint.h"

/* The translation of a ground program into SMT, along the lines of Niemela,
   "Stable models and difference logic": the Clark completion of the program,
   plus a level ranking that rules out an atom supported by nothing but a
   positive cycle.

   build_encoding() is the translation itself, which solve.cc hands to cvc5 and
   searches. encode_smtlib() is the same translation as text, for a reader or
   another solver.

   The two say the same thing, so the script is what solve.cc asserts, and
   running it answers what solving answers. Weak constraints take several runs
   either way. solve.cc asks for a cheaper answer set until there is none left,
   and the script carries those same steps for a reader to follow by hand.
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

// One priority level of the weak constraints. Two minimize statements sharing
// a priority make one level, and its cost is the total over their literals.
struct Level {
  BigInt priority;
  std::vector<aspif::WeightedLit> lits;
  // The formula for each of `lits`, in the same order.
  std::vector<cvc5::Term> lit_terms;
  // The weights of the true literals added up.
  cvc5::Term cost;
};

// Whether `level` costs at most `bound`.
//
// Unit weights make the cost a count of the true literals, which at_least()
// bounds without arithmetic. Any other level compares its sum instead.
//
// Which one is used matters near the least cost. A sum gives the solver almost
// nothing to propagate there: on an ASP Competition still-life instance the
// count settled a bound in seconds that the sum left open past 15 minutes.
cvc5::Term cost_at_most(cvc5::TermManager& tm, const Level& level,
                        const BigInt& bound);

// One group of assertions and what it is called. They all have to hold, so the
// grouping changes nothing for the solver. It gives the script headings a
// reader can navigate by.
struct Section {
  const char* title = nullptr;
  // Comment lines the script prints above the assertions, each already led by
  // a ';'. Empty where the title says enough.
  const char* comment = "";
  std::vector<cvc5::Term> assertions;
};

// The whole translation of a ground program: the variables it declares and the
// formulas that hold of them.
struct Encoding {
  // The SMT logic the formulas fit in: QF_IDL, the wider QF_LIA, or ALL where
  // a head cycle brings the quantified minimality assertion.
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
  // The priority levels, the most important first, which is the order they
  // have to be settled in. Empty for a program with no weak constraints.
  std::vector<Level> levels;
  // One formula per atom the program's query matched, empty where it matched
  // none. These are questions rather than assertions, so they are kept out of
  // the sections above. Asking one takes a search for an answer set that
  // falsifies it, which solve.cc runs.
  std::vector<cvc5::Term> query;
};

// The sections together describe the answer sets exactly: a model of all of
// them is an answer set, and every answer set is one of their models. A rule
// with two head atoms on a common positive cycle takes a quantified minimality
// assertion for that, which is a section of its own.

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
//   - Weak constraints, which name the cost of each priority level and print
//     it. Bringing a cost down to the least takes a run per step, so this is
//     the one script a reader edits between runs rather than running once.
//     The steps are written out in the script itself. Where a query comes
//     with them, they settle the costs first, and the query then asks about
//     the optimal answer sets.
//
// Returns an UnimplementedError only for what build_encoding refuses.
absl::StatusOr<std::string> encode_smtlib(const aspif::Program& prog);

#endif  // ENCODE_H_
