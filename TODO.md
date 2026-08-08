* Consider adding `--encode=sat`, which dumps the encoding as DIMACS.
* Add non-standard language constructs like #show, #const, #minimize
  and ranges so we can run all benchmarks
* ComplexOptimizationOfAnswerSets is 4 of 20 where clingo is 13, and the gap is
  in solving. Grounding is at gringo's pace, 4.1s against 5.0s on 0043.
  * The jnh instances are stuck on the minimality check. On 0021 clasp solves
    the program pgass grounds in 5.2s, faster than gringo's grounding of it at
    11.4s, where pgass does not finish in 150s and grounds in 0.9s. So work
    against a saved grounding, aiming at 5.2s. The encoding saturates, which
    leaves one head-cyclic component of 13662 nodes, so the check returns an
    unfounded set of some 4600 atoms out of 8889 with nothing smaller inside it
    and its nogood rules out little.
  * The check runs only on total models, so its nogood cannot fire until as
    many atoms are decided again, which is too late to save anything. clasp
    checks 2912 of 4181 times on partial assignments, keeps no loop nogood at
    all, and needs 3022 conflicts. cvc5's `Plugin` cannot host a partial check,
    offering `check`, `notifySatClause` and `notifyTheoryLemma` and no read of
    the trail. CaDiCaL's `ExternalPropagator` can, but that means driving
    CaDiCaL rather than cvc5.
  * The edge* instances are stuck on the first check-sat instead, 31.7s of the
    55s 0043 spends solving. cvc5 spends it in simplex, on the level ranking's
    253741 integer variables. The profile is Rational, DeltaRational and
    MatrixEntry all the way down.
  * Measured and rejected: splitting the unfounded set into loops, since it is
    already one. Handing dropped atoms back to the witness, 3268 to 3150 over
    24 checker calls. Growing the set to a fixpoint instead, so that one round
    finds what six rounds each add two or three atoms to, 0043 from 72s to past
    5 minutes. Dropping the level ranking over the component, three times the
    rounds per second, no sooner converging, and 0074 from 17.5s to 81.9s.
    Expansion, which instantiates the smaller-model conditions at the witness
    subset and keeps that alongside the loop nogood, since the instantiation is
    a disjunction of conjunctions, 5638 of them on 0021 and 165303 on 0043,
    where the loop nogood already says the same counterexample as a clause.
    0021 went 1286 rounds to 1255 and 0043 stayed at 7. And the cvc5 options
    decision=justification, sat-solver=minisat, static-learning, arith-prop,
    arith-brab, theoryof-mode and simplex-check-period, none of which beat the
    defaults on 0074.
* Work out a rule's join order once and keep it, rather than redoing it on
  every derivation pass. Then the cap on how many orders join_order tries
  can go away.
* Stop the parser building error messages it throws away. `parse_binop` is
  called in five places just to see whether an operator comes next, and each
  failure formats a message the caller drops when it backtracks.
