# Grounder

* Build PredGraph on normalized program to isolate strongly connected components that need to be
  grounded.
  * Compute SCCs of graph.pos_succ.
  * Bucket rules by `component_of[head predicate]`. constraints (no head) go only in the flat rule
    list, not a bucket.
  * Replace the single global fixpoint loop in `derive_atoms` with one fixpoint loop per component
    in asc component id order.
  * Run `emit_rules` after every component has derived. Required for the "drop `not q` if never
    derived" optimization to stay correct.
* Rewrite aggregates as ASPIF "weight body"s.
* Rewrite weak constraints (_viol instances) as ASPIF minimize statements.
* Support function terms (currently rejected in eval_term).
* Support arithmetic terms (NegatedTermKind, TermOperationKind; currently rejected in eval_term).
* Support binding a variable via assignment (e.g. `X = Y + 1`), not just positive body literals.
* Support queries: wire Program::query through to ASPIF assumptions (aspif.h already supports the
  statement, ground() currently rejects any program with a query).
