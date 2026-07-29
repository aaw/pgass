% Put each pigeon in a hole, no two pigeons sharing one. With more pigeons
% than holes there is no way to do it, so this program has no answer sets and
% pgass prints UNSATISFIABLE. The first constraint gives every pigeon exactly
% one hole, the second stops any hole taking two.
%
% Proving it impossible costs a solver time that grows steeply with the number
% of holes: no short argument exists, so it has to work through the ways the
% pigeons could be arranged. Three pigeons is instant, but adding a pigeon and
% a hole at a time gets slow quickly, which makes this a handy stress test.
% Try: pgass pigeonhole.lp

pigeon(1). pigeon(2). pigeon(3).
hole(a). hole(b).

{ in(P, H) } :- pigeon(P), hole(H).

:- pigeon(P), #count{ H : in(P, H) } != 1.
:- hole(H), #count{ P : in(P, H) } > 1.
