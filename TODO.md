* Ordinary recursion (a single-headed rule deriving an atom from itself
  through a positive cycle) is ranked with an Int level variable per atom, and
  CDCL(T) has to make real theory decisions to order them. clingo does this as
  Boolean unit propagation during search, never touching arithmetic. Labyrinth
  shows this in isolation: no head cycle, no loop to run, one checkSat call
  ranking `reach(X,Y,T)` is the whole 55.8s a timeout instance spends. Atom
  count does not predict the gap: 0060 ranks 1728 atoms and solves in 2.76s;
  0045 ranks only 1000 and takes 12.52s, since what is expensive is deciding
  through the arithmetic layer, not the size of it. MinimalDiagnosis has the
  same shape of recursion in `reach`/`coppo` over its graph, and shows the
  same signature independent of any other cost: a SAT instance needing no
  loop nogood at all, 0023, still spends 6.6s where clingo takes 0.77s.
  * The fix is dropping level ranking for ordinary recursive components in
    favor of the checker already built for head cycles, but only for a
    program that already pays for that checker: gate it on the program
    having a head cycle somewhere else. Labyrinth has none, so it stays
    untouched; MinimalDiagnosis already runs the checker for its disjunctive
    components, so `reach`/`coppo` rides the same round trips instead of
    starting a second mechanism from zero.

* Minimality and optimality are settled by round trips to a second solver
  over a complete candidate model, not by propagation inside one search.
  Whenever a program is head-cyclic (a disjunctive head, or a weak constraint
  being walked down to its least cost), pgass proposes a full model, asks a
  checker whether something strictly smaller satisfies the reduct, and if so
  asserts a nogood ruling out just that one witness before trying again. Each
  round re-pays cvc5's preprocessing in full, and because a nogood only rules
  out the specific witness found, convergence is not guaranteed to be
  fast: on ComplexOptimizationOfAnswerSets, clasp checks most of its conflicts
  on a partial assignment and keeps no loop nogood at all, where pgass runs
  1286 whole-model rounds on 0021 and is no closer. MinimalDiagnosis shows the
  same pattern on its disjunctive `vlabel`/`llabel`/`mvlabel`/`mllabel`: the
  unfounded set discovered grows round over round, 8 atoms at round 1 still
  climbing past 100 by round 12, rather than shrinking toward the empty set
  that would end the search.
  * The fix is checking minimality on partial assignments instead of complete
    models, the same operation clasp already runs as native propagation. cvc5's
    `Plugin` interface cannot host this, offering `check`, `notifySatClause`
    and `notifyTheoryLemma` with no read of the trail, so it means driving
    CaDiCaL through its `ExternalPropagator`. The propagator would have to
    carry unfounded-set propagation in place of level ranking, which is not
    overhead to drop for free: without the ranking section, cvc5 is still
    going at 300s on a check it answers in 42s with it.
  * Measured and rejected, so do not spend on them again: expansion at the
    witness subset, growing the unfounded set (batching it whole, growing it
    a chunk at a time, core-guided refinement off cvc5's unsat assumptions),
    minimizing it, splitting it into loops, dropping the level ranking
    without replacing it, bit-vector levels, z3 and its difference logic
    engines, and a sweep of cvc5's decision, sat-solver and arithmetic
    options. Every shape of round-trip batching loses to just taking the
    witness cvc5 hands back, so the partial-assignment propagator above is
    the one avenue left.

* Stop the parser building error messages it throws away. `parse_binop` is
  called in five places just to see whether an operator comes next, and each
  failure formats a message the caller drops when it backtracks.
