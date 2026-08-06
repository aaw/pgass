% difficulty: easy
% expect: SAT
%
% ASP Competition 2015, PermutationPatternMatching, instance 0117.
% Pattern matching that succeeds. Mostly a test of positive recursion.
% Took 2.26s when it was added.

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

t(1,60).
t(2,59).
t(3,47).
t(4,68).
t(5,42).
t(6,62).
t(7,44).
t(8,8).
t(9,23).
t(10,57).
t(11,52).
t(12,46).
t(13,50).
t(14,6).
t(15,31).
t(16,5).
t(17,53).
t(18,55).
t(19,49).
t(20,18).
t(21,58).
t(22,14).
t(23,22).
t(24,33).
t(25,16).
t(26,69).
t(27,56).
t(28,34).
t(29,61).
t(30,11).
t(31,37).
t(32,24).
t(33,35).
t(34,21).
t(35,54).
t(36,25).
t(37,7).
t(38,29).
t(39,17).
t(40,64).
t(41,36).
t(42,1).
t(43,41).
t(44,10).
t(45,70).
t(46,4).
t(47,48).
t(48,30).
t(49,40).
t(50,39).
t(51,26).
t(52,38).
t(53,12).
t(54,32).
t(55,63).
t(56,66).
t(57,3).
t(58,2).
t(59,27).
t(60,9).
t(61,15).
t(62,45).
t(63,43).
t(64,65).
t(65,19).
t(66,28).
t(67,20).
t(68,67).
t(69,51).
t(70,13).
p(1,4).
p(2,8).
p(3,12).
p(4,9).
p(5,2).
p(6,1).
p(7,6).
p(8,3).
p(9,11).
p(10,7).
p(11,14).
p(12,10).
p(13,13).
p(14,5).
patternlength(14).
