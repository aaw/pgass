% Color a graph with two colors so no edge connects same-colored nodes.
% Each node is red or green (the two rules below force exactly one choice),
% and the constraints reject colorings where an edge's ends match.
% Try: pgass --ground two-coloring.lp | clasp 0

node(1). node(2). node(3). node(4).
edge(1, 2).
edge(2, 3).
edge(3, 4).
edge(4, 1).

red(X) :- node(X), not green(X).
green(X) :- node(X), not red(X).

:- edge(X, Y), red(X), red(Y).
:- edge(X, Y), green(X), green(Y).
