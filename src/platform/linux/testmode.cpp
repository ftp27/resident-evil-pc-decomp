// testmode.cpp - scripted / recorded input runs (see testmode.h and
// docs/TESTING.md for the script format).
#include "testmode.h"

#include "Globals.h"
#include "marni/MarniDX.h"
#include "platform/platform.h"
#include "system/AssetPath.h"

#include <SDL2/SDL.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

#include <string>
#include <vector>

extern "C" unsigned int re1_rand_state(void);   // Globals.cpp
extern bool g_marniCaptureFullDrawable;          // MarniDX_GL.cpp

namespace {

const int kFrameMs = 33;   // main.cpp kFrameIntervalMs

// ---------------------------------------------------------------------------
// Pad button names. Values are the pad word ReadPadBoth produces (the
// g_JoyRemapTbl output), not PSX controller bits.
// ---------------------------------------------------------------------------
struct ButtonName { const char* name; DWORD mask; };
const ButtonName kButtons[] = {
    { "OPTIONS",   0x0900 },   // before INVENTORY: it contains that bit
    { "UP",        0x1000 },
    { "DOWN",      0x4000 },
    { "LEFT",      0x8000 },
    { "RIGHT",     0x2000 },
    { "ACTION",    0x0080 },
    { "RUN",       0x0040 },
    { "CANCEL",    0x0040 },
    { "AIM",       0x0008 },
    { "INVENTORY", 0x0800 },
};

bool ParseButtons(const char* text, DWORD* out)
{
    DWORD mask = 0;
    std::string s(text);
    size_t start = 0;
    while (start <= s.size()) {
        size_t plus = s.find('+', start);
        std::string tok = s.substr(start, plus == std::string::npos ? std::string::npos
                                                                    : plus - start);
        for (char& c : tok) c = (char)toupper((unsigned char)c);
        if (tok.empty()) return false;
        bool found = false;
        if (tok.size() > 2 && tok[0] == '0' && tok[1] == 'X') {
            mask |= (DWORD)strtoul(tok.c_str(), NULL, 16);
            found = true;
        } else {
            for (const ButtonName& b : kButtons) {
                if (tok == b.name) { mask |= b.mask; found = true; break; }
            }
        }
        if (!found) return false;
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    *out = mask;
    return true;
}

std::string ButtonsToText(DWORD mask)
{
    std::string out;
    for (const ButtonName& b : kButtons) {
        if (strcmp(b.name, "CANCEL") == 0) continue;   // RUN already covers it
        if ((mask & b.mask) == b.mask) {
            if (!out.empty()) out += '+';
            out += b.name;
            mask &= ~b.mask;
        }
    }
    if (mask != 0) {
        char hex[16];
        snprintf(hex, sizeof(hex), "0x%04X", (unsigned)mask);
        if (!out.empty()) out += '+';
        out += hex;
    }
    return out.empty() ? "0x0000" : out;
}

// ---------------------------------------------------------------------------
// Script
// ---------------------------------------------------------------------------
enum CmdType { CMD_HOLD, CMD_RELEASE, CMD_CAPTURE, CMD_CAPTURE_FULL, CMD_DUMP, CMD_EXPECT, CMD_QUIT };

struct Command {
    int         frame;
    CmdType     type;
    DWORD       mask;      // CMD_HOLD
    int         length;    // CMD_HOLD, frames
    std::string arg;       // path / field
    std::string op;        // CMD_EXPECT
    long        value;     // CMD_EXPECT
    int         line;
};

struct Hold { DWORD mask; int endFrame; };

std::vector<Command> s_cmds;
size_t               s_nextCmd = 0;
std::vector<Hold>    s_holds;

const char* s_saveDir = NULL;     // --save-dir
char        s_tempSaveDir[512] = "";   // per-run empty save folder we own
const char* s_scriptPath = NULL;
const char* s_recordPath = NULL;
bool        s_fast   = false;
bool        s_hidden = false;
bool        s_mute   = false;
bool        s_haveSeed = false;
unsigned    s_seed = 0;

bool  s_active = false;
DWORD s_clockMs = 0;
DWORD s_padWord = 0;
int   s_frame = 0;
int   s_failures = 0;
Uint64 s_realStart = 0;

FILE* s_record = NULL;
DWORD s_recWord = 0;
int   s_recStart = 0;
int   s_recordLimit = 0;       // --record-frames: stop at this frame (0 = never)
bool  s_stopRequested = false; // window closed during a recording

bool ParseScript(const char* path)
{
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "[TEST] cannot open script %s\n", path);
        return false;
    }
    char line[512];
    int lineNo = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), f) != NULL) {
        ++lineNo;
        char* hash = strchr(line, '#');
        if (hash != NULL) *hash = '\0';
        char* tok[8] = {};
        int n = 0;
        for (char* t = strtok(line, " \t\r\n"); t != NULL && n < 8; t = strtok(NULL, " \t\r\n")) {
            tok[n++] = t;
        }
        if (n == 0) continue;

        if (strcmp(tok[0], "seed") == 0 && n == 2) {
            s_seed = (unsigned)strtoul(tok[1], NULL, 0);
            s_haveSeed = true;
            continue;
        }

        Command c = {};
        c.line = lineNo;
        char* end = NULL;
        c.frame = (int)strtol(tok[0], &end, 0);
        if (end == tok[0] || *end != '\0' || n < 2) {
            fprintf(stderr, "[TEST] %s:%d: expected '<frame> <command> ...'\n", path, lineNo);
            ok = false;
            continue;
        }
        const char* cmd = tok[1];
        if ((strcmp(cmd, "press") == 0 && (n == 3 || n == 4)) ||
            (strcmp(cmd, "hold") == 0 && n == 4)) {
            c.type = CMD_HOLD;
            if (!ParseButtons(tok[2], &c.mask)) {
                fprintf(stderr, "[TEST] %s:%d: unknown button in '%s'\n", path, lineNo, tok[2]);
                ok = false;
                continue;
            }
            c.length = (n == 4) ? atoi(tok[3]) : 3;
            if (c.length < 1) c.length = 1;
        } else if (strcmp(cmd, "release") == 0 && n == 2) {
            c.type = CMD_RELEASE;
        } else if (strcmp(cmd, "capture") == 0 && n == 3) {
            c.type = CMD_CAPTURE;
            c.arg = tok[2];
        } else if (strcmp(cmd, "capture-full") == 0 && n == 3) {
            c.type = CMD_CAPTURE_FULL;
            c.arg = tok[2];
        } else if (strcmp(cmd, "dump") == 0 && n == 3) {
            c.type = CMD_DUMP;
            c.arg = tok[2];
        } else if (strcmp(cmd, "expect") == 0 && n == 5) {
            c.type = CMD_EXPECT;
            c.arg = tok[2];
            c.op = tok[3];
            c.value = strtol(tok[4], NULL, 0);
        } else if (strcmp(cmd, "quit") == 0 && n == 2) {
            c.type = CMD_QUIT;
        } else {
            fprintf(stderr, "[TEST] %s:%d: bad command '%s'\n", path, lineNo, cmd);
            ok = false;
            continue;
        }
        s_cmds.push_back(c);
    }
    fclose(f);

    // Stable by frame, so same-frame commands keep their file order.
    for (size_t i = 1; i < s_cmds.size(); ++i) {
        for (size_t j = i; j > 0 && s_cmds[j - 1].frame > s_cmds[j].frame; --j) {
            std::swap(s_cmds[j - 1], s_cmds[j]);
        }
    }
    bool hasQuit = false;
    for (const Command& c : s_cmds) hasQuit |= (c.type == CMD_QUIT);
    if (!hasQuit) {
        fprintf(stderr, "[TEST] %s: no 'quit' command - the run will not end on its own\n", path);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Game state for dump / expect
// ---------------------------------------------------------------------------
bool StateField(const std::string& name, long* out)
{
    if (name == "frame")    { *out = s_frame; return true; }
    if (name == "stage")    { *out = g_stageId; return true; }
    if (name == "room")     { *out = g_roomId; return true; }
    if (name == "cut")      { *out = g_cutId; return true; }
    if (name == "health")   { *out = g_playerEntity.health; return true; }
    if (name == "x")        { *out = g_playerEntity.position.x; return true; }
    if (name == "y")        { *out = g_playerEntity.position.y; return true; }
    if (name == "z")        { *out = g_playerEntity.position.z; return true; }
    if (name == "angle")    { *out = g_playerEntity.directionAngle; return true; }
    if (name == "equipped") { *out = g_EquippedItemId; return true; }
    if (name == "rand")     { *out = (long)re1_rand_state(); return true; }
    if (name.compare(0, 5, "flag:") == 0) {
        // Scenario flag bit N, the bank the SCD scripts test with Flg_ck.
        long bit = strtol(name.c_str() + 5, NULL, 0);
        if (bit < 0 || bit >= 16 * 8) return false;
        *out = Flg_ck((int)O(g_ScenarioFlags), (unsigned)bit) != 0 ? 1 : 0;
        return true;
    }
    if (name.compare(0, 5, "item:") == 0) {
        // Total quantity of an item id in the player's six slots (a slot with
        // quantity 0 still counts as one).
        long id = strtol(name.c_str() + 5, NULL, 0);
        long total = 0;
        for (int i = 0; i < 6; ++i) {
            if (g_ItemsSlots[i].Id == id) total += g_ItemsSlots[i].qty ? g_ItemsSlots[i].qty : 1;
        }
        *out = total;
        return true;
    }
    return false;
}

void WriteHex(FILE* f, const unsigned char* p, int n)
{
    fputc('"', f);
    for (int i = 0; i < n; ++i) fprintf(f, "%02x", p[i]);
    fputc('"', f);
}

void DumpState(const char* path)
{
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "[TEST] cannot write %s\n", path);
        return;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"frame\": %d,\n", s_frame);
    fprintf(f, "  \"stage\": %d,\n  \"room\": %d,\n  \"cut\": %d,\n",
            (int)g_stageId, (int)g_roomId, (int)g_cutId);
    fprintf(f, "  \"player\": { \"x\": %d, \"y\": %d, \"z\": %d, \"angle\": %d, \"health\": %d },\n",
            g_playerEntity.position.x, g_playerEntity.position.y, g_playerEntity.position.z,
            g_playerEntity.directionAngle, g_playerEntity.health);
    fprintf(f, "  \"equipped\": %d,\n", (int)g_EquippedItemId);
    fprintf(f, "  \"items\": [");
    for (int i = 0; i < 6; ++i) {
        fprintf(f, "%s[%d, %d]", i ? ", " : "", g_ItemsSlots[i].Id, g_ItemsSlots[i].qty);
    }
    fprintf(f, "],\n");
    fprintf(f, "  \"scenario_flags\": ");
    WriteHex(f, (const unsigned char*)g_ScenarioFlags, 16);
    fprintf(f, ",\n  \"enemy_flags\": ");
    WriteHex(f, (const unsigned char*)g_EnemiesFlags, 32);
    fprintf(f, ",\n  \"main_state_flags\": %u,\n", (unsigned)g_main_state_flags);
    fprintf(f, "  \"rand_state\": %u\n}\n", re1_rand_state());
    fclose(f);
    printf("[TEST] frame %d: state -> %s\n", s_frame, path);
}

bool Compare(long a, const std::string& op, long b, bool* ok)
{
    *ok = true;
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    if (op == "<")  return a < b;
    if (op == "<=") return a <= b;
    if (op == ">")  return a > b;
    if (op == ">=") return a >= b;
    *ok = false;
    return false;
}

// ---------------------------------------------------------------------------
// PNG writer (stored deflate blocks - no compression, no zlib)
// ---------------------------------------------------------------------------
unsigned Crc32(const unsigned char* p, size_t n, unsigned crc = 0xFFFFFFFFu)
{
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc;
}

void Put32(std::vector<unsigned char>& v, unsigned x)
{
    v.push_back((unsigned char)(x >> 24)); v.push_back((unsigned char)(x >> 16));
    v.push_back((unsigned char)(x >> 8));  v.push_back((unsigned char)x);
}

void Chunk(FILE* f, const char* type, const std::vector<unsigned char>& data)
{
    std::vector<unsigned char> buf;
    Put32(buf, (unsigned)data.size());
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    unsigned crc = Crc32(buf.data() + 4, buf.size() - 4) ^ 0xFFFFFFFFu;
    Put32(buf, crc);
    fwrite(buf.data(), 1, buf.size(), f);
}

bool WritePng(const char* path, const unsigned char* rgba, int w, int h)
{
    FILE* f = fopen(path, "wb");
    if (f == NULL) return false;
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    fwrite(sig, 1, 8, f);

    std::vector<unsigned char> ihdr;
    Put32(ihdr, (unsigned)w); Put32(ihdr, (unsigned)h);
    ihdr.push_back(8); ihdr.push_back(2);   // 8-bit RGB
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    Chunk(f, "IHDR", ihdr);

    // Raw scanlines: filter byte 0 + RGB.
    std::vector<unsigned char> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 3));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const unsigned char* row = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            raw.push_back(row[x * 4 + 0]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4 + 2]);
        }
    }
    std::vector<unsigned char> z;
    z.push_back(0x78); z.push_back(0x01);
    unsigned a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    size_t pos = 0;
    do {
        size_t len = raw.size() - pos;
        if (len > 65535) len = 65535;
        z.push_back(pos + len == raw.size() ? 1 : 0);
        z.push_back((unsigned char)len); z.push_back((unsigned char)(len >> 8));
        z.push_back((unsigned char)~len); z.push_back((unsigned char)(~len >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
        pos += len;
    } while (pos < raw.size());
    Put32(z, (b << 16) | a);
    Chunk(f, "IDAT", z);
    Chunk(f, "IEND", std::vector<unsigned char>());
    fclose(f);
    return true;
}

void Capture(const char* path)
{
    MarniDX* dx = Marni_DX();
    void* pixels = NULL;
    DWORD w = 0, h = 0;
    if (dx == NULL || !dx->CaptureBackbufferToRGBA(&pixels, &w, &h) || pixels == NULL) {
        fprintf(stderr, "[TEST] frame %d: capture failed\n", s_frame);
        return;
    }
    if (WritePng(path, (const unsigned char*)pixels, (int)w, (int)h)) {
        printf("[TEST] frame %d: capture -> %s (%ux%u)\n", s_frame, path, (unsigned)w, (unsigned)h);
    } else {
        fprintf(stderr, "[TEST] cannot write %s\n", path);
    }
    operator_delete(pixels);
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------
void RecordFlushRun(int endFrame)
{
    if (s_record != NULL && s_recWord != 0 && endFrame > s_recStart) {
        fprintf(s_record, "%d hold %s %d\n", s_recStart,
                ButtonsToText(s_recWord).c_str(), endFrame - s_recStart);
    }
    s_recStart = endFrame;
}

// Close a recording at the start of `frame`: the state checks written here
// read the same state a replay's `expect` lines will see at that frame, so
// replaying the file verifies that the run reproduced exactly.
void RecordFinish(int frame, bool withChecks)
{
    if (s_record == NULL) return;
    RecordFlushRun(frame);
    if (withChecks) {
        static const char* kFields[] = { "stage", "room", "x", "y", "z", "angle", "health", "rand" };
        fprintf(s_record, "# final state - replaying this file must reproduce it\n");
        for (const char* name : kFields) {
            long v = 0;
            StateField(name, &v);
            fprintf(s_record, "%d expect %s == %ld\n", frame, name, v);
        }
    }
    fprintf(s_record, "%d quit\n", frame);
    fclose(s_record);
    s_record = NULL;
    printf("[TEST] recording -> %s (%d frames)\n", s_recordPath, frame);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool test_parse_arg(int argc, char** argv, int* i)
{
    const char* a = argv[*i];
    if (strcmp(a, "--script") == 0 && *i + 1 < argc) { s_scriptPath = argv[++*i]; return true; }
    if (strcmp(a, "--record") == 0 && *i + 1 < argc) { s_recordPath = argv[++*i]; return true; }
    if (strcmp(a, "--seed") == 0 && *i + 1 < argc) {
        s_seed = (unsigned)strtoul(argv[++*i], NULL, 0);
        s_haveSeed = true;
        return true;
    }
    if (strcmp(a, "--record-frames") == 0 && *i + 1 < argc) {
        s_recordLimit = atoi(argv[++*i]);
        return true;
    }
    if (strcmp(a, "--save-dir") == 0 && *i + 1 < argc) { s_saveDir = argv[++*i]; return true; }
    if (strcmp(a, "--fast") == 0)   { s_fast = true; return true; }
    if (strcmp(a, "--hidden") == 0) { s_hidden = true; return true; }
    if (strcmp(a, "--mute") == 0)   { s_mute = true; return true; }
    return false;
}

bool test_init(void)
{
    if (s_scriptPath != NULL && s_recordPath != NULL) {
        fprintf(stderr, "[TEST] --script and --record are exclusive\n");
        return false;
    }
    s_active = (s_scriptPath != NULL || s_recordPath != NULL);
    if (!s_active) return true;

    if (s_scriptPath != NULL && !ParseScript(s_scriptPath)) return false;

    if (s_recordPath != NULL) {
        s_record = fopen(s_recordPath, "w");
        if (s_record == NULL) {
            fprintf(stderr, "[TEST] cannot write %s\n", s_recordPath);
            return false;
        }
        fprintf(s_record, "# recorded by residentevil --record\n");
        if (s_haveSeed) fprintf(s_record, "seed %u\n", s_seed);
    }
    // Hermetic saves: a save file left by an earlier run changes the title
    // flow, so every run starts from an empty folder of its own unless one is
    // named (e.g. a fixture for a load-game scenario).
    if (s_saveDir == NULL) {
        const char* tmp = getenv("TMPDIR");
        snprintf(s_tempSaveDir, sizeof(s_tempSaveDir), "%s/re1-test-save-XXXXXX",
                 (tmp != NULL && tmp[0] != '\0') ? tmp : "/tmp");
        if (mkdtemp(s_tempSaveDir) == NULL) {
            fprintf(stderr, "[TEST] cannot create a temporary save folder\n");
            return false;
        }
        SetSaveRoot(s_tempSaveDir);
    } else {
        SetSaveRoot(s_saveDir);
    }

    if (s_haveSeed) re1_srand(s_seed);
    if (s_fast) s_mute = true;   // there is no real time to play sound in

    s_clockMs = 1000;            // non-zero, like a real uptime
    s_realStart = SDL_GetPerformanceCounter();
    printf("[TEST] %s run%s%s\n", s_scriptPath ? "scripted" : "recording",
           s_fast ? ", fast" : "", s_haveSeed ? ", seeded" : "");
    return true;
}

bool test_requested(void) { return s_scriptPath != NULL || s_recordPath != NULL; }
bool test_active(void)    { return s_active; }
bool test_replaying(void) { return s_active && s_scriptPath != NULL; }
bool test_fast(void)      { return s_fast; }
bool test_hidden(void)    { return s_hidden; }
bool test_mute(void)      { return s_mute; }
DWORD test_clock_ms(void) { return s_clockMs; }

bool test_frame_begin(int frame)
{
    s_frame = frame;
    bool keepRunning = true;

    if (s_record != NULL &&
        (s_stopRequested || (s_recordLimit > 0 && frame >= s_recordLimit))) {
        RecordFinish(frame, true);
        return false;
    }

    while (s_nextCmd < s_cmds.size() && s_cmds[s_nextCmd].frame <= frame) {
        const Command& c = s_cmds[s_nextCmd++];
        switch (c.type) {
        case CMD_HOLD:
            s_holds.push_back(Hold{ c.mask, c.frame + c.length });
            break;
        case CMD_RELEASE:
            s_holds.clear();
            break;
        case CMD_CAPTURE:
            Capture(c.arg.c_str());
            break;
        case CMD_CAPTURE_FULL:
            g_marniCaptureFullDrawable = true;
            Capture(c.arg.c_str());
            g_marniCaptureFullDrawable = false;
            break;
        case CMD_DUMP:
            DumpState(c.arg.c_str());
            break;
        case CMD_EXPECT: {
            long actual = 0;
            bool opOk = false;
            if (!StateField(c.arg, &actual)) {
                printf("[TEST] FAIL line %d: unknown field '%s'\n", c.line, c.arg.c_str());
                ++s_failures;
                break;
            }
            bool pass = Compare(actual, c.op, c.value, &opOk);
            if (!opOk) {
                printf("[TEST] FAIL line %d: unknown operator '%s'\n", c.line, c.op.c_str());
                ++s_failures;
            } else if (pass) {
                printf("[TEST] pass frame %d: %s %s %ld (is %ld)\n",
                       frame, c.arg.c_str(), c.op.c_str(), c.value, actual);
            } else {
                printf("[TEST] FAIL frame %d line %d: %s %s %ld, but it is %ld\n",
                       frame, c.line, c.arg.c_str(), c.op.c_str(), c.value, actual);
                ++s_failures;
            }
            break;
        }
        case CMD_QUIT:
            keepRunning = false;
            break;
        }
    }

    DWORD word = 0;
    for (size_t i = 0; i < s_holds.size();) {
        if (frame >= s_holds[i].endFrame) {
            s_holds.erase(s_holds.begin() + (long)i);
        } else {
            word |= s_holds[i].mask;
            ++i;
        }
    }
    s_padWord = word;
    return keepRunning;
}

void test_frame_end(void)
{
    if (!s_active) return;
    s_clockMs += kFrameMs;
    plat_audio_push_tick(kFrameMs);
    if (!s_fast) {
        // Keep the virtual clock from running ahead of real time.
        const Uint64 freq = SDL_GetPerformanceFrequency();
        const Uint64 target = s_realStart + (Uint64)(s_frame + 1) * freq * kFrameMs / 1000;
        for (;;) {
            Uint64 now = SDL_GetPerformanceCounter();
            if (now >= target) break;
            Uint64 ms = (target - now) * 1000 / freq;
            SDL_Delay(ms > 1 ? (Uint32)(ms - 1) : 0);
            if (ms <= 1) break;
        }
    }
}

int test_exit_code(void)
{
    return s_failures == 0 ? 0 : 1;
}

void test_request_stop(void)
{
    s_stopRequested = true;
}

void test_shutdown(void)
{
    // Remove the per-run save folder (only ever the one mkdtemp made).
    if (s_tempSaveDir[0] != '\0') {
        if (DIR* d = opendir(s_tempSaveDir)) {
            while (struct dirent* e = readdir(d)) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                std::string f = std::string(s_tempSaveDir) + "/" + e->d_name;
                unlink(f.c_str());
            }
            closedir(d);
        }
        rmdir(s_tempSaveDir);
        s_tempSaveDir[0] = '\0';
    }

    // A recording that ended any other way (the game quit from its own menu)
    // has no clean frame boundary to check state at: inputs only.
    if (s_record != NULL) RecordFinish(s_frame + 1, false);
    if (s_active && s_scriptPath != NULL) {
        printf("[TEST] %s: %s (%d failure%s)\n", s_scriptPath,
               s_failures == 0 ? "PASS" : "FAIL", s_failures, s_failures == 1 ? "" : "s");
    }
}

// ReadPadBoth hook (platform.h).
DWORD plat_test_filter_pad(DWORD word)
{
    if (!s_active) return word;
    if (s_scriptPath != NULL) return s_padWord;

    // Recording: log runs of identical words per frame. ReadPadBoth can be
    // called more than once in a frame; the first call opens the run.
    if (word != s_recWord) {
        RecordFlushRun(s_frame);
        s_recWord = word;
    }
    return word;
}
