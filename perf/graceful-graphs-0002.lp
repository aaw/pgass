% difficulty: hard
% expect: SAT
%
% ASP Competition 2015, GracefulGraphs, instance 0002, triple cone 6.
% Triple cone, 24 edges. Long search over a wide labelling space.
% Took 36.39s when it was added.

% nodes and values
node(X) :- edge(X,Y).
node(Y) :- edge(X,Y).

%num_edges(N) :- N = #sum { 1,X,Y : edge(X,Y) }.
% Updated March 25th, 2013
num_edges(N) :- N = #count { X,Y : edge(X,Y) }.
num(0).
num(N) :- num(N1), N=N1+1, num_edges(E), N<=E.

% assignment to nodes
{ value(X,N) : num(N) } = 1 :- node(X).

% assignment to edges
{ edge_value(edge(X,Y),N) : num(N), N>0 } = 1 :- edge(X,Y).

% relates node values with edge values
:- not edge_value(edge(X,Y),M-N), edge(X,Y), value(X,M), value(Y,N), N < M.
:- not edge_value(edge(X,Y),N-M), edge(X,Y), value(X,M), value(Y,N), N > M.

% alldifferent values
:- value(X,N), value(Y,N), num(N), X<Y.
:- edge_value(X,N), edge_value(Y,N), num(N), X<Y.

% ---------------- instance

edge(6,1).
edge(6,9).
edge(5,9).
edge(4,9).
edge(3,9).
edge(2,9).
edge(1,9).
edge(6,8).
edge(5,8).
edge(4,8).
edge(3,8).
edge(2,8).
edge(1,8).
edge(6,7).
edge(5,7).
edge(4,7).
edge(3,7).
edge(2,7).
edge(1,7).
edge(5,6).
edge(4,5).
edge(3,4).
edge(2,3).
edge(1,2).
