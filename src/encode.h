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
  // Whether a model of the assertions is only a candidate answer set, which is
  // what a head cycle leaves behind. Where this is false the assertions
  // describe the answer sets exactly.
  bool needs_reduct_check = false;
};

// Returns an UnimplementedError for a choice rule head. Nothing produces one:
// normalization rewrites choice rules into disjunctive ones.
absl::StatusOr<Encoding> build_encoding(cvc5::TermManager& tm,
                                        const aspif::Program& prog);

// The translation as an SMT-LIB script, ending in '(check-sat)' and
// '(get-model)'. Its models are the answer sets of `prog`, one model each, and
// a constant is named after the symbol the program prints for its atom, so that
// a model reads back as an answer set.
//
// Returns an UnimplementedError where one script cannot say what an answer set
// is. A program with weak constraints is one, its optimal cost being known only
// after several queries. So is a program with a head cycle, whose models are
// candidates a query against the reduct still has to pass.
absl::StatusOr<std::string> encode_smtlib(const aspif::Program& prog);

#endif  // ENCODE_H_
