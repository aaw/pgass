* Consider adding `--encode=sat`, which dumps the encoding as DIMACS.
* An interval's variable is bound after the join, so a positive literal reading
  it under arithmetic the join cannot work backwards leaves it unbound:
  "p(X) :- X = 1..3, q(X*2)." reports X as unbound where clingo grounds it.
  `q(X+1)` is fine, since invertible() reads that one backwards and binds X.
  Aggregate output variables are bound at the same point and have the same gap.
* PermutationPatternMatching costs what it costs to hand cvc5 a million
  clauses, not to search them. Grounding is ahead of gringo, 0.29s against
  0.93s on 0092, and the CNF is 6413 variables and 996605 clauses that clasp
  reports solving in 0.00s. Writing an integrity constraint as a clause rather
  than as '(=> (and ...) false)' took 0092 from 8.1s to 4.3s, 0129 from 49.1s
  to 22.0s and 0018 from a timeout to 53.4s. clingo is at 0.8s, 5.6s and 10.4s.
  * What is left is cvc5's preprocessing, 65% of the run on 0129 against a
    search of about nothing: NonClausalSimp 4240 samples, TheoryPreprocess
    2733, ApplySubsts 2317, StaticLearning 1627, StaticRewrite 1131.
    build_encoding is another 15%. None of it is work this domain needs, since
    the encoding is QF_UF with no Int constant anywhere: the program is tight,
    so there is no level ranking and nothing for a theory to do.
  * So the thing to build is the propositional path, which is the `--encode=sat`
    item above: for an encoding that is pure Boolean, go from the clauses to
    the SAT solver without cvc5's nodes, rewriter and preprocessing passes in
    between. Per ground rule pgass pays about 4.5us against clingo's 0.8us,
    and that is the whole of the difference.
  * Setting `simplification=none` and `static-learning=false` is not the way
    there. It is worth a fifth on 0092 and a twentieth on 0129, and it would
    apply to every domain, including the search-bound ones below.
  * Measured and rejected: flattening every rule to a clause, not just the
    constraints. It reads the same on PermutationPatternMatching, where 99.5%
    of rules are constraints, but a rule that derives something wants the
    variable cvc5 names its body with. partner-units-166 went from 5.4s to
    54.7s, 93% of it inside CaDiCaL, and crossing-minimization-0008 from 1.5s
    to 4.1s.
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
