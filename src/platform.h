#ifndef PGASS_PLATFORM_H_
#define PGASS_PLATFORM_H_

// Gives the thread that calls this every byte of stack the process is allowed,
// by raising the soft stack limit to the hard one. Call it once, before any
// solving.
//
// cvc5 recurses once per bound variable of a quantifier, so the stack a check
// needs grows with the size of the program. A program of 8 000 atoms on a
// positive cycle already exhausts the 8 MB a main thread starts with. The cvc5
// command line binary does this same raise at startup, which is why it decides
// programs that an application linking the library cannot.
void raise_stack_limit();

#endif  // PGASS_PLATFORM_H_
