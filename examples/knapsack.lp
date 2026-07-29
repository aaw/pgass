% Pick a subset of items whose total weight does not exceed the budget.
% Each item is freely picked or not (the choice rule below), and the #sum
% aggregate constraint rejects any pick whose weights add up past 8.
% Try: pgass --models 0 knapsack.lp

item(a, 5). item(b, 4). item(c, 3).

{ pick(I) } :- item(I, _).

:- #sum{ W,I : pick(I), item(I, W) } > 8.
