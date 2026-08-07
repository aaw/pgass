* Consider adding `--encode=sat`, which dumps the encoding as DIMACS.
* Add non-standard language constructs like #show, #const, #minimize
  and ranges so we can run all benchmarks
* Minimality for a disjunctive program goes to cvc5 as one `forall` over every
  atom in a head-cyclic component. On ComplexOptimizationOfAnswerSets that
  quantifier is the whole cost. Instance 0001 grounds in half a second and then
  times out solving, where clingo answers the lot in 0.2s. Guessing a model
  without the quantifier is fast, and so is checking whether the guess is
  minimal. What fails is blocking a guess that loses, which today needs a
  clause naming every atom, so each round is slower than the last. Derive a
  loop nogood instead, from the unfounded set the check finds and the rules
  that could still support it. That is what clasp does, and it rules out a
  family of guesses per round rather than one.
* Grounding the larger ComplexOptimizationOfAnswerSets instances is still
  several times slower than gringo, e.g. 16s against 2s on 0060, which spends a
  quarter of the benchmark's time limit before solving starts. What is left is
  the join inner loop: interning a function term like `pos(P)` on every
  candidate atom, and building an `absl::Status` with a formatted message for
  every argument that fails to match.
* Work out a rule's join order once and keep it, rather than redoing it on
  every derivation pass. Then the cap on how many orders join_order tries
  can go away.
* Stop the parser building error messages it throws away. `parse_binop` is
  called in five places just to see whether an operator comes next, and each
  failure formats a message the caller drops when it backtracks.
