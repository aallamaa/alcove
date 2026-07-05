#!/bin/sh
# Build release artifacts into dist/. Run from the repo root:
#
#   sh tools/release.sh
#
# Produces (skipping what the host can't build, with a note):
#   dist/alcove-VERSION-linux-x86_64.tar.gz     full build (JIT, readline, FFI),
#                                               -march=x86-64-v2 portable baseline
#   dist/alcove-VERSION-linux-aarch64.tar.gz    static cross build (JIT; no
#                                               readline/FFI), smoke-run via qemu
#   dist/alcove-VERSION-wasm.tar.gz             the web/ bundle (both cores +
#                                               wrappers + demo pages)
#
# Each binary tarball carries README.md, agpl-3.0.txt, and the language doc.
# Also produced for supply-chain provenance:
#   dist/SBOM.txt        minimal software bill of materials
#   dist/SHA256SUMS       sha256 of every tarball + the SBOM
#   dist/*.asc            detached GPG signatures (only if ALCOVE_GPG_KEY is set)
# The script rebuilds ./alcove and ./adder for the HOST at the end, so your
# dev binaries stay tuned (-march=native) after a release build.
set -e

VERSION=$(sed -n 's/^#define ALCOVE_VERSION "\(.*\)"/\1/p' alcove.h)
[ -n "$VERSION" ] || { echo "release.sh: can't read ALCOVE_VERSION from alcove.h" >&2; exit 1; }
echo "== alcove v$VERSION =="

DIST=dist
EXTRAS="README.md agpl-3.0.txt docs/alcove-language.md"
rm -rf "$DIST"
mkdir -p "$DIST"

stage() { # stage NAME BIN1 BIN2 -> tarball
  name="alcove-$VERSION-$1"
  rm -rf "$DIST/$name"
  mkdir -p "$DIST/$name/docs"
  cp "$2" "$DIST/$name/alcove"
  cp "$3" "$DIST/$name/adder"
  cp README.md agpl-3.0.txt "$DIST/$name/"
  cp docs/alcove-language.md "$DIST/$name/docs/"
  tar -C "$DIST" -czf "$DIST/$name.tar.gz" "$name"
  rm -rf "$DIST/$name"
  echo "   -> $DIST/$name.tar.gz"
}

# ---- 1. linux x86_64: full-featured, portable baseline ---------------------
if [ "$(uname -sm)" = "Linux x86_64" ]; then
  echo "== linux-x86_64 (full: JIT + readline + FFI, -march=x86-64-v2) =="
  make -s MARCH='-march=x86-64-v2' jit adder
  strip -o /tmp/alcove.rel alcove
  strip -o /tmp/adder.rel adder
  # smoke: the full suite must pass on the artifact itself
  /tmp/alcove.rel --noload test.alc 2>&1 | sed 's/\x1b\[[0-9;]*m//g' \
    | grep 'TEST RESULT' | grep -q ' 0 failed' \
    || { echo "x86_64 release binary FAILED test.alc" >&2; exit 1; }
  /tmp/adder.rel --version >/dev/null
  stage linux-x86_64 /tmp/alcove.rel /tmp/adder.rel
  rm -f /tmp/alcove.rel /tmp/adder.rel
else
  echo "== skipping linux-x86_64 (host is $(uname -sm)) =="
fi

# ---- 2. linux aarch64: static cross build (JIT; no readline/FFI) ------------
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  echo "== linux-aarch64 (static cross: JIT, no readline/FFI) =="
  aarch64-linux-gnu-gcc -static -Wall -W -fno-strict-aliasing -O3 \
    -DALCOVE_JIT=1 -o /tmp/alcove.a64 alcove.c -lm
  aarch64-linux-gnu-gcc -static -Wall -W -fno-strict-aliasing -O3 \
    -DALCOVE_JIT=1 -DADFMT_NO_MAIN -o /tmp/adder.a64 adder.c adfmt.c -lm
  aarch64-linux-gnu-strip /tmp/alcove.a64 /tmp/adder.a64
  if command -v qemu-aarch64 >/dev/null 2>&1; then
    qemu-aarch64 /tmp/alcove.a64 --noload test.alc 2>&1 \
      | sed 's/\x1b\[[0-9;]*m//g' | grep 'TEST RESULT' | grep -q ' 0 failed' \
      || { echo "aarch64 release binary FAILED test.alc under qemu" >&2; exit 1; }
  else
    echo "   (qemu-aarch64 not found — artifact built but NOT smoke-tested)"
  fi
  stage linux-aarch64 /tmp/alcove.a64 /tmp/adder.a64
  rm -f /tmp/alcove.a64 /tmp/adder.a64
else
  echo "== skipping linux-aarch64 (no aarch64-linux-gnu-gcc) =="
fi

# ---- 3. wasm bundle ----------------------------------------------------------
if command -v emcc >/dev/null 2>&1; then
  echo "== wasm bundle (make web) =="
  make -s web
  node web/test_web.js >/dev/null 2>&1 \
    || { echo "wasm bundle FAILED the smoke battery" >&2; exit 1; }
  name="alcove-$VERSION-wasm"
  rm -rf "$DIST/$name"
  mkdir -p "$DIST/$name"
  cp web/alcove.js web/alcove-core.js web/alcove-core.wasm \
     web/adder.js web/adder-core.js web/adder-core.wasm \
     web/index.html web/playground.html web/learn.html web/mandelbrot.html \
     README.md agpl-3.0.txt "$DIST/$name/"
  tar -C "$DIST" -czf "$DIST/$name.tar.gz" "$name"
  rm -rf "$DIST/$name"
  echo "   -> $DIST/$name.tar.gz"
else
  echo "== skipping wasm (no emcc) =="
fi

# ---- 4. SBOM, checksums, and (optional) signatures --------------------------
# Supply-chain provenance: a minimal SBOM, a SHA256SUMS manifest over every
# artifact (always), and detached GPG signatures when a key is available.
echo "== SBOM + checksums + signatures =="
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILT=$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo unknown)

# Minimal, honest bill of materials. alcove's own code is AGPL-3.0; the only
# third-party code is the C runtime + optional readline/FFI (full build) and the
# Emscripten runtime (wasm). No vendored libraries.
cat > "$DIST/SBOM.txt" <<SBOM
alcove $VERSION — Software Bill of Materials
commit:    $COMMIT
generated: $BUILT

Project components (this source tree, AGPL-3.0):
  alcove  — the interpreter (JIT'd numeric Lisp)
  adder   — the Pythonic dialect front end (same source)

Third-party runtime dependencies, by artifact:
  alcove-$VERSION-linux-x86_64    libc, libm, libreadline, libffi  (dynamic)
  alcove-$VERSION-linux-aarch64   libc, libm                       (static; no readline/FFI)
  alcove-$VERSION-wasm            Emscripten runtime               (bundled; no external libs)

Build flags:
  linux-x86_64    cc  -O3 -march=x86-64-v2 -DALCOVE_JIT=1  (+readline +FFI)
  linux-aarch64   aarch64-linux-gnu-gcc -static -O3 -DALCOVE_JIT=1
  wasm            emcc  (see the Makefile 'web' target)
SBOM
echo "   -> $DIST/SBOM.txt"

# SHA256 manifest over the tarballs + the SBOM (portable: GNU or BSD tooling).
checksum() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"
  else shasum -a 256 "$@"; fi
}
( cd "$DIST" && checksum *.tar.gz SBOM.txt > SHA256SUMS )
echo "   -> $DIST/SHA256SUMS"

# Detached, armored GPG signatures — only when a signing key is configured, so
# the script still works on hosts without signing infra. Signs each tarball and
# the SHA256SUMS manifest (signing the manifest covers the SBOM transitively).
if command -v gpg >/dev/null 2>&1 && [ -n "${ALCOVE_GPG_KEY:-}" ]; then
  echo "   signing with GPG key $ALCOVE_GPG_KEY"
  for f in "$DIST"/*.tar.gz "$DIST/SHA256SUMS"; do
    gpg --batch --yes --local-user "$ALCOVE_GPG_KEY" --detach-sign --armor "$f"
    echo "   -> $f.asc"
  done
else
  echo "   (no GPG signature — set ALCOVE_GPG_KEY (and install gpg) to sign;"
  echo "    SHA256SUMS is always produced so downloads can be verified)"
fi

# ---- restore host-tuned dev binaries ----------------------------------------
echo "== restoring host-tuned ./alcove and ./adder =="
make -s jit adder

echo "== artifacts =="
ls -lh "$DIST"
echo "== verify: sha256sum -c dist/SHA256SUMS (in dist/) =="
