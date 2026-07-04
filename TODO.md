Normalization
=============

* Remove integrity constraint normalization, don't need for QF_IDL translation.

* Add head disjunction normalization:

  a | b | c :- d, e, not f

  normalizes to:

  a :- d, e, not f, not b, not c.
  b :- d, e, not f, not a, not c.
  c :- d, e, not f, not a, not b.

  BUT need to couple this with a positive head-cycle freeness (?) check. Example:

  a | b.
  a :- b.
  b :- a.

  has an answer set of {a,b}. but the normalized version:

  a :- not b.
  b :- not a.
  a :- b.
  b :- a.

  has no answer sets. What does clingo do for this type of program? I think we should reject
  during normalization if we detect.

* Remove classical negation: -p becomes _neg_p along with new rule ":- p, _neg_p"

* Rewrite weak constraints. ":~ node(X), color(X, red). [1@0, X]" becomes
  "_viol(0, 1, X) :- node(X), color(X, red)." along with a side-channel directive
  of "cost at level L is the sum of W over all true _viol(L, W, ...) atoms" to
  be consumed later in the pipeline. _viol is a global violation "accumulator".