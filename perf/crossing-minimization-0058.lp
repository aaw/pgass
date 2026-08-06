% difficulty: hard
% expect: SAT
% cost: 22
%
% ASP Competition 2015, CrossingMinimization, instance 0058.
% The largest optimum in this domain that still finishes.
% Took 20.66s when it was added.

pvalue(L,1)   :- width(L,T), 0 < T.
pvalue(L,X+1) :- pvalue(L,X), width(L,T), X < T.

{ gt(L,N,P) } :- in_layer(L,N), pvalue(L,P), not width(L,P).
:- pvalue(L,P), gt(L,N,P+1), not gt(L,N,P).

position(N,1) :- in_layer(L,N), width(L,T), 0 < T, not gt(L,N,1).
position(N,Q) :- gt(L,N,P), Q = P+1, not gt(L,N,Q).
:- pvalue(L,P), 2 <= #count{ N : in_layer(L,N), position(N,P) }.

edge_layer(L1,L2,N1,N2) :- edge(N1,N2), in_layer(L1,N1), in_layer(L2,N2), L1 < L2.
edge_layer(L1,L2,N1,N2) :- edge(N2,N1), in_layer(L1,N1), in_layer(L2,N2), L1 < L2.

edge_pair(N1,N2,N3,N4) :- edge_layer(L1,L2,N1,N2), edge_layer(L1,L2,N3,N4), N1 < N3, N2 != N4.
edge_paired(N2,N4)     :- edge_pair(N1,N2,N3,N4).

compare(N1,N3) :- edge_pair(N1,N2,N3,N4).
compare(N2,N4) :- edge_paired(N2,N4), N2 < N4.
compare(N4,N2) :- edge_paired(N2,N4), N4 < N2.

inorder(N1,N2) :- compare(N1,N2), gt(L,N2,P), not gt(L,N1,P).

crossing(N1,N2,N3,N4) :- edge_pair(N1,N2,N3,N4), not edge_pair(N1,N4,N3,N2), compare(N2,N4), inorder(N1,N3), not inorder(N2,N4).
crossing(N1,N2,N3,N4) :- edge_pair(N1,N2,N3,N4), not edge_pair(N1,N4,N3,N2), compare(N2,N4), inorder(N2,N4), not inorder(N1,N3).
crossing(N1,N4,N3,N2) :- edge_pair(N1,N4,N3,N2), not edge_pair(N1,N2,N3,N4), compare(N2,N4), inorder(N1,N3), inorder(N2,N4).
crossing(N1,N4,N3,N2) :- edge_pair(N1,N4,N3,N2), not edge_pair(N1,N2,N3,N4), compare(N2,N4), not inorder(N1,N3), not inorder(N2,N4).

:~ crossing(N1,N2,N3,N4). [1,N1,N2,N3,N4]

% ---------------- instance

layers(5).
width(0,8).
in_layer(0,n1).
in_layer(0,n2).
in_layer(0,n3).
in_layer(0,n4).
in_layer(0,n5).
in_layer(0,n6).
in_layer(0,n7).
in_layer(0,n8).
width(1,8).
in_layer(1,n9).
in_layer(1,n10).
in_layer(1,n11).
in_layer(1,n12).
in_layer(1,n13).
in_layer(1,n14).
in_layer(1,n15).
in_layer(1,n16).
width(2,8).
in_layer(2,n17).
in_layer(2,n18).
in_layer(2,n19).
in_layer(2,n20).
in_layer(2,n21).
in_layer(2,n22).
in_layer(2,n23).
in_layer(2,n24).
width(3,8).
in_layer(3,n25).
in_layer(3,n26).
in_layer(3,n27).
in_layer(3,n28).
in_layer(3,n29).
in_layer(3,n30).
in_layer(3,n31).
in_layer(3,n32).
width(4,8).
in_layer(4,n33).
in_layer(4,n34).
in_layer(4,n35).
in_layer(4,n36).
in_layer(4,n37).
in_layer(4,n38).
in_layer(4,n39).
in_layer(4,n40).
edge(n29,n40).
edge(n14,n17).
edge(n8,n9).
edge(n15,n21).
edge(n3,n9).
edge(n15,n18).
edge(n2,n16).
edge(n26,n38).
edge(n30,n34).
edge(n29,n37).
edge(n6,n13).
edge(n19,n25).
edge(n15,n24).
edge(n4,n11).
edge(n18,n26).
edge(n22,n25).
edge(n20,n32).
edge(n9,n23).
edge(n5,n10).
edge(n30,n36).
edge(n7,n9).
edge(n19,n28).
edge(n10,n18).
edge(n27,n37).
edge(n13,n19).
edge(n7,n10).
edge(n9,n24).
edge(n7,n14).
edge(n6,n9).
edge(n12,n23).
edge(n28,n40).
edge(n6,n16).
edge(n24,n27).
edge(n21,n32).
edge(n2,n9).
edge(n25,n38).
edge(n6,n10).
edge(n30,n37).
edge(n11,n24).
edge(n11,n19).
edge(n13,n24).
edge(n18,n25).
edge(n27,n34).
edge(n19,n32).
edge(n3,n15).
edge(n5,n11).
edge(n16,n24).
edge(n7,n16).
edge(n26,n40).
edge(n22,n27).
edge(n23,n25).
