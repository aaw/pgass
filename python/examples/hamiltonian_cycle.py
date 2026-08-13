"""Find a tour of the graph that leaves and enters every node exactly once.
See ../../examples/hamiltonian-cycle.lp."""

import pgass


class Node(pgass.Predicate):
    x: int


class Arc(pgass.Predicate):
    x: int
    y: int


class In(pgass.Predicate):
    x: int
    y: int


class Reach(pgass.Predicate):
    x: int


def main():
    X, Y = pgass.vars("X Y")
    program = pgass.Program()
    program.add(Node(x) for x in range(1, 5))
    program.add(Arc(x, y) for x, y in
                [(1, 2), (2, 3), (3, 4), (4, 1), (1, 3), (3, 2), (2, 4)])

    program.add(pgass.choice(In(X, Y)) << Arc(X, Y))
    program.forbid(Node(X) & (pgass.count(In(X, Y)) != 1))
    program.forbid(Node(Y) & (pgass.count(In(X, Y)) != 1))

    program.add(Reach(1))
    program.add(Reach(Y) << (Reach(X) & In(X, Y)))
    program.forbid(Node(X) & ~Reach(X))

    for answer_set in program.solve(models=0):
        tour = sorted((a.x, a.y) for a in answer_set[In])
        print(tour)


if __name__ == "__main__":
    main()
