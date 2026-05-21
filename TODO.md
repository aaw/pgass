# TODO

## Better safety error messages

Currently `verify_safe` returns a bare bool. Improve to report which variables
are unbound and in which rule, e.g.:

```
p(X, Y) :- not q(X).
         ^--- variable Y is unsafe (not bound by any positive literal)
```
