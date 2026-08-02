#ifndef EXIT_CODE_H_
#define EXIT_CODE_H_

// The exit codes an answer set solver is expected to return, from "Answer Set
// Solver Output" version 1.1 by Thomas Krennwallner:
//
//   https://www.mat.unical.it/aspcomp2013/files/aspoutput.txt
//
// A code is a set of bits:
//
//   bit 7   bit 6   bit 5    bit 4     bit 3   bit 2     bit 1   bit 0
//   NORUN   RES     ALLOPT   EXHAUST   SAT     EXHAUST   SAT     INT
enum ExitCode {
  // A run that ground the program.
  kGrounded = 0,

  // Terminated by a signal or out of some resource, with nothing to show.
  kInterrupted = 1,

  // The program has an answer set. Also how a true query answers.
  kSatisfiable = 10,

  // Terminated as above, but an answer set had been found by then.
  kInterruptedSat = 11,

  // The program has no answer set. Also how a false query answers.
  kUnsatisfiable = 20,

  // The program has an answer set and the search covered the whole space.
  // Under weak constraints it means an optimum is proven.
  kExhausted = 30,

  // Terminated as above, but an optimum had been proven by then.
  kInterruptedOptimum = 31,

  // Every optimal answer set of a program with weak constraints was found.
  kAllOptima = 62,

  // The run never started. A syntax error, an unsupported program, or a bad
  // command line all land here. This bit excludes every other one.
  kNoRun = 128,
};

#endif  // EXIT_CODE_H_
