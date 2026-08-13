"""Pick the one meeting slot that annoys the fewest people, where some kinds
of annoyance matter more than others: two weak-constraint elements at
different priorities, added via two program.minimize() calls. See
../../examples/meeting-time.lp."""

import pgass


class Slot(pgass.Predicate):
    name: str


class Person(pgass.Predicate):
    name: str


class Cannot(pgass.Predicate):
    person: str
    slot: str


class Dislikes(pgass.Predicate):
    person: str
    slot: str


class Meet(pgass.Predicate):
    slot: str


def main():
    S, P = pgass.vars("S P")
    program = pgass.Program()
    program.add(Slot("morning"), Slot("noon"), Slot("evening"))
    program.add(Person("ana"), Person("bo"), Person("cy"))
    program.add(Cannot("ana", "morning"), Cannot("bo", "evening"),
                Cannot("cy", "evening"))
    program.add(Dislikes("ana", "noon"), Dislikes("bo", "morning"))

    program.add(pgass.choice(Meet(S)) << Slot(S))
    program.forbid(pgass.count(Meet(S)) != 1)

    program.minimize(Meet(S) & Cannot(P, S), weight=1, priority=1, terms=(P,))
    program.minimize(Meet(S) & Dislikes(P, S), weight=1, priority=0, terms=(P,))

    answer_set = next(program.solve())
    print(answer_set[Meet][0], "cost", answer_set.cost)


if __name__ == "__main__":
    main()
