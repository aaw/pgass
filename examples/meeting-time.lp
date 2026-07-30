% Pick the one meeting slot that annoys the fewest people, where some kinds
% of annoyance matter more than others. The two weak constraints sit at
% different priority levels: level 1 outranks level 0, so the solver first
% minimizes the people who cannot attend at all, and only then breaks ties by
% minimizing the people who merely dislike the slot.
%
% Noon wins: everyone can make it, at the cost of the one person who dislikes
% it, so the cost printed is 0 at level 1 and 1 at level 0.
% Try: pgass meeting-time.lp

slot(morning). slot(noon). slot(evening).
person(ana). person(bo). person(cy).

% cannot(P, S): P is unavailable at S.
cannot(ana, morning).
cannot(bo, evening).
cannot(cy, evening).

% dislikes(P, S): P can make S work, but would rather not.
dislikes(ana, noon).
dislikes(bo, morning).

{ meet(S) } :- slot(S).

:- #count{ S : meet(S) } != 1.

:~ meet(S), cannot(P, S). [1@1, P]
:~ meet(S), dislikes(P, S). [1@0, P]
