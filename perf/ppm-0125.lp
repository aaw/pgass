% difficulty: medium
% expect: SAT
%
% ASP Competition 2015, PermutationPatternMatching, instance 0125.
% A pattern of 14 over a sequence of 140, so twice the sequence the other two
% pattern matching cases carry. It grounds to 1029624 rules, 99.6% of them
% binary integrity constraints. That is the shape the clause form of a
% constraint is for, and this case is the one that holds it in place.
% Took 5.5s when it was added.

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

t(1,27).
t(2,70).
t(3,73).
t(4,127).
t(5,102).
t(6,118).
t(7,106).
t(8,111).
t(9,135).
t(10,7).
t(11,46).
t(12,138).
t(13,97).
t(14,107).
t(15,53).
t(16,140).
t(17,93).
t(18,32).
t(19,13).
t(20,34).
t(21,24).
t(22,71).
t(23,60).
t(24,69).
t(25,52).
t(26,66).
t(27,103).
t(28,30).
t(29,61).
t(30,94).
t(31,81).
t(32,33).
t(33,22).
t(34,74).
t(35,122).
t(36,119).
t(37,126).
t(38,82).
t(39,21).
t(40,37).
t(41,65).
t(42,45).
t(43,115).
t(44,95).
t(45,128).
t(46,29).
t(47,125).
t(48,35).
t(49,51).
t(50,84).
t(51,44).
t(52,3).
t(53,76).
t(54,109).
t(55,11).
t(56,16).
t(57,42).
t(58,72).
t(59,87).
t(60,15).
t(61,63).
t(62,41).
t(63,98).
t(64,4).
t(65,68).
t(66,100).
t(67,1).
t(68,90).
t(69,62).
t(70,120).
t(71,6).
t(72,96).
t(73,79).
t(74,121).
t(75,133).
t(76,83).
t(77,47).
t(78,43).
t(79,39).
t(80,136).
t(81,91).
t(82,101).
t(83,49).
t(84,130).
t(85,23).
t(86,114).
t(87,88).
t(88,129).
t(89,110).
t(90,105).
t(91,28).
t(92,2).
t(93,77).
t(94,132).
t(95,78).
t(96,25).
t(97,117).
t(98,56).
t(99,124).
t(100,104).
t(101,18).
t(102,31).
t(103,55).
t(104,116).
t(105,75).
t(106,86).
t(107,112).
t(108,12).
t(109,5).
t(110,17).
t(111,59).
t(112,92).
t(113,99).
t(114,80).
t(115,123).
t(116,50).
t(117,26).
t(118,48).
t(119,9).
t(120,137).
t(121,134).
t(122,20).
t(123,40).
t(124,57).
t(125,85).
t(126,54).
t(127,67).
t(128,10).
t(129,19).
t(130,131).
t(131,58).
t(132,108).
t(133,113).
t(134,36).
t(135,8).
t(136,89).
t(137,139).
t(138,64).
t(139,14).
t(140,38).
p(1,2).
p(2,5).
p(3,14).
p(4,1).
p(5,11).
p(6,6).
p(7,3).
p(8,12).
p(9,9).
p(10,4).
p(11,8).
p(12,10).
p(13,13).
p(14,7).
patternlength(14).
