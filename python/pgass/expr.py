"""The internal IR rule-building operators assemble.

Nothing here is meant to be constructed directly by a user. A Program builds
these by combining Predicate instances with &, ~, |, <<, and the choice()/
count()/sum()/min()/max() functions below. compile.py is the only thing that
reads them.
"""

from __future__ import annotations

import typing

from .predicate import Predicate, Var


class Negation:
    """not literal, in a rule body."""

    def __init__(self, literal: Predicate):
        self.literal = literal

    def __and__(self, other):
        return Conjunction((self,)) & other

    def __repr__(self) -> str:
        return f"~{self.literal!r}"


class Comparison:
    """A builtin comparison in a rule body, e.g. pgass.ne(a, b) for 'a != b'.
    Var keeps its own == and != for value equality, so comparisons are built
    with eq()/ne()/etc. instead of overloading those operators on Var."""

    def __init__(self, left, op: str, right):
        self.left = left
        self.op = op
        self.right = right

    def __and__(self, other):
        return Conjunction((self,)) & other

    def __repr__(self) -> str:
        return f"{self.left!r} {self.op} {self.right!r}"


def eq(left, right) -> Comparison:
    return Comparison(left, "=", right)


def ne(left, right) -> Comparison:
    return Comparison(left, "!=", right)


def lt(left, right) -> Comparison:
    return Comparison(left, "<", right)


def le(left, right) -> Comparison:
    return Comparison(left, "<=", right)


def gt(left, right) -> Comparison:
    return Comparison(left, ">", right)


def ge(left, right) -> Comparison:
    return Comparison(left, ">=", right)


BodyItem = typing.Union[Predicate, Negation, "Aggregate", Comparison]


class Conjunction:
    """A rule body, item & item & ...."""

    def __init__(self, items: tuple[BodyItem, ...]):
        self.items = items

    def __and__(self, other):
        more = other.items if isinstance(other, Conjunction) else (other,)
        return Conjunction(self.items + more)

    def __repr__(self) -> str:
        return " & ".join(repr(item) for item in self.items)


class Disjunction:
    """A disjunctive head, a(X) | b(X)."""

    def __init__(self, literals: tuple[Predicate, ...]):
        self.literals = literals

    def __or__(self, other):
        more = other.literals if isinstance(other, Disjunction) else (other,)
        return Disjunction(self.literals + more)

    def __lshift__(self, body):
        return Rule(self, body)

    def __repr__(self) -> str:
        return " | ".join(repr(lit) for lit in self.literals)


class Choice:
    """A choice head, { a(X) }, or lb { a(X) } ub with either bound omitted."""

    def __init__(self, literal: Predicate, lb: int | None = None,
                 ub: int | None = None):
        self.literal = literal
        self.lb = lb
        self.ub = ub

    def __lshift__(self, body):
        return Rule(self, body)

    def __repr__(self) -> str:
        return f"choice({self.literal!r}, lb={self.lb!r}, ub={self.ub!r})"


def choice(literal: Predicate, lb: int | None = None, ub: int | None = None) -> Choice:
    """{ literal } :- ..., or lb { literal } ub with either bound omitted."""
    return Choice(literal, lb, ub)


class Aggregate:
    """#count/#sum/#min/#max over a body's matches, e.g.

        pgass.count(cover(X)) >= 1
        pgass.sum(pick(I) & item(I, W), terms=(W, I)) > 8

    `body` is the element's condition: a literal, or a literal & literal &
    ... conjunction. `terms` are the values the element is read by. For
    #sum/#min/#max, the first term is the one summed, maxed or mined, so
    give it explicitly whenever `body` has more than one variable. Defaults
    to every variable `body` mentions, which only picks the right term when
    there is just one. `bound` holds (operator, value) once a comparison has
    set it.
    """

    def __init__(self, func: str, body, terms: tuple[Var, ...] | None = None,
                 bound: tuple[str, object] | None = None):
        self.func = func
        self.body = body
        self.terms = terms
        self.bound = bound

    def _bounded(self, op: str, value) -> "Aggregate":
        if not isinstance(value, (int, Var)):
            return NotImplemented
        return Aggregate(self.func, self.body, self.terms, bound=(op, value))

    # 'pgass.count(x) >= 1' and its reflection '1 <= pgass.count(x)' both
    # arrive here (Python retries int.__le__(1, agg)'s NotImplemented as
    # agg.__ge__(1)), and both mean the same thing: the count is at least 1.
    def __ge__(self, value):
        return self._bounded(">=", value)

    def __gt__(self, value):
        return self._bounded(">", value)

    def __le__(self, value):
        return self._bounded("<=", value)

    def __lt__(self, value):
        return self._bounded("<", value)

    # 'pgass.sum(x) == total' assigns the aggregate's value to `total`. It
    # does not compare against something already known. compile.py's
    # check_rule_safety treats this case specially.
    def __eq__(self, value):
        return self._bounded("=", value)

    def __ne__(self, value):
        return self._bounded("!=", value)

    __hash__ = None  # unhashable: __eq__ builds a bound, not a bool

    def __and__(self, other):
        return Conjunction((self,)) & other

    def __repr__(self) -> str:
        return f"{self.func}({self.body!r})"


def count(body, terms: tuple[Var, ...] | None = None) -> Aggregate:
    return Aggregate("count", body, terms)


def sum(body, terms: tuple[Var, ...] | None = None) -> Aggregate:  # noqa: A001
    return Aggregate("sum", body, terms)


def min(body, terms: tuple[Var, ...] | None = None) -> Aggregate:  # noqa: A001
    return Aggregate("min", body, terms)


def max(body, terms: tuple[Var, ...] | None = None) -> Aggregate:  # noqa: A001
    return Aggregate("max", body, terms)


Head = typing.Union[Predicate, Disjunction, Choice]


class Rule:
    """head << body, i.e. ASP-Core-2's 'head :- body.'. body is None for a
    disjunctive fact added directly, 'a | b.', with no ':-' at all."""

    def __init__(self, head: Head, body=None):
        self.head = head
        self.body = body

    def __repr__(self) -> str:
        return f"{self.head!r} << {self.body!r}"
