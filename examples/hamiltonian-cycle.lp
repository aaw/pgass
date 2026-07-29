% Find a tour of the graph that leaves and enters every node exactly once.
% The two #count constraints pick out one arc leaving and one entering each
% node, which on its own would also allow several disjoint smaller loops. The
% last three rules rule those out: 'reach' walks the chosen arcs from node 1,
% and every node has to come out reachable, so the chosen arcs form one tour
% rather than several loops.
%
% 'reach' recurses over 'in' atoms the solver chooses, so which atoms lie on a
% positive cycle differs from one answer set to the next. That makes this the
% example that leans hardest on the level ranking in the QF_IDL translation.
% Try: pgass --models 0 hamiltonian-cycle.lp

node(1). node(2). node(3). node(4).

arc(1, 2). arc(2, 3). arc(3, 4). arc(4, 1).
arc(1, 3). arc(3, 2). arc(2, 4).

{ in(X, Y) } :- arc(X, Y).

:- node(X), #count{ Y : in(X, Y) } != 1.
:- node(Y), #count{ X : in(X, Y) } != 1.

reach(1).
reach(Y) :- reach(X), in(X, Y).
:- node(X), not reach(X).
