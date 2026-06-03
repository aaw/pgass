# pgass
A pretty good [answer set](https://en.wikipedia.org/wiki/Answer_set_programming) solver

## The Plan

- [x] Parse [ASP-Core-2 Input Language Format](https://arxiv.org/pdf/1911.04326)
- [ ] Implement a normalizer [in progress]
- [ ] Implement a simple grounder
- [ ] Translate grounded, normalized program to [QF_IDL](https://smt-lib.org/logics-all.shtml#QF_IDL) as described by [Niemelä](https://link.springer.com/article/10.1007/s10472-009-9118-9), solve with [cvc5](https://cvc5.github.io)
- [ ] Implement a better, maybe lazy grounder