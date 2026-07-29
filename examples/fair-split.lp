% Split the chores between two people so that both take on the same total
% effort. Each chore goes to one of them, and the two #sum aggregates name
% their totals: '#sum{...} = A' binds A to the sum rather than comparing it
% against a number already known. Naming both totals is what lets the last
% rule compare them against each other, which no single aggregate can do.
% Try: pgass --models 0 fair-split.lp

chore(dishes, 3). chore(laundry, 2). chore(vacuum, 4). chore(shopping, 5).

{ does(ana, C) } :- chore(C, _).
does(bo, C) :- chore(C, _), not does(ana, C).

effort(ana, A) :- #sum{ E,C : does(ana, C), chore(C, E) } = A.
effort(bo, B) :- #sum{ E,C : does(bo, C), chore(C, E) } = B.

:- effort(ana, A), effort(bo, B), A != B.
