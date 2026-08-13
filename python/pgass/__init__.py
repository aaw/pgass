"""pgass: build ASP programs as typed Python objects and solve them with the
pgass C++ solver, without writing any ASP-Core-2 text yourself.

    class Node(pgass.Predicate):
        x: int

    class Edge(pgass.Predicate):
        x: int
        y: int

    class Cover(pgass.Predicate):
        x: int

    X, Y = pgass.vars("X Y")

    program = pgass.Program()
    program.add(Node(x) for x in range(1, 6))
    program.add(Edge(x, y) for x, y in
                [(1, 2), (2, 3), (3, 4), (4, 5), (5, 1), (1, 3)])
    program.add(pgass.choice(Cover(X)) << Node(X))
    program.forbid(Edge(X, Y) & ~Cover(X) & ~Cover(Y))
    program.minimize(Cover(X), weight=1, priority=0)

    for answer_set in program.solve(models=0):
        print(sorted(c.x for c in answer_set[Cover]), answer_set.cost)
"""

from .errors import (
    GroundingError,
    GroundingResourceExhausted,
    ParseError,
    PgassError,
    SafetyError,
    SolveError,
    UnboundVariableError,
    UndeclaredPredicateError,
)
from .expr import choice, count, eq, ge, gt, le, lt, max, min, ne, sum
from .predicate import Predicate, Var, vars
from .program import Program, QueryResult
from .result import AnswerSet

__all__ = [
    "AnswerSet",
    "GroundingError",
    "GroundingResourceExhausted",
    "ParseError",
    "PgassError",
    "Predicate",
    "Program",
    "QueryResult",
    "SafetyError",
    "SolveError",
    "UnboundVariableError",
    "UndeclaredPredicateError",
    "Var",
    "choice",
    "count",
    "eq",
    "ge",
    "gt",
    "le",
    "lt",
    "max",
    "min",
    "ne",
    "sum",
    "vars",
]
