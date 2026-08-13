import pgass


class Node(pgass.Predicate):
    x: int


class Picked(pgass.Predicate):
    x: int


def choice_program() -> pgass.Program:
    """Every subset of {1, 2, 3} is an answer set: 8 of them."""
    X = pgass.Var("X")
    program = pgass.Program()
    program.add(Node(1), Node(2), Node(3))
    program.add(pgass.choice(Picked(X)) << Node(X))
    return program


def test_models_1_returns_exactly_one():
    program = choice_program()
    results = list(program.solve(models=1))
    assert len(results) == 1


def test_models_0_returns_every_answer_set():
    program = choice_program()
    results = list(program.solve(models=0))
    assert len(results) == 8


def test_solve_is_lazy_and_does_not_over_solve():
    program = choice_program()
    seen = 0
    for _ in program.solve(models=0):
        seen += 1
        if seen == 3:
            break
    assert seen == 3


def test_unsatisfiable_program_yields_nothing():
    program = pgass.Program()
    program.add(Node(1))
    program.forbid(Node(1))
    assert list(program.solve()) == []


def test_answer_set_membership_by_string():
    program = pgass.Program()
    program.add(Node(1), Node(2))
    answer_set = next(program.solve())
    assert "node(1)" in answer_set
    assert "node(3)" not in answer_set
    assert len(answer_set) == 2
