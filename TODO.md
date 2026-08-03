* Settle head cycles in solve.cc the way `--encode=smtlib` does. It asserts
  encode.cc's `minimality_term()` once, quantified over the atoms of
  head-cyclic components alone (`Encoding::in_head_cycle`), every other atom
  standing for itself. solve.cc instead runs `subset_models_reduct()` per
  candidate, over a subset variable for every candidate atom, which is the same
  over-quantification the script had before it was restricted. Measured on
  2-QBF saturation instances (n x-vars, 3 y-vars, n clauses, in the style of
  examples/saturation.lp), cvc5 on the script against pgass, same answers
  throughout: sq_16 0.01s against 1.37s, qbf_12 0.06s against 8.57s, sq_18
  0.02s against over 60s.
  - The solver logic has to become ALL for such a program, and a program
    without a head cycle has to keep its quantifier-free logic.
  - `Search::find()` loses its candidate rejection loop, every model being an
    answer set once the assertion is in.
  - Check that ALL does not cost incremental enumeration (`--models=0`), which
    is where solve.cc pushes, pops, and blocks. The measurements above are all
    one model deep.
  - Failing all that, restrict `subset_models_reduct()` to head-cyclic atoms,
    which is the same win without the quantifier.
  - `reduct_rule()` in encode.cc and `reduct_body()` in solve.cc then say the
    reduct twice. Factor them into one builder taking whatever stands for the
    candidate.
* Make `--encode=smtlib` one-shot for weak constraints, so that every script it
  writes runs through cvc5 or z3 unedited. Weak constraints are the last case
  that asks its reader to add a bound and run again. Assert optimality the way
  a head cycle asserts minimality: no answer set has a lexicographically
  smaller cost.

      (assert (forall (<one Bool per atom, the other answer set M'>)
        (=> (and <rules and support over M'> <M' is minimal>)
            (not <cost(M') lexicographically below cost(M)>))))

  - `<M' is minimal>` is `minimality_term()` over M', so the two quantifiers
    nest. Characterizing M' by the reduct rather than by the level ranking
    keeps every bound variable Boolean, the ranking being the only part that
    would need Ints. Only the costs stay arithmetic, so the logic is ALL.
  - The lexicographic comparison is one disjunct per level: the levels above it
    equal, that level strictly below.
  - Then a query and weak constraints compose. Today the script says to settle
    every level before asking the query, and this settles them.
  - Restricting what the quantifier ranges over is what made the minimality
    assertion usable, from over 45s to 0.03s, so look for the same thing here
    before measuring anything else. Measure against
    `minimize_by_stepping_down()`, on programs with weak constraints rather
    than on the 2-QBF instances above.
* Consider adding `--encode=sat`, which dumps the encoding as DIMACS.
