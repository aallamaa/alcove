# examples/llm — a native Anthropic API client (M2)

`lib/llm.adr` turns the M0 evolve loop's stub model into a real one: an
`(llm/complete prompt)` call that hits the **Anthropic Messages API** over native
HTTPS, with libcurl bound through Alcove's FFI — no subprocess.

```
prompt ──build-body (JSON)──▶ libcurl POST (FFI) ──▶ response.json ──extract-text──▶ text
```

## Transport

The HTTP body is captured the simple way: bind libc `fopen`/`fclose` via FFI,
point `CURLOPT_WRITEDATA` at a `FILE*`, let curl's default writer fill it, then
`read-string` the file — no write-callback memory reads. The request is built
with `json-encode` and parsed with `json-decode`; the API key is read from
`$ANTHROPIC_API_KEY`. Model: **`claude-opus-4-8`** (override `llm/model`).

## Run

```sh
make adder                                    # FFI-enabled build (default)

# 1. Transport check — NO key needed (hits httpbin, which echoes the request):
./adder examples/llm/smoke.adr

# 2. A real completion — needs a key:
export ANTHROPIC_API_KEY=sk-ant-...
./adder examples/llm/complete.adr

# 3. The self-improvement loop, driven by the real model:
./adder examples/llm/evolve-live.adr
```

## Contract

`(llm/complete prompt)` returns the model's text **or `nil`** on any failure —
missing key, network error, HTTP error, malformed JSON, or a refusal. Callers
branch on `nil`; nothing is raised (error *values* propagate through normal calls
in Alcove, so the transport is wrapped in `try` and converts failures to `nil`).

That nil-or-string contract is exactly what `lib/evolve.adr` expects from a
candidate generator, so swapping `evolve/stub-llm` for `llm/complete` leaves the
loop unchanged — see `evolve-live.adr`.

## Tested

The **pure** helpers — `llm/build-body` (request JSON) and `llm/extract-text`
(`content[0].text` out of a response) — are covered in `test.alc` (the `llm …`
asserts) and run in the gate, in both the alcove and adder binaries. The network
path is non-deterministic and key-gated, so it lives here as a manual check
(`smoke.adr` verifies the FFI transport end-to-end without a key).
