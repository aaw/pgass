% difficulty: easy
% expect: SAT
%
% ASP Competition 2015, GracefulGraphs, instance 0044, prism 6x3.
% Distinct-difference labelling over a prism graph.
% Took 3.37s when it was added.

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

edge(17,18).
edge(16,17).
edge(15,16).
edge(14,15).
edge(13,14).
edge(11,12).
edge(10,11).
edge(9,10).
edge(8,9).
edge(7,8).
edge(5,6).
edge(4,5).
edge(3,4).
edge(2,3).
edge(1,2).
edge(18,13).
edge(12,7).
edge(6,1).
edge(12,18).
edge(11,17).
edge(10,16).
edge(9,15).
edge(8,14).
edge(7,13).
edge(6,12).
edge(5,11).
edge(4,10).
edge(3,9).
edge(2,8).
edge(1,7).
