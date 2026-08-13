"""AnswerSet: turns the atom strings the native extension returns back into
instances of the Predicate classes that produced them. compile.py never emits
a nested functor (every field is an int, a str or a Var), so the term parser
below only has to handle a flat, comma-separated list of numbers, symbolic
constants (ana) and quoted strings ("ana, again").
"""

from __future__ import annotations

import re

_LITERAL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)(?:\((.*)\))?$", re.DOTALL)


def _parse_args(text: str) -> list:
    args: list = []
    i, n = 0, len(text)
    while i < n:
        if text[i] == '"':
            j = i + 1
            chars = []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    chars.append(text[j + 1])
                    j += 2
                else:
                    chars.append(text[j])
                    j += 1
            args.append("".join(chars))
            i = j + 1
        else:
            j = i
            while j < n and text[j] != ",":
                j += 1
            token = text[i:j].strip()
            # A bare token is either a number or a symbolic constant like
            # ana. Anything else would have come back quoted.
            args.append(int(token) if token.lstrip("-").isdigit() else token)
            i = j
        while i < n and text[i] in ", ":
            i += 1
    return args


def parse_literal(text: str) -> tuple[str, tuple]:
    match = _LITERAL_RE.match(text)
    if not match:
        raise ValueError(f"pgass: cannot parse solver output atom {text!r}")
    name, args_text = match.group(1), match.group(2)
    args = () if not args_text else tuple(_parse_args(args_text))
    return name, args


class AnswerSet:
    """One answer set: which facts hold, and what it costs under any weak
    constraints. Index with a Predicate class to get back typed instances:

        answer_set[Cover]  # -> list[Cover]
    """

    def __init__(self, atoms: list[str], costs: list[str]):
        self._atoms = atoms
        self.cost = [int(c) for c in costs]

    def __getitem__(self, predicate_cls) -> list:
        name = predicate_cls.__pgass_name__
        arity = len(predicate_cls.__fields__)
        results = []
        for text in self._atoms:
            atom_name, args = parse_literal(text)
            if atom_name == name and len(args) == arity:
                results.append(predicate_cls(*args))
        return results

    def __contains__(self, literal) -> bool:
        return str(literal) in self._atoms

    def __iter__(self):
        return iter(self._atoms)

    def __len__(self) -> int:
        return len(self._atoms)

    def __repr__(self) -> str:
        return f"AnswerSet({self._atoms!r}, cost={self.cost!r})"
