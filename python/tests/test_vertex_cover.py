"""The README's vertex-cover example, built with the DSL instead of text, as
an end-to-end check that facts, choice heads, constraints and weak
constraints all compile and solve correctly together."""

import pgass


class Node(pgass.Predicate):
    x: int


class Edge(pgass.Predicate):
    x: int
    y: int


class Cover(pgass.Predicate):
    x: int


def build_program() -> pgass.Program:
    X, Y = pgass.vars("X Y")
    program = pgass.Program()
    program.add(Node(x) for x in range(1, 6))
    program.add(Edge(x, y) for x, y in
                [(1, 2), (2, 3), (3, 4), (4, 5), (5, 1), (1, 3)])
    program.add(pgass.choice(Cover(X)) << Node(X))
    program.forbid(Edge(X, Y) & ~Cover(X) & ~Cover(Y))
    program.minimize(Cover(X), weight=1, priority=0)
    return program


def test_minimum_vertex_cover_matches_the_cli():
    program = build_program()
    covers = {
        frozenset(c.x for c in answer_set[Cover])
        for answer_set in program.solve(models=0)
    }
    assert covers == {
        frozenset({1, 3, 4}),
        frozenset({1, 3, 5}),
        frozenset({2, 3, 5}),
        frozenset({1, 2, 4}),
    }


def test_every_answer_set_costs_the_minimum():
    program = build_program()
    for answer_set in program.solve(models=0):
        assert answer_set.cost == [3]


def test_result_atoms_are_typed_instances_not_strings():
    program = build_program()
    answer_set = next(program.solve())
    for cover in answer_set[Cover]:
        assert isinstance(cover, Cover)
        assert isinstance(cover.x, int)
