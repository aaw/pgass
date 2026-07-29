% Pick a set of nodes touching every edge, as small as possible.
% Each node is freely picked or not (the choice rule below), the constraint
% rejects any pick that leaves an edge uncovered, and the weak constraint
% charges 1 per picked node, so the cheapest answer set is a minimum cover.
%
% pgass grounds the weak constraint but cannot yet solve one: cvc5 has no
% optimization API, so finding the cheapest answer set needs a branch-and-bound
% loop pgass does not have. clasp does the optimizing here.
% Try: pgass --ground vertex-cover.lp | clasp --opt-mode=opt

node(1). node(2). node(3). node(4). node(5).

edge(1, 2).
edge(2, 3).
edge(3, 4).
edge(4, 5).
edge(5, 1).
edge(1, 3).

{ cover(X) } :- node(X).

:- edge(X, Y), not cover(X), not cover(Y).

:~ cover(X). [1@0, X]
