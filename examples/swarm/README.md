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

## Files

- `lib/swarm.adr` — the RESP client (pure `encode`/`parse` codec + `tcp` calls)
- `seed.adr` — writes `count` + `cand:0..5` to the board
- `worker.adr` — claim → `evolve/compile` + `evolve/score` → publish, in a loop
- `check.adr` — reads the board, prints the winning candidate

## Going further

Swap the seeded candidates for ones a worker **generates** per claim via
`lib/llm` (`llm/complete`) and the same blackboard becomes a distributed,
model-driven evolutionary search — the M0 → M2 → M3 pieces composed. The blackboard
also persists: a designated worker can `(savedb)` the board so an interrupted run
resumes where it left off.
