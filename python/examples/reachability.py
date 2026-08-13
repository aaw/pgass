"""Which nodes can reach which over directed edges? See ../../examples/reachability.lp."""

import pgass


class Edge(pgass.Predicate):
    x: str
    y: str


class Reach(pgass.Predicate):
    x: str
    y: str


def main():
    X, Y, Z = pgass.vars("X Y Z")
    program = pgass.Program()
    program.add(Edge("a", "b"), Edge("b", "c"), Edge("b", "d"), Edge("d", "a"))
    program.add(Reach(X, Y) << Edge(X, Y))
    program.add(Reach(X, Z) << (Reach(X, Y) & Edge(Y, Z)))

    answer_set = next(program.solve())
    for reach in sorted(answer_set[Reach], key=lambda r: (r.x, r.y)):
        print(reach)


if __name__ == "__main__":
    main()
