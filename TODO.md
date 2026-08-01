## Bugs

* Use unlimited precision integers for numbers and arithmetic. A number is a
  `uint64_t` in the AST and an `int64_t` once grounded, in a `SymEntry` or
  inlined into a `Sym` handle. ASP-Core-2 doesn't bound integers. Today
  arithmetic wraps and returns wrong models, and a literal past 2^64 is a parse
  error.

## Everything else

* Why can clingo solve PHP much faster than we can? Is it using some symmetry-breaking?
* Add `--encode=sat`, which dumps the encoding as DIMACS. `--encode=smtlib` is in.
* Give `--encode=smtlib` something to say about a program it now refuses: weak
  constraints, whose optimum takes repeated queries, head cycles, whose models
  are only candidates, and a query of other than one instance, which is other
  than one check-sat.
* Take a pass over all absl::UnimplementedError we throw and compare to clingo