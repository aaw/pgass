"""Put each pigeon in a hole, no two pigeons sharing one. With more pigeons
than holes there is no way to do it, so program.solve() simply yields no
answer sets. See ../../examples/pigeonhole.lp."""

import pgass


class Pigeon(pgass.Predicate):
    p: int


class Hole(pgass.Predicate):
    h: str


class In(pgass.Predicate):
    p: int
    h: str


def main():
    P, H = pgass.vars("P H")
    program = pgass.Program()
    program.add(Pigeon(1), Pigeon(2), Pigeon(3))
    program.add(Hole("a"), Hole("b"))

    program.add(pgass.choice(In(P, H)) << (Pigeon(P) & Hole(H)))
    program.forbid(Pigeon(P) & (pgass.count(In(P, H)) != 1))
    program.forbid(Hole(H) & (pgass.count(In(P, H)) > 1))

    results = list(program.solve())
    print("UNSATISFIABLE" if not results else results[0][In])


if __name__ == "__main__":
    main()
