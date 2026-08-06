% difficulty: medium
% expect: UNSAT
%
% ASP Competition 2015, KnightTourWithHoles, instance 227, 85 holes.
% 85 holes make the tour impossible. Heavy on recursive reachability.
% Took 12.60s when it was added.

number(X)   :- size(X).
number(X-1) :- number(X), 1 < X.
cell(X,Y)   :- number(X), number(Y), not forbidden(X,Y).

hasx(X) :- cell(X,Y).
domx(X) :- hasx(X), hasx(XX), XX < X.
minx(X) :- hasx(X), not domx(X).

hasy(Y) :- cell(X,Y), hasx(X).
domy(Y) :- hasy(Y), hasy(YY), YY < Y.
miny(Y) :- hasy(Y), not domy(Y).

delta(1,2). delta(1,-2). delta(2,1). delta(2,-1).

conn(X,Y,X+DX,Y+DY) :- cell(X,Y), delta(DX,DY), cell(X+DX,Y+DY).

valid(X,Y,XX,YY) :- conn(X,Y,XX,YY).
valid(XX,YY,X,Y) :- conn(X,Y,XX,YY).

other(X,Y,XX,YY) :- valid(X,Y,XX,YY), move(A,B,XX,YY), X != A.
other(X,Y,XX,YY) :- valid(X,Y,XX,YY), move(X,B,XX,YY), Y != B.
other(X,Y,XX,YY) :- valid(X,Y,XX,YY), move(X,Y,A,BB), XX != A.
other(X,Y,XX,YY) :- valid(X,Y,XX,YY), move(X,Y,XX,B), YY != B.

move(X,Y,XX,YY) :- valid(X,Y,XX,YY), not other(X,Y,XX,YY).

from(X,Y) :- move(X,Y,XX,YY).
:- cell(X,Y), not from(X,Y).

reach(X,Y)   :- minx(X), miny(Y).
reach(XX,YY) :- move(X,Y,XX,YY), reach(X,Y).

:- cell(X,Y), not reach(X,Y).

% ---------------- instance

size(85).
forbidden(5,70).
forbidden(36,20).
forbidden(79,11).
forbidden(46,41).
forbidden(38,27).
forbidden(36,73).
forbidden(82,10).
forbidden(33,46).
forbidden(41,6).
forbidden(55,38).
forbidden(44,84).
forbidden(45,84).
forbidden(29,69).
forbidden(22,70).
forbidden(25,33).
forbidden(17,71).
forbidden(18,9).
forbidden(5,53).
forbidden(19,8).
forbidden(8,14).
forbidden(34,1).
forbidden(1,72).
forbidden(52,75).
forbidden(74,50).
forbidden(80,43).
forbidden(2,81).
forbidden(84,3).
forbidden(36,27).
forbidden(28,58).
forbidden(53,9).
forbidden(47,69).
forbidden(79,21).
forbidden(34,40).
