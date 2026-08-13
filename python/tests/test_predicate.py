import pytest

import pgass


def test_pascal_case_class_compiles_to_a_lowercase_predicate_name():
    class Cover(pgass.Predicate):
        x: int

    assert Cover.__pgass_name__ == "cover"


def test_zero_arity_predicate():
    class Done(pgass.Predicate):
        pass

    assert Done.__fields__ == ()
    assert str(Done()) == "done"


def test_equality_and_hash_are_by_value():
    class Node(pgass.Predicate):
        x: int

    assert Node(1) == Node(1)
    assert Node(1) != Node(2)
    assert hash(Node(1)) == hash(Node(1))
    assert Node(1) != "node(1)"


def test_instances_are_immutable():
    class Node(pgass.Predicate):
        x: int

    node = Node(1)
    with pytest.raises(AttributeError):
        node.x = 2


def test_field_type_must_be_int_or_str():
    with pytest.raises(TypeError):

        class Bad(pgass.Predicate):
            x: float


def test_wrong_arity_raises():
    class Node(pgass.Predicate):
        x: int

    with pytest.raises(TypeError):
        Node(1, 2)


def test_variables_reports_var_fields_in_order():
    class Edge(pgass.Predicate):
        x: int
        y: int

    X, Y = pgass.vars("X Y")
    assert Edge(X, Y).variables() == (X, Y)
    assert Edge(1, Y).variables() == (Y,)
    assert Edge(1, 2).variables() == ()


def test_str_matches_asp_core_2_syntax():
    class Edge(pgass.Predicate):
        x: int
        y: int

    assert str(Edge(1, 2)) == "edge(1, 2)"


def test_var_requires_uppercase_start():
    with pytest.raises(ValueError):
        pgass.Var("x")
