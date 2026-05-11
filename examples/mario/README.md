# Mario-style platformer demo in alcove

A small graphical, scrolling platformer written in alcove with SDL2
for rendering and input, built to demonstrate the language's FFI and
bytecode VM. Cross-platform (macOS + Linux).

> **Notice — unofficial fan demo.** This is a non-commercial code
> sample. It is **not affiliated with, endorsed by, or sponsored by
> Nintendo Co., Ltd.** "Mario," "Super Mario Bros.," and related
> names, characters, and designs are trademarks of Nintendo. See the
> [Legal notice](#legal-notice) below before forking or redistributing.

```
./alcove                      mario.alc                 libmariogfx.so
+----------------+   FFI    +-------------+   sprite    +----------------+
| bytecode VM /  |--------->|  game logic |------------>|  SDL2 renderer |
|     JIT        |          |   physics   |             |  + key state   |
+----------------+          +-------------+             +----------------+
```

## Build & run

You need SDL2 + SDL2_mixer, plus `curl` (used by `make run` to fetch
the music track on first launch):

| OS         | Install                                                        |
|------------|----------------------------------------------------------------|
| macOS      | `brew install sdl2 sdl2_mixer curl`                            |
| Debian     | `sudo apt install libsdl2-dev libsdl2-mixer-dev curl`          |
| Fedora     | `sudo dnf install SDL2-devel SDL2_mixer-devel curl`            |

Then, from this directory:

```sh
make           # builds libmariogfx.so (or .dylib + .so symlink on macOS)
make run       # build then launch
# or, manually:
../../alcove --noload --noinit mario.alc
```

If you're hacking on the C shim and want to verify the FFI bindings
without running the full game, there's a 2-second smoke test that
opens the window and draws a single sprite:

```sh
../../alcove --noload --noinit smoketest.alc
```

`--noload` skips loading the persistent KV store; `--noinit` skips
`.init.alc`. Neither is required, but together they make startup
faster and isolate the game from your REPL state.

### Bigger window

Set `MARIO_SCALE` (1–8) to upscale the window. The internal 800×480
resolution is unchanged — SDL stretches it to fit, so pixel art stays
crisp:

```sh
MARIO_SCALE=2 make run     # 1600×960
MARIO_SCALE=3 make run     # 2400×1440
```

## Controls

| Action  | Keys                           |
|---------|--------------------------------|
| Move    | ← / → or `A` / `D`             |
| Jump    | `SPACE`, `W`, `↑`, `Z`, `K`    |
| Quit    | `ESC` or `Q`                   |

## Gameplay

- Single-screen viewport scrolls horizontally over a level 120 tiles
  wide.
- Five goombas patrol the ground; stomp them by landing on their
  heads (your downward velocity must be positive on contact).
- Two pits will kill you if you fall in. You start with 3 lives.
- Three pipes block your path — jump over them.
- Three coins float above a brick row mid-level. 100 coins = 1-up.
- A staircase leads up to the flag pole; touch it to win.
- A castle is drawn near the end of the level.

## Architecture

### Two layers

1. **`mario_gfx.c` (the shim).** ~250 lines of C. Wraps SDL2 behind
   flat-int entry points. alcove's libffi bridge has tight constraints
   (max 8 args, no struct-by-value, no varargs, no callbacks), so this
   file flattens SDL into functions like `gfx_init(w, h)`,
   `gfx_fill(x, y, w, h, rgb)`, `gfx_left()`, etc. Sprites are drawn
   as composites of colored rectangles indexed by sprite ID. An 8x8
   bitmap font (uppercase letters + digits + a few punctuation marks)
   handles HUD text via `gfx_text_set` (write a byte into the shim's
   internal buffer) + `gfx_text` (render `len` glyphs).

2. **`mario.alc` (the game).** All physics, collision, AI, level
   data, and rendering control lives here. The level is a flat
   integer vector indexed by `(+ x (* y LW))`. Mario's bounding box is
   collision-checked one pixel per axis-step against the tile grid.
   Goombas live in parallel vectors (`gpx`, `gpy`, `gdir`, `galive`,
   `gdeath`) since alcove has no record type. The main loop calls
   `update`, `render`, polls quit input, and sleeps to maintain 60
   Hz pacing using the shim's monotonic clock.

### Why a C shim?

alcove's FFI cannot pass structs by value, take callbacks, or call
varargs functions. SDL2's API uses all three. The shim collapses
those APIs into the strict subset alcove can call:

- All sprite drawing: 4 ints + sprite ID.
- All input: zero-arg `int` returns (a snapshot of held key state).
- All text: write the bytes one at a time, then call render with a
  length.

This is the same pattern documented in
`docs/alcove-language.md` §5: keep the `.alc` side pure Lisp; do
struct-juggling in C.

### Coordinate systems

- **Tile coordinates** (`tx`, `ty`): integers, indexed into `level`.
  120 wide × 15 tall.
- **Pixel coordinates** (`mpx`, `mpy`, `gpx[i]`, `gpy[i]`,
  `camera`): integers in *level-pixel space*. One tile = `TILE`
  pixels (32).
- **Screen coordinates** (passed to `gfx_*`): pixel coords minus
  `camera`. The viewport is `SCREEN-W` × `SCREEN-H` = 800 × 480.

### Physics

Per-frame, axis-separated AABB collision against the tile grid:

```
mvx = WALK-SPEED × (input direction)        # horizontal first
mvy += GRAVITY (clamped to MAX-VY)          # gravity always
step-x(mvx)                                  # 1px at a time, stops on wall
step-y(mvy)                                  # 1px at a time, sets on-ground
```

Each `step-*` advances the bounding box one pixel at a time and bails
at the first solid-tile contact. This is slower than a fused move but
trivially correct — Mario never tunnels through a wall, however large
his velocity.

Jump is gated on `on-ground`. `on-ground` is reset to 0 at the top of
each frame and set to 1 when `step-y` hits a solid tile while moving
downward.

### Goomba AI

Each frame, for every alive goomba:

1. Try to step in the current direction.
2. If a wall blocks the next pixel, flip direction.
3. If the tile below the front foot is *not* solid, flip direction
   (prevents walking off ledges).
4. If a death timer is counting down, decrement; on zero, mark
   permanently gone.

Stomping detection uses Mario's downward velocity (`mvy > 0`) plus a
vertical-overlap threshold so brushing the side of a goomba kills
Mario, not the goomba.

## Files

```
Makefile          # cross-platform build (Darwin → .dylib + .so symlink, Linux → .so)
mario_gfx.c       # SDL2 shim: lifecycle, input, drawing, font, timing
mario.alc         # game logic, physics, AI, level data, render loop
smoketest.alc     # 2-second FFI sanity check — opens window, draws, quits
README.md         # this file
```

## Hacking

- **Make Mario faster:** raise `WALK-SPEED` (but watch `MARIO-W`
  vs. tile thinness for tunneling — `step-x` already handles the
  worst case).
- **Add levels:** mario.alc currently builds the level inline at
  startup. Factor that out into a `(load-level "path.alc")` and you
  can ship multiple levels easily.
- **Add power-ups:** the question blocks are drawn but inert. Wire
  them up in `step-y` (head-bonk) by dispatching on the tile above
  Mario when `mvy < 0`.
- **Sound:** alcove's FFI can call a small C shim around SDL2_mixer
  the same way `mario_gfx.c` wraps SDL2's video/event subsystems.

## Known constraints

- **Logical resolution is fixed** at 800×480. Window size scales via
  `MARIO_SCALE` (see Build & run), but the game's coordinate system
  stays the same — there's no way to show *more* of the level at once.
- **No sound.**
- **No saves.** The level resets on every launch.
- **HUD text is uppercase-only**, by design — the bitmap font lives
  in 8×8 cells and has only the letters / digits / punctuation the
  game uses.

## Legal notice

This project is an independent, non-commercial code sample that
demonstrates alcove's FFI, bytecode VM, and JIT. It is **not
affiliated with, endorsed by, or sponsored by Nintendo Co., Ltd.**

- "Mario," "Super Mario Bros.," "Goomba," and the related characters,
  names, designs, and gameplay elements referenced here are
  trademarks and/or copyrighted works of Nintendo Co., Ltd. All
  rights to those marks and works belong to Nintendo.
- The sprite art, level layout, and character names in this repo are
  homages drawn from memory; they are placeholders intended to make
  the demo recognisable and should be replaced with your own assets
  before any public or commercial use.
- The music track downloaded by `make run` ("Ground Theme,"
  composed by Koji Kondo) is copyrighted by Nintendo. This
  repository does **not** bundle or redistribute the audio file; the
  Makefile fetches it from a third-party host (the Internet Archive)
  for local playback only. The availability, legality, and licensing
  of that third-party host are outside this project's control. If
  you are unsure whether you may download or use the file in your
  jurisdiction, delete the `theme.ogg` target from the Makefile and
  supply your own legally-obtained audio (any SDL_mixer-compatible
  format works).
- The alcove source code in this directory (`mario_gfx.c`,
  `mario.alc`, `Makefile`) is licensed under the same terms as the
  rest of the alcove repository. The Nintendo trademarks and music
  referenced above are **not** covered by that license.

If you fork or publish derivative work, you are responsible for
ensuring your use complies with applicable trademark and copyright
law in your jurisdiction. The authors make no warranty as to the
legality of any particular downstream use.
