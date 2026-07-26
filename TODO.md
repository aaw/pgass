# Grounder

* Support binding a variable to an aggregate's value (e.g. `#sum{X : p(X)} = S`); safety.cc already
  accepts it, ground() rejects it as unimplemented.
* Support queries: wire Program::query through to ASPIF assumptions (aspif.h already supports the
  statement, ground() currently rejects any program with a query).

# Misc.

* Implement operator precedence in parser (mixed +/*, for example).