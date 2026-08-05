/* match.h — `match(c, ...)` / `ifmatch(c, ...)`: is `c` any of these values?
 *
 *   if (c == ' ' || c == '\t' || c == '\r')  ->  ifmatch (c, ' ', '\t', '\r')
 *
 * A character-class test written as a chain of `==` repeats the subject once
 * per alternative, which is exactly where a typo hides — a repeated
 * alternative, or the wrong subject halfway along a long line. Naming the
 * subject once makes the SET the thing you read.
 *
 * `c` is evaluated ONCE PER ALTERNATIVE, so pass a variable or a plain
 * subscript — never a call, an assignment, or anything with a side effect.
 * (This is a plain macro, not a statement-expression: it has to work in the
 * `for (...)` conditions and one-line predicates it is used in.)
 *
 * `ifmatch` is registered in .clang-format's IfMacros — without that,
 * clang-format reads `else ifmatch (...) {` as a call and re-indents the
 * block after it.
 *
 * Part of the single TU, but ALSO included by the standalone adfmt.c and (via
 * adr.h) adr_test.c — so it stands alone and depends on nothing.
 */
#ifndef ALCOVE_MATCH_H
#define ALCOVE_MATCH_H

/* clang-format off */
#define MATCH_1(c, a)        ((c) == (a))
#define MATCH_2(c, a, ...)   ((c) == (a) || MATCH_1(c, __VA_ARGS__))
#define MATCH_3(c, a, ...)   ((c) == (a) || MATCH_2(c, __VA_ARGS__))
#define MATCH_4(c, a, ...)   ((c) == (a) || MATCH_3(c, __VA_ARGS__))
#define MATCH_5(c, a, ...)   ((c) == (a) || MATCH_4(c, __VA_ARGS__))
#define MATCH_6(c, a, ...)   ((c) == (a) || MATCH_5(c, __VA_ARGS__))
#define MATCH_7(c, a, ...)   ((c) == (a) || MATCH_6(c, __VA_ARGS__))
#define MATCH_8(c, a, ...)   ((c) == (a) || MATCH_7(c, __VA_ARGS__))
#define MATCH_9(c, a, ...)   ((c) == (a) || MATCH_8(c, __VA_ARGS__))
#define MATCH_10(c, a, ...)  ((c) == (a) || MATCH_9(c, __VA_ARGS__))
#define MATCH_11(c, a, ...)  ((c) == (a) || MATCH_10(c, __VA_ARGS__))
#define MATCH_12(c, a, ...)  ((c) == (a) || MATCH_11(c, __VA_ARGS__))
#define MATCH_13(c, a, ...)  ((c) == (a) || MATCH_12(c, __VA_ARGS__))
#define MATCH_14(c, a, ...)  ((c) == (a) || MATCH_13(c, __VA_ARGS__))
#define MATCH_15(c, a, ...)  ((c) == (a) || MATCH_14(c, __VA_ARGS__))
#define MATCH_16(c, a, ...)  ((c) == (a) || MATCH_15(c, __VA_ARGS__))

#define MATCH_PICK(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,    \
                   NAME, ...) NAME
#define match(c, ...)                                                         \
  MATCH_PICK(__VA_ARGS__, MATCH_16, MATCH_15, MATCH_14, MATCH_13, MATCH_12,   \
             MATCH_11, MATCH_10, MATCH_9, MATCH_8, MATCH_7, MATCH_6, MATCH_5, \
             MATCH_4, MATCH_3, MATCH_2, MATCH_1, )(c, __VA_ARGS__)
/* clang-format on */

#define ifmatch(c, ...) if (match(c, __VA_ARGS__))

#endif /* ALCOVE_MATCH_H */
