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
// #count and #sum aggregates are also supported, rewritten as ASPIF weight
// bodies. Everything else (#min/#max aggregates, arithmetic, function terms
// like f(X), weak constraints, queries) is rejected with an error for now.
absl::StatusOr<aspif::Program> ground(const Program& prog);

#endif  // GROUND_H_
