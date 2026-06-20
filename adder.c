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

/* `adder fmt` — the Adder source formatter, compiled as its own TU (adfmt.c,
   built with -DADFMT_NO_MAIN so it exports no main) and linked in. Dispatched
   before the interpreter starts, so it shares the adder binary without touching
   the runtime path. */
int adfmt_cli_main(int argc, char **argv);

int main(int argc, char *argv[]) {
  if (argc > 1 && !strcmp(argv[1], "fmt"))
    return adfmt_cli_main(argc - 1, argv + 1); /* shift so files start at argv[1] */
  return alcove_real_main(argc, argv);
}
