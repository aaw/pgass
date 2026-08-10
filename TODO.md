* Consider adding `--encode=sat`, which dumps the encoding as DIMACS.
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
* MinimalDiagnosis is 8 of 20 where clingo is 20, both given 120s. Grounding
  is not the story, 0.5s on 0001 against clingo's own pace. The gap is the
  same two causes as ComplexOptimizationOfAnswerSets above and Labyrinth
  below, landing on one program together.
  * The encoding ranks `reach(U,V)` and `coppo(W,V)`, both ordinary recursive
    predicates over a ~150-300 vertex graph, the same shape as Labyrinth's
    `reach(X,Y,T)`. 0001 declares 17252 Int level variables for it. Even a SAT
    instance that needs no loop nogood at all, 0023, spends 6.6s deciding one
    checkSat this way against clingo's 0.77s.
  * The encoding also disjuncts `vlabel`/`llabel` for the consistency check and
    `mvlabel`/`mllabel` for the minimality test, both head-cyclic, so every
    round asks the checker of ComplexOptimizationOfAnswerSets. 0001 (UNSAT)
    never gets an empty unfounded set: the droppable atoms it rules out grow
    round over round, 8, 11, 14, 17, 24, 54, 76, 81, 84, 91, 96, 102 at round
    12 and still climbing at 150s, so the partial-assignment check that entry
    already asks for is what would close this one too.
* Stop the parser building error messages it throws away. `parse_binop` is
  called in five places just to see whether an operator comes next, and each
  failure formats a message the caller drops when it backtracks.
* Labyrinth is 8 of 20 where clingo is 15, both given 120s. Grounding is not
  the story, 0.22s on 0082 against clingo's own pace. The whole 55.8s pgass
  spends there is one checkSat call: no head cycle, so find() never loops,
  and 0082 declares 26734 atoms of which 2197 are level-ranked and none
  droppable. That checkSat runs 733460 decisions through cvc5's arithmetic
  theory (2280 conflicts, 45730 propagations there) to rank `reach(X,Y,T)`,
  which recurses through `dneighbor`/`conn` within a time step and so sits on
  a large positive cycle every instance has, ranked or not. clingo needs none
  of that: it finds the unfounded sets of a normal recursive predicate as
  Boolean propagation during search, the same operation pgass already runs
  as a post-hoc check for head-cyclic programs (see ComplexOptimizationOfAnswerSets
  above), never as arithmetic.
  * Atom count does not predict the gap. 0060 ranks 1728 atoms and solves in
    2.76s; 0045 ranks only 1000 and takes 12.52s. What is expensive is
    letting CDCL(T) decide through the difference-logic layer while it
    searches for a push sequence, not the size of the layer.
  * The fix this points at is dropping level ranking for ordinary recursive
    components too, not only head-cyclic ones: mark every atom on a positive
    cycle droppable, skip declaring its Int, and let the existing checker in
    solve.cc find unfounded sets and assert loop nogoods on demand instead.
    That is a bigger change than Labyrinth alone, since level ranking is what
    every other domain's recursion leans on for correctness, so it needs
    checking against the full suite, not just this one, before it lands.
  * Not yet measured: whether the checker's per-round cost (asserted as
    cheap above only for head cycles) stays cheap over a reach-sized
    component of ~2000 atoms and ~200 cells, or whether it is worth reusing
    or the partial-assignment propagator ComplexOptimizationOfAnswerSets
    already asks for.
