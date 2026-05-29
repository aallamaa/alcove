/* adder.c — alcove with the Adder front end.
 *
 * This is the whole alcove runtime, unchanged, plus a string->string
 * transpiler (adr.h) wired in at every input chokepoint. You get the
 * Python-like `.adr` surface syntax in the REPL (with the same readline
 * syntax highlighting / completion / history) and for files, piped
 * stdin, and -e.
 *
 * Build:  make als        ->  ./adder
 *
 * Mechanism: we compile alcove.c straight into this translation unit.
 * ALCOVE_ALS switches on the (otherwise inert) #ifdef hooks in
 * alcove.c; `#define main` renames alcove's entry point so this file
 * owns main() and simply delegates. ./alcove itself is built without
 * ALCOVE_ALS and is byte-for-byte the classic s-expression interpreter.
 */
#define ALCOVE_ALS 1

#define main alcove_real_main
#include "alcove.c"
#undef main

int main(int argc, char *argv[]) { return alcove_real_main(argc, argv); }
