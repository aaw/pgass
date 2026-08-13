"""Color a graph with two colors so no edge connects same-colored nodes. See
../../examples/two-coloring.lp."""

import pgass


class Node(pgass.Predicate):
    x: int


class Edge(pgass.Predicate):
    x: int
    y: int


class Red(pgass.Predicate):
    x: int


class Green(pgass.Predicate):
    x: int


def main():
    X, Y = pgass.vars("X Y")
    program = pgass.Program()
    program.add(Node(x) for x in range(1, 5))
    program.add(Edge(x, y) for x, y in [(1, 2), (2, 3), (3, 4), (4, 1)])

    program.add(Red(X) << (Node(X) & ~Green(X)))
    program.add(Green(X) << (Node(X) & ~Red(X)))
    program.forbid(Edge(X, Y) & Red(X) & Red(Y))
    program.forbid(Edge(X, Y) & Green(X) & Green(Y))

    for answer_set in program.solve(models=0):
        reds = sorted(r.x for r in answer_set[Red])
        greens = sorted(g.x for g in answer_set[Green])
        print(f"red{reds} green{greens}")


if __name__ == "__main__":
    main()
