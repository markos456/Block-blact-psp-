/* Block Blast for PSP - homebrew (pspdev / PSPSDK)
 * Controls: D-pad move | X place | O or R next piece | L previous piece
 *           SELECT mute | START restart
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

PSP_MODULE_INFO("BlockBlast", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

/* ---------- exit callback (HOME button) ---------- */
static int exit_cb(int a, int b, void *c) { sceKernelExitGame(); return 0; }
static int cb_thread(SceSize args, void *argp) {
    int id = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    int t = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

/* ---------- sound: tiny software synth ---------- */
typedef struct {
    volatile int on;
    float f, f2, ph, vol;
    int t, dur, delay, type;
} Voice;
#define NV 16
static Voice V[NV];
static int muted = 0;

static void tone(float f, float f2, float d, int type, float vol, float delay) {
    if (muted) return;
    for (int i = 0; i < NV; i++) {
        if (!V[i].on) {
            Voice *v = &V[i];
            v->f = f; v->f2 = f2; v->ph = 0; v->t = 0;
            v->dur = (int)(d * 44100); v->delay = (int)(delay * 44100);
            v->vol = vol; v->type = type;
            v->on = 1;
            return;
        }
    }
}

enum { S_PICK, S_MOVE, S_PLACE, S_BAD, S_CLEAR, S_OVER };
static void sfx(int k, int n) {
    switch (k) {
    case S_PICK:  tone(520, 0, .07f, 3, .30f, 0); break;
    case S_MOVE:  tone(300, 0, .03f, 1, .10f, 0); break;
    case S_PLACE: tone(180, 90, .12f, 0, .60f, 0); break;
    case S_BAD:   tone(150, 100, .15f, 2, .25f, 0); break;
    case S_CLEAR: for (int i = 0; i < 3 + n; i++) tone(440.0f * powf(1.1225f, i * 2), 0, .18f, 3, .45f, i * .07f); break;
    case S_OVER: { float fr[4] = {392, 330, 262, 196};
                   for (int i = 0; i < 4; i++) tone(fr[i], 0, .3f, 3, .5f, i * .18f); } break;
    }
}

static short abuf[512 * 2] __attribute__((aligned(64)));
static int audio_thread(SceSize a, void *p) {
    int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 512, PSP_AUDIO_FORMAT_STEREO);
    while (1) {
        for (int i = 0; i < 512; i++) {
            float s = 0;
            for (int k = 0; k < NV; k++) {
                Voice *v = &V[k];
                if (!v->on) continue;
                if (v->delay > 0) { v->delay--; continue; }
                float p2 = (float)v->t / (float)v->dur;
                float fr = v->f2 > 0 ? v->f * powf(v->f2 / v->f, p2) : v->f;
                v->ph += fr / 44100.0f;
                if (v->ph >= 1) v->ph -= 1;
                float w;
                switch (v->type) {
                case 1:  w = v->ph < .5f ? 1 : -1; break;
                case 2:  w = 2 * v->ph - 1; break;
                case 3:  w = v->ph < .5f ? 4 * v->ph - 1 : 3 - 4 * v->ph; break;
                default: w = sinf(6.2831853f * v->ph);
                }
                float e = 1 - p2;
                s += w * v->vol * e * e;
                if (++v->t >= v->dur) v->on = 0;
            }
            if (s > 1) s = 1;
            if (s < -1) s = -1;
            short o = (short)(s * 12000);
            abuf[i * 2] = o; abuf[i * 2 + 1] = o;
        }
        sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, abuf);
    }
    return 0;
}

/* ---------- graphics (direct framebuffer) ---------- */
#define RGB(r, g, b) (0xFF000000u | ((b) << 16) | ((g) << 8) | (r))
static u32 *fb[2];
static int cur = 0;
extern unsigned char msx[]; /* 8x8 font from libpspdebug */

static const u32 PAL[8] = {
    0, RGB(0xff, 0x5d, 0x73), RGB(0xff, 0xa6, 0x3d), RGB(0xf5, 0xc4, 0x00),
    RGB(0x2f, 0xcf, 0x75), RGB(0x2b, 0xb8, 0xf0), RGB(0x6c, 0x7b, 0xff), RGB(0xb9, 0x54, 0xf5)};
#define C_BG RGB(0x14, 0x12, 0x2b)
#define C_EMPTY RGB(0x2b, 0x27, 0x57)
#define C_PANEL RGB(0x1f, 0x1c, 0x3d)
#define C_WHITE RGB(0xff, 0xff, 0xff)

static void rect(int x, int y, int w, int h, u32 c) {
    u32 *b = fb[cur];
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= 272) continue;
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < 480) b[j * 512 + i] = c;
    }
}
static void chr(int x, int y, int s, u32 c, unsigned char ch) {
    for (int i = 0; i < 8; i++) {
        unsigned char bits = msx[ch * 8 + i];
        for (int j = 0; j < 8; j++)
            if (bits & (0x80 >> j)) rect(x + j * s, y + i * s, s, s, c);
    }
}
static void text(int x, int y, int s, u32 c, const char *str) {
    for (; *str; str++, x += 8 * s) chr(x, y, s, c, (unsigned char)*str);
}

/* ---------- game ---------- */
#define N 8
typedef struct { int w, h; char m[5][5]; int c; int ok; } Piece;

static const char *SH[] = {
    "#", "##", "#/#", "###", "#/#/#", "####", "#/#/#/#", "#####", "#/#/#/#/#",
    "##/##", "###/###/###", "###/###", "##/##/##",
    "#./#./##", ".#/.#/##", "##/#./#.", "##/.#/.#",
    "#./##", "##/#.", ".#/##", "##/.#",
    "###/.#.", ".#./###", ".##/##.", "##/.##", "###/#../#..", "###/..#/..#"};
#define NSH ((int)(sizeof(SH) / sizeof(SH[0])))

static int g[N][N];
static Piece pc[3];
static int sel, kr, kc, score, best, over, flashT, flashLines, msgT;
static char kill[N][N];
static char msg[32];
static char bestPath[256];

static Piece newPiece(void) {
    Piece p;
    memset(&p, 0, sizeof p);
    const char *s = SH[rand() % NSH];
    int r = 0, c = 0;
    for (; *s; s++) {
        if (*s == '/') { r++; c = 0; continue; }
        if (*s == '#') p.m[r][c] = 1;
        c++;
        if (c > p.w) p.w = c;
    }
    p.h = r + 1;
    p.c = 1 + rand() % 7;
    p.ok = 1;
    return p;
}

static int canPlace(const Piece *p, int r, int c) {
    if (r < 0 || c < 0 || r + p->h > N || c + p->w > N) return 0;
    for (int i = 0; i < p->h; i++)
        for (int j = 0; j < p->w; j++)
            if (p->m[i][j] && g[r + i][c + j]) return 0;
    return 1;
}
static int fits(const Piece *p) {
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (canPlace(p, r, c)) return 1;
    return 0;
}

static void saveBest(void) {
    FILE *f = fopen(bestPath, "wb");
    if (f) { fwrite(&best, sizeof best, 1, f); fclose(f); }
}
static void loadBest(void) {
    FILE *f = fopen(bestPath, "rb");
    if (f) { if (fread(&best, sizeof best, 1, f) != 1) best = 0; fclose(f); }
}

static void fixSel(void) {
    if (!pc[sel].ok)
        for (int i = 0; i < 3; i++) if (pc[i].ok) { sel = i; break; }
}

static void after(void) {
    if (!pc[0].ok && !pc[1].ok && !pc[2].ok) {
        for (int i = 0; i < 3; i++) pc[i] = newPiece();
        sel = 0;
    }
    fixSel();
    if (score > best) { best = score; saveBest(); }
    int any = 0;
    for (int i = 0; i < 3; i++) if (pc[i].ok && fits(&pc[i])) any = 1;
    if (!any) { over = 1; sfx(S_OVER, 0); }
}

static void initGame(void) {
    memset(g, 0, sizeof g);
    score = 0; over = 0; flashT = 0; msgT = 0; sel = 0; kr = 3; kc = 3;
    for (int i = 0; i < 3; i++) pc[i] = newPiece();
}

static void place(int i) {
    Piece *p = &pc[i];
    int n = 0;
    for (int r = 0; r < p->h; r++)
        for (int c = 0; c < p->w; c++)
            if (p->m[r][c]) { g[kr + r][kc + c] = p->c; n++; }
    score += n;
    p->ok = 0;
    sfx(S_PLACE, 0);
    int lines = 0;
    memset(kill, 0, sizeof kill);
    for (int k = 0; k < N; k++) {
        int rf = 1, cf = 1;
        for (int j = 0; j < N; j++) { if (!g[k][j]) rf = 0; if (!g[j][k]) cf = 0; }
        if (rf) { for (int j = 0; j < N; j++) kill[k][j] = 1; lines++; }
        if (cf) { for (int j = 0; j < N; j++) kill[j][k] = 1; lines++; }
    }
    if (lines) { flashT = 14; flashLines = lines; sfx(S_CLEAR, lines); }
    else after();
}

static void cycle(int d) {
    for (int s = 1; s <= 3; s++) {
        int i = (((sel + d * s) % 3) + 3) % 3;
        if (pc[i].ok) { sel = i; sfx(S_PICK, 0); return; }
    }
}

static int held[4];
static int rep(int k, int on) {
    if (!on) { held[k] = 0; return 0; }
    held[k]++;
    return held[k] == 1 || (held[k] > 14 && (held[k] - 14) % 4 == 0);
}

static u32 prev = 0;
static void update(const SceCtrlData *pad) {
    u32 b = pad->Buttons, pr = b & ~prev;
    prev = b;
    if (pr & PSP_CTRL_SELECT) muted = !muted;
    if (over) { if (pr & (PSP_CTRL_CROSS | PSP_CTRL_START)) initGame(); return; }
    if (pr & PSP_CTRL_START) { initGame(); return; }
    if (flashT > 0) {
        if (--flashT == 0) {
            for (int r = 0; r < N; r++)
                for (int c = 0; c < N; c++) if (kill[r][c]) g[r][c] = 0;
            int pts = flashLines * flashLines * 10;
            score += pts;
            if (flashLines > 1) snprintf(msg, sizeof msg, "COMBO x%d +%d", flashLines, pts);
            else snprintf(msg, sizeof msg, "+%d", pts);
            msgT = 70;
            after();
        }
        return;
    }
    if (msgT > 0) msgT--;
    int up = rep(0, b & PSP_CTRL_UP), dn = rep(1, b & PSP_CTRL_DOWN);
    int lf = rep(2, b & PSP_CTRL_LEFT), rt = rep(3, b & PSP_CTRL_RIGHT);
    Piece *p = &pc[sel];
    int moved = 0;
    if (up && kr > 0) { kr--; moved = 1; }
    if (dn && kr < N - p->h) { kr++; moved = 1; }
    if (lf && kc > 0) { kc--; moved = 1; }
    if (rt && kc < N - p->w) { kc++; moved = 1; }
    if (moved) sfx(S_MOVE, 0);
    if (pr & PSP_CTRL_LTRIGGER) cycle(-1);
    if (pr & (PSP_CTRL_RTRIGGER | PSP_CTRL_CIRCLE)) cycle(1);
    p = &pc[sel];
    if (kr > N - p->h) kr = N - p->h;
    if (kc > N - p->w) kc = N - p->w;
    if (pr & PSP_CTRL_CROSS) {
        if (canPlace(p, kr, kc)) place(sel);
        else sfx(S_BAD, 0);
    }
}

#define BX 12
#define BY 16
#define CS 30
static void draw(void) {
    rect(0, 0, 480, 272, C_BG);
    rect(BX - 4, BY - 4, N * CS + 8, N * CS + 8, C_PANEL);
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            u32 col = g[r][c] ? PAL[g[r][c]] : C_EMPTY;
            if (flashT > 0 && kill[r][c] && ((flashT / 2) & 1)) col = C_WHITE;
            rect(BX + c * CS + 1, BY + r * CS + 1, CS - 2, CS - 2, col);
        }
    if (!over && flashT == 0 && pc[sel].ok) {
        Piece *p = &pc[sel];
        u32 gc = canPlace(p, kr, kc) ? RGB(0xd8, 0xd8, 0xff) : RGB(0xff, 0x3b, 0x3b);
        for (int r = 0; r < p->h; r++)
            for (int c = 0; c < p->w; c++)
                if (p->m[r][c])
                    rect(BX + (kc + c) * CS + 3, BY + (kr + r) * CS + 3, CS - 6, CS - 6, gc);
    }
    char buf[32];
    text(272, 14, 2, C_WHITE, "SCORE");
    snprintf(buf, sizeof buf, "%d", score);
    text(272, 32, 3, PAL[3], buf);
    text(272, 64, 2, C_WHITE, "BEST");
    snprintf(buf, sizeof buf, "%d", best);
    text(272, 82, 3, PAL[5], buf);
    for (int i = 0; i < 3; i++) {
        int x = 272 + i * 68, y = 122;
        rect(x, y, 60, 60, C_PANEL);
        if (i == sel && !over) {
            rect(x - 2, y - 2, 64, 2, PAL[6]); rect(x - 2, y + 60, 64, 2, PAL[6]);
            rect(x - 2, y - 2, 2, 64, PAL[6]); rect(x + 60, y - 2, 2, 64, PAL[6]);
        }
        Piece *p = &pc[i];
        if (!p->ok) continue;
        int ox = x + (60 - p->w * 12) / 2, oy = y + (60 - p->h * 12) / 2;
        for (int r = 0; r < p->h; r++)
            for (int c = 0; c < p->w; c++)
                if (p->m[r][c]) rect(ox + c * 12 + 1, oy + r * 12 + 1, 10, 10, PAL[p->c]);
    }
    if (msgT > 0) text(272, 192, 2, PAL[2], msg);
    text(272, 214, 1, RGB(0x9a, 0x96, 0xc8), "D-PAD move   X place");
    text(272, 226, 1, RGB(0x9a, 0x96, 0xc8), "L / R piece  START new");
    text(272, 238, 1, RGB(0x9a, 0x96, 0xc8), muted ? "SELECT sound: OFF" : "SELECT sound: ON");
    if (over) {
        rect(60, 80, 360, 110, RGB(0, 0, 0));
        text(100, 96, 4, RGB(0xff, 0x5d, 0x73), "GAME OVER");
        snprintf(buf, sizeof buf, "SCORE %d", score);
        text(160, 140, 2, C_WHITE, buf);
        text(130, 166, 1, C_WHITE, "Press X or START to play again");
    }
}

int main(int argc, char **argv) {
    setup_callbacks();
    /* best.dat next to EBOOT.PBP */
    strncpy(bestPath, argc > 0 ? argv[0] : "", sizeof bestPath - 16);
    char *sl = strrchr(bestPath, '/');
    if (sl) sl[1] = 0; else bestPath[0] = 0;
    strcat(bestPath, "best.dat");
    loadBest();

    u32 base = 0x40000000 | (u32)sceGeEdramGetAddr();
    fb[0] = (u32 *)base;
    fb[1] = (u32 *)(base + 512 * 272 * 4);
    sceDisplaySetMode(0, 480, 272);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    srand(sceKernelGetSystemTimeLow());

    int th = sceKernelCreateThread("audio", audio_thread, 0x12, 0x10000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);

    initGame();
    SceCtrlData pad;
    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        update(&pad);
        draw();
        sceDisplayWaitVblankStart();
        sceDisplaySetFrameBuf(fb[cur], 512, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
        cur ^= 1;
    }
    return 0;
}
