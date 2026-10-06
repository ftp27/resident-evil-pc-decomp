# Scripted input tests

The Linux and macOS builds can be driven from a script of controller input,
or record a human session into such a script, and check the game's state as
it goes. A run is deterministic: the same script produces the same frames,
the same positions and the same random numbers on every run and every
machine, and runs far faster than real time.

```
bash tests/run_scenarios.sh                 # every tests/scenarios/*.re1
bash tests/run_scenarios.sh build/macos/residentevil tests/scenarios/attract_demo.re1
```

The runner needs game data (`config.ini` beside the binary pointing `[Assets]
Path` at a USA tree). Each scenario runs in `test-output/<name>/`, where its
`run.log`, captures and state dumps land. Exit status 0 means every scenario
passed; a failed expectation, a crash and a timeout
(`RE1_TEST_TIMEOUT`, default 300 s) are reported separately.

## Why a run is deterministic

Three things decide what the game does, and a test run pins all of them:

- **Input.** Scripted buttons replace the pad word in `ReadPadBoth`
  (`InputSystem.cpp`), the one point every gameplay input passes through. The
  real keyboard is ignored during a replay.
- **Time.** `plat_time_ms()` reads a virtual clock that advances exactly 33 ms
  per main-loop frame. The frame limiter, movie pacing and the PS1 credits all
  read it, so a movie lasts the same number of frames however fast the machine
  is. Audio is mixed once per frame from the main loop instead of by the sound
  card's thread, so "has this sound finished?" polls get frame-exact answers.
- **Randomness.** The game's `rand()` is `re1_rand` (`Globals.cpp`), the MSVC
  generator the 1997 executable used (the PS1 one in Director's Cut mode), on
  every build. It starts from MSVC's default seed; `--seed N` or a `seed N`
  line overrides it.

## Command line

| Option | Meaning |
|---|---|
| `--script FILE` | Replay `FILE`. |
| `--record FILE` | Play normally and record the input into `FILE`. |
| `--record-frames N` | Stop a recording at frame `N`. |
| `--fast` | No real-time pacing (and no sound): run as fast as the machine can. |
| `--seed N` | Seed the game's random generator before the first frame. |
| `--mute` | Mix audio but do not play it. |
| `--hidden` | Create the window hidden. Captures may come back blank on some GL drivers. |

A test run skips the single-instance lock, so several can run at once.

## Script format

One command per line; `#` starts a comment. Frames count main-loop frames from
0, movie frames included.

```
seed 1234                      # optional; also --seed
700   press ACTION             # hold for 3 frames
900   press ACTION 10          # hold for 10 frames
1000  hold UP+RUN 90           # buttons combine with '+'
1100  release                  # drop every held button
1200  capture shot.png         # the game's picture as PNG
1200  capture-full win.png     # the whole window, KeepAspect bars included
1200  dump state.json          # game state as JSON
1200  expect room == 5         # assertion; ops: == != < <= > >=
1300  quit                     # end the run (required)
```

Commands at frame `F` run before frame `F`'s game logic, so `capture`, `dump`
and `expect` see the result of frames `0..F-1`; buttons pressed at `F` are
held during frame `F`.

**Buttons** are the game's own pad word bits (the `g_JoyRemapTbl` output, the
same whatever the key bindings are): `UP DOWN LEFT RIGHT ACTION RUN` (alias
`CANCEL`) `AIM INVENTORY OPTIONS`, or a raw mask like `0x0100`.

**Fields** for `expect`: `frame stage room cut health x y z angle equipped
rand` (the generator state), `flag:N` (scenario flag bit `N`, as the SCD
scripts number it) and `item:ID` (quantity of that item across the player's
six slots).

## Recording a test

```
./residentevil --record mytest.re1
```

Play, then close the window. The file holds the input as `hold` runs and ends
with `expect` lines for the final state (stage, room, position, health, the
random generator state). Replaying it therefore checks that the run reproduced
exactly; add your own `capture` / `expect` lines at the frames that matter.

Only input that reaches the game through the pad word is recorded. That is
all gameplay and menus, but not the debug screens (F-keys) or the options
screen's key-rebinding scan, which read the keyboard directly.

With the default bindings, ACTION is C / Return / Space, RUN is V / Ctrl /
Esc, AIM is X, INVENTORY is Z, OPTIONS is A.

## Limits

- Scripts depend on the game data: a different release or a PS1 audio
  overlay changes timings.
- The Windows build ignores test options (`plat_test_filter_pad` is a
  pass-through there), so recordings are made and replayed on Linux or macOS.
  The game logic and the random generator are the same on every build, so a
  divergence between Linux and macOS replays is a bug in one of them.
