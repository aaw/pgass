* Use unlimited precision integers for numbers and arithmetic. A number is a
  `uint64_t` in the AST and an `int64_t` once grounded. ASP-Core-2 doesn't bound integers.
* Intern ground values into a symbol table. A `Value` carries a `std::string`
  and a `std::vector<Value>` by value, so every tuple the store holds, hashes,
  or compares walks that. Interning each distinct value to an integer handle
  would make a tuple a few words, which is the remaining gap to gringo on
  densely connected graphs.
