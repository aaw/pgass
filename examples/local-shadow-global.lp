%*
   This example should generate a warning. ASP-Core-2 local variables don't
   shadow, so the X in the aggregation is the _same_ X defined globally to the
   rule. If the variable shadowed, this program would generate p(3,2) but
   it generates p(3,0) instead.
*%

dom(1). dom(2). dom(3).
r(1). r(2).

p(X, Y) :- dom(X), Y = #count{ X : r(X) }.
