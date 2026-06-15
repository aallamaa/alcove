/* numeric.h — exact non-integer numeric types for Alcove.
 *
 * rational : int64 numerator / int64 denominator (den > 0, gcd-reduced)
 * decimal  : (added in a later increment)
 *
 * BOUNDED BY DESIGN. There is no bignum substrate: any operation whose exact
 * result would not fit the int64 components raises an "exact overflow" error
 * rather than silently losing precision or wrapping. This keeps the fixnum JIT
 * fast path untouched (these are heap types; the JIT deopts to the VM on any
 * non-fixnum/non-float operand) while giving users exact fractional math when
 * they explicitly opt in via (rational …) / the n/d literal.
 *
 * Heap layout: ptr -> alc_rat_t, refcounted and freed like EXP_BLOB (a single
 * malloc, no nested owned refs).
 */
#ifndef ALCOVE_NUMERIC_H
#define ALCOVE_NUMERIC_H

/* alc_rat_t is defined in alcove.h (needed by print.h / equality, which run
   before this fragment is #included). */

#define isrational(e) (is_ptr(e) && (e)->type == EXP_RATIONAL)
/* A value that participates in the exact rational tower: fixnum or rational. */
#define is_exact(e) (isnumber(e) || isrational(e))

static inline int64_t alc_gcd64(int64_t a, int64_t b) {
  if (a < 0)
    a = -a;
  if (b < 0)
    b = -b;
  while (b) {
    int64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/* Build a reduced rational from int64 components already known to be in range.
   den must be != 0. Reduces sign into num (den > 0) and divides out the gcd.
   If the result is integral (den == 1) and fits the fixnum tag, returns a
   fixnum so integer-valued rationals collapse to plain ints. Returns an owned
   exp_t, or NULL only if den == 0 (caller raises division-by-zero). */
static exp_t *make_rational(int64_t num, int64_t den) {
  if (den == 0)
    return NULL;
  /* Normalize sign. INT64_MIN cannot be negated; route through the i128 path
     by callers, so here a bare INT64_MIN den is treated as out-of-range. */
  if (den < 0) {
    num = -num;
    den = -den;
  }
  int64_t g = alc_gcd64(num, den);
  if (g > 1) {
    num /= g;
    den /= g;
  }
  if (den == 1 && FIX_FITS(num))
    return MAKE_FIX(num);
  exp_t *e = make_nil();
  e->type = EXP_RATIONAL;
  alc_rat_t *r = (alc_rat_t *)memalloc(1, sizeof(alc_rat_t));
  r->num = num;
  r->den = den;
  e->ptr = r;
  return e;
}

/* Reduce a 128-bit num/den to an owned exp_t. Sets *over = 1 (and returns NULL)
   if either reduced component doesn't fit int64 — the "exact overflow" signal.
   den must be != 0 (callers check for division by zero first). */
static exp_t *rat_from_i128(__int128 num, __int128 den, int *over) {
  *over = 0;
  if (den < 0) {
    num = -num;
    den = -den;
  }
  __int128 a = num < 0 ? -num : num, b = den;
  while (b) {
    __int128 t = a % b;
    a = b;
    b = t;
  }
  if (a > 1) {
    num /= a;
    den /= a;
  }
  if (num < (__int128)INT64_MIN || num > (__int128)INT64_MAX ||
      den > (__int128)INT64_MAX) {
    *over = 1;
    return NULL;
  }
  return make_rational((int64_t)num, (int64_t)den);
}

/* Extract num/den for any exact value (fixnum -> n/1). */
static inline void exact_parts(exp_t *e, int64_t *num, int64_t *den) {
  if (isrational(e)) {
    alc_rat_t *r = (alc_rat_t *)e->ptr;
    *num = r->num;
    *den = r->den;
  } else { /* fixnum */
    *num = FIX_VAL(e);
    *den = 1;
  }
}

static inline double rat_to_double(exp_t *e) {
  alc_rat_t *r = (alc_rat_t *)e->ptr;
  return (double)r->num / (double)r->den;
}

/* op: '+','-','*','/'. a and b are exact (fixnum or rational). Returns an owned
   result (fixnum or rational), or NULL with *err set to a static reason:
   "overflow" or "divzero". Used by the arithmetic fold once an exact non-fixnum
   operand appears (the all-fixnum case stays on the MATH_CMD fast path). */
static exp_t *rat_binop(char op, exp_t *a, exp_t *b, const char **err) {
  *err = NULL;
  int64_t an, ad, bn, bd;
  exact_parts(a, &an, &ad);
  exact_parts(b, &bn, &bd);
  __int128 num, den;
  switch (op) {
  case '+':
    num = (__int128)an * bd + (__int128)bn * ad;
    den = (__int128)ad * bd;
    break;
  case '-':
    num = (__int128)an * bd - (__int128)bn * ad;
    den = (__int128)ad * bd;
    break;
  case '*':
    num = (__int128)an * bn;
    den = (__int128)ad * bd;
    break;
  case '/':
    if (bn == 0) {
      *err = "divzero";
      return NULL;
    }
    num = (__int128)an * bd;
    den = (__int128)ad * bn;
    break;
  default:
    *err = "overflow";
    return NULL;
  }
  int over;
  exp_t *r = rat_from_i128(num, den, &over);
  if (over)
    *err = "overflow";
  return r;
}

static inline double apply_op_d(char op, double a, double b) {
  switch (op) {
  case '+':
    return a + b;
  case '-':
    return a - b;
  case '*':
    return a * b;
  case '/':
    return a / b;
  }
  return 0;
}

/* Value of an exact (fixnum or rational) as a double — used when the rational
   tower meets a float and the whole expression contaminates to float. */
static inline double exact_to_double(exp_t *e) {
  return isrational(e) ? rat_to_double(e) : (double)FIX_VAL(e);
}

/* Three-way compare of two exact values without overflow (i128 cross-multiply).
   Returns -1, 0, +1. Denominators are positive, so the sign is the cross diff. */
static inline int rat_cmp(exp_t *a, exp_t *b) {
  int64_t an, ad, bn, bd;
  exact_parts(a, &an, &ad);
  exact_parts(b, &bn, &bd);
  __int128 lhs = (__int128)an * bd;
  __int128 rhs = (__int128)bn * ad;
  return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

#endif /* ALCOVE_NUMERIC_H */
