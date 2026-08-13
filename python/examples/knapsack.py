"""Pick a subset of items whose total weight does not exceed the budget. See
../../examples/knapsack.lp."""

import pgass


class Item(pgass.Predicate):
    name: str
    weight: int


class Pick(pgass.Predicate):
    name: str


def main():
    I, W = pgass.vars("I W")
    program = pgass.Program()
    program.add(Item("a", 5), Item("b", 4), Item("c", 3))
    program.add(pgass.choice(Pick(I)) << Item(I, W))
    program.forbid(pgass.sum(Pick(I) & Item(I, W), terms=(W, I)) > 8)

    for answer_set in program.solve(models=0):
        print(sorted(p.name for p in answer_set[Pick]))


if __name__ == "__main__":
    main()
