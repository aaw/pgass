"""A proposal passes if at least 2 of the 3 committee members vote yes. See
../../examples/majority-vote.lp."""

import pgass


class Member(pgass.Predicate):
    name: str


class Yes(pgass.Predicate):
    name: str


class Passes(pgass.Predicate):
    pass


def main():
    M = pgass.Var("M")
    program = pgass.Program()
    program.add(Member("alice"), Member("bob"), Member("carol"))
    program.add(pgass.choice(Yes(M)) << Member(M))
    program.add(Passes() << (pgass.count(Yes(M)) >= 2))

    for answer_set in program.solve(models=0):
        yes = sorted(y.name for y in answer_set[Yes])
        print(f"yes={yes} passes={bool(answer_set[Passes])}")


if __name__ == "__main__":
    main()
