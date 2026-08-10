* cvc5's preprocessing pipeline taxes every clause, even where the encoding is
  pure Boolean and there is no theory to reason about. PermutationPatternMatching
  is the clean case: clasp reports solving its CNF in 0.00s, while pgass spends
  seconds getting there, 65% of one run in NonClausalSimp, TheoryPreprocess,
  ApplySubsts, StaticLearning and StaticRewrite. Per ground rule pgass pays
  about 4.5us against clingo's 0.8us, and that is the whole of the difference
  on a domain whose search costs nothing. This tax is paid on every domain
  with a lot of ground clauses, not just this one.
  * The fix is a propositional path: for an encoding that is QF_UF with no Int
    anywhere, go from clauses straight to a SAT solver, skipping cvc5's nodes,
    rewriter and preprocessing passes. `--encode=sat`, dumping the encoding as
    DIMACS, is a step toward that.
  * Measured and rejected: `simplification=none` and `static-learning=false`
    move the needle a fifth to a twentieth, not the multiple needed, and would
    apply to every domain including the search-bound ones below. Flattening
    every rule to a clause, not only the constraints, backfires where a rule
    that derives something wants the variable cvc5 names its body with:
    partner-units-166 went from 5.4s to 54.7s, crossing-minimization-0008 from
    1.5s to 4.1s.
  * Measured and rejected, a second way: skipping cvc5 outright for a QF_UF
    program with no head cycle and no weak constraint, Tseitin-clausifying the
    encoding straight into CaDiCaL through its IPASIR ABI. Cut every ppm-* case
    by half or more and helped several other domains by 2-4s, but
    partner-units-166 went from ~5s to a 60s timeout, the same instance the
    clause-flattening attempt above already broke.

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
  * The fix is dropping level ranking for ordinary recursive components, not
    only head-cyclic ones: mark every atom on a positive cycle droppable, skip
    declaring its Int, and let the checker already built for head cycles find
    unfounded sets and assert loop nogoods on demand instead. This is a bigger
    change than either domain alone, since level ranking is what every other
    domain's recursion leans on for correctness, so it needs checking against
    the full suite before it lands.
  * Not yet measured: whether the checker's per-round cost, cheap where
    measured for head cycles, stays cheap over a much larger recursive
    component, or whether it is worth sharing with the partial-assignment
    propagator below.

* Minimality and optimality are settled by round trips to a second solver
  over a complete candidate model, not by propagation inside one search.
  Whenever a program is head-cyclic (a disjunctive head, or a weak constraint
  being walked down to its least cost), pgass proposes a full model, asks a
  checker whether something strictly smaller satisfies the reduct, and if so
  asserts a nogood ruling out just that one witness before trying again. Each
  round re-pays the preprocessing tax above in full, and because a nogood only
  rules out the specific witness found, convergence is not guaranteed to be
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
