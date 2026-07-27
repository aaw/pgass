#ifndef GROUND_H_
#define GROUND_H_

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
// rewritten as ASPIF minimize statements. A query becomes an ASPIF assumption.
// #min and #max aggregates are rejected with an error for now.
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
absl::StatusOr<aspif::Program> ground(const Program& prog);

#endif  // GROUND_H_
