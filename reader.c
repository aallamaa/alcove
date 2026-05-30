/* reader.c — the s-expression reader / tokenizer: make_atom_from_token (the
 * number/symbol/string classifier), callmacrochar (the (/[/{/'/`/, reader
 * macros), escapereader (\n, \xAB, ...), and reader() (the main DFA, incl. the
 * #-dispatch for #\ char, #[ vector, #{ set, #b\"...\" blob, and `# `
 * comments).
 *
 * FRAGMENT #included into alcove.c after the value-model constructors it calls
 * (make_node/make_symbol/make_quote/make_char/make_blob/make_integer/make_float,
 * the token helpers, utf8_decode_stream, error, unrefexp) — single TU, so those
 * hot inline calls stay inlined into the per-byte reader loop. NOT standalone,
 * NOT separately compiled. The chrmap/schrmap/chr2hex tables live in char.h.
 * Exercised + fuzzed by parser_test.c.
 */
exp_t *make_atom_from_token(token_t *token) {
  char *str = token->data;
  int length = token->size;
  // Generate an atom from a string during parsing
  //  TEST -> 0: + or - in front, 1: digit after first + or -, 2: E mantissa,
  //  3:+ or - sign, 4: digit of mantissa
  int test = 0;
  int dot = 0;
  char v;
  char *stro = str;
  /* A token literally starting with `"` is a quoted string. Normal strings
     are read in the reader's escape mode and never reach here; this path
     catches a `"` that arrived via a single-escape (e.g. the input `\"`),
     which can yield a 1-char `"` token — length-2 would then be negative and
     blow up make_string_from_token's memcpy. Require the full open+close so
     a stray quote falls through to the symbol/number scanner instead. */
  if (str[0] == '\"' && length >= 2)
    return make_string_from_token(token, 1, length - 2);
  /* Hex literals: 0xNN / 0XNN, optionally with leading +/-. The decimal
     state machine below rejects 'x', so without this fast path 0xFF
     would be tokenised as the symbol "0xFF". */
  {
    const char *p = stro;
    int neg = 0;
    if (*p == '+' || *p == '-') {
      neg = (*p == '-');
      p++;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && p[2]) {
      const char *q = p + 2;
      int all_hex = 1;
      for (; *q; q++) {
        if (!isxdigit((unsigned char)*q)) {
          all_hex = 0;
          break;
        }
      }
      if (all_hex) {
        errno = 0;
        long long hv = strtoll(p, NULL, 16);
        /* Guard the two's-complement one-value asymmetry: -LLONG_MIN
           is UB. Treat it as out-of-range and let the symbol path
           take it. */
        if (errno != ERANGE && !(neg && hv == LLONG_MIN)) {
          int64_t fix_max = ((int64_t)1 << 60) - 1;
          int64_t fix_min = -((int64_t)1 << 60);
          if (neg)
            hv = -hv;
          if (hv >= fix_min && hv <= fix_max) {
            freetoken(token);
            return MAKE_FIX((int64_t)hv);
          }
        }
      }
    }
  }
  while (length--) {
    v = (char)*(str++);
    if ((v == '+') || (v == '-')) {
      if ((test == 1) || (test == 3)) {
        break; // A sign after another sign => not an integer or following
               // format +AB+
      } else if (test == 7) {
        // OK MANTISSA there
        test = 15;
      } else if (test == 0)
        test = 1;
      else
        break;
    } else if (v == '.') {
      if ((test <= 3) || !dot)
        dot += 1;
      else
        break;
    } else if ((v == 'E') || (v == 'e')) {
      // set mentisa on if not seen mantisa yet
      if (test == 3)
        test = 7;
      else
        break;

    } else if ((v <= '9') && (v >= '0')) {
      if (test <= 3) {
        test = 3;
      } else if ((test == 7) || (test == 15) | (test == 31))
        test = 31;
      else
        break;
    } else
      break;
  }

  if (length != -1) {
    // not an integer then must be a symbol
    return make_symbol_from_token(token);
  } else {
    if (test == 1)
      return make_symbol_from_token(token);
    else if ((test == 3) && !dot) {
      exp_t *ret = make_integer(stro);
      freetoken(token);
      return ret;
    } else if ((test == 31) || (test == 3)) {
      exp_t *ret = make_float(stro);
      freetoken(token);
      return ret;
    } else
      return make_symbol_from_token(token);
  }
}

exp_t *callmacrochar(FILE *stream, unsigned char x) {
  exp_t *lnode = NULL; // Initial List Node
  exp_t *vnode = NULL; // Val Node
  exp_t *cnode = NULL; // Current Node

  if (x == '(') {
    vnode = reader(stream, ')', 0);

    if (vnode) {
      if (iserror(vnode))
        return vnode;
      lnode = make_node(vnode);
      vnode = NULL;
      cnode = lnode;
      while ((vnode = reader(stream, ')', 0))) {
        if (iserror(vnode)) {
          unrefexp(lnode);
          return vnode;
        }
        cnode = cnode->next = make_node(vnode);
      }
    }
  } else if (x == '[') {
    vnode = reader(stream, ']', 0);
    // ?? why ?? lnode=vnode;
    if (vnode) {
      if (iserror(vnode))
        return vnode;
      cnode = make_node(vnode); // body
      vnode = NULL;
      lnode = make_node(make_node(make_symbol("_", 1))); // header
      lnode->next = make_node(make_node(cnode));
      lnode->type = EXP_LAMBDA;
      while ((vnode = reader(stream, ']', 0))) {
        if (iserror(vnode)) {
          unrefexp(lnode);
          return vnode;
        } // cleaning to be done gc
        cnode = cnode->next = make_node(vnode);
        vnode = NULL;
      }
    }
  } else if (x == '{') {
    /* Clojure-style hash-map literal: {k v, k v} → (hash-map k v k v ...).
       Comma is treated as whitespace LOCALLY here only — the reader
       proper still classifies `,` as a TERMMACRO globally, so we don't
       silently change comma behavior in other contexts. */
    lnode = make_node(make_symbol("hash-map", 8));
    cnode = lnode;
    for (;;) {
      int c;
      /* Eat inter-form whitespace, commas, and line comments. */
      while ((c = getc(stream)) != EOF) {
        if (c == ',')
          continue;
        if (c < 256 && (ISWHITESPACE & chrmap[c]))
          continue;
        if (c == ';') { /* line comment */
          while ((c = getc(stream)) != EOF && c != '\n')
            ;
          continue;
        }
        break;
      }
      if (c == EOF) {
        unrefexp(lnode);
        return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                     "End of file in hash-map literal");
      }
      if (c == '}')
        return lnode;
      ungetc(c, stream);
      vnode = reader(stream, '}', 0);
      if (!vnode)
        return lnode; /* `}` consumed by inner reader */
      if (iserror(vnode)) {
        unrefexp(lnode);
        return vnode;
      }
      cnode = cnode->next = make_node(vnode);
    }
  } else if (x == '\'') {
    vnode = reader(stream, 0, 0);
    return make_quote(vnode);
  } else if (x == '`') {
    vnode = reader(stream, 0, 0);
    exp_t *qq = make_node(make_symbol("quasiquote", 10));
    qq->next = make_node(vnode);
    return qq;
  } else if (x == ',') {
    int c = getc(stream);
    int splice = (c == '@');
    if (!splice)
      ungetc(c, stream);
    vnode = reader(stream, 0, 0);
    char *tag = splice ? "unquote-splicing" : "unquote";
    exp_t *uq = make_node(make_symbol(tag, strlen(tag)));
    uq->next = make_node(vnode);
    return uq;
  } else if (x == ';') {
    /* Line comment — skip to EOL or EOF. Without this handler `;` was
     * a TERMMACRO that returned EXP_ERROR_PARSING_MACROCHAR; the error
     * worked at top level (the file driver swallowed it before the
     * next form) but inside a list it bailed out mid-build, dropping
     * everything before the comment (tickets 7 & 8 root cause). */
    int c;
    while ((c = getc(stream)) != EOF && c != '\n')
      ;
    return NULL; /* signal "no form here" — caller's loop continues. */
  }
  /* Note: `|` was previously hooked here as a reader macro that built
     a wrapped list — it didn't implement Common Lisp's |sym with spaces|
     and isn't part of the Arc spec (paulgraham.com/arcll1.html doesn't
     mention it). We reclassified `|` as a normal constituent in
     char.h so it can be used as a function name (bit-or alias). */

  else
    return error(EXP_ERROR_PARSING_MACROCHAR, NULL, NULL,
                 "call to macro char %c unkown!", x);

  if (lnode)
    return lnode;
  else
    return NIL_EXP;
}

exp_t *escapereader(FILE *stream, token_t **ptoken, int lastchar) {
  /* Parse \n \b ... */
  /* Parse \xAB as char 0xAB */
  /* Parse \u001000 as unicode char 001000 in hex mode */
  int zchar = lastchar;
  int nchar = 0;
  if (schrmap[lastchar]) {
    zchar = schrmap[lastchar];
  } else if (lastchar == 'x') {
    if ((nchar = getc(stream)) == EOF)
      return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                   "End of file reached while parsing");
    if (chr2hex[nchar] < 0)
      goto error;
    zchar = chr2hex[nchar] * 16;
    if ((nchar = getc(stream)) == EOF)
      return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                   "End of file reached while parsing");
    if (chr2hex[nchar] < 0)
      goto error;
    zchar += chr2hex[nchar];
  }
  if (*ptoken) {
    tokenadd(*ptoken, zchar);
  } else {
    *ptoken = tokenize(zchar);
  }

  return NULL;
error:
  return error(EXP_ERROR_PARSING_ESCAPE, NULL, NULL,
               "invalid escape %c unkown!", nchar);
}

exp_t *reader(FILE *stream, unsigned char clmacro, int keepwspace) {
  int x, y, z;
  token_t *token = NULL;
  exp_t *ret = NULL;
  int pushtoken = 0;
  int escape = 0;

  while ((x = getc(stream)) != EOF) {
    pushtoken = 0;
    escape = 0;
    if (x > 127) { /* UTF-8 SUPPORT */
      token = tokenize(x);
      do {
        if ((y = getc(stream)) != EOF) {
          if ((y < 192) && (y > 127))
            tokenadd(token, y);
        } else {
          freetoken(token);
          return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                       "End of file reached while parsing");
        }
      } while ((y < 192) && (y > 127));
      ungetc(y, stream);
    } else if ((x < 0) || (x > 255) || !chrmap[x])
      return error(EXP_ERROR_PARSING_ILLEGAL_CHAR, NULL, NULL,
                   "Error illegal char %d", x);

    else if (ISWHITESPACE & chrmap[x])
      continue;
    else if ((ISTERMMACRO | ISNTERMMACRO) & chrmap[x]) {
      if (clmacro == x) {
        if (keepwspace & PARSER_TERMMACROMODE)
          ungetc(x, stream);
        return NULL; /* OK */
      }
      if (x == '#') {
        // Dispatch macro — or a `# ` line comment.
        if ((y = getc(stream)) != EOF) {
          if (y == ' ' || y == '\t' || y == '\n') {
            /* `# ...` line comment, running to end of line (Adder uses the
               same rule). Only `#` + whitespace is a comment; `#` glued to a
               token stays a dispatch macro (#\ char, #[ vector, #{ set,
               #b"..." blob). `;` remains a comment too. This was previously a
               hard parse error, so no valid program is affected. */
            if (y != '\n')
              while ((z = getc(stream)) != EOF && z != '\n')
                ;
            continue;
          }
          if (y == '\\') { // returning char
            if ((z = getc(stream)) != EOF) {
              return make_char(utf8_decode_stream(z, stream));
            } else
              return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                           "End of file reached while parsing");

          } else if (y == '[') {
            /* Vector literal: #[1 2 3] → (vector 1 2 3). The plain
               `[...]` form is reserved for Arc-lambda; we use the `#`
               prefix (Scheme/EDN convention) so the two don't collide.
               `vec` is the n-ary allocator, `vector` is the populator. */
            exp_t *vlnode = make_node(make_symbol("vector", 6));
            exp_t *vcnode = vlnode;
            exp_t *vvnode;
            while ((vvnode = reader(stream, ']', 0))) {
              if (iserror(vvnode)) {
                unrefexp(vlnode);
                return vvnode;
              }
              vcnode = vcnode->next = make_node(vvnode);
            }
            return vlnode;
          } else if (y == 'b') {
            /* Blob literal: #b"..." → an EXP_BLOB holding the string's bytes.
               This is the form the printer emits for a printable blob, so the
               value round-trips. The next char must open a string. */
            if ((z = getc(stream)) != '"') {
              if (z != EOF)
                ungetc(z, stream);
              return error(EXP_ERROR_PARSING_MACROCHAR, NULL, NULL,
                           "#b must be followed by a string literal");
            }
            ungetc(z, stream); /* hand the `"` back to the string reader */
            exp_t *str = reader(stream, 0, 0);
            if (!str)
              return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                           "End of file in #b\"...\" literal");
            if (iserror(str))
              return str;
            const char *sb = exp_text(str);
            exp_t *blob = make_blob(sb ? sb : "", sb ? strlen(sb) : 0);
            unrefexp(str);
            return blob;
          } else if (y == '{') {
            /* Set literal: #{1 2 3} → (hash-set 1 2 3). This is the form the
               printer emits for an EXP_SET, so a printed set re-reads as the
               same value. (Plain `{...}` is the hash-map literal.) */
            exp_t *slnode = make_node(make_symbol("hash-set", 8));
            exp_t *scnode = slnode;
            exp_t *svnode;
            while ((svnode = reader(stream, '}', 0))) {
              if (iserror(svnode)) {
                unrefexp(slnode);
                return svnode;
              }
              scnode = scnode->next = make_node(svnode);
            }
            return slnode;
          } else
            return error(EXP_ERROR_PARSING_MACROCHAR, NULL, NULL,
                         "call to dispatch macro char %c unkown!", y);
        } else
          return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                       "End of file reached while parsing");
      }
      if ((ret = callmacrochar(stream, x)))
        return ret;
      else
        continue;
    } else if (ISSINGLEESCAPE & chrmap[x]) { // step 5
      if ((y = getc(stream)) != EOF) {
        if ((ret = escapereader(stream, &token, y))) {
          if (token)
            freetoken(token);
          return ret;
        }
      } else
        return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                     "End of file reached while parsing");

    } else if (ISMULTIPLEESCAPE & chrmap[x]) {
      token = tokenize(-1);
      escape = 1;
    } // step 6
    else if (ISCONSTITUENT & chrmap[x])
      token = tokenize(x); // step 7
    while (!pushtoken) {
      if (!escape) {
        // step 8
        if ((y = getc(stream)) != EOF) {
          if (y > 127) {
            tokenadd(token, y);
            do {
              if ((y = getc(stream)) != EOF) {
                if ((y < 192) && (y > 127))
                  tokenadd(token, y);
              } else {
                freetoken(token);
                return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                             "End of file reached while parsing");
              }
            } while ((y < 192) && (y > 127));
            ungetc(y, stream);
          } else if ((y < 0) || (y > 255) || !chrmap[y]) {
            freetoken(token);
            return error(EXP_ERROR_PARSING_ILLEGAL_CHAR, NULL, NULL,
                         "Error illegal char %d", x);
          } else if ((ISCONSTITUENT | ISNTERMMACRO) & chrmap[y]) {
            tokenadd(token, y);
            continue;
          } else if (ISSINGLEESCAPE & chrmap[y]) {
            if ((z = getc(stream)) != EOF) {
              if ((ret = escapereader(stream, &token, z))) {
                if (token)
                  freetoken(token); /* a bad escape (e.g. \x with non-hex)
                                       must not leak the in-progress token */
                return ret;
              }
            } else {
              freetoken(token);
              return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                           "End of file reached while parsing");
            }
          } else if (ISMULTIPLEESCAPE & chrmap[y]) {
            pushtoken = 1;
            ungetc(y, stream);
          } // escape=1;
          else if (ISTERMMACRO & chrmap[y]) {
            ungetc(y, stream);
            pushtoken = 1;
          } else if (ISWHITESPACE & chrmap[y]) {
            pushtoken = 1; // ungetc if appropriate
            if (keepwspace & 1)
              tokenadd(token, y);
          } else
            pushtoken = 1;
        } else {
          /* EOF immediately after a token's last byte — no trailing
             delimiter. The token is nonetheless complete, so flush it as an
             atom instead of discarding the whole form. Without this, a final
             bare atom with no newline was silently dropped: `alcove -e 42`
             and newline-less piped input produced nothing. (Unterminated
             strings are handled in escape mode below and still error.) */
          pushtoken = 1;
        }
      } else { // Escape mode
        // step 9
        if ((y = getc(stream)) != EOF) {
          if (y > 127) {
            tokenadd(token, y);
            do {
              if ((y = getc(stream)) != EOF) {
                if ((y < 192) && (y > 127))
                  tokenadd(token, y);
              } else {
                freetoken(token);
                return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                             "End of file reached while parsing");
              }
            } while ((y < 192) && (y > 127));
            ungetc(y, stream);
          } else if ((y < 0) || (y > 255)) {
            freetoken(token);
            return error(EXP_ERROR_PARSING_ILLEGAL_CHAR, NULL, NULL,
                         "Error illegal char %d", x);
          } else if ((ISWHITESPACE | ISCONSTITUENT | ISTERMMACRO |
                      ISNTERMMACRO) &
                     chrmap[y])
            tokenadd(token, y);
          else if (ISSINGLEESCAPE & chrmap[y]) {
            if ((z = getc(stream)) != EOF) {
              if ((ret = escapereader(stream, &token, z))) {
                if (token)
                  freetoken(token); /* bad escape inside a "string" — free the
                                       partial token before bailing */
                return ret;
              }
            } else {
              freetoken(token);
              return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                           "End of file reached while parsing");
            }
          } else if (ISMULTIPLEESCAPE & chrmap[y]) {
            ret = make_string_from_token(token, 0, token->size);
            return ret;
            /*escape=0;pushtoken=1;*/
          } else
            tokenadd(token, y);

        } else {
          freetoken(token);
          return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                       "End of file reached while parsing");
        }
      }
    }
    if (pushtoken) {
      // TOKEN AND STUFF TO BE FREED
      ret = make_atom_from_token(token);
      token = NULL;
      return ret;
    } else
      return NULL;
  }

  if (x == EOF) {
    return error(EXP_ERROR_PARSING_EOF, NULL, NULL,
                 "End of file reached while parsing");
    // END OF FILE PROCESSING TO BE DONE STEP 1
  }
  return NULL;
}
