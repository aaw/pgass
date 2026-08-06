% difficulty: medium
% expect: SAT
%
% ASP Competition 2015, PartnerUnits, instance 166.
% The satisfiable side of the same domain, at 100 units.
% Took 5.19s when it was added.

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


maxPU(4).

comUnit(1).
comUnit(2).
comUnit(3).
comUnit(4).
comUnit(5).
comUnit(6).
comUnit(7).
comUnit(8).
comUnit(9).
comUnit(10).
comUnit(11).
comUnit(12).
comUnit(13).
comUnit(14).
comUnit(15).
comUnit(16).
comUnit(17).
comUnit(18).
comUnit(19).
comUnit(20).
comUnit(21).
comUnit(22).
comUnit(23).
comUnit(24).
comUnit(25).
comUnit(26).
comUnit(27).
comUnit(28).
comUnit(29).
comUnit(30).
comUnit(31).
comUnit(32).
comUnit(33).
comUnit(34).
comUnit(35).
comUnit(36).
comUnit(37).
comUnit(38).
comUnit(39).
comUnit(40).
comUnit(41).
comUnit(42).
comUnit(43).
comUnit(44).
comUnit(45).
comUnit(46).
comUnit(47).
comUnit(48).
comUnit(49).
comUnit(50).

% relation between zones and door sensors
zone2sensor(1,75).
zone2sensor(1,13).
zone2sensor(2,75).
zone2sensor(2,62).
zone2sensor(3,55).
zone2sensor(3,38).
zone2sensor(4,5).
zone2sensor(4,55).
zone2sensor(4,36).
zone2sensor(5,36).
zone2sensor(5,84).
zone2sensor(6,84).
zone2sensor(7,82).
zone2sensor(8,82).
zone2sensor(8,3).
zone2sensor(8,53).
zone2sensor(9,13).
zone2sensor(9,37).
zone2sensor(10,62).
zone2sensor(10,37).
zone2sensor(10,97).
zone2sensor(10,19).
zone2sensor(11,38).
zone2sensor(11,97).
zone2sensor(11,46).
zone2sensor(11,95).
zone2sensor(12,46).
zone2sensor(12,12).
zone2sensor(13,12).
zone2sensor(13,66).
zone2sensor(14,66).
zone2sensor(14,23).
zone2sensor(14,96).
zone2sensor(15,23).
zone2sensor(15,99).
zone2sensor(16,53).
zone2sensor(16,99).
zone2sensor(16,45).
zone2sensor(17,72).
zone2sensor(17,24).
zone2sensor(18,19).
zone2sensor(18,72).
zone2sensor(18,52).
zone2sensor(18,87).
zone2sensor(19,95).
zone2sensor(19,52).
zone2sensor(19,43).
zone2sensor(20,58).
zone2sensor(21,69).
zone2sensor(22,96).
zone2sensor(22,70).
zone2sensor(22,33).
zone2sensor(23,70).
zone2sensor(23,15).
zone2sensor(24,45).
zone2sensor(24,41).
zone2sensor(25,24).
zone2sensor(26,87).
zone2sensor(26,48).
zone2sensor(26,18).
zone2sensor(27,43).
zone2sensor(27,48).
zone2sensor(27,34).
zone2sensor(28,58).
zone2sensor(28,92).
zone2sensor(29,69).
zone2sensor(29,50).
zone2sensor(30,33).
zone2sensor(30,50).
zone2sensor(30,49).
zone2sensor(30,25).
zone2sensor(31,15).
zone2sensor(31,49).
zone2sensor(31,22).
zone2sensor(31,26).
zone2sensor(32,41).
zone2sensor(32,22).
zone2sensor(33,2).
zone2sensor(33,71).
zone2sensor(33,63).
zone2sensor(34,18).
zone2sensor(34,71).
zone2sensor(34,9).
zone2sensor(35,34).
zone2sensor(35,31).
zone2sensor(35,73).
zone2sensor(36,92).
zone2sensor(36,31).
zone2sensor(36,60).
zone2sensor(37,60).
zone2sensor(37,93).
zone2sensor(37,59).
zone2sensor(38,25).
zone2sensor(38,93).
zone2sensor(39,26).
zone2sensor(39,8).
zone2sensor(40,78).
zone2sensor(41,63).
zone2sensor(41,40).
zone2sensor(42,9).
zone2sensor(42,40).
zone2sensor(42,30).
zone2sensor(43,73).
zone2sensor(44,28).
zone2sensor(44,10).
zone2sensor(45,59).
zone2sensor(45,28).
zone2sensor(46,32).
zone2sensor(46,100).
zone2sensor(47,8).
zone2sensor(47,32).
zone2sensor(47,88).
zone2sensor(47,27).
zone2sensor(48,78).
zone2sensor(48,88).
zone2sensor(48,4).
zone2sensor(49,61).
zone2sensor(49,14).
zone2sensor(50,30).
zone2sensor(50,61).
zone2sensor(51,98).
zone2sensor(51,79).
zone2sensor(52,10).
zone2sensor(52,98).
zone2sensor(52,90).
zone2sensor(53,20).
zone2sensor(53,54).
zone2sensor(54,100).
zone2sensor(54,20).
zone2sensor(54,29).
zone2sensor(55,27).
zone2sensor(55,29).
zone2sensor(55,76).
zone2sensor(55,86).
zone2sensor(56,76).
zone2sensor(56,56).
zone2sensor(57,14).
zone2sensor(57,1).
zone2sensor(57,64).
zone2sensor(57,94).
zone2sensor(58,64).
zone2sensor(58,68).
zone2sensor(59,79).
zone2sensor(59,16).
zone2sensor(60,90).
zone2sensor(60,16).
zone2sensor(60,44).
zone2sensor(60,35).
zone2sensor(61,54).
zone2sensor(61,44).
zone2sensor(61,11).
zone2sensor(61,77).
zone2sensor(62,11).
zone2sensor(62,80).
zone2sensor(63,86).
zone2sensor(63,83).
zone2sensor(64,56).
zone2sensor(64,83).
zone2sensor(65,94).
zone2sensor(65,65).
zone2sensor(65,67).
zone2sensor(66,68).
zone2sensor(66,65).
zone2sensor(66,42).
zone2sensor(66,91).
zone2sensor(67,42).
zone2sensor(67,51).
zone2sensor(68,35).
zone2sensor(68,51).
zone2sensor(68,17).
zone2sensor(68,7).
zone2sensor(69,77).
zone2sensor(69,17).
zone2sensor(69,21).
zone2sensor(70,80).
zone2sensor(70,74).
zone2sensor(70,47).
zone2sensor(71,74).
zone2sensor(71,39).
zone2sensor(72,39).
zone2sensor(73,67).
zone2sensor(73,6).
zone2sensor(74,91).
zone2sensor(74,57).
zone2sensor(75,57).
zone2sensor(75,81).
zone2sensor(76,7).
zone2sensor(76,81).
zone2sensor(77,21).
zone2sensor(78,47).
zone2sensor(78,89).
zone2sensor(79,89).
zone2sensor(79,85).
zone2sensor(80,85).
