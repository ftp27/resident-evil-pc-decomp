// testmode.h - scripted / recorded input runs (docs/TESTING.md).
//
// A test run drives the game from a script of per-frame pad words instead of
// the keyboard, or records a human session into such a script. Either way the
// run switches to a virtual clock: plat_time_ms() advances exactly one frame
// interval per main-loop frame, and audio is mixed per frame instead of by the
// sound card's thread. Everything the game observes (the frame limiter, movie
// pacing, sound-finished polls, rand) then depends only on the frame number,
// so the same script reproduces the same run on every machine and build.
#pragma once

#include "../types.h"

// Consume one test argument at argv[*i] (advancing *i past its values).
// Returns false when argv[*i] is not a test argument.
bool test_parse_arg(int argc, char** argv, int* i);

// After the config is loaded, before audio/video init. Loads the script,
// opens the recording, applies the seed. Returns false on a bad script.
bool test_init(void);

bool test_requested(void);       // --script or --record was given (before test_init)
bool test_active(void);          // a script or recording run is in progress
bool test_replaying(void);       // inputs come from a script (ignore keyboard)
bool test_fast(void);            // run as fast as possible (no real-time pacing)
bool test_hidden(void);          // create the window hidden
bool test_mute(void);            // mix audio but do not send it to the device

// Virtual clock, valid while test_active().
DWORD test_clock_ms(void);

// Start of main-loop frame `frame`: runs the commands scheduled for it
// (capture / dump / expect read the state left by frames 0..frame-1), then
// latches the pad word scripted for it. Returns false when the run is over.
bool test_frame_begin(int frame);

// End of a frame: advances the virtual clock and, unless fast, sleeps to keep
// real time.
void test_frame_end(void);

// The window was closed. A recording ends at the next frame boundary (with
// its final-state checks); anything else should just stop.
void test_request_stop(void);

// Process exit code for the run (0 = every expectation held).
int test_exit_code(void);

// Flush and close the recording / report.
void test_shutdown(void);

// audio.cpp: during a test run the mixer is driven from here, one frame of
// samples per call, instead of by the sound card's callback thread.
void plat_audio_push_tick(int ms);
