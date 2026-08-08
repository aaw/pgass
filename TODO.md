* Consider adding `--encode=sat`, which dumps the encoding as DIMACS.
* Add non-standard language constructs like #show, #const, #minimize
  and ranges so we can run all benchmarks
* Grounding the larger ComplexOptimizationOfAnswerSets instances is still
  several times slower than gringo, 4s against 2s on 0060 and 20s against 5s on
  0043. What is left is the join inner loop: interning a function term like
  `pos(P)` on every candidate atom, and building an `absl::Status` with a
  formatted message for every argument that fails to match.
* ComplexOptimizationOfAnswerSets is 3 of 20 where clingo is 13, and the gap is
  in solving. On 0021 clasp solves the program pgass grounds in 5.2s, faster
  than gringo's grounding of it at 11.4s, where pgass does not finish in 150s
  and grounds in 1.1s. So work against a saved grounding, aiming at 5.2s.
  The encoding saturates, which leaves one head-cyclic component of 13662
  nodes, so the check returns an unfounded set of some 4600 atoms out of 8889
  with nothing smaller inside it and its nogood rules out little. Measured and
  rejected: splitting the set into loops, since it is already one; handing
  dropped atoms back to the witness, 3268 to 3150 over 24 checker calls;
  dropping the level ranking over the component, three times the rounds per
  second, no sooner converging, and 0074 from 17.5s to 81.9s.
* The minimality check runs only on total models, so its nogood cannot fire
  until as many atoms are decided again, which is too late to save anything.
  clasp checks 2912 of 4181 times on partial assignments, keeps no loop nogood
  at all, and needs 3022 conflicts. cvc5's `Plugin` cannot host a partial
  check, offering `check`, `notifySatClause` and `notifyTheoryLemma` and no
  read of the trail. CaDiCaL's `ExternalPropagator` can, but that means driving
  CaDiCaL rather than cvc5.
* Try expansion in find() and unfounded_set(). Minimality is a 2QBF question,
  and the loop pgass runs is the textbook 2QBF algorithm, guess then check then
  refine by one counterexample, which is known to need exponentially many
  refinements. The 3650 rounds 0021 spends is that, not a bug. Expansion keeps
  the counterexamples instead and expands the universally quantified variables
  over the set of them, so each round solves a stronger abstraction. Those
  variables are `MinimalityCheck::subset_var`, so a round instantiates the
  smaller-model conditions at the witness subset, over the model's atom
  variables, kept alongside the loop nogood. Timebox it, since the same bound
  applies and is only reached later, and note that generalized reasons were
  already tried: they shrank to 10 to 17 atoms and 0021 still did not finish.
  Janota and Marques-Silva, Abstraction-Based Algorithm for 2QBF,
  https://sat.inesc-id.pt/~mikolas/sat11.pdf
  Janota et al., On Expansion and Resolution in CEGAR Based QBF Solving,
  https://arxiv.org/pdf/1803.09559
  Lierler, cmodels, which checks only total candidates as pgass does,
  https://ceur-ws.org/Vol-142/page85.pdf
* Work out a rule's join order once and keep it, rather than redoing it on
  every derivation pass. Then the cap on how many orders join_order tries
  can go away.
* Stop the parser building error messages it throws away. `parse_binop` is
  called in five places just to see whether an operator comes next, and each
  failure formats a message the caller drops when it backtracks.
