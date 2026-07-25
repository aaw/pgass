% A proposal passes if at least 2 of the 3 committee members vote yes.
% Each member's vote is a free choice (the choice rule below), and the
% #count aggregate counts how many voted yes in that choice.
% Try: pgass --ground majority-vote.lp | clasp 0

member(alice). member(bob). member(carol).

{ yes(M) } :- member(M).

passes :- #count{ M : yes(M) } >= 2.
