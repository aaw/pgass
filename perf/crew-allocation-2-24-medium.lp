% difficulty: medium
% expect: UNSAT
%
% ASP Competition 2017, CrewAllocation, crew-2-24-medium.
% Unsatisfiable and hard for clingo, which does not finish it in 180s.
% Worth keeping as the case where pgass is far ahead.
% Took 4.47s when it was added.

% This encoding is based on an encoding by Hakan Kjellerstrand, hakank@gmail.com
% See also http://www.hakank.org/answer_set_programming/

%
% Crew allocation in ASP.
%
% From Gecode example crew
% examples/crew.cc
%
% (Original text from crew.cc)
% """
% Example: Airline crew allocation
%
% Assign x flight attendants to y flights. Each flight needs a certain
% number of cabin crew, and they have to speak certain languages.
% Every cabin crew member has z flights off after an attended flight.
% """

% a flight must have a crew of exactly Num persons
{ crew(Flight, Person) : p(Person, Type) } = Num :- num_crew(Flight, Num).

% the crew must have at least Num persons of a certain type
:- flight(Flight, Type, Num), #count{ Person : crew(Flight, Person), p(Person, Type) } < Num.

% every cabin crew member must have (a least) number of flights off after an attended flight
:- crew(Flight1, Person), crew(Flight2, Person), min_rest(X), Flight1 < Flight2, Flight2 <= Flight1+X.

% check whether better than optimal solution exists
:- minimumcrew(N), #count{ Person : crew(Flight, Person) } >= N.

% symmetry breaking

diff(Person1, Person2) :- p(Person1, Type1), p(Person2, Type2), flight(Flight, Type1, Num), not p(Person2, Type1).
same(Person1, Person2) :- p(Person1, Type1), p(Person2, Type2), Person1 < Person2, not diff(Person1, Person2), not diff(Person2, Person1).

dist(Person1, Person2) :- same(Person1, Person), same(Person, Person2).
next(Person1, Person2) :- same(Person1, Person2), not dist(Person1, Person2).

init(Flight) :- #min{ F : num_crew(F, Num) } = Flight.
last(Flight) :- #max{ F : num_crew(F, Num) } = Flight.

symm(Person1, Person2, Flight)   :- next(Person1, Person2), init(Flight), num_crew(Flight, Num).
symm(Person1, Person2, Flight+1) :- symm(Person1, Person2, Flight), not last(Flight), crew(Flight, Person2).
symm(Person1, Person2, Flight+1) :- symm(Person1, Person2, Flight), not last(Flight), not crew(Flight, Person1).

:- symm(Person1, Person2, Flight), crew(Flight, Person2), not crew(Flight, Person1).

% ---------------- instance

minimumcrew(20).
min_rest(2).
flights(1).
flights(2).
flights(3).
flights(4).
flights(5).
flights(6).
num_crew(1,6).
num_crew(2,7).
num_crew(3,7).
num_crew(4,6).
num_crew(5,4).
num_crew(6,4).
flight(1,steward,2).
flight(2,steward,3).
flight(3,steward,3).
flight(4,steward,2).
flight(5,steward,1).
flight(6,steward,1).
flight(1,hostess,2).
flight(2,hostess,3).
flight(3,hostess,3).
flight(4,hostess,2).
flight(5,hostess,1).
flight(6,hostess,1).
flight(1,spanish,1).
flight(2,spanish,1).
flight(3,spanish,1).
flight(4,spanish,1).
flight(5,spanish,1).
flight(6,spanish,1).
flight(2,french,1).
flight(3,french,1).
flight(4,french,1).
flight(1,german,1).
flight(2,german,1).
flight(3,german,1).
p(a,hostess).
p(b,hostess).
p(c,hostess).
p(d,hostess).
p(e,hostess).
p(f,hostess).
p(g,hostess).
p(h,hostess).
p(i,hostess).
p(j,hostess).
p(k,hostess).
p(l,hostess).
p(m,steward).
p(n,steward).
p(o,steward).
p(p,steward).
p(q,steward).
p(r,steward).
p(s,steward).
p(t,steward).
p(u,steward).
p(v,steward).
p(w,steward).
p(x,steward).
p(r,spanish).
p(w,spanish).
p(f,spanish).
p(j,spanish).
p(b,spanish).
p(a,spanish).
p(s,spanish).
p(g,spanish).
p(t,spanish).
p(v,spanish).
p(v,french).
p(u,french).
p(q,french).
p(k,french).
p(p,french).
p(o,french).
p(x,french).
p(g,french).
p(x,german).
p(h,german).
p(j,german).
p(e,german).
p(c,german).
p(q,german).
flights(7).
flights(0).
num_crew(7,3).
num_crew(0,3).
flight(7,steward,1).
flight(0,steward,1).
flight(7,hostess,1).
flight(0,hostess,1).
flight(7,spanish,1).
flight(0,spanish,1).
