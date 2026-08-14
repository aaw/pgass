# pgass
A pretty good [answer set](https://en.wikipedia.org/wiki/Answer_set_programming) solver.

pgass implements all of [ASP-Core-2](https://arxiv.org/pdf/1911.04326) with modular grounder and
solver components and a [cvc5](https://cvc5.github.io/) solver backend. It's competitive with
[clingo](https://potassco.org/clingo/) for day-to-day usage and on many of the
[2015](http://aspcomp2015.dibris.unige.it/) and [2017](http://aspcomp2017.dibris.unige.it/) ASP
competition benchmarks. It also ships modern, Pythonic bindings, so you can build ASP programs as
typed Python objects instead of `.lp` text.

Answer set programming is a powerful framework for expressing optimization problems. Here's an encoding of a
[minimum vertex cover](https://en.wikipedia.org/wiki/Vertex_cover) problem:

```
node(1..5).
edge(1,2). edge(2,3). edge(3,4). edge(4,5). edge(5,1). edge(1,3).
{ cover(X) } :- node(X).
:- edge(X,Y), not cover(X), not cover(Y).
#minimize { 1@0, X : cover(X) }.
#show cover/1.
```

Running `pgass` on this input will return:

```
$ pgass examples/vertex-cover.lp
Answer: 1
cover(1) cover(3) cover(4)
Cost: 3
SATISFIABLE
```

That output tells you that there is a minimum vertex cover of cost 3: `cover(1) cover(3) cover(4)`.

pgass also has Python bindings. An an [equivalent vertex cover solver](python/examples/vertex_cover.py) in Python looks like:

```python
import pgass

class Node(pgass.Predicate):
    x: int

class Edge(pgass.Predicate):
    x: int
    y: int

class Cover(pgass.Predicate):
    x: int

program = pgass.Program()
program.add(Node(x) for x in range(1, 6))
program.add(Edge(x, y) for x, y in
            [(1, 2), (2, 3), (3, 4), (4, 5), (5, 1), (1, 3)])

X, Y = pgass.vars("X Y")
program.add(pgass.choice(Cover(X)) << Node(X))
program.forbid(Edge(X, Y) & ~Cover(X) & ~Cover(Y))
program.minimize(Cover(X), weight=1, priority=0)
```

## Installation

Build from source: you need CMake 3.15 or newer and a C++20 compiler. Just type `make` and the binary
will end up in the `build/` directory.

Or grab a prebuilt binary from the [latest release](https://github.com/aaw/pgass/releases/latest).

Python bindings are on [PyPI](https://pypi.org/project/pgass/): `pip install pgass`.

## Usage

Pass one or more ASP input files on the command line or via standard input. By default, `pgass` prints the first answer set it finds
(or UNSAT if there are none). To enumerate all answer sets, pass `--models=0`:

```
$ pgass examples/vertex-cover.lp --models=0
Answer: 1
cover(1) cover(3) cover(4)
Cost: 3
Answer: 2
cover(1) cover(3) cover(5)
Cost: 3
Answer: 3
cover(2) cover(3) cover(5)
Cost: 3
Answer: 4
cover(1) cover(2) cover(4)
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

## Benchmarking

`make perf` runs a curated set of timed cases, mostly from ASP Competition benchmarks.

For a broader comparison, `scripts/bench.py` runs pgass over the ASP Competition 2015 and
2017 benchmark suites, downloading each domain on demand into a local cache directory.
`bench.py ls` lists the available
domains, `bench.py fetch <domain>` downloads one, and `bench.py run <domain> --compare` runs every
instance in a domain and checks answers with the domain's official solution checker. Pass an
instance number after the domain name to run just that one, e.g. `bench.py run CrossingMinimization
0002`.

## Language

pgass supports all of ASP-Core-2 plus a few non-standard language constructs that appear in benchmarks:

* `#show cover/1.` prints only the cover atoms of an answer set, and
  `#show pair(X,Y) : cover(X), cover(Y).` prints a term whether or not the program
  derives an atom by that name.
* `#const n = 3.` names a term, and every `n` in the program stands for it.
* `#minimize{ 1@0, X : cover(X) }.` is the weak constraints it spells out, one per
  element. `#maximize` is the same with the sign of each weight flipped.
* `1..3` is an interval, which stands for each integer in it: `node(1..3).` is three
  facts. The endpoints don't have to be literal numbers, either: in `p(1..N) :- q(N).`,
  `N` is whatever value `q(N)` bound it to.

## References

- [ASP-Core-2 Input Language Format](https://arxiv.org/pdf/1911.04326)
- [How to build your own ASP-based system ?!](https://arxiv.org/pdf/2008.06692): appendix B describes ASPIF, the grounder output format.
- [Stable models and difference logic](https://link.springer.com/article/10.1007/s10472-009-9118-9): Niemelä's translation of stable models into
  [QF_IDL](https://smt-lib.org/logics.shtml). pgass follows this translation for programs without weight bodies or weak constraints.