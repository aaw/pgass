* Use unlimited precision integers for numbers and arithmetic. A number is a
  `uint64_t` in the AST and an `int64_t` once grounded, in a `SymEntry` or
  inlined into a `Sym` handle. ASP-Core-2 doesn't bound integers.
* Why can clingo solve PHP much faster than we can? Is it using some symmetry-breaking?
* Add `--encode=sat`, which dumps the encoding as DIMACS. `--encode=smtlib` is in.
* Give `--encode=smtlib` something to say about a program it now refuses: weak
  constraints, whose optimum takes repeated queries, and head cycles, whose
  models are only candidates.
