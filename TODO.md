## Bugs

* Parse a left-side aggregate guard that starts with an identifier.
  `q :- a > #count{ X : p(X) } >= 1.` is a parse error: the body item parser
  reads `a` as a literal and never backs up to try an aggregate. Same for a
  function term, e.g. `f(1) < #count{...}`. Numbers, strings, variables, and
  arithmetic like `1+1` already work on the left, and the right side takes any
  term, so this is only about guards the literal parser swallows first.
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
  constraints, whose optimum takes repeated queries, head cycles, whose models
  are only candidates, and a query of other than one instance, which is other
  than one check-sat.
* Take a pass over all absl::UnimplementedError we throw and compare to clingo