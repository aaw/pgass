# pgass
A pretty good [answer set](https://en.wikipedia.org/wiki/Answer_set_programming) solver

## The Plan

- [x] Parse ASP-Core-2 input into an AST
- [x] Implement a normalizer
- [x] Implement a simple grounder
- [ ] Translate grounded, normalized program to [QF_IDL](https://smt-lib.org/logics-all.shtml#QF_IDL) (or maybe QF_LIA) as described by [Niemelä](https://link.springer.com/article/10.1007/s10472-009-9118-9), solve with [cvc5](https://cvc5.github.io) [in progress]
- [ ] Implement a better, maybe lazy grounder

## References

- [ASP-Core-2 Input Language Format](https://arxiv.org/pdf/1911.04326)
- [How to build your own ASP-based system ?!](https://arxiv.org/pdf/2008.06692): appendix B describes ASPIF, the grounder output format that clasp accepts.