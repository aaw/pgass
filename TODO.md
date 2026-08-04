* Consider adding `--encode=sat`, which dumps the encoding as DIMACS.
* Deriving ignores aggregates, so a recursion an aggregate cuts off never
  reaches a fixpoint. gringo grounds `num(1). num(2). num(3). s(1). s(X+1) :-
  s(X), #count{ N : num(N), N > X } >= 1.` to s(1), s(2), s(3). pgass counts
  upwards forever.
