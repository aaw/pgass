# Grounder

* Support function terms (currently rejected in eval_term).
* Support arithmetic terms (NegatedTermKind, TermOperationKind; currently rejected in eval_term).
* Support binding a variable via assignment (e.g. `X = Y + 1`), not just positive body literals.
* Support queries: wire Program::query through to ASPIF assumptions (aspif.h already supports the
  statement, ground() currently rejects any program with a query).
