# Grounder

* Support queries: wire Program::query through to ASPIF assumptions (aspif.h already supports the
  statement, ground() currently rejects any program with a query).

# Misc.

* Implement operator precedence in parser (mixed +/*, for example).