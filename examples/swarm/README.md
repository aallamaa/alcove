# examples/swarm — a self-organizing worker swarm (M3)

The last piece of the AI-harness arc: many **worker processes** collaborating
through one alcove RESP keyspace used as a shared blackboard. Each worker runs the
M0 fitness machinery (`lib/evolve`); the swarm splits the candidate pool, scores
it in parallel, and the best emerges — with no central coordinator doing the work.

```
            ┌───────────────────────────────┐
            │  ./alcove -r PORT  (blackboard) │   count, cand:i, next, result:i
            └──────────────┬────────────────┘
        RESP over tcp-connect (lib/swarm)
        ┌──────────────────┼──────────────────┐
   worker 1            worker 2            worker N      (separate processes)
   INCR next → ticket → score cand → SET result:i      (claim, compute, publish)
```

## Why it needs no compare-and-swap

There is no in-language `spawn`, so a swarm is separate OS processes reaching the
server as redis clients. Coordination avoids every read-compare-write race:

- **Claim** is `INCR next` — atomic on the lock-free keyspace, so each candidate
  is handed to exactly one worker (no double-evaluation).
- **Publish** is `SET result:<ticket>` — a *unique* key per claim, so no two
  workers ever write the same key (no lost updates).

The best candidate is read off the board afterward (`check.adr`); nothing has to
be atomically merged during the run.

## Run

```sh
make jit adder
make swarm-smoke          # or: sh scripts/swarm-smoke.sh
```

The smoke driver starts the server, seeds 6 candidates (a mix of good, bad,
crashing, and unparseable code), runs **2 worker processes**, and asserts the
swarm converged on `(fn (x) (* x x))` with `score=0` and all 6 evaluated
(`filled=6/6`). Pure alcove — no `redis-cli` needed (the RESP client is
`lib/swarm.adr`, built on `tcp-connect`).

The driver also asserts the server's own **`BEST`** command agrees
(`SWARM SERVER BEST: 2 0`) — see "server-side aggregation" below.

## Server-side aggregation (server coordinates, workers compute)

`server-init.alc` is loaded as the server's `.init.alc` and registers a custom
`BEST` RESP command via `redis-defcmd`. It scans `result:*` with `redis-keys` /
`redis-get` and returns the top `idx score` — running **server-side** in the
`redis-defcmd` sandbox (read-only w.r.t. Lisp globals, keyspace-only). That is the
RFC's split: the server only *aggregates* what workers publish; it never runs the
LLM or evals a candidate (those are `FLAG_UNSAFE` and refused in a callback).

## Generative swarm (M0 + M2 + M3)

`worker-llm.adr` is the whole arc in one process: claim (M3) → **ask the model**
for a candidate (M2 `lib/llm`) → parse + score in the sandbox (M0 `lib/evolve`) →
publish. Run a fleet of them:

```sh
export ANTHROPIC_API_KEY=sk-ant-...
sh examples/swarm/run-llm.sh            # SWARM_BUDGET / SWARM_WORKERS to scale
```

With no key it still runs end to end (placeholder candidates) — only the
candidate *quality* depends on the model.

## Files

- `lib/swarm.adr` — the RESP client (pure `encode`/`parse` codec + `tcp` calls)
- `seed.adr` — writes `count` + `cand:0..5` to the board
- `worker.adr` — claim → `evolve/compile` + `evolve/score` → publish, in a loop
- `check.adr` — reads the board (and queries `BEST`), prints the winner
- `server-init.alc` — server-side `BEST` aggregation command
- `worker-llm.adr` + `run-llm.sh` — the generative (model-driven) swarm
