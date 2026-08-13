"""Predicates: typed classes whose instances are ASP facts and rule literals."""

from __future__ import annotations

import inspect

_FIELD_TYPES = (int, str)


class Var:
    """An ASP variable, e.g. the X in cover(X). Build one with pgass.vars()."""

    __slots__ = ("name",)

    def __init__(self, name: str):
        if not name or not (name[0].isupper() or name[0] == "_"):
            raise ValueError(
                f"'{name}' is not a valid ASP variable name. Variables start "
                "with an uppercase letter.")
        self.name = name

    def __repr__(self) -> str:
        return f"Var({self.name!r})"

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Var) and self.name == other.name

    def __hash__(self) -> int:
        return hash(("pgass.Var", self.name))


def vars(names: str) -> tuple[Var, ...]:
    """vars("X Y Z") -> three Var objects, one per whitespace-separated name."""
    return tuple(Var(name) for name in names.split())


class Predicate:
    """Base class for a predicate. Subclass it with typed fields:

        class cover(Predicate):
            x: int

    An instance with concrete field values, cover(1), is a fact. An instance
    holding a Var in a field, cover(X), is a pattern used in a rule. Both are
    the same class. The difference is only what a field holds.
    """

    __fields__: tuple[str, ...] = ()
    __field_types__: tuple[type, ...] = ()
    __pgass_name__: str = ""

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        # cls.__dict__["__annotations__"] misses this class's own fields
        # under Python 3.14's deferred annotations. get_annotations handles
        # that and resolves 'from __future__ import annotations' strings.
        if hasattr(inspect, "get_annotations"):
            own_annotations = inspect.get_annotations(cls, eval_str=True)
        else:  # Python < 3.10
            own_annotations = cls.__dict__.get("__annotations__", {})
        fields = tuple(own_annotations)
        field_types = tuple(own_annotations[name] for name in fields)
        for name, field_type in zip(fields, field_types):
            if field_type not in _FIELD_TYPES:
                raise TypeError(
                    f"{cls.__name__}.{name}: predicate fields must be int or "
                    f"str, not {field_type!r}")
        cls.__fields__ = fields
        cls.__field_types__ = field_types
        # ASP-Core-2 reserves a leading uppercase letter for variables, so a
        # PascalCase class name like Cover would otherwise compile into
        # something the parser reads as a variable, not a predicate.
        cls.__pgass_name__ = cls.__name__[0].lower() + cls.__name__[1:]

    def __init__(self, *args):
        if len(args) != len(self.__fields__):
            raise TypeError(
                f"{type(self).__name__} takes {len(self.__fields__)} "
                f"argument(s), got {len(args)}")
        for name, value in zip(self.__fields__, args):
            if not isinstance(value, (Var, int, str)):
                raise TypeError(
                    f"{type(self).__name__}.{name} takes an int, str or "
                    f"pgass.Var, not {value!r}")
            object.__setattr__(self, name, value)

    def __setattr__(self, name: str, value) -> None:
        raise AttributeError(f"{type(self).__name__} is immutable")

    def __eq__(self, other: object) -> bool:
        if type(self) is not type(other):
            return NotImplemented
        return all(getattr(self, f) == getattr(other, f) for f in self.__fields__)

    def __hash__(self) -> int:
        return hash((type(self),) + tuple(getattr(self, f) for f in self.__fields__))

    def __repr__(self) -> str:
        args = ", ".join(f"{f}={getattr(self, f)!r}" for f in self.__fields__)
        return f"{type(self).__name__}({args})"

    def __str__(self) -> str:
        if not self.__fields__:
            return self.__pgass_name__
        args = ", ".join(str(getattr(self, f)) for f in self.__fields__)
        return f"{self.__pgass_name__}({args})"

    def variables(self) -> tuple[Var, ...]:
        """The Var instances this literal's fields hold, in field order."""
        return tuple(v for f in self.__fields__ if isinstance(v := getattr(self, f), Var))

    def __invert__(self):
        from . import expr
        return expr.Negation(self)

    def __and__(self, other):
        from . import expr
        return expr.Conjunction((self,)) & other

    def __or__(self, other):
        from . import expr
        return expr.Disjunction((self,)) | other

    def __lshift__(self, body):
        from . import expr
        return expr.Rule(self, body)
