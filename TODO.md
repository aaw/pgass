* Use unlimited precision integers for numbers and arithmetic. A number is a
  `uint64_t` in the AST and an `int64_t` once grounded, in a `SymEntry` or
  inlined into a `Sym` handle. ASP-Core-2 doesn't bound integers.
* Why can clingo solve PHP much faster than we can? Is it using some symmetry-breaking?
* Add an --encode flag that dumps SMT-LIB when possible (and something else useful when
  an optimization over SMT-LIB needs to run?)
* Add an ASSERT_OK macro, clean up multiline strings in tests
