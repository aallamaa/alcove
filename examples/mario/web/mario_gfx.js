// mario_gfx.js — browser-side implementation of the gfx_* API consumed by
// mario.alc. Mirrors the function set in ../mario_gfx.c, but draws into an
// HTML5 Canvas and uses Web Audio + DOM keyboard events instead of SDL2.
//
// All 25 gfx-* functions are registered as alcove builtins via the
// alcove_register_cmd / Module.addFunction bridge. Sprite drawing uses
// flat colored rectangles per sprite ID — pixel-faithful sprite art is
// left for you to port in from mario_gfx.c (mechanical translation of
// each px(x,y,w,h,rgb) call to ctx.fillStyle/fillRect).
//
// Dependencies the host must expose on `Module`:
//   _alcove_register_cmd, _alcove_arg_int, _alcove_arg_string,
//   _alcove_make_int, addFunction, cwrap, UTF8ToString, Asyncify

(function (global) {
  "use strict";

  // -------------------------------------------------------------------------
  // Constants
  // -------------------------------------------------------------------------
  const VIEW_W = 800;
  const VIEW_H = 480;
  const TILE = 32;

  // Sprite IDs — must mirror SPR_* in mario_gfx.c and T-* in mario.alc.
  const SPR = {
    MARIO: 0, MARIO_R: 1, MARIO_L: 2,
    GROUND: 3, BRICK: 4, QBLOCK: 5, USED_BLOCK: 6,
    PIPE_TL: 7, PIPE_TR: 8, PIPE_BL: 9, PIPE_BR: 10,
    GOOMBA: 11, COIN: 12, FLAG_POLE: 13, FLAG: 14, DEAD_GOOMBA: 15,
    CASTLE: 16, CLOUD: 17,
  };

  // Generic placeholder palette — one flat color per sprite ID. Swap with
  // pixel-art rendering once you transcribe the draw_* functions from
  // mario_gfx.c into the drawSprite() switch below.
  const SPRITE_COLOR = {
    [SPR.MARIO]: 0xd33030, [SPR.MARIO_R]: 0xd33030, [SPR.MARIO_L]: 0xd33030,
    [SPR.GROUND]: 0x6cc04f,
    [SPR.BRICK]: 0xc65a2c,
    [SPR.QBLOCK]: 0xe0a040,
    [SPR.USED_BLOCK]: 0x8b4513,
    [SPR.PIPE_TL]: 0x10a050, [SPR.PIPE_TR]: 0x10a050,
    [SPR.PIPE_BL]: 0x10a050, [SPR.PIPE_BR]: 0x10a050,
    [SPR.GOOMBA]: 0x8b4513,
    [SPR.COIN]: 0xfce000,
    [SPR.FLAG_POLE]: 0xc0c0c0,
    [SPR.FLAG]: 0xe52521,
    [SPR.DEAD_GOOMBA]: 0x5c2e0d,
    [SPR.CASTLE]: 0xc0c0c0,
    [SPR.CLOUD]: 0xffffff,
  };

  function rgbCss(rgb) {
    const r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return "rgb(" + r + "," + g + "," + b + ")";
  }

  // -------------------------------------------------------------------------
  // State
  // -------------------------------------------------------------------------
  let canvas = null, ctx = null;
  let keys = { left: 0, right: 0, jump: 0, quit_pressed: 0, should_close: 0 };
  let textBuf = new Uint8Array(64);
  let startMs = 0;
  let audioCtx = null;
  let musicNodes = null;     // active oscillator chain for synth music
  let musicBuffer = null;    // decoded AudioBuffer for file-based music
  let musicSource = null;    // currently-playing BufferSource
  let pendingPlay = null;    // {loops} queued while musicBuffer is still loading
  let audioGestureArmed = false;

  // -------------------------------------------------------------------------
  // Lifecycle + input
  // -------------------------------------------------------------------------
  function onKey(e) {
    const down = e.type === "keydown" ? 1 : 0;
    let consumed = true;
    switch (e.key) {
      case "ArrowLeft":  case "a": case "A": keys.left = down; break;
      case "ArrowRight": case "d": case "D": keys.right = down; break;
      case " ":          case "ArrowUp":
      case "w": case "W": case "z": case "Z": case "k": case "K":
        keys.jump = down; break;
      case "Escape":     case "q": case "Q":
        if (down) { keys.quit_pressed = 1; keys.should_close = 1; }
        break;
      default: consumed = false; break;
    }
    if (consumed) e.preventDefault();
  }

  function gfxInit(w, h) {
    canvas = document.getElementById("canvas");
    if (!canvas) { console.error("mario_gfx: no <canvas id=\"canvas\"> in DOM"); return 0; }
    canvas.width  = w > 0 ? w : VIEW_W;
    canvas.height = h > 0 ? h : VIEW_H;
    ctx = canvas.getContext("2d");
    ctx.imageSmoothingEnabled = false;
    window.addEventListener("keydown", onKey);
    window.addEventListener("keyup",   onKey);
    startMs = (typeof performance !== "undefined" ? performance.now() : Date.now());
    return 1;
  }

  function gfxQuit() {
    window.removeEventListener("keydown", onKey);
    window.removeEventListener("keyup",   onKey);
    if (audioCtx) { try { audioCtx.close(); } catch (e) {} audioCtx = null; }
    return 0;
  }

  function gfxPump() {
    // DOM listeners fire async; keep keys.quit_pressed as one-shot.
    keys.quit_pressed = 0;
    return 0;
  }

  function gfxLeft()         { return keys.left; }
  function gfxRight()        { return keys.right; }
  function gfxJump()         { return keys.jump; }
  function gfxQuitPressed()  { return keys.quit_pressed; }
  function gfxShouldClose()  { return keys.should_close; }

  // -------------------------------------------------------------------------
  // Drawing primitives
  // -------------------------------------------------------------------------
  function gfxClear(rgb) {
    if (!ctx) return 0;
    ctx.fillStyle = rgbCss(rgb);
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    return 0;
  }
  function gfxPresent() { return 0; } // Canvas2D commits per-call; nothing to do
  function gfxFill(x, y, w, h, rgb) {
    if (!ctx) return 0;
    ctx.fillStyle = rgbCss(rgb);
    ctx.fillRect(x, y, w, h);
    return 0;
  }

  // -------------------------------------------------------------------------
  // Sprite drawing — placeholder rectangles per ID.
  //
  // To upgrade to pixel-faithful art, replace the body of drawSprite() with a
  // switch that calls per-sprite draw helpers translated from mario_gfx.c.
  // Each `px(x, y, w, h, rgb)` in the C side maps 1:1 to `gfxFill(x, y, w, h, rgb)`
  // here. Animation frames + facing direction are already plumbed through.
  // -------------------------------------------------------------------------
  function drawHero(x, y, dir, frame) {
    const cap_hi  = 0xFF5050;
    const cap     = 0xCC1818;
    const cap_dk  = 0x801010;
    const skin_hi = 0xFFD0A0;
    const skin    = 0xF8B080;
    const skin_dk = 0xC08060;
    const hair    = 0x402010;
    const eye_w   = 0xF0F0F0;
    const eye_b   = 0x101020;
    const blue_hi = 0x60A0FF;
    const blue    = 0x2860C9;
    const blue_dk = 0x103080;
    const yellow  = 0xFCE000;
    const boot    = 0x6B3D1A;
    const boot_dk = 0x402010;

    const MX = (off, w) => (dir > 0 ? x + off : x + 24 - off - w);

    // ---- Cap (rows 0-7). Bulges forward and droops a brim.
    // Back of cap (smaller)
    gfxFill(MX(6, 4),  y+1,  4, 2, cap);
    // Cap mid-section
    gfxFill(MX(4, 8),  y+3,  8, 2, cap);
    // Cap front bulge
    gfxFill(MX(8, 10), y+1,  10, 2, cap);
    gfxFill(MX(8, 12), y+3,  12, 2, cap_hi);    // highlight band
    gfxFill(MX(4, 16), y+5,  16, 2, cap);
    // Brim — extends further forward than backward
    gfxFill(MX(2, 18), y+7,  18, 2, cap_dk);
    gfxFill(MX(18, 4), y+5,   4, 2, cap_dk);    // front lip droops

    // ---- Head (rows 8-14). Skin in profile, hair at the back.
    gfxFill(MX(4, 2),  y+8,  2, 5, hair);       // sideburn / back of head
    gfxFill(MX(6, 12), y+8, 12, 6, skin);       // face
    gfxFill(MX(6, 1),  y+8,  1, 6, skin_dk);    // shadow at back of face
    gfxFill(MX(15, 2), y+8,  2, 1, skin_hi);    // forehead highlight (front)

    // Single visible eye on the front side
    gfxFill(MX(13, 2), y+10, 2, 2, eye_w);
    gfxFill(MX(14, 1), y+10, 1, 2, eye_b);

    // Nose pokes out the front
    gfxFill(MX(17, 2), y+11, 2, 2, skin);
    gfxFill(MX(18, 1), y+12, 1, 1, skin_dk);    // nostril shadow

    // Mustache curls under nose toward the front
    gfxFill(MX(10, 6), y+13, 6, 2, hair);
    gfxFill(MX(15, 3), y+13, 3, 1, hair);       // mustache curl tip

    // Chin
    gfxFill(MX(8, 6),  y+14, 6, 1, skin);

    // ---- Body (rows 15-23)
    // Shirt collar (front higher than back)
    gfxFill(MX(6, 12), y+15, 12, 2, cap_dk);
    gfxFill(MX(15, 3), y+14, 3, 1, cap_dk);
    // Shirt
    gfxFill(MX(4, 16), y+17, 16, 3, cap);
    gfxFill(MX(4, 1),  y+17,  1, 3, cap_dk);    // back shadow
    gfxFill(MX(18, 2), y+17,  2, 3, cap_hi);    // front highlight

    // Arms
    if (frame == 3) {
        // Jump: both arms raised
        gfxFill(MX(2, 4),  y+15, 4, 5, blue);   // back arm
        gfxFill(MX(18, 4), y+15, 4, 5, blue);   // front arm
    } else if (frame == 1) {
        // Walk-A: front arm swings back, back arm forward (counter-balance)
        gfxFill(MX(3, 4),  y+16, 4, 5, blue);   // back arm forward-ish
        gfxFill(MX(16, 4), y+19, 4, 4, blue);   // front arm lower
    } else if (frame == 2) {
        // Walk-B: front arm forward, back arm back
        gfxFill(MX(2, 4),  y+19, 4, 4, blue);
        gfxFill(MX(17, 4), y+16, 4, 5, blue);
    } else {
        // Stand: arms at sides, front arm slightly forward
        gfxFill(MX(3, 4),  y+18, 4, 5, blue);
        gfxFill(MX(17, 4), y+18, 4, 5, blue);
    }

    // Overalls
    gfxFill(MX(4, 16), y+20, 16, 4, blue);
    gfxFill(MX(4, 1),  y+20,  1, 4, blue_dk);
    gfxFill(MX(19, 1), y+20,  1, 4, blue_hi);

    // Suspender straps (visible from this angle: one front, one wrapping)
    gfxFill(MX(7, 2),  y+15, 2, 6, blue);
    gfxFill(MX(15, 2), y+15, 2, 6, blue);
    // Buttons — only the front one is clearly visible
    gfxFill(MX(15, 2), y+17, 2, 2, yellow);
    gfxFill(MX(7, 2),  y+17, 2, 2, yellow);

    // ---- Legs / boots (rows 24-29). Toe extends in facing direction.
    if (frame == 3) {
        // Jump: legs tucked together
        gfxFill(MX(8, 4),  y+22, 4, 4, blue);
        gfxFill(MX(14, 4), y+22, 4, 4, blue);
        gfxFill(MX(7, 6),  y+24, 6, 4, boot);
        gfxFill(MX(13, 6), y+24, 6, 4, boot);
        // Toe extension on the front (jump-tucked toe)
        gfxFill(MX(19, 2), y+25, 2, 2, boot);
        gfxFill(MX(7, 12), y+27, 12, 1, boot_dk);
    } else if (frame == 1) {
        // Walk-A: front foot planted, back foot mid-step
        gfxFill(MX(14, 5), y+24, 5, 3, blue);   // front thigh planted
        gfxFill(MX(4, 5),  y+25, 5, 2, blue);   // back thigh raised
        // Front boot — toe extends forward
        gfxFill(MX(13, 7), y+27, 7, 2, boot);
        gfxFill(MX(19, 3), y+27, 3, 2, boot);   // toe tip
        // Back boot — angled back
        gfxFill(MX(3, 6),  y+27, 6, 2, boot);
        gfxFill(MX(13, 7), y+28, 7, 1, boot_dk);
        gfxFill(MX(3, 6),  y+28, 6, 1, boot_dk);
    } else if (frame == 2) {
        // Walk-B: back foot planted, front foot stepping
        gfxFill(MX(4, 5),  y+24, 5, 3, blue);
        gfxFill(MX(14, 5), y+25, 5, 2, blue);
        // Back boot under hip
        gfxFill(MX(3, 7),  y+27, 7, 2, boot);
        // Front boot — toe lifted, still toes forward
        gfxFill(MX(14, 6), y+27, 6, 2, boot);
        gfxFill(MX(19, 2), y+27, 2, 1, boot);   // small toe tip
        gfxFill(MX(3, 7),  y+28, 7, 1, boot_dk);
        gfxFill(MX(14, 6), y+28, 6, 1, boot_dk);
    } else {
        // Stand: feet parallel; toes point in facing direction
        gfxFill(MX(6, 4),  y+24, 4, 3, blue);   // back leg
        gfxFill(MX(14, 4), y+24, 4, 3, blue);   // front leg
        gfxFill(MX(5, 7),  y+27, 7, 2, boot);   // back boot
        gfxFill(MX(13, 7), y+27, 7, 2, boot);   // front boot
        gfxFill(MX(19, 2), y+27, 2, 2, boot);   // front toe tip
        gfxFill(MX(5, 7),  y+28, 7, 1, boot_dk);
        gfxFill(MX(13, 9), y+28, 9, 1, boot_dk);
    }
  }

  function drawGround(x, y) {
    const grass_hi = 0xA5E060;
    const grass    = 0x6CC04F;
    const grass_dk = 0x3F7822;
    const dirt_hi  = 0xD8A070;
    const dirt     = 0xB97A56;
    const dirt_dk  = 0x7A4828;
    const rock     = 0x4C2A14;

    // --- Grass strip (top 6 px) with tufts poking up
    gfxFill(x,     y,     32, 5, grass);
    gfxFill(x+1,   y,      2, 1, grass_hi);     // tuft 1
    gfxFill(x+4,   y+1,    3, 1, grass_hi);
    gfxFill(x+9,   y,      2, 1, grass_hi);     // tuft 2
    gfxFill(x+13,  y+1,    2, 1, grass_hi);
    gfxFill(x+17,  y,      3, 1, grass_hi);     // tuft 3
    gfxFill(x+22,  y+1,    2, 1, grass_hi);
    gfxFill(x+26,  y,      2, 1, grass_hi);     // tuft 4
    gfxFill(x+29,  y+1,    2, 1, grass_hi);
    gfxFill(x,     y+5,   32, 1, grass_dk);     // grass shadow edge

    // --- Topsoil band (a brighter strip just below the grass)
    gfxFill(x,     y+6,   32, 2, dirt_hi);

    // --- Dirt body
    gfxFill(x,     y+8,   32, 18, dirt);
    // Pebbles / clods
    gfxFill(x+3,   y+10,   5, 3, rock);
    gfxFill(x+16,  y+13,   6, 4, rock);
    gfxFill(x+9,   y+19,   4, 2, rock);
    gfxFill(x+23,  y+19,   5, 3, rock);
    gfxFill(x+5,   y+22,   3, 2, rock);
    gfxFill(x+27,  y+10,   3, 2, rock);
    // Brighter dirt specks
    gfxFill(x+11,  y+9,    2, 1, dirt_hi);
    gfxFill(x+25,  y+12,   2, 1, dirt_hi);
    gfxFill(x+6,   y+16,   2, 1, dirt_hi);
    gfxFill(x+19,  y+22,   2, 1, dirt_hi);
    gfxFill(x+1,   y+24,   2, 1, dirt_hi);
    // Darker streaks
    gfxFill(x+13,  y+17,   2, 4, dirt_dk);
    gfxFill(x+28,  y+21,   2, 3, dirt_dk);

    // --- Bottom dark band (deep soil)
    gfxFill(x,     y+26,  32, 6, dirt_dk);
    gfxFill(x,     y+26,  32, 1, dirt);         // thin highlight on top of dark band
  }

  function drawBrick(x, y) {
    const brick_hi = 0xE88060;
    const brick    = 0xC65A2C;
    const brick_dk = 0x8A3B17;
    const mortar   = 0x4A1F08;

    // --- Background mortar fills every gap
    gfxFill(x, y, 32, 32, mortar);

    // --- Top row: two bricks side-by-side
    // Brick A
    gfxFill(x+2,  y+2,  13, 12, brick);
    gfxFill(x+2,  y+2,  13,  1, brick_hi);   // top highlight
    gfxFill(x+2,  y+2,   1, 12, brick_hi);   // left highlight
    gfxFill(x+2,  y+13, 13,  1, brick_dk);   // bottom shadow
    gfxFill(x+14, y+2,   1, 12, brick_dk);   // right shadow
    // Brick B
    gfxFill(x+17, y+2,  13, 12, brick);
    gfxFill(x+17, y+2,  13,  1, brick_hi);
    gfxFill(x+17, y+2,   1, 12, brick_hi);
    gfxFill(x+17, y+13, 13,  1, brick_dk);
    gfxFill(x+29, y+2,   1, 12, brick_dk);

    // --- Bottom row: offset by half-brick (half + full + half)
    // Left half-brick
    gfxFill(x,    y+18,  7, 12, brick);
    gfxFill(x,    y+18,  7,  1, brick_hi);
    gfxFill(x,    y+29,  7,  1, brick_dk);
    gfxFill(x+6,  y+18,  1, 12, brick_dk);
    // Center brick
    gfxFill(x+9,  y+18, 13, 12, brick);
    gfxFill(x+9,  y+18, 13,  1, brick_hi);
    gfxFill(x+9,  y+18,  1, 12, brick_hi);
    gfxFill(x+9,  y+29, 13,  1, brick_dk);
    gfxFill(x+21, y+18,  1, 12, brick_dk);
    // Right half-brick
    gfxFill(x+24, y+18,  8, 12, brick);
    gfxFill(x+24, y+18,  8,  1, brick_hi);
    gfxFill(x+24, y+29,  8,  1, brick_dk);
    gfxFill(x+24, y+18,  1, 12, brick_hi);
  }

  function drawQblock(x, y, used) {
    const border = 0x8B4513;
    const face   = used ? 0x8B4513 : 0xE0A040;
    gfxFill(x, y, 32, 32, face);
    gfxFill(x, y,    32, 2, border);
    gfxFill(x, y+30, 32, 2, border);
    gfxFill(x, y,    2,  32, border);
    gfxFill(x+30, y, 2,  32, border);
    if (!used) {
        gfxFill(x+12, y+8,  8, 2, 0xFFFFFF);
        gfxFill(x+12, y+10, 2, 6, 0xFFFFFF);
        gfxFill(x+18, y+10, 2, 4, 0xFFFFFF);
        gfxFill(x+12, y+16, 8, 2, 0xFFFFFF);
        gfxFill(x+18, y+18, 2, 2, 0xFFFFFF);
        gfxFill(x+14, y+22, 4, 2, 0xFFFFFF);
    }
  }

  function drawPipe(x, y, kind) {
    const dark  = 0x0E5A12;
    const mid   = 0x10A050;
    const light = 0x4FE05F;
    const top = (kind == SPR.PIPE_TL || kind == SPR.PIPE_TR);
    const left = (kind == SPR.PIPE_TL || kind == SPR.PIPE_BL);
    if (top) {
        // Lip extends 4px wider in real Mario; here we keep within tile.
        gfxFill(x, y, 32, 8, mid);
        gfxFill(x, y, 32, 2, dark);
        gfxFill(x+2, y+2, 4, 4, light);
        gfxFill(x, y+6, 32, 2, dark);
        gfxFill(x, y+8, 32, 24, mid);
        gfxFill(x+2, y+8, 4, 24, light);
        if (left) gfxFill(x+30, y+8, 2, 24, dark);
        else      gfxFill(x, y+8, 2, 24, dark);
    } else {
        gfxFill(x, y, 32, 32, mid);
        gfxFill(x+2, y, 4, 32, light);
        if (left) gfxFill(x+30, y, 2, 32, dark);
        else      gfxFill(x, y, 2, 32, dark);
    }
  }

  function drawWalker(x, y, dead, frame) {
    const body_hi = 0xB5651D;
    const body    = 0x8B4513;
    const body_dk = 0x5C2E0D;
    const dark    = 0x40220A;
    const eye_w   = 0xF0F0F0;
    const eye_b   = 0x202020;
    const teeth   = 0xF8E8C8;

    if (dead) {
        // Squashed flat
        gfxFill(x+2,  y+24, 28, 6, body);
        gfxFill(x+4,  y+22, 24, 2, body_hi);
        gfxFill(x+2,  y+28, 28, 2, body_dk);
        return;
    }

    // Head/cap dome (rows 0-13)
    gfxFill(x+8,  y+2,  16, 2, body);             // dome top
    gfxFill(x+6,  y+4,  20, 2, body);
    gfxFill(x+4,  y+6,  24, 2, body);
    gfxFill(x+2,  y+8,  28, 6, body);
    gfxFill(x+4,  y+4,  4,  2, body_hi);          // top-left highlight
    gfxFill(x+4,  y+8,  4,  4, body_hi);          // side highlight
    gfxFill(x+22, y+12, 6,  2, body_dk);          // bottom-right shadow

    // Furrowed brow (angry look)
    gfxFill(x+6,  y+9,  4, 2, dark);
    gfxFill(x+22, y+9,  4, 2, dark);

    // Eyes
    gfxFill(x+8,  y+10, 4, 4, eye_w);
    gfxFill(x+20, y+10, 4, 4, eye_w);
    gfxFill(x+10, y+11, 2, 3, eye_b);             // pupils — centered, looking forward
    gfxFill(x+22, y+11, 2, 3, eye_b);

    // Body underneath head (rows 14-23)
    gfxFill(x+6,  y+14, 20, 8, body);
    gfxFill(x+6,  y+14,  2, 8, body_dk);          // left shadow
    gfxFill(x+24, y+14,  2, 8, body_dk);          // right shadow

    // Small fangs/teeth on lower face
    gfxFill(x+10, y+20, 2, 2, teeth);
    gfxFill(x+14, y+20, 2, 2, teeth);
    gfxFill(x+18, y+20, 2, 2, teeth);

    // Feet — alternate position between the two walk frames
    if (frame == 0) {
        // Feet apart (mid-stride)
        gfxFill(x+2,  y+24, 10, 6, dark);
        gfxFill(x+20, y+24, 10, 6, dark);
        gfxFill(x+2,  y+28, 10, 2, body_dk);
    } else {
        // Feet together (other side of stride)
        gfxFill(x+6,  y+24, 8, 6, dark);
        gfxFill(x+18, y+24, 8, 6, dark);
        gfxFill(x+6,  y+28, 8, 2, body_dk);
    }
  }

  function drawCoin(x, y, frame) {
    const gold     = 0xFCE000;
    const gold_hi  = 0xFFF8A0;
    const gold_dk  = 0xA07000;
    const outline  = 0x402800;

    const cx = x + 16;     // center
    const cy = y + 16;
    if (frame == 0 || frame == 4) {
        // Full face — round disc with shine
        gfxFill(cx-6, cy-12, 12, 2, outline);     // top arc
        gfxFill(cx-8, cy-10, 16, 2, outline);
        gfxFill(cx-9, cy-8,  18, 16, gold);       // body
        gfxFill(cx-7, cy-10, 14, 2, gold);
        gfxFill(cx-8, cy+6,  16, 2, outline);
        gfxFill(cx-6, cy+10, 12, 2, outline);
        gfxFill(cx-2, cy-6,  2, 12, gold_hi);     // vertical shine
        gfxFill(cx+4, cy-4,  2,  8, gold_dk);     // shadow band
    } else if (frame == 1) {
        // Narrower — 3/4 view
        gfxFill(cx-4, cy-12,  8, 2, outline);
        gfxFill(cx-5, cy-10, 10, 2, outline);
        gfxFill(cx-6, cy-8,  12, 16, gold);
        gfxFill(cx-5, cy+6,  10, 2, outline);
        gfxFill(cx-4, cy+10,  8, 2, outline);
        gfxFill(cx-1, cy-6,   2, 12, gold_hi);
        gfxFill(cx+3, cy-4,   1,  8, gold_dk);
    } else if (frame == 2) {
        // Edge-on — thin bar
        gfxFill(cx-2, cy-12, 4, 24, gold_dk);
        gfxFill(cx-1, cy-10, 2, 20, gold);
    } else {
        // Narrower opposite — 3/4 view mirrored
        gfxFill(cx-4, cy-12,  8, 2, outline);
        gfxFill(cx-5, cy-10, 10, 2, outline);
        gfxFill(cx-6, cy-8,  12, 16, gold);
        gfxFill(cx-5, cy+6,  10, 2, outline);
        gfxFill(cx-4, cy+10,  8, 2, outline);
        gfxFill(cx-3, cy-6,   2, 12, gold_hi);
        gfxFill(cx+1, cy-4,   1,  8, gold_dk);
    }
  }

  function drawFlagPole(x, y) {
    const dark = 0x404040;
    const silver = 0xC0C0C0;
    gfxFill(x+15, y, 2, 32, silver);
    gfxFill(x+14, y, 4, 4, dark);
  }

  function drawFlag(x, y) {
    const red    = 0xE52521;
    const dark   = 0x6B2F0F;
    const silver = 0xC0C0C0;
    gfxFill(x+15, y, 2, 32, silver);
    gfxFill(x+2,  y+4, 14, 12, red);
    gfxFill(x+2,  y+4, 14, 2, dark);
    gfxFill(x+2,  y+14, 14, 2, dark);
    gfxFill(x+2,  y+4, 2,  12, dark);
  }

  function drawCastle(x, y) {
    const stone = 0xC0C0C0;
    const dark  = 0x606060;
    const door  = 0x202020;
    gfxFill(x, y, 32, 32, stone);
    gfxFill(x, y, 32, 2, dark);
    gfxFill(x, y+30, 32, 2, dark);
    gfxFill(x, y, 2, 32, dark);
    gfxFill(x+30, y, 2, 32, dark);
    gfxFill(x+0,  y, 4, 6, dark);
    gfxFill(x+8,  y, 4, 6, dark);
    gfxFill(x+20, y, 4, 6, dark);
    gfxFill(x+28, y, 4, 6, dark);
    gfxFill(x+12, y+12, 8, 20, door);
  }

  function drawCloud(x, y) {
    const white = 0xFFFFFF;
    gfxFill(x+8,  y+4,  16, 16, white);
    gfxFill(x+4,  y+8,  24, 8,  white);
    gfxFill(x,    y+12, 32, 4,  white);
  }

  function drawSprite(sprite, x, y, dir, frame) {
    switch (sprite) {
      case SPR.MARIO:       drawHero(x, y, dir, frame); break;
      case SPR.MARIO_R:     drawHero(x, y,  1,  frame); break;
      case SPR.MARIO_L:     drawHero(x, y, -1,  frame); break;
      case SPR.GROUND:      drawGround(x, y); break;
      case SPR.BRICK:       drawBrick(x, y); break;
      case SPR.QBLOCK:      drawQblock(x, y, 0); break;
      case SPR.USED_BLOCK:  drawQblock(x, y, 1); break;
      case SPR.PIPE_TL:
      case SPR.PIPE_TR:
      case SPR.PIPE_BL:
      case SPR.PIPE_BR:     drawPipe(x, y, sprite); break;
      case SPR.GOOMBA:      drawWalker(x, y, 0, frame); break;
      case SPR.DEAD_GOOMBA: drawWalker(x, y, 1, frame); break;
      case SPR.COIN:        drawCoin(x, y, frame); break;
      case SPR.FLAG_POLE:   drawFlagPole(x, y); break;
      case SPR.FLAG:        drawFlag(x, y); break;
      case SPR.CASTLE:      drawCastle(x, y); break;
      case SPR.CLOUD:       drawCloud(x, y); break;
      default: break;
    }
  }

  function gfxDraw(sprite, x, y)                    { if (ctx) drawSprite(sprite, x, y, 1, 0);     return 0; }
  function gfxDrawDir(sprite, x, y, dir)            { if (ctx) drawSprite(sprite, x, y, dir, 0);   return 0; }
  function gfxDrawAnim(sprite, x, y, dir, frame)    { if (ctx) drawSprite(sprite, x, y, dir, frame); return 0; }

  // -------------------------------------------------------------------------
  // Text — uses Canvas's built-in fillText with a monospace font. The native
  // build's gfx_text takes int-encoded bytes piggybacked via gfx_text_set;
  // we mirror that contract so mario.alc doesn't need to change.
  // -------------------------------------------------------------------------
  function gfxTextSet(idx, ch) {
    if (idx < 0 || idx >= textBuf.length) return 0;
    textBuf[idx] = ch & 0xff;
    return 0;
  }
  const g_glyphs = [
    {code: ' '.charCodeAt(0), rows: [0,0,0,0,0,0,0,0]},
    {code: '!'.charCodeAt(0), rows: [0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00]},
    {code: '-'.charCodeAt(0), rows: [0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00]},
    {code: ':'.charCodeAt(0), rows: [0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00]},
    {code: '.'.charCodeAt(0), rows: [0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00]},
    {code: 'x'.charCodeAt(0), rows: [0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00]},
    {code: '0'.charCodeAt(0), rows: [0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00]},
    {code: '1'.charCodeAt(0), rows: [0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00]},
    {code: '2'.charCodeAt(0), rows: [0x3C,0x66,0x06,0x1C,0x30,0x66,0x7E,0x00]},
    {code: '3'.charCodeAt(0), rows: [0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00]},
    {code: '4'.charCodeAt(0), rows: [0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00]},
    {code: '5'.charCodeAt(0), rows: [0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00]},
    {code: '6'.charCodeAt(0), rows: [0x3C,0x60,0x60,0x7C,0x66,0x66,0x3C,0x00]},
    {code: '7'.charCodeAt(0), rows: [0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00]},
    {code: '8'.charCodeAt(0), rows: [0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00]},
    {code: '9'.charCodeAt(0), rows: [0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00]},
    {code: 'A'.charCodeAt(0), rows: [0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00]},
    {code: 'B'.charCodeAt(0), rows: [0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00]},
    {code: 'C'.charCodeAt(0), rows: [0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00]},
    {code: 'D'.charCodeAt(0), rows: [0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00]},
    {code: 'E'.charCodeAt(0), rows: [0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00]},
    {code: 'F'.charCodeAt(0), rows: [0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00]},
    {code: 'G'.charCodeAt(0), rows: [0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00]},
    {code: 'H'.charCodeAt(0), rows: [0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00]},
    {code: 'I'.charCodeAt(0), rows: [0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00]},
    {code: 'K'.charCodeAt(0), rows: [0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00]},
    {code: 'L'.charCodeAt(0), rows: [0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00]},
    {code: 'M'.charCodeAt(0), rows: [0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00]},
    {code: 'N'.charCodeAt(0), rows: [0x63,0x73,0x7B,0x6F,0x67,0x63,0x63,0x00]},
    {code: 'O'.charCodeAt(0), rows: [0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00]},
    {code: 'P'.charCodeAt(0), rows: [0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00]},
    {code: 'R'.charCodeAt(0), rows: [0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00]},
    {code: 'S'.charCodeAt(0), rows: [0x3E,0x60,0x60,0x3C,0x06,0x06,0x7C,0x00]},
    {code: 'T'.charCodeAt(0), rows: [0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00]},
    {code: 'U'.charCodeAt(0), rows: [0x66,0x66,0x66,0x66,0x66,0x66,0x3E,0x00]},
    {code: 'V'.charCodeAt(0), rows: [0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00]},
    {code: 'W'.charCodeAt(0), rows: [0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00]},
    {code: 'Y'.charCodeAt(0), rows: [0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00]},
    {code: 'Z'.charCodeAt(0), rows: [0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00]},
  ];

  function findGlyph(code) {
    if (code >= 'a'.charCodeAt(0) && code <= 'z'.charCodeAt(0)) code -= 32;
    for (let i = 0; i < g_glyphs.length; i++) {
        if (g_glyphs[i].code === code) return g_glyphs[i];
    }
    return null;
  }

  function drawGlyph(g, x, y, scale, rgb) {
    for (let row = 0; row < 8; row++) {
        const bits = g.rows[row];
        for (let col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                gfxFill(x + col * scale, y + row * scale, scale, scale, rgb);
            }
        }
    }
  }

  function gfxText(x, y, len, scale, rgb) {
    if (!ctx) return 0;
    if (scale < 1) scale = 1;
    if (len < 0) len = 0;
    if (len > textBuf.length) len = textBuf.length;
    let cx = x;
    for (let i = 0; i < len; i++) {
        const gl = findGlyph(textBuf[i]);
        if (gl) drawGlyph(gl, cx, y, scale, rgb);
        cx += 9 * scale;
    }
    return 0;
  }

  function gfxTextInt(x, y, value, scale, rgb) {
    if (!ctx) return 0;
    if (scale < 1) scale = 1;
    const str = String(value | 0);
    let cx = x;
    for (let i = 0; i < str.length; i++) {
        const gl = findGlyph(str.charCodeAt(i));
        if (gl) drawGlyph(gl, cx, y, scale, rgb);
        cx += 9 * scale;
    }
    return 0;
  }

  // -------------------------------------------------------------------------
  // Time
  // -------------------------------------------------------------------------
  function gfxNowMs() {
    const now = (typeof performance !== "undefined" ? performance.now() : Date.now());
    return ((now - startMs) | 0);
  }

  // NOTE: gfx_sleep_ms is intentionally NOT implemented here. addFunction'd
  // JS shims can't unwind alcove's eval frames through Asyncify, so any
  // setTimeout/Promise-based wait here would not suspend the synchronous
  // game loop in mario.alc — the page would freeze. The web build instead
  // aliases gfx-sleep-ms to alcove's C-side (sleep-ms N) builtin, which
  // calls emscripten_sleep() and suspends the whole WASM stack cleanly.
  // See mario.alc's `(when (web?) (= gfx-sleep-ms sleep-ms))` line.

  // -------------------------------------------------------------------------
  // Audio — Web Audio scaffolding. The note tables and SFX recipes from
  // mario_gfx.c are the user's own composition; transcribe them here when
  // wanting the full chiptune. For now these are no-op stubs that prove the
  // bridge end-to-end; gfx-music-load-file + gfx-music-play-file fully work
  // for any audio file you ship alongside the page.
  // -------------------------------------------------------------------------
  function ensureAudio() {
    if (audioCtx) return audioCtx;
    const AC = global.AudioContext || global.webkitAudioContext;
    if (!AC) return null;
    audioCtx = new AC();
    armAudioGesture();
    return audioCtx;
  }

  // Browser autoplay policy: AudioContext.state is "suspended" until a
  // user gesture. Install a one-shot listener that resumes it on the
  // first click/keydown anywhere on the page. Idempotent.
  function armAudioGesture() {
    if (audioGestureArmed) return;
    audioGestureArmed = true;
    const resume = () => {
      if (audioCtx && audioCtx.state === "suspended") {
        audioCtx.resume().catch(() => {});
      }
      document.removeEventListener("click", resume, true);
      document.removeEventListener("keydown", resume, true);
      document.removeEventListener("touchstart", resume, true);
    };
    document.addEventListener("click", resume, true);
    document.addEventListener("keydown", resume, true);
    document.addEventListener("touchstart", resume, true);
  }

  function actuallyPlay(loops) {
    if (!audioCtx || !musicBuffer) return -1;
    if (musicSource) { try { musicSource.stop(); } catch (e) {} musicSource = null; }
    const src = audioCtx.createBufferSource();
    src.buffer = musicBuffer;
    src.loop = (loops !== 0);
    const gain = audioCtx.createGain();
    gain.gain.value = 0.5;
    src.connect(gain).connect(audioCtx.destination);
    src.start();
    musicSource = src;
    return 0;
  }

  function gfxMusicStart() {
    // Procedural synth start — left as a stub. To implement: build oscillator
    // chains (square + triangle), schedule frequency changes per the note
    // tables from mario_gfx.c. Store nodes in musicNodes for stop/cleanup.
    ensureAudio();
    return 0;
  }
  function gfxMusicStop() {
    if (musicNodes) {
      try { musicNodes.forEach(function (n) { try { n.stop(); } catch (e) {} }); } catch (e) {}
      musicNodes = null;
    }
    return 0;
  }
  function gfxSfx(_kind) {
    // SFX trigger stub. Implement by reading sfx_table entries and scheduling
    // short oscillator bursts on the shared AudioContext.
    ensureAudio();
    return 0;
  }

  // File music — uses fetch + decodeAudioData. The path comes through as a
  // C string pointer; we read it via Module.UTF8ToString in the bridge.
  // Load is async, but mario.alc calls (gfx-music-play-file) immediately
  // after (gfx-music-load-file) — if the buffer is still loading we queue
  // the play and trigger it when decodeAudioData resolves.
  function gfxMusicLoadFile(path) {
    const ac = ensureAudio();
    if (!ac || !path) return 0;
    const url = path.replace(/^\.\//, "");
    fetch(url)
      .then(function (r) { if (!r.ok) throw new Error("fetch " + url + " → " + r.status); return r.arrayBuffer(); })
      .then(function (buf) { return ac.decodeAudioData(buf); })
      .then(function (decoded) {
        musicBuffer = decoded;
        if (pendingPlay) {
          const p = pendingPlay; pendingPlay = null;
          actuallyPlay(p.loops);
        }
      })
      .catch(function (e) {
        console.error("mario_gfx: load music failed:", e);
        pendingPlay = null;
      });
    return 1; // optimistic: the load is in flight
  }

  function gfxMusicPlayFile(loops) {
    if (!audioCtx) return -1;
    if (musicBuffer) return actuallyPlay(loops);
    pendingPlay = { loops: loops };
    return 0;
  }

  function gfxMusicUnloadFile() {
    if (musicSource) { try { musicSource.stop(); } catch (e) {} musicSource = null; }
    musicBuffer = null;
    pendingPlay = null;
    return 0;
  }

  // -------------------------------------------------------------------------
  // Bridge: register each function above as an alcove builtin.
  // -------------------------------------------------------------------------
  function makeRegistrar(Module) {
    const argInt    = Module.cwrap("alcove_arg_int",    "number", ["number", "number", "number"]);
    const argStrPtr = Module.cwrap("alcove_arg_string", "number", ["number", "number", "number"]);
    const makeInt   = Module.cwrap("alcove_make_int",   "number", ["number"]);
    const register  = Module.cwrap("alcove_register_cmd", "number", ["string", "number", "number"]);

    // `spec` is a string of arg types: "" = no args, "i" = int, "s" = string.
    function wrap(jsFn, spec) {
      const fnPtr = Module.addFunction(function (e, env) {
        const args = [];
        for (let i = 0; i < spec.length; i++) {
          if (spec[i] === "i") {
            args.push(argInt(e, env, i));
          } else { // "s"
            const ptr = argStrPtr(e, env, i);
            args.push(ptr ? Module.UTF8ToString(ptr) : null);
          }
        }
        const r = jsFn.apply(null, args);
        // gfxSleepMs returns whatever Asyncify.handleAsync returns; treat
        // anything non-numeric as 0 (the C-side gfx_sleep_ms returns 0).
        return makeInt(typeof r === "number" ? (r | 0) : 0);
      }, "iii");
      return fnPtr;
    }

    return function (name, jsFn, spec) {
      const ptr = wrap(jsFn, spec);
      register(name, ptr, 0);
    };
  }

  // -------------------------------------------------------------------------
  // Public entry point: call once after the WASM Module is ready.
  // -------------------------------------------------------------------------
  global.marioGfxRegister = function (Module) {
    const reg = makeRegistrar(Module);
    reg("gfx-init",               gfxInit,             "ii");
    reg("gfx-quit",               gfxQuit,             "");
    reg("gfx-pump",               gfxPump,             "");
    reg("gfx-left",               gfxLeft,             "");
    reg("gfx-right",              gfxRight,            "");
    reg("gfx-jump",               gfxJump,             "");
    reg("gfx-quit-pressed",       gfxQuitPressed,      "");
    reg("gfx-should-close",       gfxShouldClose,      "");
    reg("gfx-clear",              gfxClear,            "i");
    reg("gfx-present",            gfxPresent,          "");
    reg("gfx-fill",               gfxFill,             "iiiii");
    reg("gfx-draw",               gfxDraw,             "iii");
    reg("gfx-draw-dir",           gfxDrawDir,          "iiii");
    reg("gfx-draw-anim",          gfxDrawAnim,         "iiiii");
    reg("gfx-text-set",           gfxTextSet,          "ii");
    reg("gfx-text",               gfxText,             "iiiii");
    reg("gfx-text-int",           gfxTextInt,          "iiiii");
    reg("gfx-now-ms",             gfxNowMs,            "");
    // gfx-sleep-ms is intentionally NOT registered from JS — mario.alc's
    // web branch aliases it to alcove's C-side (sleep-ms) which calls
    // emscripten_sleep() and yields properly under Asyncify. A JS shim
    // here would be selected over alcove's reserved_symbol lookup and
    // freeze the browser (addFunction shims can't unwind alcove's eval
    // frames through Asyncify).
    reg("gfx-music-start",        gfxMusicStart,       "");
    reg("gfx-music-stop",         gfxMusicStop,        "");
    reg("gfx-sfx",                gfxSfx,              "i");
    reg("gfx-music-load-file",    gfxMusicLoadFile,    "s");
    reg("gfx-music-play-file",    gfxMusicPlayFile,    "i");
    reg("gfx-music-unload-file",  gfxMusicUnloadFile,  "");
  };

})(typeof window !== "undefined" ? window : globalThis);
