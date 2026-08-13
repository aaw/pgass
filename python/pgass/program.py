"""pgass.Program: accumulates facts, rules, constraints and weak constraints
built from Predicate instances and the expr.py operators, then compiles them
to ASP-Core-2 text (compile.py) and hands that to the native extension.
"""

from __future__ import annotations

import typing

from . import _native
from . import compile as _compile
from . import expr
from .errors import UndeclaredPredicateError
from .predicate import Predicate, Var
from .result import AnswerSet


class QueryResult(typing.NamedTuple):
    answer: bool
    holds: list[str]
    no_answer_set: bool


class Program:
    def __init__(self):
        self._facts: list[Predicate] = []
        self._rules: list[expr.Rule] = []
        self._constraints: list = []
        self._minimize_elements: list[tuple[object, int, int, tuple[Var, ...]]] = []
        self._query: Predicate | None = None

    def add(self, *items) -> None:
        """Adds facts (Predicate instances), rules (built with <<), and
        disjunctive facts (a() | b(), with no <<). Also accepts an iterable
        of any of these, so a generator expression works directly:
        program.add(Node(x) for x in range(1, 6))."""
        for item in items:
            if isinstance(item, Predicate):
                self._facts.append(item)
            elif isinstance(item, expr.Rule):
                _compile.check_rule_safety(item)
                self._rules.append(item)
            elif isinstance(item, expr.Disjunction):
                self.add(expr.Rule(item))
            elif hasattr(item, "__iter__") and not isinstance(item, (str, bytes)):
                self.add(*item)
            else:
                raise UndeclaredPredicateError(
                    f"{item!r} is not a pgass.Predicate instance or a rule "
                    "built with <<. Did you forget to subclass Predicate?")

    def forbid(self, body) -> None:
        """An integrity constraint: no answer set may satisfy `body`."""
        self._constraints.append(body)

    def minimize(self, body, *, weight: int = 1, priority: int = 0,
                 terms: tuple[Var, ...] | None = None) -> None:
        """A weak-constraint element: every match of `body` (a literal, or a
        literal & literal & ... conjunction) costs `weight` at level
        `priority` (lower levels are more important). `terms` distinguishes
        one match from another and defaults to every variable `body`'s
        positive literals mention. Give it explicitly when only some of
        those variables should count, e.g. when a match is already unique
        without all of them."""
        if terms is None:
            terms = _compile.body_variables(body)
        self._minimize_elements.append((body, weight, priority, terms))

    def __str__(self) -> str:
        """The ASP-Core-2 text this program compiles to. It's the same text
        .solve() and .query() hand the native parser, so what you see here is
        exactly what gets solved."""
        return _compile.to_text(self)

    __repr__ = __str__

    def query(self, literal: Predicate) -> QueryResult:
        """Whether `literal` holds in every answer set."""
        self._query = literal
        try:
            text = _compile.to_text(self)
            native_program = _native.NativeProgram.compile(text, 0)
            answer, holds, no_answer_set = native_program.query()
        finally:
            self._query = None
        return QueryResult(answer, holds, no_answer_set)

    def solve(self, models: int = 1, max_ground_atoms: int = 0
              ) -> typing.Iterator[AnswerSet]:
        """The program's answer sets, one at a time. models=0 asks for all of
        them. Iterating lazily means stopping early, with a break or a
        single next(), never pays to find the rest."""
        text = _compile.to_text(self)
        native_program = _native.NativeProgram.compile(text, max_ground_atoms)
        iterator = native_program.iterator()
        found = 0
        while models == 0 or found < models:
            result = iterator.next()
            if result is None:
                return
            atoms, costs = result
            yield AnswerSet(atoms, costs)
            found += 1
