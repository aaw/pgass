% Is there an assignment to x1 and x2 that satisfies (x1 or Y) and (x2 or not Y)
% for *every* Y? Finding an assignment that works for one Y is an ordinary
% search; checking that no Y refutes it is a second search inside the first, and
% that is a step harder than anything one SAT solver call decides.
%
% Saturation is how ASP asks the harder question. The rules below guess an
% assignment, guess a Y, and derive 'sat' when the formula holds under the two.
% Everything the Y guess derives is then derived back from 'sat', so every Y
% that satisfies the formula collapses into one saturated answer set. A Y that
% refuted the formula would derive no 'sat', leaving a strictly smaller model,
% and a smaller model is not an answer set: the candidate assignment dies with
% it. So an answer set survives exactly for an assignment no Y refutes.
%
% That trick needs 'y | ny.' and 'y :- sat.' at once, which puts y and ny in the
% head of one rule and on a positive cycle through sat. Such a head cycle is
% what a solver cannot decide by one translation, and pgass checks each
% candidate for minimality to settle it.
%
% The answer is x1 and x2 together: Y true leans on x2, Y false leans on x1.
% Try: pgass --models 0 saturation.lp

% Guess an assignment to the variables the question is asking for.
x1 | nx1.
x2 | nx2.

% Guess a Y to test that assignment against.
y | ny.

% The two clauses, under the guesses above.
c1 :- x1.
c1 :- y.
c2 :- x2.
c2 :- ny.

% The formula holds when both clauses do.
sat :- c1, c2.

% Saturate: derive everything the Y guess produced, so that a satisfying Y is
% indistinguishable from any other and only a refuting Y stays small.
y :- sat.
ny :- sat.
c1 :- sat.
c2 :- sat.

% Keep only the assignments no Y refutes.
:- not sat.
