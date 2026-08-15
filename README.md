# Battle Squadron Amiga decomp/recomp (in progress)

This is the Amiga version of **Battle Squadron: The Destruction of the Barrax
Empire**, recovered from the user's WHDLoad V1.8 archive on a Retroid Pocket
Flip2. It is separate from the mistaken Mega Drive prototype examined earlier.

## Current result

- The untouched `BattleSquadron_v1.8_0941.lha` was copied from the Retroid and
  verified on both sides with SHA-256.
- The WHDLoad install contains the original loader and 23 overlay descriptors.
  `LODTAK` is referenced but absent from this install; all other files are
  present.
- Fifteen overlays use the game's backward BOND compressor. The original
  `$AB46` routine has been translated into a bounds-checked Python depacker,
  including its embedded XOR checksum.
- Canonical runtime images are generated for every available overlay. See
  [`docs/module-map.json`](docs/module-map.json) for their actual Amiga load
  addresses, packed sizes, runtime sizes, staging addresses, and modes.
- `LOADER`, the depacked `LODGAM` game-audio overlay, and the raw `LODCOM`
  common-audio overlay have conservative 68000 disassemblies.
- Recursive discovery currently proves 32,016 loader code bytes (47.4%), 2,404
  game-audio code bytes (6.3%), and 1,494 common-audio code bytes (7.4%). The
  common-audio increase comes from the interrupt handler installed by its init
  routine and the channel-update routine reached from that handler.
- All three sources currently reassemble byte-for-byte.
- A playable **transitional Musashi/OCS reference runner** boots the original
  loader at `$100`,
  services its named-file interface from the WHDLoad drawer, and executes the
  original BOND depacker and game code. Its host implements the OCS blitter,
  copper, six bitplanes/dual playfields, attached sprites, two joystick ports,
  keyboard serial input, CIA-B music timer, and four-channel Paula mixing.
- The raylib frontend presents the original 320x256 display at 50 Hz with
  stereo sound, keyboard and gamepad control. It preserves the spoken
  "Welcome to Battle Squadron" introduction and survives the music handoff
  into gameplay.
- A 50,000-frame deterministic smoke run reaches 20 overlay loads and more
  than two million blits without stopping. A separate autofire integration
  test crosses the title-to-game load while checking that music remains live.

The original assets and 68000 program remain byte-exact. The currently
playable host is a focused compatibility layer around them, not yet the
finished recompilation: it still executes game code through Musashi.

## Genuine recompilation status

The separate recompilation path never links Musashi. It includes the loader's
`$AB46` BOND decompressor, complete overlay descriptor/load path, and a direct
native translation from boot through a complete 8,000-frame recorded combat
run. It reaches the demo-exit boundary at loader PC `$D16` after 221,419
fail-closed dispatch steps and 12 real overlay loads. No unknown PC or object
or projectile type is silently interpreted.

The translated path now covers music-driver state initialisation, both palette
algorithms, the six-page text intro/font compositor, title keyboard-help
rendering, object relocation, work-RAM clearing, game-mode/player
initialisation, the complete 256-frame transform scroll, terrain and wave
progression, player movement/respawn/primary fire/Nova state, the active object
types reached by the demo, scheduled enemy shots, projectile types `$00`,
`$01`, `$03`, `$04`, `$06`, `$07`, and `$08`, projectile impacts, and the
debris/effect pool. Type `$00` includes its chained velocity-script format and
is exercised by the real new-game opening rather than the attract script. The
live path also covers the opening type-`$01` ground launcher and its growing
type-`$07` shot.
Object graphics are copied directly into five native bitplanes; projectiles
use a direct five-plane cookie-cut compositor with clipping. A focused test
checks its mask/source/destination result on every plane.

Oracle checkpoints compare registers and focused memory images at each major
bootstrap and first-frame boundary, including every screen plane on transform
frames one, two and 256. The first scripted projectile allocation is also
byte-exact. Later combat is currently deterministic and coverage-gated, but
the newer hand translations still need wider reference-parity checkpoints.

That longer parity gate caught and fixed a real native scrolling defect: the
terrain-column loop must clear all of `D1` on every map-word fetch. Preserving
its upper word made frame one look correct but corrupted subsequent columns.

Native C also parses all 23 descriptors, loads all 22 files present in this
install, and reproduces every one of the 15 packed runtime images byte-for-byte.

```sh
make recomp-test
```

The current no-Musashi visual milestone has a live-input raylib preview:

```sh
make show-recomp
```

That preview now joins the exact spoken `LODSPE` introduction, title/start
screen, the real `$926-$A9E` new-game initialisation, its opening starfield,
and live combat. The native transition loads `LODGAM`/`LODS0S`, initialises
both player records, and performs the original terrain-only prefill; it does
not seed the attract map or run demo wave spawns. A reusable native Paula
PCM component consumes translated sample descriptors and has no crossfeed or
echo filter. Presentation is clocked at the game's completed `$AA0` edge;
`$1078` advances twice per terrain update and using it as a display clock was
the cause of the earlier duplicated-frame/25 Hz map motion. The intermediate
`$B54` edge is also unsafe because object restoration has not run yet. A visible-frame
test now proves that consecutive terrain images differ by exactly one pixel
across the unobstructed playfield. It also runs 3,000 real-game frames while
requiring every Copper bitplane pointer to remain attached to the current
terrain-ring row; this catches stale upper-register state that previously
made the Copper display unrelated chip RAM as apparently random map data.
The OCS renderer also preserves fine-scroll carry pixels outside Battle
Squadron's nominal 304-pixel DMA span, preventing a black 16-pixel edge strip
from disappearing and reappearing at each coarse-word rollover.
Title DMA and its CIA timer are stopped before `LODJOY` replaces the title
driver, preventing a latched tone during the transition.

There is also a reusable terrain extractor/player. Battle Squadron's adapter
decodes each real 384-pixel map row, verifies it against the row written into
the 256-line five-plane playfield ring, and records source address, tile phase,
world progress, and coarse/fine horizontal scroll. The default extraction
checks 2,048 consecutive rows and writes both a replay trace and an unrolled
PPM map:

```sh
make map-test
make map-extract
make show-map
```

The player advances one scanline at a time. `C` toggles a coarse-word-only
simulation of the familiar 16-pixel block-jump bug; `G` overlays tile seams.
A separate native scenery-object layer fills the map's turret/gun sockets;
`O` toggles it without changing the exact terrain layer. The capture occurs
before projectiles and player sprites, avoiding moving trails in the extracted
map. Left/right pans horizontally one pixel at a time (Shift accelerates),
up/down scrubs rows, and `A` restores the recorded automatic camera. Colours
come from live Copper RGB4 writes rather than guesses from title
memory. See [`docs/AMIGA_RECOMP_KIT.md`](docs/AMIGA_RECOMP_KIT.md) for the
title-neutral trace/playback and Copper-palette contracts.

This is not yet a complete playable recompilation claim. The recorded combat
run and live-input preview work without an interpreter fallback, and exact
intro PCM is connected. The full LODMUS/LODGAM timer-driven music and SFX
sequencers plus the `$D16` demo-exit/title transition still have to be
translated.
Projectile/object types not reached by the current 8,000-frame run remain
fail-closed. The recompilation tests enforce a link audit with no Musashi or
`m68k_*` symbols.

## Reproduce it

Requirements are Python 3 with Capstone, 7-Zip, IRA, and vasm
(`vasmm68k_mot`). The copyrighted WHDLoad archive is not part of the source
tree.

```sh
cd /home/jon/BattleSquadron-Amiga
./regen_asm.sh
```

The shorter integrity gate is:

```sh
make verify
```

The transitional reference runner reuses the sibling SWIV project's Musashi
source:

```sh
make native-smoke
```

Build and launch the playable raylib frontend with:

```sh
make playable
make run
```

Player 1 uses the arrow keys, Space/Ctrl/Enter to fire, and X/Shift for Nova.
Player 2 uses WASD, Alt/C to fire, and V/Tab for Nova. Gamepads use the D-pad
or left stick; A/X/Start/right trigger fire and B/Y/shoulders activate Nova.

Tests are split into fast unit checks and deterministic end-to-end execution:

```sh
make unit-test
make integration-test
# or both
make test
```

See [`docs/TESTING.md`](docs/TESTING.md) for the covered failure cases and
native milestone assertions.

Generated originals, depacked modules, build output, and the extracted WHDLoad
drawer are ignored. The checked-in assembly and module map can always be
regenerated from `/home/jon/Downloads/BattleSquadron_v1.8_0941.lha`.

## Important files

- [`asm/loader.asm`](asm/loader.asm): original system takeover, module loader,
  BOND depacker, and game bootstrap at load address `$100`.
- [`asm/lodgam.asm`](asm/lodgam.asm): canonical game-audio overlay at
  `$246F0`.
- [`asm/lodcom.asm`](asm/lodcom.asm): common overlay at `$3D800`.
- [`tools/depack_bond.py`](tools/depack_bond.py): verified BOND decompressor.
- [`tools/extract_modules.py`](tools/extract_modules.py): parses the loader's
  own module table and produces runtime images.
- [`src/host/amiga.c`](src/host/amiga.c): chip RAM, file loading, CIA, input,
  OCS video/blitter, interrupt, and Paula audio host.
- [`src/host/frontend.c`](src/host/frontend.c): raylib video, audio, keyboard,
  and gamepad frontend.
- [`src/platform/scroll_map.c`](src/platform/scroll_map.c): reusable checked
  map trace, playback, validation, persistence, and PPM export.
- [`src/recomp/bs_map.c`](src/recomp/bs_map.c): Battle Squadron map/tile/ring
  adapter with source-to-playfield pixel parity.
- [`docs/MAP.md`](docs/MAP.md): current code and memory-map notes.
