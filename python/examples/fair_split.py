"""Split the chores between two people so that both take on the same total
effort. Each chore goes to one of them, and the two #sum aggregates name
their totals: pgass.sum(...) == total binds total to the sum rather than
comparing it against a number already known. Naming both totals is what lets
the last constraint compare them against each other, which no single
aggregate can do. See ../../examples/fair-split.lp."""

import pgass


class Chore(pgass.Predicate):
    name: str
    effort: int


class DoesAna(pgass.Predicate):
    name: str


class DoesBo(pgass.Predicate):
    name: str


class EffortAna(pgass.Predicate):
    total: int


class EffortBo(pgass.Predicate):
    total: int


def main():
    C, E, A, B = pgass.vars("C E A B")
    program = pgass.Program()
    program.add(Chore("dishes", 3), Chore("laundry", 2), Chore("vacuum", 4),
                Chore("shopping", 5))

    program.add(pgass.choice(DoesAna(C)) << Chore(C, E))
    program.add(DoesBo(C) << (Chore(C, E) & ~DoesAna(C)))

    program.add(EffortAna(A) <<
                (pgass.sum(DoesAna(C) & Chore(C, E), terms=(E, C)) == A))
    program.add(EffortBo(B) <<
                (pgass.sum(DoesBo(C) & Chore(C, E), terms=(E, C)) == B))

    program.forbid(EffortAna(A) & EffortBo(B) & pgass.ne(A, B))

    for answer_set in program.solve(models=0):
        ana = sorted(d.name for d in answer_set[DoesAna])
        bo = sorted(d.name for d in answer_set[DoesBo])
        print(f"ana={ana} bo={bo}")


if __name__ == "__main__":
    main()
