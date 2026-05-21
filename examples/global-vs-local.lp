dom(1). dom(2).
r(1,a). r(1,b). r(2,c).

p(X, Y) :- dom(X), Y = #count{ Z : r(X, Z) }.
