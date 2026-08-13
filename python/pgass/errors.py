"""pgass's exception hierarchy. Every error pgass raises is a PgassError.

ParseError/SafetyError/GroundingError/GroundingResourceExhausted/SolveError
come straight from the native extension, thrown by the parse/safety/
normalize/ground/solve pipeline in src/. UndeclaredPredicateError and
UnboundVariableError are DSL-side: compile.py raises them before any text
reaches that pipeline, so they point at the Program call that caused them
rather than at generated text the user never wrote.
"""

from __future__ import annotations

from . import _native

PgassError = _native.PgassError
ParseError = _native.ParseError
SafetyError = _native.SafetyError
GroundingError = _native.GroundingError
GroundingResourceExhausted = _native.GroundingResourceExhausted
SolveError = _native.SolveError


class UndeclaredPredicateError(PgassError):
    """Program.add() was given something that isn't a Predicate instance or a
    rule built with <<."""


class UnboundVariableError(PgassError):
    """A rule head, negated literal, or aggregate bound uses a variable no
    positive body literal binds."""
