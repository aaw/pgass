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
* Decide whether an aggregate element may hold a compound term.
  `S = #sum{ X*2 : p(X) }` and `C = #count{ f(X) : p(X) }` are parse errors,
  because the ASP-Core-2 grammar gives an element `<basic_terms>`: a constant,
  a string, a signed number, or a variable. The spec's own prose says an
  element is "t1, ..., tm : l1, ..., ln where t1, ..., tm are terms", and
  clingo takes both, so encodings written for clingo hit this. Following the
  prose means parsing `<terms>` there instead.
* Report an unsafe variable from the safety pass, not from grounding.
  `p(X) :- q(X+1).` and `p :- #count{ Z : q(1) } = 1.` are unsafe under the
  spec: a variable inside an arithmetic term binds nothing, and an element's
  term binds nothing on its own. Both are rejected, but by the grounder, with
  "variable 'X' is not bound by the rule body" and no source line.

## Everything else

* Why can clingo solve PHP much faster than we can? Is it using some symmetry-breaking?
* Add `--encode=sat`, which dumps the encoding as DIMACS. `--encode=smtlib` is in.
* Give `--encode=smtlib` something to say about a program it now refuses: weak
  constraints, whose optimum takes repeated queries, head cycles, whose models
  are only candidates, and a query of other than one instance, which is other
  than one check-sat.
* Take a pass over all absl::UnimplementedError we throw and compare to clingo