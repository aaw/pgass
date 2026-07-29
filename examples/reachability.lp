% Which nodes can reach which over directed edges?
% Try: pgass reachability.lp

edge(a, b).
edge(b, c).
edge(b, d).
edge(d, a).

reach(X, Y) :- edge(X, Y).
reach(X, Z) :- reach(X, Y), edge(Y, Z).
