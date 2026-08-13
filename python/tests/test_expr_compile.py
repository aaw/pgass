import pytest

import pgass
from pgass import compile as _compile


class Node(pgass.Predicate):
    x: int


class Edge(pgass.Predicate):
    x: int
    y: int


class Cover(pgass.Predicate):
    x: int


def compiled(build) -> str:
    program = pgass.Program()
    build(program)
    return _compile.to_text(program)


def test_fact():
    assert compiled(lambda p: p.add(Node(1))) == "node(1)."


def test_zero_arity_fact():
    class Done(pgass.Predicate):
        pass

    assert compiled(lambda p: p.add(Done())) == "done."


def test_string_field_is_quoted_and_escaped():
    class Label(pgass.Predicate):
        text: str

    text = compiled(lambda p: p.add(Label('he said "hi"')))
    assert text == r'label("he said \"hi\"").'


def test_choice_rule():
    X = pgass.Var("X")
    text = compiled(lambda p: p.add(pgass.choice(Cover(X)) << Node(X)))
    assert text == "{ cover(X) } :- node(X)."


def test_bounded_choice_rule():
    X = pgass.Var("X")
    text = compiled(
        lambda p: p.add(pgass.choice(Cover(X), lb=1, ub=3) << Node(X)))
    assert text == "1 <= { cover(X) } <= 3 :- node(X)."


def test_disjunctive_rule():
    class A(pgass.Predicate):
        x: int

    class B(pgass.Predicate):
        x: int

    X = pgass.Var("X")
    text = compiled(lambda p: p.add((A(X) | B(X)) << Node(X)))
    assert text == "a(X) | b(X) :- node(X)."


def test_negation_in_body():
    X, Y = pgass.vars("X Y")
    text = compiled(lambda p: p.forbid(Edge(X, Y) & ~Cover(X) & ~Cover(Y)))
    assert text == ":- edge(X, Y), not cover(X), not cover(Y)."


def test_minimize():
    X = pgass.Var("X")
    text = compiled(lambda p: p.minimize(Cover(X), weight=1, priority=0))
    assert text == "#minimize{ 1@0, X : cover(X) }."


def test_aggregate_with_bound():
    X = pgass.Var("X")
    text = compiled(
        lambda p: p.forbid((pgass.count(Cover(X)) >= 1) & Node(X)))
    assert text == ":- #count{ X : cover(X) } >= 1, node(X)."


def test_unbound_head_variable_raises_before_solving():
    X, Y = pgass.vars("X Y")
    with pytest.raises(pgass.UnboundVariableError):
        pgass.Program().add(Cover(Y) << Node(X))


def test_unbound_negated_variable_raises():
    X, Y = pgass.vars("X Y")
    with pytest.raises(pgass.UnboundVariableError):
        pgass.Program().add(pgass.choice(Cover(X)) << (Node(X) & ~Edge(X, Y)))
