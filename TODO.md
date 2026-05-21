# TODO

## Better parse/tokenization error messages

Currently errors report a position but don't show the source context. Improve
to show the offending line with a caret pointing at the failure site:

```
p(X, Y) :- q(X, Y
                 ^--- unexpected end of input
```

## Better safety error messages

Currently `verify_safe` returns a bare bool. Improve to report which variables
are unbound and in which rule, e.g.:

```
p(X, Y) :- not q(X).
         ^--- variable Y is unsafe (not bound by any positive literal)
```
