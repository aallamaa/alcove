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

/* Overflow-checked fixnum arithmetic: *acc = *acc <op> b. Returns 1 on int64
   overflow (caller raises — no silent wrap, no implicit float). Division by
   zero is handled by the caller; INT64_MIN / -1 is flagged as overflow. The
   61-bit fixnum-tag range is checked separately via FIX_FITS on the result. */
static inline int fix_op_ovf(char op, int64_t *acc, int64_t b) {
  switch (op) {
  case '+':
    return __builtin_add_overflow(*acc, b, acc);
  case '-':
    return __builtin_sub_overflow(*acc, b, acc);
  case '*':
    return __builtin_mul_overflow(*acc, b, acc);
  case '/':
    if (*acc == INT64_MIN && b == -1)
      return 1;
    *acc /= b;
    return 0;
  }
  return 0;
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
   Returns -1, 0, +1. Denominators are positive, so the sign is the cross diff.
 */
static inline int rat_cmp(exp_t *a, exp_t *b) {
  int64_t an, ad, bn, bd;
  exact_parts(a, &an, &ad);
  exact_parts(b, &bn, &bd);
  __int128 lhs = (__int128)an * bd;
  __int128 rhs = (__int128)bn * ad;
  return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

/* ---------------- decimal (bounded base-10) ---------------- */

#define isdecimal(e) (is_ptr(e) && (e)->type == EXP_DECIMAL)
#define DEC_MAX_SCALE 28 /* fractional digits kept; rust_decimal-class */

/* __int128 overflow-checked ops (GCC/Clang __builtin_* support __int128). */
static inline int i128_mul_ovf(__int128 a, __int128 b, __int128 *r) {
  return __builtin_mul_overflow(a, b, r);
}
static inline int i128_add_ovf(__int128 a, __int128 b, __int128 *r) {
  return __builtin_add_overflow(a, b, r);
}
static inline int i128_sub_ovf(__int128 a, __int128 b, __int128 *r) {
  return __builtin_sub_overflow(a, b, r);
}
/* 10^n for n in 0..38 (10^39 overflows __int128). */
static inline __int128 dec_pow10(int n) {
  __int128 p = 1;
  while (n-- > 0)
    p *= 10;
  return p;
}

/* Build a normalized decimal: trim trailing fractional zeros, round any scale
   beyond DEC_MAX_SCALE (round-half-up on magnitude), reject a coefficient of
   more than 29 digits. *over: 0 ok, 1 overflow. Returns owned exp_t or NULL. */
static exp_t *make_decimal_raw(__int128 coef, int32_t scale, int *over) {
  *over = 0;
  while (scale > 0 && coef % 10 == 0) { /* trim trailing zeros */
    coef /= 10;
    scale--;
  }
  while (scale > DEC_MAX_SCALE) { /* round off excess fractional precision */
    __int128 r = coef % 10;
    if (r < 0)
      r = -r;
    coef /= 10;
    scale--;
    if (r >= 5)
      coef += (coef < 0 ? -1 : 1);
    while (scale > 0 && coef % 10 == 0) {
      coef /= 10;
      scale--;
    }
  }
  __int128 mag = coef < 0 ? -coef : coef;
  if (mag >= dec_pow10(29)) {
    *over = 1;
    return NULL;
  }
  exp_t *e = make_nil();
  e->type = EXP_DECIMAL;
  alc_dec_t *d = (alc_dec_t *)memalloc(1, sizeof(alc_dec_t));
  d->coef = coef;
  d->scale = scale;
  e->ptr = d;
  return e;
}

static inline double dec_to_double(exp_t *e) {
  alc_dec_t *d = (alc_dec_t *)e->ptr;
  return (double)d->coef / (double)dec_pow10(d->scale);
}

/* Parse [+-]?digits[.digits] (no exponent). Returns owned decimal or NULL with
 *over: 1 = too many digits, 3 = malformed. len bytes at s. */
static exp_t *dec_parse(const char *s, size_t len, int *over) {
  *over = 0;
  size_t i = 0;
  int neg = 0;
  if (i < len && (s[i] == '+' || s[i] == '-')) {
    neg = (s[i] == '-');
    i++;
  }
  __int128 coef = 0;
  int32_t scale = 0;
  int seen = 0, dot = 0, ndig = 0;
  for (; i < len; i++) {
    char c = s[i];
    if (c == '.') {
      if (dot) {
        *over = 3;
        return NULL;
      }
      dot = 1;
      continue;
    }
    if (c < '0' || c > '9') {
      *over = 3;
      return NULL;
    }
    seen = 1;
    if (++ndig > 38) { /* would overflow __int128 before normalization */
      *over = 1;
      return NULL;
    }
    coef = coef * 10 + (c - '0');
    if (dot)
      scale++;
  }
  if (!seen) {
    *over = 3;
    return NULL;
  }
  if (neg)
    coef = -coef;
  return make_decimal_raw(coef, scale, over);
}

/* Align two decimals to a common (larger) scale. *over set on scale-up
   overflow. */
static inline int dec_align(alc_dec_t *a, alc_dec_t *b, __int128 *ca,
                            __int128 *cb, int32_t *s) {
  *s = a->scale > b->scale ? a->scale : b->scale;
  if (i128_mul_ovf(a->coef, dec_pow10(*s - a->scale), ca))
    return 1;
  if (i128_mul_ovf(b->coef, dec_pow10(*s - b->scale), cb))
    return 1;
  return 0;
}

static exp_t *dec_addsub(alc_dec_t *a, alc_dec_t *b, int sub, int *over) {
  __int128 ca, cb, r;
  int32_t s;
  if (dec_align(a, b, &ca, &cb, &s)) {
    *over = 1;
    return NULL;
  }
  if (sub ? i128_sub_ovf(ca, cb, &r) : i128_add_ovf(ca, cb, &r)) {
    *over = 1;
    return NULL;
  }
  return make_decimal_raw(r, s, over);
}

static exp_t *dec_mul(alc_dec_t *a, alc_dec_t *b, int *over) {
  __int128 r;
  if (i128_mul_ovf(a->coef, b->coef, &r)) {
    *over = 1;
    return NULL;
  }
  return make_decimal_raw(r, a->scale + b->scale, over);
}

/* a / b to DEC_MAX_SCALE fractional digits, round-half-up. *over: 1 overflow,
   2 = divide by zero. */
static exp_t *dec_div(alc_dec_t *a, alc_dec_t *b, int *over) {
  if (b->coef == 0) {
    *over = 2;
    return NULL;
  }
  __int128 N, D;
  if (i128_mul_ovf(a->coef, dec_pow10(b->scale), &N) ||
      i128_mul_ovf(b->coef, dec_pow10(a->scale), &D)) {
    *over = 1;
    return NULL;
  }
  int neg = (N < 0) ^ (D < 0);
  __int128 n = N < 0 ? -N : N, d = D < 0 ? -D : D;
  __int128 q = n / d, rem = n % d;
  int32_t scale = 0;
  while (rem != 0 && scale < DEC_MAX_SCALE) {
    __int128 q10;
    if (i128_mul_ovf(q, 10, &q10) || i128_mul_ovf(rem, 10, &rem)) {
      *over = 1;
      return NULL;
    }
    q = q10 + rem / d;
    rem = rem % d;
    scale++;
  }
  if (rem != 0 && rem * 2 >= d) /* round half up */
    q += 1;
  return make_decimal_raw(neg ? -q : q, scale, over);
}

static int dec_cmp(alc_dec_t *a, alc_dec_t *b) {
  if (a->scale == b->scale)
    return a->coef < b->coef ? -1 : (a->coef > b->coef ? 1 : 0);
  __int128 ca, cb;
  int32_t s;
  if (!dec_align(a, b, &ca, &cb, &s))
    return ca < cb ? -1 : (ca > cb ? 1 : 0);
  /* extreme magnitudes whose aligned form overflows i128: compare via double */
  double da = (double)a->coef / (double)dec_pow10(a->scale);
  double db = (double)b->coef / (double)dec_pow10(b->scale);
  return da < db ? -1 : (da > db ? 1 : 0);
}

/* op '+','-','*','/' on two decimal exp_t. Returns owned decimal or NULL with
 *over: 1 overflow (exceeds bounds), 2 divide-by-zero. */
static exp_t *dec_binop(char op, exp_t *a, exp_t *b, int *over) {
  *over = 0;
  alc_dec_t *da = (alc_dec_t *)a->ptr, *db = (alc_dec_t *)b->ptr;
  switch (op) {
  case '+':
    return dec_addsub(da, db, 0, over);
  case '-':
    return dec_addsub(da, db, 1, over);
  case '*':
    return dec_mul(da, db, over);
  case '/':
    return dec_div(da, db, over);
  }
  *over = 1;
  return NULL;
}

/* Render coef*10^-scale into buf (caller provides >= 48 bytes). Returns length.
   Non-static + prototyped in alcove.h: print.h (run before this fragment) calls
   it for the EXP_DECIMAL print arm. */
int dec_to_str(alc_dec_t *d, char *buf) {
  char digits[48];
  __int128 m = d->coef < 0 ? -d->coef : d->coef;
  int nd = 0;
  if (m == 0)
    digits[nd++] = '0';
  while (m > 0) {
    digits[nd++] = (char)('0' + (int)(m % 10));
    m /= 10;
  }
  /* ensure at least scale+1 digits so there's an integer part */
  while (nd <= d->scale)
    digits[nd++] = '0';
  int o = 0;
  if (d->coef < 0)
    buf[o++] = '-';
  int intlen = nd - d->scale;
  for (int i = nd - 1; i >= 0; i--) {
    if (d->scale > 0 && (nd - 1 - i) == intlen)
      buf[o++] = '.';
    buf[o++] = digits[i];
  }
  buf[o] = 0;
  return o;
}

#endif /* ALCOVE_NUMERIC_H */
