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

* `comparisons_hold()` in ground.cc re-evaluates comparisons `bind_assignments()` already settled as assignments.

* `collect_agg_tuples()` in ground.cc re-splits each aggregate element's literals via `split_naf_literals()` on every call instead of once per element.

* `matching_atoms()` in ground.cc linear-scans a predicate's atoms even when some args are ground, instead of reusing the join's probe/index machinery.

* `settle_aggregate()` in ground.cc defers every negated aggregate instead of consulting the per-rule `aggregate_in_own_component` flag already computed.

* `mark_aggregates_in_own_component()`, `mark_settled_negation()`, and `bucket_rule_views()` in ground.cc each recompute `head_component()` in a separate pass over the same rules. Mergeable into one.
