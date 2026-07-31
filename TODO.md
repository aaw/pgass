## Spec gaps

* Implement `#min` and `#max`. `ground.cc` rejects both. They range over the
  total term order, not just integers, and an empty set gives `#min` +infinity
  and `#max` -infinity.
* Answer queries cautiously. A query is an ASPIF assumption today, which asks
  whether some answer set contains it. The spec asks whether every one does, so
  `a | b. a?` should say no.
* Report the substitutions a non-ground query holds under in every answer set.
  `p(1). p(2). p(X)?` prints an answer set instead.

## Bugs

* Say something when arithmetic on a non-number drops a rule. `p(-a).` grounds
  to an empty program in silence. Decide first whether `-a` is a term or is
  ill-formed.
* Use unlimited precision integers for numbers and arithmetic. A number is a
  `uint64_t` in the AST and an `int64_t` once grounded, in a `SymEntry` or
  inlined into a `Sym` handle. ASP-Core-2 doesn't bound integers. Today
  arithmetic wraps and returns wrong models, and a literal past 2^64 is a parse
  error.
* Stop grounding a program that never finishes. `p(1). p(X+1) :- p(X).` spins
  forever. A size limit or a message about the growing predicate beats a hang.

## Everything else

* Why can clingo solve PHP much faster than we can? Is it using some symmetry-breaking?
* Add `--encode=sat`, which dumps the encoding as DIMACS. `--encode=smtlib` is in.
* Give `--encode=smtlib` something to say about a program it now refuses: weak
  constraints, whose optimum takes repeated queries, and head cycles, whose
  models are only candidates.
