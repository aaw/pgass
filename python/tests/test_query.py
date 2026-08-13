import pgass


class Edge(pgass.Predicate):
    x: int
    y: int


class Reachable(pgass.Predicate):
    x: int
    y: int


def reachability_program() -> pgass.Program:
    X, Y, Z = pgass.vars("X Y Z")
    program = pgass.Program()
    program.add(Edge(1, 2), Edge(2, 3))
    program.add(Reachable(X, Y) << Edge(X, Y))
    program.add(Reachable(X, Z) << (Reachable(X, Y) & Edge(Y, Z)))
    return program


def test_query_true_when_every_answer_set_holds_it():
    program = reachability_program()
    result = program.query(Reachable(1, 3))
    assert result.answer is True


def test_query_false_when_no_atom_matches():
    program = reachability_program()
    result = program.query(Reachable(3, 1))
    assert result.answer is False


def test_query_on_unsatisfiable_program_is_true():
    program = pgass.Program()
    program.add(Edge(1, 2))
    program.forbid(Edge(1, 2))
    result = program.query(Edge(1, 2))
    assert result.answer is True
    assert result.no_answer_set is True
