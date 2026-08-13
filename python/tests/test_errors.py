import pytest

import pgass


class Node(pgass.Predicate):
    x: int


def test_undeclared_predicate_error_on_add():
    with pytest.raises(pgass.UndeclaredPredicateError):
        pgass.Program().add("node(1)")


def test_unbound_variable_error_on_add():
    X, Y = pgass.vars("X Y")
    with pytest.raises(pgass.UnboundVariableError):
        pgass.Program().add(Node(Y) << Node(X))


def test_pgass_error_is_the_common_base():
    assert issubclass(pgass.ParseError, pgass.PgassError)
    assert issubclass(pgass.SafetyError, pgass.PgassError)
    assert issubclass(pgass.GroundingError, pgass.PgassError)
    assert issubclass(pgass.GroundingResourceExhausted, pgass.GroundingError)
    assert issubclass(pgass.SolveError, pgass.PgassError)
    assert issubclass(pgass.UndeclaredPredicateError, pgass.PgassError)
    assert issubclass(pgass.UnboundVariableError, pgass.PgassError)
