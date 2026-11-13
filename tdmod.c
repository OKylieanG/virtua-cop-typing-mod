/*
 * Typing mod for Virtua Cop PC (PPJDD.EXE), built as a stand-in DPLAY.dll.
 *
 * Every shootable enemy gets a word. Typing its first letter locks onto it; finishing the word
 * aims the gun at it and fires through the game's own input, so the game's hit test, scoring and
 * reactions all run normally. F8 toggles between typing and normal mouse play.
 *
 * Game hooks (addresses are for the 1996 PC release, sha256 6668...0752):
 *   0x40C580  hit test (player, obj)  - called every frame per shootable object; we project its hit boxes
 *   0x40BD20  object update loop      - once per game frame; drives timers and the fire sequence
 *   0x4208BA  just before Flip        - draw the words onto the back buffer with GDI
 */
#include <windows.h>

/* ---------------------------------------------------------------- game addresses */
#define G_HITTEST       0x40C580
#define G_OBJLOOP       0x40BD20
#define G_PRESENT_AT    0x4208BA
#define G_PRESENT_BACK  0x4208C1
#define G_P1            0x556220
#define G_MOUSEPOS      0x4866AC   /* (y << 16) | x in window pixels */
#define G_LMB           0x4866B0   /* WORD, 1 = held */
#define G_RMB           0x4866B4   /* WORD, 1 = held */
#define G_SCRW          0x529004   /* WORD */
#define G_SCRH          0x529008   /* WORD */
#define G_FOCAL         0x55C4D4   /* float */
#define G_BACKBUF       0x4866E0   /* IDirectDrawSurface* */
#define G_HWND          0x12CF5B0

#define OBJ_SKIP        0x47C
#define OBJ_CALLBACK    0x480
#define OBJ_BONEMASK    0x484
#define OBJ_NBOX        0x4A0
#define OBJ_BONES       0x29C
#define HITBOX_SIZE     0x1C

/* characters (enemies, hostages) - checked every frame by 0x433740(person) */
#define G_PERSONCHECK   0x433740
#define PER_ALIVE       0x0B
#define PER_STATE       0x0C
#define PER_MODEL       0xD8       /* -> model; model+4 -> hit sphere array */
#define PER_BONES       0x13C
#define PER_NSPHERES    21
#define SPHERE_SIZE     0x14       /* byte bone, float x,y,z at +4, float radius at +0x10 */
#define MODEL_NWEAPONS  0x0C       /* 0 = unarmed = hostage */
#define PER_HITFLAGS    0x5D3      /* bit 0x10 = P1's shot hit this character this frame */
#define G_P1_AMMO       0x556248   /* short, rounds in P1's gun */
#define G_P1_TRIGCD     0x55624E   /* WORD, frames until P1's trigger is accepted again */
#define G_CANSHOOT      0x5554A6   /* BYTE, 0 = game ignores the trigger */
#define G_WEAPONCALL    0x434E82   /* player update: mov byte [esi+2],0; push esi; call [esi+0x3C] */

typedef int  (__cdecl *HitTestFn)(char *player, char *obj);
typedef int  (__cdecl *VoidFn)(void);
typedef void (__cdecl *MulFn)(void *m);
typedef void (__cdecl *TransFn)(float x, float y, float z);
typedef void (__cdecl *RotFn)(int angle);
typedef void (__cdecl *XformFn)(float x, float y, float z, float *out);

static const VoidFn  mtx_push  = (VoidFn)0x446EC0;
static const VoidFn  mtx_pop   = (VoidFn)0x446EA0;
static const MulFn   mtx_mul   = (MulFn)0x446CD0;
static const TransFn mtx_trans = (TransFn)0x4472A0;
static const RotFn   mtx_rotA  = (RotFn)0x446F20;
static const RotFn   mtx_rotB  = (RotFn)0x447030;
static const XformFn mtx_xform = (XformFn)0x41AC60;

static HitTestFn g_origHitTest;
static int (__cdecl *g_origPersonCheck)(char *en);

/* ---------------------------------------------------------------- freestanding helpers */
void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, unsigned n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static DWORD g_seed;
static int rnd(int n)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (int)((g_seed >> 16) & 0x7FFF) % n;
}

static char g_dir[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;

static void mlog(const char *fmt, ...)
{
    char buf[512];
    DWORD n;
    va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
    WriteFile(g_log, buf, n, &n, NULL);
}

static void path_in_dir(char *out, const char *name)
{
    int i = 0, j = 0;
    while (g_dir[i]) { out[i] = g_dir[i]; i++; }
    while (name[j]) out[i++] = name[j++];
    out[i] = 0;
}

/* ---------------------------------------------------------------- word list */
#define WORDBUF   32768
#define MAXWORDS  1024
#define MAXWORDLEN  40
static char  g_wordbuf[WORDBUF];
static char *g_tier[3][MAXWORDS];  /* 0 short (<=6), 1 medium (7-12), 2 long (13+ or multi-word) */
static int   g_ntier[3];

static const char g_defaultWords[] =
    "gain\nfader\nmixer\nphase\ndelay\nreverb\ngate\npan\nmute\nsolo\nhertz\nohm\nvolt\nwatt\nquark\nboson\n"
    "photon\nproton\nmuon\ngluon\nflux\ntorque\nplasma\nvector\ntensor\njoule\nnewton\nkelvin\ndither\nchorus\n"
    "compressor\nequalizer\ncondenser\nsidechain\nheadroom\ncrossover\nimpedance\ntransient\nwaveform\n"
    "nyquist\naliasing\nfeedback\nsaturation\nharmonics\nfrequency\namplitude\nwavelength\nresonance\n"
    "momentum\nvelocity\nfriction\nneutrino\nelectron\nmagnetism\ninduction\nrefraction\nspacetime\n"
    "phantom power\nsignal to noise ratio\ninverse square law\ndoppler effect\nangular momentum\n"
    "uncertainty principle\nproximity effect\nfast fourier transform\nconservation of energy\n";

static void add_word(char *w)
{
    int len = slen(w), t;
    int spaces = 0, i;
    if (len < 2 || len >= MAXWORDLEN) return;
    for (i = 0; i < len; i++) if (w[i] == ' ') spaces++;
    t = (len <= 6) ? 0 : (len <= 12 && !spaces) ? 1 : 2;
    if (g_ntier[t] < MAXWORDS) g_tier[t][g_ntier[t]++] = w;
}

/* Parse newline-separated words in place: lowercased, '#' starts a comment, only a-z 0-9 space - ' kept. */
static void parse_words(char *s, int n)
{
    int i = 0;
    while (i < n) {
        char *start = s + i, *w = start;
        int keep = 1;
        while (i < n && s[i] != '\n' && s[i] != '\r') {
            char c = s[i++];
            if (c == '#') keep = 0;
            if (!keep) continue;
            if (c >= 'A' && c <= 'Z') c += 32;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '\'')
                *w++ = c;
        }
        while (w > start && w[-1] == ' ') w--;
        while (*start == ' ' && start < w) start++;
        if (i < n) i++;
        *w = 0;
        if (w > start) add_word(start);
    }
}

static void load_words(void)
{
    char path[MAX_PATH + 16];
    HANDLE f;
    DWORD got = 0;
    path_in_dir(path, "words.txt");
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        ReadFile(f, g_wordbuf, WORDBUF - 1, &got, NULL);
        CloseHandle(f);
    }
    if (got == 0) {
        got = sizeof(g_defaultWords) - 1;
        memcpy(g_wordbuf, g_defaultWords, got);
    }
    g_wordbuf[got] = 0;
    parse_words(g_wordbuf, (int)got);
    mlog("words: %d short, %d medium, %d long (%s)", g_ntier[0], g_ntier[1], g_ntier[2],
         f != INVALID_HANDLE_VALUE ? "words.txt" : "built-in");
}

/* ---------------------------------------------------------------- enemies */
#define MAX_EN        24
#define STALE_TICKS   8      /* not hit-tested for this many ticks = gone */
#define SHOT_COOLDOWN 45     /* ticks after a kill shot before a survivor gets a new word */

typedef struct {
    int   used;
    char *obj;
    DWORD cb;
    int   firstSeen, lastSeen;
    int   sx, sy;            /* hit box center, arcade units relative to screen center */
    int   tx, ty;            /* top of the hit box (label anchor) */
    int   word;              /* -1 = none */
    int   tier;
    int   typed;
    int   cooldown;
    int   ax[4], ay[4];      /* aim points, best first (arcade units from screen center) */
    int   naim;
    int   hitTick;           /* last tick the game's own hit test reported P1 hitting this */
    int   landed;            /* the game registered a hit from our last completed word */
    int   tough;             /* survived this many landed words */
    int   person;            /* 1 = character (enemy/hostage), 0 = shootable prop */
} Enemy;

static Enemy  g_en[MAX_EN];
static char  *g_lastHostage;
static char  *g_words[MAX_EN];   /* word text per enemy slot */
static int    g_tick;
static Enemy *g_target;
static int    g_typing = 1;
static int    g_keysOk, g_keysMiss, g_wordsDone;

static Enemy *find_enemy(char *obj)
{
    int i;
    for (i = 0; i < MAX_EN; i++) if (g_en[i].used && g_en[i].obj == obj) return &g_en[i];
    return NULL;
}

static Enemy *new_enemy(char *obj, int person)
{
    int i;
    for (i = 0; i < MAX_EN; i++) {
        if (!g_en[i].used) {
            Enemy *e = &g_en[i];
            memset(e, 0, sizeof(*e));
            e->used = 1;
            e->obj = obj;
            e->word = -1;
            e->firstSeen = g_tick;
            e->person = person;
            if (person) {
                /* logged to help tell hostages from enemies */
                e->cb = *(DWORD *)(obj + PER_MODEL);
                mlog("person obj=%08x model=%08x b8-f=%02x %02x %02x %02x %02x %02x %02x %02x d4=%08x dc=%08x",
                     (DWORD)obj, e->cb, (BYTE)obj[8], (BYTE)obj[9], (BYTE)obj[10], (BYTE)obj[11],
                     (BYTE)obj[12], (BYTE)obj[13], (BYTE)obj[14], (BYTE)obj[15],
                     *(DWORD *)(obj + 0xD4), *(DWORD *)(obj + 0xDC));
            } else {
                e->cb = *(DWORD *)(obj + OBJ_CALLBACK);
                mlog("prop obj=%08x cb=%06x boxes=%d", (DWORD)obj, e->cb, (int)(signed char)obj[OBJ_NBOX]);
            }
            return e;
        }
    }
    return NULL;
}

/* Project the object's hit boxes with the game's own matrix stack, exactly as the hit test does,
 * and keep the box with the largest on-screen height (the body). */
static void track(char *obj)
{
    int n = (signed char)obj[OBJ_NBOX], i;
    char *boxes;
    DWORD mask;
    float f = *(float *)G_FOCAL;
    float bestH = -1.0f;
    int cx = 0, cy = 0, tx = 0, ty = 0;
    Enemy *e;

    if (obj[OBJ_SKIP] || n <= 0) return;
    boxes = *(char **)(*(char **)(obj + 4) + 0x10);
    mask = *(DWORD *)(obj + OBJ_BONEMASK);

    for (i = 0; i < n; i++) {
        char *b = boxes + i * HITBOX_SIZE;
        int bone = *(int *)b;
        float h = *(float *)(b + 0x14);
        float c[3], t[3], sh;
        if (!((mask >> (b[0] & 31)) & 1)) continue;
        mtx_push();
        mtx_mul(obj + OBJ_BONES + bone * 0x30);
        mtx_trans(*(float *)(b + 4), *(float *)(b + 8), *(float *)(b + 0xC));
        mtx_rotA(*(unsigned short *)(b + 0x18));
        mtx_rotB(*(unsigned short *)(b + 0x1A));
        mtx_xform(0.0f, h * 0.5f, 0.0f, c);
        mtx_xform(0.0f, h, 0.0f, t);
        mtx_pop();
        if (c[2] < 1.0f || t[2] < 1.0f) continue;
        sh = (t[1] * f / t[2]) - (c[1] * f / c[2]);
        if (sh < 0) sh = -sh;
        if (sh > bestH) {
            bestH = sh;
            cx = (int)(c[0] * f / c[2]);
            cy = (int)(c[1] * f / c[2]);
            tx = (int)(t[0] * f / t[2]);
            ty = (int)(t[1] * f / t[2]);
        }
    }
    if (bestH < 0) return;

    e = find_enemy(obj);
    if (!e) e = new_enemy(obj, 0);
    if (!e) return;
    e->lastSeen = g_tick;
    e->sx = cx; e->sy = cy;
    e->ax[0] = cx; e->ay[0] = cy;
    e->naim = 1;
    /* label goes above whichever end of the box is higher on screen */
    e->tx = tx; e->ty = (ty < cy) ? ty : cy - (ty - cy);
}

static int __cdecl HitTestHook(char *player, char *obj)
{
    int r;
    if (player == (char *)G_P1 && g_typing) track(obj);
    r = g_origHitTest(player, obj);
    if (r == 1 && player == (char *)G_P1) {
        Enemy *e = find_enemy(obj);
        if (e) e->hitTick = g_tick;
    }
    return r;
}

/* People (enemies, hostages): 21 hit spheres per character, each tied to a bone matrix.
 * Aim at the biggest sphere on screen (the torso), label above the highest sphere. */
static void track_person(char *en)
{
    char *spheres;
    float f = *(float *)G_FOCAL;
    float rr[3] = { -1.0f, -1.0f, -1.0f };   /* three biggest spheres on screen, biggest first */
    int px[3] = { 0, 0, 0 }, py[3] = { 0, 0, 0 };
    int i, k, n = 0, topX = 0, topY = 0x7FFFFFFF, headX = 0, headY = 0;
    Enemy *e;

    if (en[PER_ALIVE] != 1 || (en[PER_STATE] & 0x0C)) return;
    /* hostages are the characters whose model carries no weapon */
    if (*(DWORD *)(*(char **)(en + PER_MODEL) + MODEL_NWEAPONS) == 0) {
        if (en != g_lastHostage) {
            g_lastHostage = en;
            mlog("hostage skipped obj=%08x model=%08x", (DWORD)en, *(DWORD *)(en + PER_MODEL));
        }
        return;
    }
    spheres = *(char **)(*(char **)(en + PER_MODEL) + 4);
    if (!spheres) return;

    for (i = 0; i < PER_NSPHERES; i++) {
        char *s = spheres + i * SPHERE_SIZE;
        float r = *(float *)(s + 0x10);
        float c[3], sx, sy, sr;
        if (r <= 0.0f) continue;
        mtx_push();
        mtx_mul(en + PER_BONES + (signed char)s[0] * 0x30);
        mtx_xform(*(float *)(s + 4), *(float *)(s + 8), *(float *)(s + 0xC), c);
        mtx_pop();
        if (c[2] < 1.0f) continue;
        sx = c[0] * f / c[2];
        sy = c[1] * f / c[2];
        sr = r * f / c[2];
        for (k = 0; k < 3; k++) {
            if (sr > rr[k]) {
                int j;
                for (j = 2; j > k; j--) { rr[j] = rr[j - 1]; px[j] = px[j - 1]; py[j] = py[j - 1]; }
                rr[k] = sr; px[k] = (int)sx; py[k] = (int)sy;
                break;
            }
        }
        if ((int)(sy - sr) < topY) { topY = (int)(sy - sr); topX = (int)sx; headX = (int)sx; headY = (int)sy; }
    }
    if (rr[0] < 0) return;
    for (k = 0; k < 3 && rr[k] >= 0; k++) n++;

    e = find_enemy(en);
    if (!e) e = new_enemy(en, 1);
    if (!e) return;
    e->lastSeen = g_tick;
    e->sx = px[0]; e->sy = py[0];
    /* aim order after misses: torso, head (peeks over cover), then the next biggest spheres */
    e->ax[0] = px[0]; e->ay[0] = py[0];
    e->naim = 1;
    if (headX != px[0] || headY != py[0]) { e->ax[1] = headX; e->ay[1] = headY; e->naim = 2; }
    for (k = 1; k < n && e->naim < 4; k++) {
        if (px[k] == headX && py[k] == headY) continue;
        e->ax[e->naim] = px[k]; e->ay[e->naim] = py[k]; e->naim++;
    }
    e->tx = topX; e->ty = topY;
}

static int __cdecl PersonCheckHook(char *en)
{
    int r;
    if (g_typing) track_person(en);
    r = g_origPersonCheck(en);
    /* the check ORs (player id << 4) into +0x5D3 when that player's shot hit a sphere */
    if ((BYTE)en[PER_HITFLAGS] & 0x10) {
        Enemy *e = find_enemy(en);
        if (e) e->hitTick = g_tick;
    }
    return r;
}

static int word_in_use_with_initial(char c, Enemy *except)
{
    int i;
    for (i = 0; i < MAX_EN; i++) {
        Enemy *e = &g_en[i];
        if (e != except && e->used && e->word >= 0 && g_words[i][0] == c) return 1;
    }
    return 0;
}

static void assign_word(Enemy *e)
{
    int slot = (int)(e - g_en), tries, t;
    char *w = NULL;
    /* fresh enemies: mostly short words; one survived hit: medium; damage sponges (bosses): long */
    t = e->tough >= 2 ? 2 : e->tough == 1 ? 1 : (rnd(10) < 7 ? 0 : 1);
    if (g_ntier[t] == 0) t = g_ntier[1] ? 1 : g_ntier[0] ? 0 : 2;
    if (g_ntier[t] == 0) return;
    for (tries = 0; tries < 40; tries++) {
        w = g_tier[t][rnd(g_ntier[t])];
        if (!word_in_use_with_initial(w[0], e)) break;
    }
    g_words[slot] = w;
    e->word = slot;
    e->tier = t;
    e->typed = 0;
}

/* ---------------------------------------------------------------- firing */
/*
 * Shots are driven off the game's own state instead of blind timing:
 *   WAIT   aim; wait until the gun has ammo, its trigger cooldown is 0 and shooting is allowed
 *   PRESS  hold the button for two ticks (the game fires on the press edge)
 *   CHECK  release; watch for the ammo count to drop (shot went off) and for the game's hit test
 *          to flag the target (shot hit). Hit = done. Fired but missed = re-aim at the next
 *          sphere and go again. Never fired = back to WAIT.
 * Reloads are confirmed the same way: press reload, wait for the ammo count to go up.
 */
typedef struct { char *obj; int misses; int waited; } Shot;
#define MAX_SHOTS     16
#define MAX_MISSES    4      /* fired-but-missed attempts per shot before giving up */
#define MAX_WAIT      240    /* ticks to wait for the game to accept the trigger */
#define AWAITING_SHOT 100000 /* cooldown value while a word's shots haven't landed yet */
enum { S_WAIT, S_PRESS1, S_PRESS2, S_CHECK, S_CONFIRM };
enum { R_IDLE, R_CHECK };
static Shot g_shots[MAX_SHOTS];
static int  g_nshots, g_shotState, g_checkTicks, g_pressTick, g_ammoBefore;
static int  g_reloadState, g_reloadTicks, g_reloadAmmo, g_idleTicks, g_maxAmmo = 6, g_lastAmmo;

static void queue_shots(Enemy *e, int n)
{
    while (n-- > 0 && g_nshots < MAX_SHOTS) {
        g_shots[g_nshots].obj = e->obj;
        g_shots[g_nshots].misses = 0;
        g_shots[g_nshots].waited = 0;
        g_nshots++;
    }
}

static void pop_shot(void)
{
    int i;
    for (i = 1; i < g_nshots; i++) g_shots[i - 1] = g_shots[i];
    g_nshots--;
    g_shotState = S_WAIT;
}

static int gun_ammo(void)   { return *(short *)G_P1_AMMO; }
static int gun_ready(void)  { return *(WORD *)G_P1_TRIGCD == 0 && *(BYTE *)G_CANSHOOT != 0; }

/* Reload the way the game itself does after a double-tap of reload or an offscreen shot:
 * raise the player's reload request flag. The flag is delivered by WeaponHook right before the
 * game runs P1's weapon handler, so nothing else in the frame can clear it first. */
static int g_reloadWant, g_reloadDelivered;

static void start_reload(void)
{
    g_reloadState = R_CHECK;
    g_reloadAmmo = gun_ammo();
    g_reloadTicks = 0;
    g_reloadWant = 1;
    g_reloadDelivered = 0;
}

static void __cdecl WeaponHook(char *player)
{
    if (player == (char *)G_P1 && g_reloadWant) {
        player[0x2C] = 1;
        g_reloadWant = 0;
        g_reloadDelivered = 1;
    }
}

/* returns 1 while a reload must finish before shooting (only when the gun is empty) */
static int run_reload(void)
{
    int ammo = gun_ammo();
    if (g_reloadState == R_IDLE) return 0;
    if (ammo > g_reloadAmmo) {                 /* refilled: that's this weapon's capacity */
        g_maxAmmo = ammo;
        g_reloadState = R_IDLE;
        g_reloadWant = 0;
        return 0;
    }
    if (!g_reloadDelivered) return ammo <= 0;  /* player not under control yet; keep waiting */
    if (++g_reloadTicks > 10) {
        /* this weapon doesn't reload (or not right now): treat what's left as full */
        mlog("reload not accepted (ammo %d)", ammo);
        if (ammo > 0) g_maxAmmo = ammo;
        g_reloadState = R_IDLE;
        return 0;
    }
    return ammo <= 0;
}

static void aim_point(Enemy *e, int which)
{
    int w = *(unsigned short *)G_SCRW, h = *(unsigned short *)G_SCRH;
    int k = e->naim > 0 ? which % e->naim : 0;
    int ax = e->naim > 0 ? e->ax[k] : e->sx, ay = e->naim > 0 ? e->ay[k] : e->sy;
    int px = (ax + 160) * w / 320, py = (ay + 120) * h / 240;
    if (px < 0) px = 0;
    if (px > w - 1) px = w - 1;
    if (py < 0) py = 0;
    if (py > h - 1) py = h - 1;
    *(DWORD *)G_MOUSEPOS = ((DWORD)py << 16) | (DWORD)px;
}

static void run_fire_sequence(void)
{
    int ammo = gun_ammo();
    Shot *s;
    Enemy *e;

    /* ammo going up on its own = new weapon picked up (or the game reloaded): that's full */
    if (ammo > g_lastAmmo && g_reloadState == R_IDLE) g_maxAmmo = ammo;
    g_lastAmmo = ammo;
    if (g_shotState != S_PRESS1 && g_shotState != S_PRESS2) *(WORD *)G_LMB = 0;
    if (run_reload()) return;

    if (g_nshots == 0) {
        /* idle: top the gun up so the next word never meets an empty chamber */
        if (++g_idleTicks > 8 && ammo < g_maxAmmo && gun_ready()) {
            g_idleTicks = 0;
            start_reload();
        }
        return;
    }
    g_idleTicks = 0;

    s = &g_shots[0];
    e = find_enemy(s->obj);
    if (!e) {                                  /* target died or left the screen */
        *(WORD *)G_LMB = 0;
        pop_shot();
        return;
    }
    aim_point(e, s->misses);

    switch (g_shotState) {
    case S_WAIT:
        if (ammo <= 0) { if (g_reloadState == R_IDLE) start_reload(); return; }
        if (!gun_ready()) {
            if (++s->waited > MAX_WAIT) {
                mlog("shot at %08x: trigger never accepted, dropping", (DWORD)s->obj);
                e->cooldown = 1;               /* fresh word, no toughness penalty */
                pop_shot();
            }
            return;
        }
        g_ammoBefore = ammo;
        g_shotState = S_PRESS1;                /* aim is in place this tick; press on the next */
        return;
    case S_PRESS1:
        *(WORD *)G_LMB = 1;
        g_pressTick = g_tick;
        g_shotState = S_PRESS2;
        return;
    case S_PRESS2:
        *(WORD *)G_LMB = 1;
        g_shotState = S_CHECK;
        g_checkTicks = 0;
        return;
    case S_CONFIRM:
        /* a regular enemy that really took the hit drops out of the game's alive/targetable check
         * within a few frames; if he's still standing, cover in front of him took the bullet */
        *(WORD *)G_LMB = 0;
        if (g_tick - e->lastSeen > 1) {
            mlog("shot at %08x hit (aim %d)", (DWORD)s->obj, s->misses);
            e->landed = 1;
            e->cooldown = SHOT_COOLDOWN;
            pop_shot();
            return;
        }
        if (++g_checkTicks < 10) return;
        mlog("shot at %08x blocked by cover (aim %d), re-aiming", (DWORD)s->obj, s->misses);
        if (++s->misses >= MAX_MISSES) {
            e->cooldown = 1;
            pop_shot();
            return;
        }
        g_shotState = S_WAIT;
        return;
    default:
        *(WORD *)G_LMB = 0;
        if (e->hitTick >= g_pressTick) {
            if (e->person && e->tough == 0) {
                g_shotState = S_CONFIRM;
                g_checkTicks = 0;
                return;
            }
            mlog("shot at %08x hit (aim %d)", (DWORD)s->obj, s->misses);
            e->landed = 1;
            e->cooldown = SHOT_COOLDOWN;
            pop_shot();
            return;
        }
        if (++g_checkTicks < 5) return;
        if (gun_ammo() < g_ammoBefore) {
            mlog("shot at %08x fired but missed (aim %d)", (DWORD)s->obj, s->misses);
            if (++s->misses >= MAX_MISSES) {
                e->cooldown = 1;
                pop_shot();
                return;
            }
        } else if (++s->waited > MAX_WAIT) {
            mlog("shot at %08x: never fired, dropping", (DWORD)s->obj);
            e->cooldown = 1;
            pop_shot();
            return;
        }
        g_shotState = S_WAIT;
        return;
    }
}

/* ---------------------------------------------------------------- typing */
static void complete_word(Enemy *e)
{
    queue_shots(e, e->tough >= 2 ? 3 : 1 + e->tough);
    e->word = -1;
    e->typed = 0;
    e->cooldown = AWAITING_SHOT;      /* the fire sequence sets the real cooldown once a shot lands */
    e->landed = 0;
    g_wordsDone++;
    g_target = NULL;
}

static void on_char(char c)
{
    if (c >= 'A' && c <= 'Z') c += 32;
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '\'')) return;

    if (!g_target) {
        /* lock onto the visible enemy whose word starts with this letter (oldest first) */
        Enemy *best = NULL;
        int i;
        for (i = 0; i < MAX_EN; i++) {
            Enemy *e = &g_en[i];
            if (!e->used || e->word < 0 || g_tick - e->lastSeen > 2) continue;
            if (g_words[i][0] != c) continue;
            if (!best || e->firstSeen < best->firstSeen) best = e;
        }
        if (!best) { g_keysMiss++; return; }
        g_target = best;
        best->typed = 1;
        g_keysOk++;
    } else {
        char *w = g_words[g_target - g_en];
        if (w[g_target->typed] != c) { g_keysMiss++; return; }
        g_target->typed++;
        g_keysOk++;
    }
    if (g_words[g_target - g_en][g_target->typed] == 0) complete_word(g_target);
}

/* ---------------------------------------------------------------- window subclass */
static WNDPROC g_origProc;
static HWND    g_hwnd;

static LRESULT CALLBACK ModProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_F8) {
        if (!(l & 0x40000000)) {
            g_typing = !g_typing;
            g_target = NULL;
            *(WORD *)G_LMB = 0;
            *(WORD *)G_RMB = 0;
        }
        return 0;
    }
    if (g_typing) {
        switch (m) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            return 0;
        case WM_KEYDOWN:
            /* Esc = drop the current target and start over */
            if (w == VK_ESCAPE) {
                if (g_target) {
                    g_target->typed = 0;
                    g_target = NULL;
                }
                return 0;
            }
            /* Enter = trigger, for PRESS START / continue screens */
            if (w == VK_RETURN) {
                if (!(l & 0x40000000)) CallWindowProcA(g_origProc, h, WM_LBUTTONDOWN, MK_LBUTTON, 0);
                return 0;
            }
            break;
        case WM_KEYUP:
            if (w == VK_RETURN) {
                CallWindowProcA(g_origProc, h, WM_LBUTTONUP, 0, 0);
                return 0;
            }
            break;
        case WM_CHAR:
            on_char((char)w);
            return 0;
        }
    }
    return CallWindowProcA(g_origProc, h, m, w, l);
}

static void ensure_subclassed(void)
{
    HWND h = *(HWND *)G_HWND;
    if (!h || h == g_hwnd) return;
    g_origProc = (WNDPROC)SetWindowLongA(h, GWL_WNDPROC, (LONG)ModProc);
    g_hwnd = h;
    mlog("subclassed window %08x", (DWORD)h);
}

/* ---------------------------------------------------------------- per-frame tick */
static void __cdecl Tick(void)
{
    int i;
    g_tick++;
    ensure_subclassed();
    if (!g_typing) return;

    for (i = 0; i < MAX_EN; i++) {
        Enemy *e = &g_en[i];
        if (!e->used) continue;
        if (g_tick - e->lastSeen > STALE_TICKS) {
            if (g_target == e) g_target = NULL;
            e->used = 0;
            continue;
        }
        if (e->cooldown > 0 && e->cooldown < AWAITING_SHOT && --e->cooldown == 0 && e->landed) {
            e->tough++;                   /* took a registered hit and is still standing */
            e->landed = 0;
        }
        if (e->word < 0 && e->cooldown == 0 && g_tick - e->firstSeen >= 2) assign_word(e);
    }
    run_fire_sequence();
}

/* ---------------------------------------------------------------- drawing */
static HFONT g_font, g_smallFont;

typedef HRESULT (__stdcall *SurfGetDC)(void *surf, HDC *dc);
typedef HRESULT (__stdcall *SurfReleaseDC)(void *surf, HDC dc);

static void text_at(HDC dc, int x, int y, const char *s, int n, COLORREF fg)
{
    SetTextColor(dc, fg);
    TextOutA(dc, x, y, s, n);
}

static void __cdecl DrawOverlay(void)
{
    void **surf = *(void ***)G_BACKBUF;
    void **vt;
    HDC dc = NULL;
    HGDIOBJ oldFont;
    int w, h, i;
    SIZE sz;

    ensure_subclassed();
    if (!surf) return;
    vt = *(void ***)surf;
    if (((SurfGetDC)vt[17])(surf, &dc) != 0 || !dc) return;

    w = *(unsigned short *)G_SCRW;
    h = *(unsigned short *)G_SCRH;
    if (!g_font) {
        g_font = CreateFontA(-(h / 20), 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
        g_smallFont = CreateFontA(-(h / 30), 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
    }
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(0, 0, 0));
    oldFont = SelectObject(dc, g_font);

    if (g_typing) {
        for (i = 0; i < MAX_EN; i++) {
            Enemy *e = &g_en[i];
            const char *word;
            int len, x, y;
            if (!e->used || e->word < 0 || g_tick - e->lastSeen > 2) continue;
            word = g_words[i];
            len = slen(word);
            GetTextExtentPoint32A(dc, word, len, &sz);
            x = (e->tx + 160) * w / 320 - sz.cx / 2;
            y = (e->ty + 120) * h / 240 - sz.cy - 4;
            if (x < 2) x = 2;
            if (x + sz.cx > w - 2) x = w - 2 - sz.cx;
            if (y < 2) y = 2;
            if (y + sz.cy > h - 2) y = h - 2 - sz.cy;
            if (e->typed > 0) {
                SIZE done;
                text_at(dc, x, y, word, e->typed, RGB(255, 40, 40));
                GetTextExtentPoint32A(dc, word, e->typed, &done);
                text_at(dc, x + done.cx, y, word + e->typed, len - e->typed, RGB(255, 255, 0));
            } else {
                text_at(dc, x, y, word, len, RGB(255, 255, 255));
            }
        }
    }

    SelectObject(dc, g_smallFont);
    {
        char line[128];
        int total = g_keysOk + g_keysMiss, n;
        if (g_typing)
            n = wsprintfA(line, " TYPING  acc %d%%  words %d   [F8 mouse] ",
                          total ? g_keysOk * 100 / total : 100, g_wordsDone);
        else
            n = wsprintfA(line, " MOUSE  [F8 typing] ");
        GetTextExtentPoint32A(dc, line, n, &sz);
        text_at(dc, w - sz.cx - 4, h - sz.cy - 4, line, n, RGB(0, 255, 255));
    }

    SelectObject(dc, oldFont);
    ((SurfReleaseDC)vt[26])(surf, dc);
}

/* ---------------------------------------------------------------- hook installation */
static BYTE *g_code;   /* executable stub area */
static int   g_codeUsed;

static BYTE *stub(int n)
{
    BYTE *p = g_code + g_codeUsed;
    g_codeUsed += (n + 15) & ~15;
    return p;
}

static void put_rel(BYTE *at, BYTE op, DWORD to)
{
    at[0] = op;
    *(DWORD *)(at + 1) = to - (DWORD)(at + 5);
}

static int patch_jmp(DWORD addr, const BYTE *expect, int n, BYTE *target, const char *name)
{
    DWORD old;
    int i;
    if (memcmp((void *)addr, expect, n) != 0) {
        mlog("hook %s at %08x: unexpected bytes, not installed", name, addr);
        return 0;
    }
    VirtualProtect((void *)addr, n, PAGE_EXECUTE_READWRITE, &old);
    put_rel((BYTE *)addr, 0xE9, (DWORD)target);
    for (i = 5; i < n; i++) ((BYTE *)addr)[i] = 0x90;
    VirtualProtect((void *)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, n);
    mlog("hook %s at %08x installed", name, addr);
    return 1;
}

static void install_hooks(void)
{
    static const BYTE hitPro[6]  = { 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 };          /* sub esp,0x88 */
    static const BYTE loopPro[5] = { 0x83, 0xEC, 0x04, 0x53, 0x56 };                /* sub esp,4; push ebx; push esi */
    static const BYTE presPro[7] = { 0x83, 0x3D, 0xD8, 0x66, 0x48, 0x00, 0x00 };    /* cmp dword [0x4866D8],0 */
    static const BYTE personPro[7] = { 0x56, 0x33, 0xC0, 0x8B, 0x74, 0x24, 0x08 };  /* push esi; xor eax,eax; mov esi,[esp+8] */
    static const BYTE weapPro[5] = { 0xC6, 0x46, 0x02, 0x00, 0x56 };                /* mov byte [esi+2],0; push esi */
    BYTE *p;

    g_code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_code) return;

    /* hit test: trampoline = original prologue + jmp back; hook is a plain C function */
    p = stub(11);
    memcpy(p, hitPro, 6);
    put_rel(p + 6, 0xE9, G_HITTEST + 6);
    g_origHitTest = (HitTestFn)p;
    patch_jmp(G_HITTEST, hitPro, 6, (BYTE *)HitTestHook, "hittest");

    /* person check: push esi; xor eax,eax; mov esi,[esp+8] (esp-relative, so fine in a trampoline) */
    p = stub(12);
    memcpy(p, personPro, 7);
    put_rel(p + 7, 0xE9, G_PERSONCHECK + 7);
    g_origPersonCheck = (int (__cdecl *)(char *))p;
    patch_jmp(G_PERSONCHECK, personPro, 7, (BYTE *)PersonCheckHook, "person");

    /* object loop: pushad; call Tick; popad; original prologue; jmp back */
    p = stub(17);
    p[0] = 0x60;
    put_rel(p + 1, 0xE8, (DWORD)Tick);
    p[6] = 0x61;
    memcpy(p + 7, loopPro, 5);
    put_rel(p + 12, 0xE9, G_OBJLOOP + 5);
    patch_jmp(G_OBJLOOP, loopPro, 5, p, "tick");

    /* weapon handler call: original mov byte [esi+2],0; pushad; push esi; call WeaponHook;
     * add esp,4; popad; original push esi; jmp back to the call [esi+0x3C] */
    p = stub(24);
    memcpy(p, weapPro, 4);
    p[4] = 0x60;
    p[5] = 0x56;
    put_rel(p + 6, 0xE8, (DWORD)WeaponHook);
    p[11] = 0x83; p[12] = 0xC4; p[13] = 0x04;
    p[14] = 0x61;
    p[15] = 0x56;
    put_rel(p + 16, 0xE9, G_WEAPONCALL + 5);
    patch_jmp(G_WEAPONCALL, weapPro, 5, p, "weapon");

    /* present: pushfd; pushad; call DrawOverlay; popad; popfd; original cmp; jmp back */
    p = stub(21);
    p[0] = 0x9C;
    p[1] = 0x60;
    put_rel(p + 2, 0xE8, (DWORD)DrawOverlay);
    p[7] = 0x61;
    p[8] = 0x9D;
    memcpy(p + 9, presPro, 7);
    put_rel(p + 16, 0xE9, G_PRESENT_BACK);
    patch_jmp(G_PRESENT_AT, presPro, 7, p, "present");
}

static int is_virtua_cop(void)
{
    char path[MAX_PATH];
    int n = (int)GetModuleFileNameA(NULL, path, MAX_PATH), i;
    if (n < 9) return 0;
    for (i = n - 9; i < n; i++) if (path[i] >= 'a' && path[i] <= 'z') path[i] -= 32;
    if (memcmp(path + n - 9, "PPJDD.EXE", 9) != 0) return 0;
    if (GetModuleHandleA(NULL) != (HMODULE)0x400000) return 0;
    /* directory of the exe, for words.txt and the log */
    for (i = n; i > 0 && path[i - 1] != '\\'; i--) ;
    memcpy(g_dir, path, i);
    g_dir[i] = 0;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH && is_virtua_cop()) {
        char path[MAX_PATH + 16];
        path_in_dir(path, "tdmod.log");
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        g_seed = GetTickCount();
        mlog("tdmod loaded");
        load_words();
        install_hooks();
    }
    return TRUE;
}
