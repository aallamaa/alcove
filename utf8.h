/* utf8.h — UTF-8 codepoint helpers (encode/decode/strlen/index/validate).
 * Pure, libc-only — depends on nothing in alcove, so it is BOTH #included into
 * the single alcove.c TU AND compilable standalone (utf8_test.c includes it
 * directly). Keep it dependency-free.
 */
#ifndef ALCOVE_UTF8_H
#define ALCOVE_UTF8_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- UTF-8 codepoint helpers ------------------------------------------
   alcove strings are UTF-8 byte buffers; chars are tagged immediates that
   hold a full 32-bit codepoint (see MAKE_CHAR/CHAR_VAL). These let the
   length/indexing/substring builtins and char read/print operate on
   Unicode codepoints rather than raw bytes. All are lenient on malformed
   input — a stray/invalid byte is consumed as a single raw byte — so we
   never loop forever or read past the NUL. */

static int utf8_encode(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Decode the codepoint at byte offset *off in NUL-terminated s, advancing
 *off past it. A malformed byte is returned as-is (advance 1). */
static uint32_t utf8_decode_at(const char *s, size_t *off) {
  const unsigned char *p = (const unsigned char *)s + *off;
  unsigned char c = p[0];
  if (c < 0x80) {
    *off += 1;
    return c;
  }
  uint32_t cp;
  int n;
  if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1Fu;
    n = 1;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0Fu;
    n = 2;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07u;
    n = 3;
  } else {
    *off += 1; /* stray continuation / invalid lead — raw byte */
    return c;
  }
  for (int i = 1; i <= n; i++) {
    unsigned char cc = p[i];
    if ((cc & 0xC0) != 0x80) { /* truncated — treat lead as a raw byte */
      *off += 1;
      return c;
    }
    cp = (cp << 6) | (cc & 0x3Fu);
  }
  *off += (size_t)(n + 1);
  return cp;
}

/* Decode one codepoint from a stream given its already-read first byte,
   reading continuation bytes as needed. On a malformed sequence keeps the
   raw lead byte (ungetc's any non-continuation byte it peeked). */
static uint32_t utf8_decode_stream(int first, FILE *stream) {
  if (first < 0x80)
    return (uint32_t)(unsigned char)first;
  uint32_t cp;
  int n;
  if ((first & 0xE0) == 0xC0) {
    cp = (uint32_t)(first & 0x1F);
    n = 1;
  } else if ((first & 0xF0) == 0xE0) {
    cp = (uint32_t)(first & 0x0F);
    n = 2;
  } else if ((first & 0xF8) == 0xF0) {
    cp = (uint32_t)(first & 0x07);
    n = 3;
  } else {
    return (uint32_t)(unsigned char)first; /* invalid lead — raw byte */
  }
  for (int i = 0; i < n; i++) {
    int cc = getc(stream);
    if (cc == EOF)
      break;
    if ((cc & 0xC0) != 0x80) {
      ungetc(cc, stream);
      break;
    }
    cp = (cp << 6) | (uint32_t)(cc & 0x3F);
  }
  return cp;
}

/* Number of codepoints in NUL-terminated UTF-8 s. */
static int64_t utf8_strlen(const char *s) {
  int64_t n = 0;
  size_t off = 0;
  while (s[off]) {
    utf8_decode_at(s, &off);
    n++;
  }
  return n;
}

/* Codepoint at codepoint-index i (>=0): returns 1 and sets *out, or 0 if
   i is out of range. */
static int utf8_index(const char *s, int64_t i, uint32_t *out) {
  if (i < 0)
    return 0;
  size_t off = 0;
  int64_t k = 0;
  while (s[off]) {
    uint32_t cp = utf8_decode_at(s, &off);
    if (k == i) {
      *out = cp;
      return 1;
    }
    k++;
  }
  return 0;
}

/* Byte offset of codepoint-index i; if i is past the end, returns the byte
   length (so substring math clamps naturally). */
static size_t utf8_byte_offset(const char *s, int64_t i) {
  size_t off = 0;
  int64_t k = 0;
  while (s[off] && k < i) {
    utf8_decode_at(s, &off);
    k++;
  }
  return off;
}

/* Codepoint count of the first nbytes bytes (byte-offset -> codepoint
   index conversion for string-index). */
static int64_t utf8_count_bytes(const char *s, size_t nbytes) {
  int64_t n = 0;
  size_t off = 0;
  while (off < nbytes && s[off]) {
    utf8_decode_at(s, &off);
    n++;
  }
  return n;
}

/* Strict UTF-8 validity over n bytes: rejects stray/truncated continuation
   bytes, overlong encodings, surrogates (U+D800..U+DFFF) and codepoints
   past U+10FFFF. On the first invalid byte, sets *bad to its offset.
   (NUL is valid UTF-8 here; callers that need NUL-free check separately.) */
static int utf8_valid(const char *s, size_t n, size_t *bad) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  while (i < n) {
    unsigned char c = p[i];
    if (c < 0x80) {
      i++;
      continue;
    }
    int len;
    uint32_t cp, min;
    if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1Fu;
      min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0Fu;
      min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07u;
      min = 0x10000;
    } else {
      if (bad)
        *bad = i;
      return 0; /* continuation byte as lead, or 0xF8+ */
    }
    if (i + (size_t)len > n) {
      if (bad)
        *bad = i;
      return 0; /* truncated sequence */
    }
    for (int k = 1; k < len; k++) {
      unsigned char cc = p[i + (size_t)k];
      if ((cc & 0xC0) != 0x80) {
        if (bad)
          *bad = i;
        return 0; /* bad continuation byte */
      }
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      if (bad)
        *bad = i; /* overlong, surrogate, or out of range */
      return 0;
    }
    i += (size_t)len;
  }
  return 1;
}

#endif /* ALCOVE_UTF8_H */
