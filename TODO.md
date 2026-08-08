* Consider adding `--encode=sat`, which dumps the encoding as DIMACS.
* Add non-standard language constructs like #show, #const, #minimize
  and ranges so we can run all benchmarks
* ComplexOptimizationOfAnswerSets is 4 of 20 where clingo is 13. Grounding is at
  gringo's pace, 4.1s against 5.0s on 0043, so the whole gap is in solving. Give
  it 120s instead of 60s and pgass reaches 8, which says where the work is.
  * Cut a third off solving and 0043 at 78s, 0044 at 70s, 0045 at 79s and 0055
    at 88s come under the limit, for 8 of 20. 0055 is one clingo does not solve.
    The first check-sat is 31.7s of the 55s 0043 spends, and 3.3M assertions go
    into cvc5, a quarter of that call being CNF conversion alone. So shrink what
    is asserted. That pays twice, since build_encoding spends 5.1s building the
    assertions and build_check is most of it.
  * Check minimality on partial assignments. Everything past those four needs
    it, both the jnh instances and the far half of edge*. Each round pgass runs
    rules out one model, and the six rounds 0043 spends are six different
    counterexamples, so no better witness collapses them. clasp checks 2912 of
    its 4181 times on a partial assignment, keeps no loop nogood at all, and
    needs 3022 conflicts, where pgass runs 1286 rounds on 0021 and is no closer.
    cvc5's `Plugin` cannot host this, offering `check`, `notifySatClause` and
    `notifyTheoryLemma` and no read of the trail, so it means driving CaDiCaL
    through its `ExternalPropagator`. The propagator has to carry unfounded set
    propagation in place of the level ranking, which is not overhead to drop:
    without the ranking section cvc5 is still going at 300s on the check it
    answers in 42s with it.
  * Measured and rejected, so do not spend on them again: expansion at the
    witness subset, growing the unfounded set, minimizing it, splitting it into
    loops, dropping the level ranking, bit-vector levels, z3 and its difference
    logic engines, and a sweep of cvc5's decision, sat-solver and arithmetic
    options.
* Work out a rule's join order once and keep it, rather than redoing it on
  every derivation pass. Then the cap on how many orders join_order tries
  can go away.
* Stop the parser building error messages it throws away. `parse_binop` is
  called in five places just to see whether an operator comes next, and each
  failure formats a message the caller drops when it backtracks.
