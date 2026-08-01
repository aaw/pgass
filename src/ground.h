#ifndef GROUND_H_
#define GROUND_H_

#include <cstddef>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "aspif.h"
#include "ast.h"

// Grounds a normalized program, replacing every rule containing variables with
// all of its variable-free instances and returns the result as an aspif
// program.
//
// Only normal programs are supported so far: rules and constraints whose
// terms are numbers, strings, constants, variables, and '_', e.g.
//
//   edge(a, b).
//   reachable(X, Y) :- edge(X, Y).
//   reachable(X, Z) :- reachable(X, Y), edge(Y, Z).
//
// Arithmetic and function terms like f(X) are supported too, as are #count
// and #sum aggregates, rewritten as ASPIF weight bodies, and weak constraints,
// rewritten as ASPIF minimize statements. A query becomes the list of its
// matching ground atoms, which solve.h asks every answer set about one at a
// time.
//
// #min and #max range over the ASP-Core-2 order on terms rather than over
// numbers, so '#max{ X : p(X) }' over p(1) and p(a) is a. Each becomes rules
// over one atom per element tuple: a #min is at most some term exactly when
// some tuple is. An empty set is +infinity for #min and -infinity for #max.
// Those compare against a term but are no term themselves, so a variable taking
// the value binds to nothing: 'q(S) :- #min{ X : p(X) } = S.' derives no q when
// nothing supports p.
//
// An atom that holds in every answer set is stated as a fact rather than
// through the rules deriving it, and drops out of the bodies asking for it, so
// the program above grounds to reachable facts and no rules at all. An
// aggregate over such atoms is worked out here rather than left to the solver,
// so '#count{ X : p(X) } >= 1' over a p fact adds nothing to the program.
//
// `prog` must have passed verify_safe() and normalize(); ground() re-checks
// neither. Grounding an unsafe program can run forever, e.g. 'p(0). p(S) :-
// #count{ X : p(X) } = S.', where every new p atom gives the count one more
// atom to range over.
//
// `max_ground_atoms` is how many ground atoms grounding may derive before it
// gives up with a ResourceExhaustedError naming the predicate it was deriving.
// 0 means no limit, which lets a program with no finite grounding, such as
// 'p(1). p(X+1) :- p(X).', run until the machine runs out of memory.
//
// A term with no value, e.g. the 'a + 1' that 'X + 1' becomes under {X: a},
// gives its rule no ground instance under that binding. Grounding drops the
// instance. If `warnings` is not null, it also appends one line naming the rule,
// so that a program dropping rules this way does not do it in silence. A rule
// warns once however many instances it drops.
absl::StatusOr<aspif::Program> ground(
    const Program& prog, std::vector<std::string>* warnings = nullptr,
    size_t max_ground_atoms = 0);

#endif  // GROUND_H_
