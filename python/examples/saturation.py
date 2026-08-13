"""Is there an assignment to x1 and x2 that satisfies (x1 or Y) and
(x2 or not Y) for *every* Y? See ../../examples/saturation.lp for the
saturation technique this uses: x1() | nx1() is a disjunctive fact, added
directly with no <<, since program.add() also accepts a bare Predicate | ...
disjunction."""

import pgass


class X1(pgass.Predicate):
    pass


class Nx1(pgass.Predicate):
    pass


class X2(pgass.Predicate):
    pass


class Nx2(pgass.Predicate):
    pass


class Y(pgass.Predicate):
    pass


class Ny(pgass.Predicate):
    pass


class C1(pgass.Predicate):
    pass


class C2(pgass.Predicate):
    pass


class Sat(pgass.Predicate):
    pass


def main():
    program = pgass.Program()

    # Guess an assignment to the variables the question is asking about.
    program.add(X1() | Nx1())
    program.add(X2() | Nx2())

    # Guess a Y to test that assignment against.
    program.add(Y() | Ny())

    # The two clauses, under the guesses above.
    program.add(C1() << X1())
    program.add(C1() << Y())
    program.add(C2() << X2())
    program.add(C2() << Ny())

    # The formula holds when both clauses do.
    program.add(Sat() << (C1() & C2()))

    # Saturate: derive everything the Y guess produced, so a satisfying Y is
    # indistinguishable from any other and only a refuting Y stays small.
    program.add(Y() << Sat())
    program.add(Ny() << Sat())
    program.add(C1() << Sat())
    program.add(C2() << Sat())

    # Keep only the assignments no Y refutes.
    program.forbid(~Sat())

    for answer_set in program.solve(models=0):
        print(f"x1={bool(answer_set[X1])} x2={bool(answer_set[X2])}")


if __name__ == "__main__":
    main()
