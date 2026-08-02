* Give `--encode=smtlib` something to say about a program it now refuses: weak
  constraints, whose optimum takes repeated queries, head cycles, whose models
  are only candidates, and a query of several instances, which is several
  check-sats.
* Consider adding `--encode=sat`, which dumps the encoding as DIMACS.
