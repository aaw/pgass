#ifndef NORMALIZE_H_
#define NORMALIZE_H_

#include <string_view>

#include "absl/status/status.h"
#include "ast.h"

// The prefix on a classically negated predicate: '-p(1)' is '_neg_p(1)' after
// normalization. Later stages match on it to print the atom as '-p(1)' again.
inline constexpr std::string_view kClassicalNegationPrefix = "_neg_";

// The predicate a shown term becomes: '#show t : body.' turns into
// '_show(t) :- body.'. name_outputs() prints the argument of each one. Every
// shown term shares this one predicate, which is what makes them a set: two
// statements producing the same term produce the same ground atom.
inline constexpr std::string_view kShowPredicate = "_show";

// The predicate a weak constraint becomes: ':~ body. [w@l, t]' turns into
// '_viol(l, w, t) :- body.'. emit_minimize() reads the cost of each level off
// these atoms.
inline constexpr std::string_view kViolationPredicate = "_viol";

// The prefix on the variable an interval is lifted to: 'p(1..3).' becomes
// 'p(_R0) :- _R0 = 1..3.'. The lexer reads no identifier starting with '_', so
// no program can write a name that collides.
inline constexpr std::string_view kIntervalVariablePrefix = "_R";

// Rewrites `prog` into the shape ground() expects: '#minimize' statements
// become weak constraints, '#const' names give way to the terms they stand
// for, '#show' statements become '_show' rules and a print filter, intervals
// become comparisons that generate them, weak constraints become '_viol'
// rules, classical negation becomes fresh '_neg_' predicates, and choice rules
// become disjunctive rules plus a counting constraint.
//
// A disjunctive head passes through as it stands. Turning 'a | b :- body.' into
// normal rules is only sound while no two head atoms lie on a common positive
// cycle, which is a question about ground atoms. solve.cc asks it.
absl::Status normalize(Program& prog);

#endif  // NORMALIZE_H_
