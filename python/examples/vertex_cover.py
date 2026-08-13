"""Pick a set of nodes touching every edge, as small as possible. See
../../examples/vertex-cover.lp."""

import pgass


class Node(pgass.Predicate):
    x: int


class Edge(pgass.Predicate):
    x: int
    y: int


class Cover(pgass.Predicate):
    x: int


def main():
    X, Y = pgass.vars("X Y")
    program = pgass.Program()
    program.add(Node(x) for x in range(1, 6))
    program.add(Edge(x, y) for x, y in
                [(1, 2), (2, 3), (3, 4), (4, 5), (5, 1), (1, 3)])

    program.add(pgass.choice(Cover(X)) << Node(X))
    program.forbid(Edge(X, Y) & ~Cover(X) & ~Cover(Y))
    program.minimize(Cover(X), weight=1, priority=0)

    for i, answer_set in enumerate(program.solve(models=0), start=1):
        covered = sorted(c.x for c in answer_set[Cover])
        print(f"Answer {i}: cover({', '.join(map(str, covered))})  "
              f"cost={answer_set.cost[0]}")


if __name__ == "__main__":
    main()
