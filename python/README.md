# pgass

Python bindings for [pgass](https://github.com/aaw/pgass), a pretty
good [answer set](https://en.wikipedia.org/wiki/Answer_set_programming)
solver. Build and solve answer set programs as typed Python classes:

```python
import pgass

class Node(pgass.Predicate):
    x: int

class Edge(pgass.Predicate):
    x: int
    y: int

class Cover(pgass.Predicate):
    x: int

X, Y = pgass.vars("X Y")

program = pgass.Program()
program.add(Node(x) for x in range(1, 6))
program.add(Edge(x, y) for x, y in
            [(1, 2), (2, 3), (3, 4), (4, 5), (5, 1), (1, 3)])
program.add(pgass.choice(Cover(X)) << Node(X))
program.forbid(Edge(X, Y) & ~Cover(X) & ~Cover(Y))
program.minimize(Cover(X), weight=1, priority=0)

for answer_set in program.solve(models=0):
    print(sorted(c.x for c in answer_set[Cover]), answer_set.cost)
```

This is a minimum vertex cover: `program.forbid` says no edge may go
uncovered at both ends, and `program.minimize` asks for the fewest covered
nodes. See the [root README](https://github.com/aaw/pgass) for what
pgass itself does and how the CLI works.
