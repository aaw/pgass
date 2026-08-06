% difficulty: medium
% expect: UNSAT
%
% ASP Competition 2015, PermutationPatternMatching, instance 0092.
% The unsatisfiable side of pattern matching.
% Took 8.40s when it was added.

% Example Input
% t(1,5).
% t(2,3).
% t(3,1).
% t(4,4).
% t(5,2).
% p(1,2).
% p(2,3).
% p(3,1).
% patternlength(3).

kval(K) :- p(K,P), patternlength(L), K <= L.
pair(K1,K2) :- kval(K1), kval(K2), p(K1,P1), p(K2,P2), P1 <= P2.

{ geq(K,I) } :- kval(K), t(I,E).
:- kval(K), t(I,E), geq(K,I+1), not geq(K,I).
:- kval(K), t(I,E), geq(K-1,I), not geq(K,I+1).
:- kval(K), not geq(K,K).

solution(K,E) :- kval(K), t(I,E), geq(K,I), not geq(K,I+1).
:- pair(K1,K2), solution(K1,E1), solution(K2,E2), E2 < E1.

% ---------------- instance

t(1,48).
t(2,56).
t(3,36).
t(4,39).
t(5,30).
t(6,57).
t(7,41).
t(8,44).
t(9,53).
t(10,2).
t(11,12).
t(12,60).
t(13,17).
t(14,14).
t(15,22).
t(16,47).
t(17,13).
t(18,55).
t(19,8).
t(20,25).
t(21,38).
t(22,26).
t(23,37).
t(24,54).
t(25,50).
t(26,28).
t(27,24).
t(28,64).
t(29,5).
t(30,70).
t(31,23).
t(32,32).
t(33,66).
t(34,34).
t(35,11).
t(36,3).
t(37,31).
t(38,21).
t(39,6).
t(40,45).
t(41,20).
t(42,1).
t(43,18).
t(44,61).
t(45,69).
t(46,51).
t(47,43).
t(48,7).
t(49,40).
t(50,42).
t(51,46).
t(52,49).
t(53,67).
t(54,15).
t(55,29).
t(56,9).
t(57,10).
t(58,33).
t(59,68).
t(60,65).
t(61,63).
t(62,58).
t(63,27).
t(64,59).
t(65,35).
t(66,52).
t(67,4).
t(68,19).
t(69,62).
t(70,16).
p(1,21).
p(2,19).
p(3,12).
p(4,4).
p(5,5).
p(6,20).
p(7,18).
p(8,17).
p(9,3).
p(10,2).
p(11,1).
p(12,16).
p(13,14).
p(14,28).
p(15,10).
p(16,8).
p(17,6).
p(18,27).
p(19,7).
p(20,9).
p(21,25).
p(22,22).
p(23,23).
p(24,13).
p(25,11).
p(26,26).
p(27,24).
p(28,15).
patternlength(28).
