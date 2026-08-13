"""Compiles a Program's accumulated facts/rules/constraints into the
ASP-Core-2 text pgass's native parser reads. This is the one place program
text is generated. Nothing upstream of it ever sees or writes ASP syntax,
and nothing downstream of it (the native extension) knows this text didn't
come from a human.
"""

from __future__ import annotations

import re

from . import expr
from .errors import UnboundVariableError
from .predicate import Predicate, Var

_ASP_OP = {">=": ">=", ">": ">", "<=": "<=", "<": "<", "=": "=", "!=": "!="}
_AGGREGATE_FUNC = {"count": "#count", "sum": "#sum", "min": "#min", "max": "#max"}

# ASP-Core-2 has two kinds of string-shaped term: a bare symbolic constant
# (ana, dishes) and a quoted string ("ana"). A Python str that already looks
# like a constant compiles to the bare form. Anything else, like a space or
# an uppercase start, compiles to a quoted string.
_CONSTANT_RE = re.compile(r"^[a-z][a-zA-Z0-9_]*$")


def format_term(value) -> str:
    if isinstance(value, Var):
        return value.name
    if isinstance(value, bool):
        raise TypeError("bool is not a valid ASP term. Use an int.")
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        if _CONSTANT_RE.match(value):
            return value
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'
    raise TypeError(f"{value!r} is not a valid ASP term (expected int, str or Var)")


def format_literal(literal: Predicate) -> str:
    if not literal.__fields__:
        return literal.__pgass_name__
    args = ", ".join(format_term(getattr(literal, f)) for f in literal.__fields__)
    return f"{literal.__pgass_name__}({args})"


def _flatten_body(body) -> tuple:
    if isinstance(body, expr.Conjunction):
        return body.items
    return (body,)


def _format_body_item(item) -> str:
    if isinstance(item, Predicate):
        return format_literal(item)
    if isinstance(item, expr.Negation):
        return f"not {format_literal(item.literal)}"
    if isinstance(item, expr.Aggregate):
        return format_aggregate(item)
    if isinstance(item, expr.Comparison):
        return (f"{format_term(item.left)} {_ASP_OP[item.op]} "
                f"{format_term(item.right)}")
    raise TypeError(f"{item!r} cannot appear in a rule body")


def format_body(body) -> str:
    return ", ".join(_format_body_item(item) for item in _flatten_body(body))


def body_variables(body) -> tuple[Var, ...]:
    """Every Var a positive literal in `body` mentions, in first-seen order
    and deduplicated. The default set of terms distinguishing one aggregate
    or weak constraint element from another."""
    seen: dict[str, Var] = {}
    for item in _flatten_body(body):
        if isinstance(item, Predicate):
            for v in item.variables():
                seen.setdefault(v.name, v)
    return tuple(seen.values())


def _aggregate_element(body, terms: tuple[Var, ...] | None) -> str:
    if terms is None:
        terms = body_variables(body)
    term_text = ", ".join(v.name for v in terms)
    condition = format_body(body)
    return f"{term_text} : {condition}" if term_text else condition


def format_aggregate(agg: expr.Aggregate) -> str:
    text = f"{_AGGREGATE_FUNC[agg.func]}{{ {_aggregate_element(agg.body, agg.terms)} }}"
    if agg.bound is not None:
        op, value = agg.bound
        text += f" {_ASP_OP[op]} {format_term(value)}"
    return text


def _head_literals(head) -> tuple[Predicate, ...]:
    if isinstance(head, expr.Disjunction):
        return head.literals
    if isinstance(head, expr.Choice):
        return (head.literal,)
    return (head,)


def format_head(head) -> str:
    if isinstance(head, expr.Disjunction):
        return " | ".join(format_literal(lit) for lit in head.literals)
    if isinstance(head, expr.Choice):
        text = ""
        if head.lb is not None:
            text += f"{head.lb} <= "
        text += f"{{ {format_literal(head.literal)} }}"
        if head.ub is not None:
            text += f" <= {head.ub}"
        return text
    return format_literal(head)


def _body_bound_variables(body) -> set[str]:
    """Every variable a positive literal in `body` binds. A negated literal
    only tests a variable already bound, it does not bind one. Neither does
    a comparison aggregate ('#count{...} >= 2'). An assignment aggregate
    ('#sum{...} = A') does bind one: A is exactly the aggregate's value, as
    safe as if a literal had derived it."""
    bound: set[str] = set()
    for item in _flatten_body(body):
        if isinstance(item, Predicate):
            bound.update(v.name for v in item.variables())
        elif isinstance(item, expr.Aggregate) and item.bound is not None:
            op, value = item.bound
            if op == "=" and isinstance(value, Var):
                bound.add(value.name)
    return bound


def check_rule_safety(rule: expr.Rule) -> None:
    """Every variable the head, a negated body literal, or an aggregate bound
    uses must be bound by a positive body literal. ground() would run forever
    or reject the rule outright without this, so it is checked here, before
    any text is generated, so the error points at the DSL call that caused it.
    """
    bound = _body_bound_variables(rule.body)

    for literal in _head_literals(rule.head):
        for v in literal.variables():
            if v.name not in bound:
                raise UnboundVariableError(
                    f"{v.name} in the head of a rule is never bound by a "
                    "positive literal in its body")

    for item in _flatten_body(rule.body):
        if isinstance(item, expr.Negation):
            for v in item.literal.variables():
                if v.name not in bound:
                    raise UnboundVariableError(
                        f"{v.name} in 'not {item.literal}' is never bound by "
                        "a positive literal in the same body")
        elif isinstance(item, expr.Aggregate):
            if item.terms is not None:
                local = {v.name for v in body_variables(item.body)}
                for v in item.terms:
                    if v.name not in local:
                        raise UnboundVariableError(
                            f"{v.name} in {item!r}'s terms is never bound by "
                            "a positive literal in the aggregate's own body")
            if item.bound is not None:
                op, value = item.bound
                # '=' assigns the aggregate's value to `value` rather than
                # comparing against something already known, so there is
                # nothing to check: see _body_bound_variables above.
                if op != "=" and isinstance(value, Var) and value.name not in bound:
                    raise UnboundVariableError(
                        f"{value.name}, bounding an aggregate, is never bound "
                        "by a positive literal in the same body")
        elif isinstance(item, expr.Comparison):
            for value in (item.left, item.right):
                if isinstance(value, Var) and value.name not in bound:
                    raise UnboundVariableError(
                        f"{value.name} in '{item!r}' is never bound by a "
                        "positive literal in the same body")


def format_minimize(elements: list[tuple[object, int, int, tuple[Var, ...]]]) -> str:
    parts = []
    for body, weight, priority, terms in elements:
        term_text = ", ".join(v.name for v in terms)
        condition = format_body(body)
        prefix = f"{weight}@{priority}"
        parts.append(f"{prefix}, {term_text} : {condition}" if term_text
                     else f"{prefix} : {condition}")
    return "#minimize{ " + "; ".join(parts) + " }."


def format_rule(rule: expr.Rule) -> str:
    head_text = format_head(rule.head)
    # A headless-body disjunction, 'a | b.', is a disjunctive fact: one of its
    # literals holds, chosen freely, with nothing else required.
    if rule.body is None:
        return f"{head_text}."
    return f"{head_text} :- {format_body(rule.body)}."


def to_text(program) -> str:
    lines = [f"{format_literal(fact)}." for fact in program._facts]
    lines += [format_rule(rule) for rule in program._rules]
    lines += [f":- {format_body(body)}." for body in program._constraints]
    if program._minimize_elements:
        lines.append(format_minimize(program._minimize_elements))
    if program._query is not None:
        lines.append(f"{format_literal(program._query)}?")
    return "\n".join(lines)
