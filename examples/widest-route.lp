% Send traffic along the widest route across the network. A route is only as
% good as its narrowest link, so '#min{...} = C' binds C to the smallest
% capacity along it. That is what #min and #max do that #count and #sum cannot:
% they pick one of the terms the elements produced instead of adding them up.
%
% The last two rules then pick the best route. 'best' is the #max over every
% route's capacity, and the constraint keeps only a route that reaches it.
% South has a link of 3 and north one of 4, so coast wins with 7.
% Try: pgass widest-route.lp

% link(Route, Name, Capacity)
link(north, a, 6). link(north, b, 4).
link(south, c, 3). link(south, d, 9). link(south, e, 5).
link(coast, f, 7). link(coast, g, 7).

route(R) :- link(R, _, _).

{ take(R) } :- route(R).
:- #count{ R : take(R) } != 1.

capacity(R, C) :- route(R), #min{ W : link(R, _, W) } = C.
best(B) :- #max{ C : capacity(_, C) } = B.

:- take(R), capacity(R, C), best(B), C != B.
