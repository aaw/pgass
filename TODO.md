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
