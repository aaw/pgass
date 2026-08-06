% difficulty: medium
% expect: UNSAT
%
% ASP Competition 2015, PartnerUnits, instance 35, polynomial variant.
% Unsatisfiable configuration instance, polynomial variant.
% Took 4.67s when it was added.

elem(z,Z) :- zone2sensor(Z,D).
elem(d,D) :- zone2sensor(Z,D).

numb(A,N) :- elem(A,N), not elem(A,N+1).

zone :- numb(z,N), numb(d,M), N > M.

rule(z) :- zone.
rule(d) :- not zone.

{ gt(A,X,U) } :- elem(A,X), comUnit(U), comUnit(U1), U1=U+1, rule(A), U < X.
{ gt(A,X,U) } :- elem(A,X), comUnit(U), comUnit(U1), U1=U+1, not rule(A).

:- gt(A,X,U), 1 < U, not gt(A,X,U-1).

unit2zone(1,Z)   :- elem(z,Z), comUnit(1), not gt(z,Z,1).
unit2zone(U+1,Z) :- gt(z,Z,U), not gt(z,Z,U+1).

:- comUnit(U), #count{ Z : unit2zone(U,Z) } > 2.

unit2sensor(1,D)   :- elem(d,D), comUnit(1), not gt(d,D,1).
unit2sensor(U+1,D) :- gt(d,D,U), not gt(d,D,U+1).

:- comUnit(U), #count{ D : unit2sensor(U,D) } > 2.

partner(d,D,U) :- unit2zone(U,Z), rule(d), zone2sensor(Z,D).
partner(z,Z,U) :- unit2sensor(U,D), rule(z), zone2sensor(Z,D).

partnerunits(U,P) :- partner(d,D,U), unit2sensor(P,D), U < P.
partnerunits(P,U) :- partner(d,D,U), unit2sensor(P,D), P < U.
partnerunits(U,P) :- partner(z,Z,U), unit2zone(P,Z), U < P.
partnerunits(P,U) :- partner(z,Z,U), unit2zone(P,Z), P < U.
partnerunits(U,P) :- partnerunits(P,U), P < U.

:- comUnit(U), maxPU(M), #count{ P : partnerunits(U,P) } > M.

% gtd(X+1,U) :- gt(A,X,U), comUnit(U+2), rule(A), elem(A,X+1).
% gtd(X+1,U) :- gtd(X,U), rule(A), elem(A,X+1).

% :- gt(A,X,U), 1 < U, rule(A), not gtd(X,U-1).

% ---------------- instance

maxPU(2).

comUnit(1). comUnit(2). comUnit(3). comUnit(4). comUnit(5). comUnit(6). comUnit(7). comUnit(8). comUnit(9). comUnit(10). comUnit(11). comUnit(12). comUnit(13). comUnit(14). comUnit(15). comUnit(16). comUnit(17). comUnit(18). comUnit(19). comUnit(20). 

% relation between zones and door sensors
zone2sensor(1,1).
zone2sensor(1,2).
zone2sensor(1,4).
zone2sensor(2,2).
zone2sensor(2,21).
zone2sensor(3,21).
zone2sensor(3,31).
zone2sensor(3,32).
zone2sensor(4,3).
zone2sensor(4,4).
zone2sensor(4,6).
zone2sensor(5,3).
zone2sensor(5,22).
zone2sensor(6,22).
zone2sensor(6,32).
zone2sensor(6,33).
zone2sensor(7,5).
zone2sensor(7,6).
zone2sensor(7,8).
zone2sensor(8,5).
zone2sensor(8,23).
zone2sensor(9,23).
zone2sensor(9,33).
zone2sensor(9,34).
zone2sensor(10,7).
zone2sensor(10,8).
zone2sensor(10,10).
zone2sensor(11,7).
zone2sensor(11,24).
zone2sensor(12,24).
zone2sensor(12,34).
zone2sensor(12,35).
zone2sensor(13,9).
zone2sensor(13,10).
zone2sensor(13,12).
zone2sensor(14,9).
zone2sensor(14,25).
zone2sensor(15,25).
zone2sensor(15,35).
zone2sensor(15,36).
zone2sensor(16,11).
zone2sensor(16,12).
zone2sensor(16,14).
zone2sensor(17,11).
zone2sensor(17,26).
zone2sensor(18,26).
zone2sensor(18,36).
zone2sensor(18,37).
zone2sensor(19,13).
zone2sensor(19,14).
zone2sensor(19,16).
zone2sensor(20,13).
zone2sensor(20,27).
zone2sensor(21,27).
zone2sensor(21,37).
zone2sensor(21,38).
zone2sensor(22,15).
zone2sensor(22,16).
zone2sensor(22,18).
zone2sensor(23,15).
zone2sensor(23,28).
zone2sensor(24,28).
zone2sensor(24,38).
zone2sensor(24,39).
zone2sensor(25,17).
zone2sensor(25,18).
zone2sensor(25,20).
zone2sensor(26,17).
zone2sensor(26,29).
zone2sensor(27,29).
zone2sensor(27,39).
zone2sensor(27,40).
zone2sensor(28,19).
zone2sensor(28,20).
zone2sensor(29,19).
zone2sensor(29,30).
zone2sensor(30,30).
zone2sensor(30,40).
zone2sensor(31,1).
zone2sensor(31,4).
zone2sensor(31,21).
zone2sensor(32,1).
zone2sensor(32,3).
zone2sensor(32,6).
zone2sensor(32,21).
zone2sensor(33,6).
zone2sensor(33,8).
zone2sensor(33,22).
zone2sensor(33,32).
zone2sensor(33,34).
zone2sensor(34,8).
zone2sensor(34,10).
zone2sensor(34,24).
