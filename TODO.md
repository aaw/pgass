Normalization
=============

* Rewrite weak constraints. ":~ node(X), color(X, red). [1@0, X]" becomes
  "_viol(0, 1, X) :- node(X), color(X, red)." along with a side-channel directive
  of "cost at level L is the sum of W over all true _viol(L, W, ...) atoms" to
  be consumed later in the pipeline. _viol is a global violation "accumulator".