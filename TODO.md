* The ASP-Core-2 spec's section 5 (Restrictions: Aggregates) says that aggregates must be
  non-recursive.

  So this is not okay: `p(X) :- dom(X), #count{ Y : p(Y) } >= X.`

  But this is okay:
  ```
  reach(X, Z) :- reach(X, Y), edge(Y, Z).
  big :- #count{ X, Y : reach(X, Y) } >= 3.
  ```

  Need to tag edges of the PredGraph with something like an "in aggregate" flag to
  create and check the predicate dependency graph described in that section of the spec.
  This needs to be the last step of normalization since there's another normalization
  step that creates aggregates.