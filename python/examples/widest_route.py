"""Send traffic along the widest route across the network: a route is only
as good as its narrowest link, so pgass.min(...) == C binds C to the
smallest capacity along it. See ../../examples/widest-route.lp."""

import pgass


class Link(pgass.Predicate):
    route: str
    name: str
    capacity: int


class Route(pgass.Predicate):
    route: str


class Take(pgass.Predicate):
    route: str


class Capacity(pgass.Predicate):
    route: str
    value: int


class Best(pgass.Predicate):
    value: int


def main():
    R, N, W, C, B = pgass.vars("R N W C B")
    program = pgass.Program()
    program.add(
        Link("north", "a", 6), Link("north", "b", 4),
        Link("south", "c", 3), Link("south", "d", 9), Link("south", "e", 5),
        Link("coast", "f", 7), Link("coast", "g", 7),
    )

    program.add(Route(R) << Link(R, N, W))
    program.add(pgass.choice(Take(R)) << Route(R))
    program.forbid(pgass.count(Take(R)) != 1)

    program.add(Capacity(R, C) <<
                (Route(R) & (pgass.min(Link(R, N, W), terms=(W, N)) == C)))
    program.add(Best(B) << (pgass.max(Capacity(R, C), terms=(C, R)) == B))

    program.forbid(Take(R) & Capacity(R, C) & Best(B) & pgass.ne(C, B))

    answer_set = next(program.solve())
    take = answer_set[Take][0]
    capacity = next(c for c in answer_set[Capacity] if c.route == take.route)
    print(f"{take.route} wins with capacity {capacity.value}")


if __name__ == "__main__":
    main()
