# pgass
A pretty good [answer set](https://en.wikipedia.org/wiki/Answer_set_programming) solver

pgass implements all of [ASP-Core-2](https://arxiv.org/pdf/1911.04326), has modular
grounder and solver components, uses cvc5 as a solver backend, and is competitive with [clingo](https://potassco.org/clingo/) on
[2015](http://aspcomp2015.dibris.unige.it/) and [2017](http://aspcomp2017.dibris.unige.it/) ASP competition benchmarks.

Answer set programming is a powerful framework for expressing optimization problems. Here's an encoding of a
[minimum vertex cover](https://en.wikipedia.org/wiki/Vertex_cover) problem:

```
node(1). node(2). node(3). node(4). node(5).
edge(1,2). edge(2,3). edge(3,4). edge(4,5). edge(5,1). edge(1,3).
{ cover(X) } :- node(X).
:- edge(X,Y), not cover(X), not cover(Y).
:~ cover(X). [1@0, X]
```

Running `pgass` on this input will return:

```
$ pgass examples/vertex-cover.lp
Answer: 1
edge(1,2) edge(2,3) edge(3,4) edge(4,5) edge(5,1) edge(1,3) node(1) node(2) node(3) node(4) node(5) cover(1) cover(3) cover(4)
Cost: 3
SATISFIABLE
```

Which tells you that there is a minimum vertex cover of cost 3: `cover(1) cover(3) cover(4)`.

## Usage

To build, you need CMake 3.14 or newer and a C++20 compiler. Just type `make` and the binary will end up in the `build/` directory.

Pass one or more ASP input files on the command line or via standard input. By default, `pgass` prints the first answer set it finds
(or UNSAT if there are none). To enumerate all answer sets, pass `--models=0`:

```
$ pgass examples/vertex-cover.lp --models=0
Answer: 1
edge(1,2) edge(2,3) edge(3,4) edge(4,5) edge(5,1) edge(1,3) node(1) node(2) node(3) node(4) node(5) cover(1) cover(3) cover(4)
Cost: 3
Answer: 2
edge(1,2) edge(2,3) edge(3,4) edge(4,5) edge(5,1) edge(1,3) node(1) node(2) node(3) node(4) node(5) cover(1) cover(3) cover(5)
Cost: 3
Answer: 3
edge(1,2) edge(2,3) edge(3,4) edge(4,5) edge(5,1) edge(1,3) node(1) node(2) node(3) node(4) node(5) cover(2) cover(3) cover(5)
Cost: 3
Answer: 4
edge(1,2) edge(2,3) edge(3,4) edge(4,5) edge(5,1) edge(1,3) node(1) node(2) node(3) node(4) node(5) cover(1) cover(2) cover(4)
Cost: 3
SATISFIABLE
```

The grounding and solving passes can be separated using the same intermediate grounding file format that gringo/clasp uses, ASPIF:

```
$ pgass --ground examples/vertex-cover.lp | pgass --solve
```

You can also ask `pgass` to dump an [SMT-LIB](https://smt-lib.org/) file that represents the grounded program to solve.
Follow instructions in the resulting file to generate answer sets with an SMT solver like cvc5 or z3:

```
$ cvc5 <(pgass --encode=smtlib examples/vertex-cover.lp)
sat
((cost@0 3))
(
(define-fun |edge(1,2)| () Bool true)
(define-fun |edge(2,3)| () Bool true)
(define-fun |edge(3,4)| () Bool true)
(define-fun |edge(4,5)| () Bool true)
(define-fun |edge(5,1)| () Bool true)
(define-fun |edge(1,3)| () Bool true)
(define-fun |node(1)| () Bool true)
(define-fun |node(2)| () Bool true)
(define-fun |node(3)| () Bool true)
(define-fun |node(4)| () Bool true)
(define-fun |node(5)| () Bool true)
(define-fun |cover(1)| () Bool false)
(define-fun a13 () Bool true)
(define-fun |cover(2)| () Bool true)
(define-fun a15 () Bool false)
(define-fun |cover(3)| () Bool true)
(define-fun a17 () Bool false)
(define-fun |cover(4)| () Bool false)
(define-fun a19 () Bool true)
(define-fun |cover(5)| () Bool true)
(define-fun a21 () Bool false)
(define-fun a22 () Bool false)
(define-fun a23 () Bool true)
(define-fun a24 () Bool true)
(define-fun a25 () Bool false)
(define-fun a26 () Bool true)
)
```

The `cover(1)`, `cover(2)`, etc. variables in the resulting solution set represent the vertex cover, so the above result
says `{2,3,5}` is a vertex cover of cost 3.


## References

- [ASP-Core-2 Input Language Format](https://arxiv.org/pdf/1911.04326)
- [How to build your own ASP-based system ?!](https://arxiv.org/pdf/2008.06692): appendix B describes ASPIF, the grounder output format.