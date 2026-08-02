* Give `--encode=smtlib` something to say about a program it now refuses: weak
  constraints, whose optimum takes repeated queries, head cycles, whose models
  are only candidates, and a query of several instances, which is several
  check-sats.
* Consider adding `--encode=sat`, which dumps the encoding as DIMACS.
* Bind a variable that appears only inside an arithmetic term of a body atom,
  as in `move(C,N,T) :- atrobot(C,T-1)`, by inverting the term. Grounding
  refuses it today, saying T is not bound by the rule body.