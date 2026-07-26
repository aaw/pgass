* Use unlimited precision integers for numbers and arithmetic. A number is a
  `uint64_t` in the AST and an `int64_t` once grounded. ASP-Core-2 doesn't bound integers.
* Intern ground values into a symbol table. A `Value` carries a `std::string`
  and a `std::vector<Value>` by value, so every tuple the store holds, hashes,
  or compares walks that. Interning each distinct value to an integer handle
  would make a tuple a few words, which is the remaining gap to gringo on
  densely connected graphs.
* Simplify the ground program before emitting it. An atom whose body is all
  facts is itself a fact, so its rule can go away. Work that out to a fixpoint
  and drop the rules it makes redundant. On a 300-node, 1500-edge transitive
  closure, gringo emits 90,004 facts and no rules; we emit those atoms plus
  445,520 rules, all of which the solver then has to chew through.
* Ground each aggregate once, not once per rule instance. `emit_rules` calls
  ground_aggregate() inside its per-instance callback, so an aggregate that
  doesn't mention the rule's variables gets re-encoded, with fresh auxiliary
  atoms, every time. 'p(X) :- dom(X), #count{ Y : r(Y) } >= 2.' with 200 dom
  and 100 r atoms grounds to 21,502 aspif lines; clingo needs 600. Cache on the
  aggregate plus the outer variables it actually uses.
