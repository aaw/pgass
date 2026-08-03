#ifndef ASPIF_H_
#define ASPIF_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "bigint.h"

/* A ground program in the aspif intermediate format (Kaminski et al.,
   "How to build your own ASP-based system?!", appendix B), the line-based
   format that gringo emits and clasp accepts. Each struct below mirrors one
   aspif statement type, so to_aspif is a straight transcription.

   Atoms are bare positive integers with no meaning of their own; the only
   place a symbolic name appears is an Output statement. There is no
   aggregate struct because aspif has no aggregate statement: a program with
   aggregates must be rewritten into the two rule forms below before it can
   be represented here.
*/
namespace aspif {

// An atom is a positive integer. A literal is a signed atom: negative means
// default-negated. 0 is never a valid literal.
using Atom = std::int32_t;
using Lit = std::int32_t;

struct WeightedLit {
  Lit lit;
  BigInt weight;

  bool operator==(const WeightedLit&) const = default;
};

// Statement 1. Serialized as '1 H B' where H is 'h m a1 ... am' and B is
// '0 n l1 ... ln' (normal) or '1 lower_bound n l1 w1 ... ln wn' (weight).
struct Rule {
  enum class HeadType { kDisjunction = 0, kChoice = 1 };
  enum class BodyType { kNormal = 0, kWeight = 1 };

  HeadType head_type = HeadType::kDisjunction;
  // Empty disjunctive head = integrity constraint. pgass normalization never
  // produces choice heads, but aspif has them. It does produce multi-atom
  // disjunctions: a choice rule becomes one per element, 'p | _ch_p'.
  std::vector<Atom> head;

  BodyType body_type = BodyType::kNormal;
  // Valid iff body_type == kNormal.
  std::vector<Lit> body;
  // Valid iff body_type == kWeight: the body holds when the weights of the
  // true literals sum to at least lower_bound. Weights must be positive.
  BigInt lower_bound;
  std::vector<WeightedLit> weighted_body;
};

// Statement 2. All weighted literals of one priority level; a solver
// minimizes the sum of weights of true literals, lower levels less important.
struct Minimize {
  BigInt priority;
  std::vector<WeightedLit> lits;
};

// Statement 4. Print `name` in every answer set where all of `condition`
// holds; an empty condition prints unconditionally.
struct Output {
  std::string name;
  std::vector<Lit> condition;
};

struct Program {
  std::vector<Rule> rules;
  std::vector<Minimize> minimize;
  std::vector<Output> outputs;
  // The atoms the program's query matched. 'p(X)?' over p(1) and p(2) puts
  // both of them here. They are alternatives. solve.h asks about each one on
  // its own, and the query holds where one of them holds in every answer set.
  //
  // No value means the program asks nothing. An empty list means it asks
  // something no atom matched, as 'p(1). q(2)?' does.
  //
  // aspif has no query statement. A query of one atom prints as statement 6,
  // an assumption. A longer list does not print at all. An assumption asks for
  // all of its literals at once, which is not what these atoms mean.
  std::optional<std::vector<Lit>> query;
  // The query as the program wrote it, e.g. "p(X)?", for anything that reports
  // on the query rather than answering it. Empty where the program was read as
  // aspif, which has no query statement to carry the text.
  std::string query_text;

  // Atom ids are allocated here so rules and auxiliaries share one space.
  Atom next_atom = 1;
  Atom new_atom() { return next_atom++; }
};

// Serializes prog as an aspif document: 'asp 1 0 0' header, one statement per
// line, terminated by a line holding 0.
std::string to_aspif(const Program& prog);

// Reads an aspif document back into a Program, so that a program ground
// elsewhere, by gringo or by another run of pgass, can be solved here. The
// statement types read are the ones to_aspif writes: rules, minimize, output
// and assumptions. A comment is skipped, and any other statement type is an
// error, since passing over one would answer a different program.
//
// A one-literal assumption becomes the query, which is what to_aspif writes a
// ground query as. Longer assumptions ask for all of their literals at once,
// which no query means, so they are an error.
//
// Program::next_atom comes back past the largest atom the document names, so
// the atoms solving allocates stay clear of the ones in use.
absl::StatusOr<Program> from_aspif(std::string_view text);

// Rewrites every choice rule of `prog` into the disjunctive rules solving
// takes: '{a1; ...; an} :- B' becomes 'ai | chi :- B' for each element, where
// chi is a fresh atom saying ai was passed over. Where B holds, a minimal
// model takes exactly one of ai and chi, which is the free choice, and chi
// appears nowhere else, so it restricts nothing. Normalization rewrites the
// choice rules of an ASP program the same way.
void replace_choice_rules(Program& prog);

}  // namespace aspif

#endif  // ASPIF_H_
