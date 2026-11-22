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
#define CHAR_POOL       0x556490
#define CHAR_STRIDE     0x60C
#define CHAR_SLOTS      32
#define G_SPAWN         0x404FF0   /* int spawn(record): fills a free character slot, returns it */

/* target kinds */
#define KIND_PROP       0
#define KIND_PERSON     1
#define KIND_ITEM       2          /* objects in the 0x402730 list: grenades, thrown objects */
#define KIND_PICKUP     3          /* weapon / life pickups (0x434440) */
#define KIND_PROJ       4          /* thrown weapons and boss shots (projectile pools, see below) */

/* item list (0x402730 walks it every frame): item+0x48 -> hit desc. desc byte 0 = shape:
 * 1 = circle (0x402860): position at +4..+0xC, radius +0x10, through the current matrix;
 * 2+ = box (0x4029F0): offset +4..+0xC, half-width +0x14, height +0x18, rotations +0x1C/+0x1E,
 *      world matrix at item+0x18 */
#define ITEM_DESC       0x48
#define ITEM_MATRIX     0x18
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
static char *g_all[MAXWORDS * 3];  /* every word, for picks by length limit */
static int   g_nall;

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
    if (g_nall < MAXWORDS * 3) g_all[g_nall++] = w;
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
#define MAX_EN        64
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
    int   landTick;          /* last tick the game's hit resolver gave P1's bullet to this character */
    int   landed;            /* the game registered a hit from our last completed word */
    int   tough;             /* survived this many landed words */
    int   person;            /* KIND_PROP, KIND_PERSON (enemy/hostage) or KIND_ITEM */
} Enemy;

static Enemy  g_en[MAX_EN];
static char  *g_lastHostage;
static char  *g_words[MAX_EN];   /* word text per enemy slot */
static int    g_tick;
static Enemy *g_target;
static int    g_typing = 1;
static int    g_enemySpeed;      /* defined with the speed settings below */
static int    g_propLetters;     /* tdmod.ini props=1: letters on static box-tested props too */
static int    g_debug;           /* F7 debug markers, defined with them below */
static int    item_moved(char *obj, const float *wp);
static void   debug_mark(int x, int y, int shape, int moving);
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
    for (i = 0; i < MAX_EN && g_en[i].used; i++) ;
    if (i == MAX_EN && (person == KIND_PERSON || person == KIND_PROJ)) {
        /* table full: characters and incoming projectiles take priority over prop/item letters */
        for (i = 0; i < MAX_EN; i++)
            if (g_en[i].person != KIND_PERSON && g_en[i].person != KIND_PROJ && &g_en[i] != g_target) break;
        if (i < MAX_EN) g_en[i].used = 0;
    }
    for (; i < MAX_EN; i++) {
        if (!g_en[i].used) {
            Enemy *e = &g_en[i];
            memset(e, 0, sizeof(*e));
            e->used = 1;
            e->obj = obj;
            e->word = -1;
            e->firstSeen = g_tick;
            e->person = person;
            if (person == KIND_PICKUP) {
                mlog("pickup obj=%08x type=%d", (DWORD)obj, (int)(BYTE)obj[1]);
            } else if (person == KIND_PROJ) {
                mlog("projectile obj=%08x kind=%d phase=%d", (DWORD)obj, (int)(BYTE)obj[8], (int)(BYTE)obj[9]);
            } else if (person == KIND_ITEM) {
                /* list items are a smaller structure: don't read prop/character fields */
                mlog("item obj=%08x type=%d", (DWORD)obj, (int)**(BYTE **)(obj + ITEM_DESC));
            } else if (person == KIND_PERSON) {
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
    if (g_debug) {
        /* marker = low 4 hex digits of the prop's per-frame callback (e.g. 2150 for 0x442150) */
        DWORD cb = *(DWORD *)(obj + OBJ_CALLBACK);
        const float *bp = (const float *)(obj + OBJ_BONES + 0x24);
        debug_mark(cx, cy, (int)(cb & 0xFFFF) + 0x10000, 1);
        if (g_tick % 10 == 0)
            mlog("dbg prop %08x cb %06x bone0 %d %d %d  screen %d %d",
                 (DWORD)obj, cb, (int)bp[0], (int)bp[1], (int)bp[2], cx, cy);
    }

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
    /* box-tested props are mostly static scenery (lights, signs): letters only if props=1 */
    if (player == (char *)G_P1 && g_typing && g_propLetters) track(obj);
    r = g_origHitTest(player, obj);
    if (r == 1 && player == (char *)G_P1) {
        Enemy *e = find_enemy(obj);
        if (e) e->hitTick = g_tick;
    }
    return r;
}

/* Sphere-tested props: grenades, the boss's thrown objects and similar. Same object pool as
 * the box-tested props, but hit-tested by 0x40C430 / 0x42D3E0 with spheres (0x14 bytes each,
 * byte bone, position at +4, radius at +0x10) at *(obj+4)+0x0C, count at obj+0x49F. */
#define OBJ_SPHERES     0x0C
#define OBJ_NSPHERE     0x49F

static void track_sphere_prop(char *obj)
{
    int n = (signed char)obj[OBJ_NSPHERE], i;
    char *spheres;
    DWORD mask;
    float f = *(float *)G_FOCAL;
    float bestR = -1.0f;
    int cx = 0, cy = 0, topY = 0x7FFFFFFF, topX = 0;
    Enemy *e;

    if (n <= 0) return;
    spheres = *(char **)(*(char **)(obj + 4) + OBJ_SPHERES);
    if (!spheres) return;
    mask = *(DWORD *)(obj + OBJ_BONEMASK);
    for (i = 0; i < n; i++) {
        char *s = spheres + i * SPHERE_SIZE;
        float r = *(float *)(s + 0x10);
        float c[3], sx, sy, sr;
        if (!((mask >> (s[0] & 31)) & 1)) continue;
        mtx_push();
        mtx_mul(obj + OBJ_BONES + (signed char)s[0] * 0x30);
        mtx_xform(*(float *)(s + 4), *(float *)(s + 8), *(float *)(s + 0xC), c);
        mtx_pop();
        if (c[2] < 1.0f) continue;
        sx = c[0] * f / c[2];
        sy = c[1] * f / c[2];
        sr = r * f / c[2];
        if (sr > bestR) { bestR = sr; cx = (int)sx; cy = (int)sy; }
        if ((int)(sy - sr) < topY) { topY = (int)(sy - sr); topX = (int)sx; }
    }
    if (bestR < 0) return;
    /* Static sphere props are mostly scenery. Label the explosive ones (barrels: their callback
     * counts down hit points at +0x49C, then switches to the explosion 0x40CF20) and anything
     * that has moved (thrown, launched). */
    {
        DWORD cb = *(DWORD *)(obj + OBJ_CALLBACK);
        int bone = (signed char)spheres[0];
        int moving = item_moved(obj, (const float *)(obj + OBJ_BONES + bone * 0x30 + 0x24));
        int explosive = cb == 0x40CE90 || cb == 0x40D090 || cb == 0x40D160 || cb == 0x40D260;
        debug_mark(cx, cy, explosive ? 91 : 90, moving || explosive);
        if (g_debug && g_tick % 10 == 0)
            mlog("dbg sphere prop %08x cb %06x moving %d explosive %d", (DWORD)obj, cb, moving, explosive);
        if (!moving && !explosive) return;
    }

    e = find_enemy(obj);
    if (!e) e = new_enemy(obj, 0);
    if (!e) return;
    e->lastSeen = g_tick;
    e->sx = cx; e->sy = cy;
    e->ax[0] = cx; e->ay[0] = cy;
    e->naim = 1;
    e->tx = topX; e->ty = topY;
}

static HitTestFn g_origSphereTest, g_origSphereTest2;

static int sphere_hook_common(HitTestFn orig, char *player, char *obj)
{
    int r;
    if (player == (char *)G_P1 && g_typing) track_sphere_prop(obj);
    r = orig(player, obj);
    if (r == 1 && player == (char *)G_P1) {
        Enemy *e = find_enemy(obj);
        if (e) e->hitTick = g_tick;
    }
    return r;
}

static int __cdecl SphereTestHook(char *player, char *obj)  { return sphere_hook_common(g_origSphereTest, player, obj); }
static int __cdecl SphereTest2Hook(char *player, char *obj) { return sphere_hook_common(g_origSphereTest2, player, obj); }

/* List items (grenades, thrown objects, pickups), hit-tested by 0x402860 (circle) or 0x4029F0
 * (box) from the per-frame walk at 0x402730. */
static const MulFn mtx_mulObj = (MulFn)0x446BF0;

/* Debug overlay (F7): a marker on every list item showing its shape and whether it counts as
 * moving, to see what the item list holds. */
#define MAX_DBG 96
typedef struct { int x, y, shape, moving, tick; } DbgMark;
static DbgMark g_dbg[MAX_DBG];
static int     g_ndbg, g_dbgTick, g_debug;

static void debug_mark(int x, int y, int shape, int moving)
{
    if (!g_debug) return;
    if (g_dbgTick != g_tick) { g_dbgTick = g_tick; g_ndbg = 0; }
    if (g_ndbg >= MAX_DBG) return;
    g_dbg[g_ndbg].x = x; g_dbg[g_ndbg].y = y;
    g_dbg[g_ndbg].shape = shape; g_dbg[g_ndbg].moving = moving;
    g_dbg[g_ndbg].tick = g_tick;
    g_ndbg++;
}

/* first-seen world position of each list item; an item counts as moving once it has drifted */
#define MAX_ITEMPOS   96
#define ITEM_MOVE_MIN 20.0f
typedef struct { char *obj; float x, y, z; int moving; int seen; } ItemPos;
static ItemPos g_itemPos[MAX_ITEMPOS];

static int item_moved(char *obj, const float *wp)
{
    int i, freeSlot = -1, oldest = 0;
    float dx, dy, dz;
    for (i = 0; i < MAX_ITEMPOS; i++) {
        if (g_itemPos[i].obj == obj) break;
        if (!g_itemPos[i].obj && freeSlot < 0) freeSlot = i;
        if (g_itemPos[i].seen < g_itemPos[oldest].seen) oldest = i;
    }
    if (i == MAX_ITEMPOS) {
        /* new (or recycled) item: remember where it started */
        i = freeSlot >= 0 ? freeSlot : oldest;
        g_itemPos[i].obj = obj;
        g_itemPos[i].x = wp[0]; g_itemPos[i].y = wp[1]; g_itemPos[i].z = wp[2];
        g_itemPos[i].moving = 0;
    } else if (g_tick - g_itemPos[i].seen > STALE_TICKS) {
        /* the game reused this item slot for something new */
        g_itemPos[i].x = wp[0]; g_itemPos[i].y = wp[1]; g_itemPos[i].z = wp[2];
        g_itemPos[i].moving = 0;
    }
    g_itemPos[i].seen = g_tick;
    if (!g_itemPos[i].moving) {
        dx = wp[0] - g_itemPos[i].x; dy = wp[1] - g_itemPos[i].y; dz = wp[2] - g_itemPos[i].z;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dz < 0) dz = -dz;
        if (dx + dy + dz > ITEM_MOVE_MIN) g_itemPos[i].moving = 1;
    }
    return g_itemPos[i].moving;
}

static void track_item(char *obj, int box)
{
    char *d = *(char **)(obj + ITEM_DESC);
    float f = *(float *)G_FOCAL;
    float c[3];
    int cx, cy, top;
    Enemy *e;

    if (!d) return;
    if (box) {
        float h = *(float *)(d + 0x18);
        float t[3];
        mtx_push();
        mtx_trans(*(float *)(d + 4), *(float *)(d + 8), *(float *)(d + 0xC));
        mtx_rotB(*(unsigned short *)(d + 0x1E));
        mtx_rotA(*(unsigned short *)(d + 0x1C));
        mtx_mulObj(obj + ITEM_MATRIX);
        mtx_xform(0.0f, h * 0.5f, 0.0f, c);
        mtx_xform(0.0f, h, 0.0f, t);
        mtx_pop();
        if (c[2] < 1.0f || t[2] < 1.0f) return;
        top = (int)(t[1] * f / t[2]);
        cy = (int)(c[1] * f / c[2]);
        if (top > cy) top = cy - (top - cy);
    } else {
        float r = *(float *)(d + 0x10);
        mtx_xform(*(float *)(d + 4), *(float *)(d + 8), *(float *)(d + 0xC), c);
        if (c[2] < 1.0f) return;
        cy = (int)(c[1] * f / c[2]);
        /* label just above the center: some list items have a very large radius */
        top = cy - ((int)(r * f / c[2]) < 12 ? (int)(r * f / c[2]) : 12);
    }
    cx = (int)(c[0] * f / c[2]);

    /* The item list also holds static scenery and invisible markers. Only label items that have
     * moved in the world (grenades, thrown objects). desc+4..+0xC is the item's world position
     * (0x4027F0 subtracts the camera position from it), so camera pans don't count. */
    {
        int moving = item_moved(obj, (const float *)(d + 4));
        debug_mark(cx, cy, **(BYTE **)(obj + ITEM_DESC), moving);
        if (g_debug && g_tick % 10 == 0) {
            /* trace for working out which field moves with grenades/thrown objects */
            const float *dp = (const float *)(d + 4), *mp = (const float *)(obj + ITEM_MATRIX + 0x24);
            mlog("dbg item %08x shape %d desc %d %d %d  mat %d %d %d  screen %d %d  moving %d",
                 (DWORD)obj, (int)(BYTE)d[0], (int)dp[0], (int)dp[1], (int)dp[2],
                 (int)mp[0], (int)mp[1], (int)mp[2], cx, cy, moving);
        }
        if (!moving) return;
    }

    e = find_enemy(obj);
    if (!e) e = new_enemy(obj, KIND_ITEM);
    if (!e) return;
    e->lastSeen = g_tick;
    e->sx = cx; e->sy = cy;
    e->ax[0] = cx; e->ay[0] = cy;
    e->naim = 1;
    e->tx = cx; e->ty = top;
}

/* Weapon/life pickups: their own table of 0x28-byte records, hit-tested every frame by
 * 0x434440(pickup, player, sphere) with the matrix already set for the pickup. On a hit the
 * game sets pickup+0 = 1 and pickup+2 = the player id; 0x4344A0 then hands out the reward. */
typedef void (__cdecl *PickupFn)(char *pickup, char *player, char *sphere);
static PickupFn g_origPickup;

static void __cdecl PickupHook(char *pickup, char *player, char *sphere)
{
    if (player == (char *)G_P1 && g_typing && pickup[0] == 0 && sphere) {
        float f = *(float *)G_FOCAL, c[3];
        mtx_xform(*(float *)(sphere + 4), *(float *)(sphere + 8), *(float *)(sphere + 0xC), c);
        if (c[2] >= 1.0f) {
            Enemy *e = find_enemy(pickup);
            int r = (int)(*(float *)(sphere + 0x10) * f / c[2]);
            if (!e) e = new_enemy(pickup, KIND_PICKUP);
            if (e) {
                e->lastSeen = g_tick;
                e->sx = (int)(c[0] * f / c[2]);
                e->sy = (int)(c[1] * f / c[2]);
                e->ax[0] = e->sx; e->ay[0] = e->sy;
                e->naim = 1;
                e->tx = e->sx; e->ty = e->sy - r;
            }
        }
    }
    g_origPickup(pickup, player, sphere);
    if (player == (char *)G_P1 && pickup[0] == 1 && pickup[2] == 1) {
        Enemy *e = find_enemy(pickup);
        if (e) e->hitTick = g_tick;
    }
}

static HitTestFn g_origItemCircle, g_origItemBox;

static int item_hook_common(HitTestFn orig, int box, char *player, char *obj)
{
    int r;
    if (player == (char *)G_P1 && g_typing) track_item(obj, box);
    r = orig(player, obj);
    if ((r & 0xFF) == 1 && player == (char *)G_P1) {   /* these return the result in al */
        Enemy *e = find_enemy(obj);
        if (e) e->hitTick = g_tick;
    }
    return r;
}

static int __cdecl ItemCircleHook(char *player, char *obj) { return item_hook_common(g_origItemCircle, 0, player, obj); }
static int __cdecl ItemBoxHook(char *player, char *obj)    { return item_hook_common(g_origItemBox, 1, player, obj); }

/*
 * Projectiles. Three pools of 0xAC-byte records, separate from the prop and item lists:
 *   0x5558D0 x10  thrown weapons (grenades/bombs kind 3, knives/axes kind 4), updated by
 *                 0x4396C0; shot down via 0x439D30 when the thrower's +0x599 flag is set
 *   0x4C2CB8 x20  boss shots, shootable while WORD +0x82 >= 20, hit test 0x437C80
 *   0x4C7338 x10  boss shots, active while +0x82 != 0, shootable while > 10; their hit sphere
 *                 is the object's origin with radius +0x82 * 0.8 (0x43A970)
 * Common layout: +0 active, +8 kind, +9 phase, +0x0A state, +0x0C world position,
 * +0x48 world matrix, +0x78 hit spheres (0x14 bytes, list ends with a 0xFF byte),
 * +0x7C thrower, +0x82 life/flight counter. The boss pools run inside the boss AI 0x4066A0.
 */
#define PROJ_POOL1 0x5558D0
#define PROJ_POOL2 0x4C2CB8
#define PROJ_POOL3 0x4C7338
#define PROJ_SIZE  0xAC

static void track_proj(char *pr, int originR)
{
    float f = *(float *)G_FOCAL;
    float bestR = -1.0f, c[3], sx, sy, sr;
    int cx = 0, cy = 0, topY = 0x7FFFFFFF, topX = 0;
    char *s;
    Enemy *e;

    mtx_push();
    mtx_mul(pr + 0x48);
    if (originR) {
        float r = (float)*(short *)(pr + 0x82) * 0.8f;
        mtx_xform(0.0f, 0.0f, 0.0f, c);
        if (c[2] >= 1.0f) {
            bestR = r * f / c[2];
            cx = topX = (int)(c[0] * f / c[2]);
            cy = (int)(c[1] * f / c[2]);
            topY = cy - (int)bestR;
        }
    } else {
        int n = 0;
        for (s = *(char **)(pr + 0x78); s && (BYTE)s[0] != 0xFF && n < 16; s += SPHERE_SIZE, n++) {
            mtx_xform(*(float *)(s + 4), *(float *)(s + 8), *(float *)(s + 0xC), c);
            if (c[2] < 1.0f) continue;
            sx = c[0] * f / c[2];
            sy = c[1] * f / c[2];
            sr = *(float *)(s + 0x10) * f / c[2];
            if (sr > bestR) { bestR = sr; cx = (int)sx; cy = (int)sy; }
            if ((int)(sy - sr) < topY) { topY = (int)(sy - sr); topX = (int)sx; }
        }
    }
    mtx_pop();
    if (bestR < 0) return;
    debug_mark(cx, cy, 0x20000 + (int)(BYTE)pr[8], 1);

    e = find_enemy(pr);
    if (!e) e = new_enemy(pr, KIND_PROJ);
    if (!e) return;
    e->lastSeen = g_tick;
    e->sx = cx; e->sy = cy;
    e->ax[0] = cx; e->ay[0] = cy;
    e->naim = 1;
    e->tx = topX; e->ty = (cy - topY < 12) ? topY : cy - 12;
}

/* thrown weapons: label while 0x439D30 would accept a shot */
static int (__cdecl *g_origProjUpdate)(char *pr);
static int (__cdecl *g_origProjHit)(char *player, char *pr);

static int __cdecl ProjUpdateHook(char *pr)
{
    char *thrower = *(char **)(pr + 0x7C);
    if (g_typing && pr[0] && thrower && thrower[0x599]
        && (pr[9] == 1 ? pr[0x0A] == 0 : pr[0x0A] <= 1))
        track_proj(pr, 0);
    return g_origProjUpdate(pr);
}

static int __cdecl ProjHitHook(char *player, char *pr)
{
    int r = g_origProjHit(player, pr);
    if ((r & 0xFF) && player == (char *)G_P1) {
        Enemy *e = find_enemy(pr);
        if (e) e->hitTick = g_tick;
    }
    return r;
}

/* boss shots: called from the boss AI hook with the camera matrix on the stack */
static short g_bossLife2[20], g_bossLife3[10];

static void track_boss_shots(void)
{
    int i;
    for (i = 0; i < 20; i++) {
        char *pr = (char *)PROJ_POOL2 + i * PROJ_SIZE;
        g_bossLife2[i] = *(short *)(pr + 0x82);
        if (g_typing && g_bossLife2[i] >= 20 && *(char **)(pr + 0x78)) track_proj(pr, 0);
    }
    for (i = 0; i < 10; i++) {
        char *pr = (char *)PROJ_POOL3 + i * PROJ_SIZE;
        g_bossLife3[i] = *(short *)(pr + 0x82);
        if (g_typing && g_bossLife3[i] > 10) track_proj(pr, 1);
    }
}

/* after the boss AI ran: a shootable boss shot whose counter dropped to 0 on a frame P1 fired
 * was shot down by P1 */
static void check_boss_shots(void)
{
    int i;
    if (!*(BYTE *)(G_P1 + 2)) return;
    for (i = 0; i < 30; i++) {
        char *pr = i < 20 ? (char *)PROJ_POOL2 + i * PROJ_SIZE : (char *)PROJ_POOL3 + (i - 20) * PROJ_SIZE;
        short before = i < 20 ? g_bossLife2[i] : g_bossLife3[i - 20];
        if (before > (i < 20 ? 19 : 10) && *(short *)(pr + 0x82) == 0) {
            Enemy *e = find_enemy(pr);
            if (e) e->hitTick = g_tick;
        }
    }
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

/* bitmask of first letters (a-z) of words currently shown, except on `except` */
static DWORD initials_in_use(Enemy *except)
{
    DWORD mask = 0;
    int i;
    for (i = 0; i < MAX_EN; i++) {
        Enemy *e = &g_en[i];
        char c;
        if (e == except || !e->used || e->word < 0) continue;
        c = g_words[i][0];
        if (c >= 'a' && c <= 'z') mask |= 1u << (c - 'a');
    }
    return mask;
}

static int initial_bit(const char *w) { return (w[0] >= 'a' && w[0] <= 'z') ? (int)(1u << (w[0] - 'a')) : 0; }

/* Random word of length <= maxlen, preferably close to it (maxlen-3 .. maxlen) and not starting
 * with a letter in `avoid`. Relaxes the initial, then the lower bound, before giving up. */
static char *pick_word(int maxlen, DWORD avoid)
{
    int pass, tries;
    if (g_nall == 0) return NULL;
    for (pass = 0; pass < 3; pass++) {
        int lo = pass < 2 ? maxlen - 3 : 1;
        for (tries = 0; tries < 400; tries++) {
            char *w = g_all[rnd(g_nall)];
            int len = slen(w);
            if (len > maxlen || len < lo) continue;
            if (pass == 0 && (initial_bit(w) & avoid)) continue;
            return w;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- level word map */
/*
 * Levels are scripted: every character appearance comes from a fixed spawn record in the game's
 * static data (0x4A40C0...), picked by the spawn routine 0x404FF0. The record address is the
 * same every playthrough, so it identifies "this appearance in this level".
 *
 * wordmap.txt lists appearances in the order they were first met, each with a word length
 * limit. New appearances are appended automatically (default limit 6). At startup one word is
 * pre-picked per entry, so each run has its own fixed word list; nearby entries (likely on
 * screen together) get different first letters.
 */
#define MAX_MAP        1024
#define MAP_DEFAULTLEN 6
#define MAP_NEIGHBORS  4
typedef struct { DWORD rec; int maxlen; char *word; } MapEntry;
static MapEntry g_map[MAX_MAP];
static int      g_nmap;
static char    *g_slotRec[CHAR_SLOTS];   /* spawn record each character slot was filled from */

static MapEntry *map_find(DWORD rec)
{
    int i;
    for (i = 0; i < g_nmap; i++) if (g_map[i].rec == rec) return &g_map[i];
    return NULL;
}

static void map_pick(int i)
{
    DWORD avoid = 0;
    int k;
    for (k = i - MAP_NEIGHBORS; k <= i + MAP_NEIGHBORS; k++)
        if (k >= 0 && k < g_nmap && k != i && g_map[k].word) avoid |= initial_bit(g_map[k].word);
    g_map[i].word = pick_word(g_map[i].maxlen, avoid);
}

static void load_map(void)
{
    static char buf[32768];
    char path[MAX_PATH + 16];
    HANDLE f;
    DWORD got = 0;
    int i = 0;
    path_in_dir(path, "wordmap.txt");
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(f);
    }
    buf[got] = 0;
    /* each line: <record hex> <max length>   anything after '#' is a comment */
    while (buf[i] && g_nmap < MAX_MAP) {
        DWORD rec = 0;
        int len = 0, digits = 0;
        while (buf[i] == ' ' || buf[i] == '\t') i++;
        for (;; i++) {
            char c = buf[i];
            int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                    (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0) break;
            rec = rec * 16 + (DWORD)v;
            digits++;
        }
        while (buf[i] == ' ' || buf[i] == '\t') i++;
        while (buf[i] >= '0' && buf[i] <= '9') len = len * 10 + (buf[i++] - '0');
        if (digits && len > 0 && !map_find(rec)) {
            g_map[g_nmap].rec = rec;
            g_map[g_nmap].maxlen = len > MAXWORDLEN ? MAXWORDLEN : len;
            g_map[g_nmap].word = NULL;
            g_nmap++;
        }
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i]) i++;
    }
    for (i = 0; i < g_nmap; i++) map_pick(i);
    mlog("word map: %d appearances", g_nmap);
}

static void map_append(char *rec)
{
    char path[MAX_PATH + 16], line[160];
    HANDLE f;
    DWORD n;
    MapEntry *m;
    if (g_nmap >= MAX_MAP) return;
    m = &g_map[g_nmap];
    m->rec = (DWORD)rec;
    m->maxlen = MAP_DEFAULTLEN;
    m->word = NULL;
    g_nmap++;
    map_pick(g_nmap - 1);

    path_in_dir(path, "wordmap.txt");
    f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    if (g_nmap == 1) {
        n = wsprintfA(line, "# Typing mod word map: one line per scripted appearance, in the order first met.\r\n"
                            "# <appearance> <max word length>   # notes\r\n");
        WriteFile(f, line, n, &n, NULL);
    }
    n = wsprintfA(line, "%08x %d   # #%d  wave %d  model %d\r\n",
                  (DWORD)rec, MAP_DEFAULTLEN, g_nmap, (BYTE)rec[1], (BYTE)rec[4]);
    WriteFile(f, line, n, &n, NULL);
    CloseHandle(f);
    mlog("word map: new appearance %08x (#%d)", (DWORD)rec, g_nmap);
}

static int (__cdecl *g_origSpawn)(char *rec);

static int __cdecl SpawnHook(char *rec)
{
    int slot = g_origSpawn(rec);
    if (slot >= 0 && slot < CHAR_SLOTS) {
        g_slotRec[slot] = rec;
        /* custom spawns (spawns.txt) live in the mod's memory: keep them out of the word map */
        if (!map_find((DWORD)rec) && (DWORD)rec < 0x10000000) map_append(rec);
    }
    return slot;
}

/* ---------------------------------------------------------------- custom spawns */
/*
 * Level data: 0x4AE020[stage] -> area table -> scene table (8 bytes per scene) -> a list of
 * 0x24-byte spawn records ending with 0xFF. When the camera reaches a scene, 0x4424D0 copies the
 * scene's records into the spawn table 0x5555A0 (8 bytes each: +0 enabled, +1 character slot or
 * 0xFF, +2 count, +4 record pointer; count at 0x555692). Waves then spawn from that table.
 *
 * spawns.txt in the game folder replaces whole scenes:
 *     [scene <stage> <area> <scene>]
 *     like=<record hex> wave=<n> model=<n> type=<n> diff=<n> x=<f> y=<f> z=<f> angle=<n> script=<hex>
 * Each line is one spawn: a copy of the record named by like= (default: the scene's first
 * record) with the given fields changed. tools/levels.py prints every scene in this format.
 * The file is read again on every scene load, so edits apply the next time the scene starts.
 *
 * Record fields (copied by 0x404FF0): +0 type (1 enemy, 0 hostage; boss bits 0x3C), +1 wave,
 * +2 delay before the next wave, +3 delay before shooting (scaled by difficulty), +4 model,
 * +5 item dropped when shot, +6 linked prop or 0xFF, +7 hit-reaction override, +8 death style,
 * +9 turn to face the camera, +0x0A difficulty, +0x0B flags, +0x0C/+0x10/+0x14 position,
 * +0x18 facing, +0x1A fly-off angle, +0x1C behaviour script, +0x20 ground height (float).
 * Script: word reaction table (c+0x553), word weapon (c+0x571, 0 unarmed), then steps of
 * 4 words (action -> handler table 0x486480, animation, parameter, turn), ending 0xFFFF.
 */
#define G_LEVELS     0x4AE020
#define G_STAGE      0x5554B8     /* WORD */
#define G_AREA       0x5554BA     /* WORD */
#define G_SCENE      0x5554BC     /* WORD */
#define G_SPAWNTAB   0x5555A0
#define G_SPAWNCOUNT 0x555692
#define SPAWN_REC    0x24
#define MAX_TABLE    30           /* the spawn table holds 30 entries before 0x555690 */
#define MAX_CUSTOM   256

#define SCRIPT_WORDS (2 + 4 * 32 + 1)   /* header, up to 32 steps, 0xFFFF */
static BYTE g_custom[MAX_CUSTOM][SPAWN_REC];
static WORD g_scripts[MAX_CUSTOM][SCRIPT_WORDS];
static void (__cdecl *g_origSceneLoad)(void);

static int rec_ok(DWORD a)
{
    return a >= 0x49D000 && a < 0x4B0000 && !IsBadReadPtr((void *)a, SPAWN_REC);
}

static char *skip_sp(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

static char *parse_int(char *p, int *out, int hex)
{
    int v = 0, neg = 0, any = 0;
    p = skip_sp(p);
    if (*p == '-') { neg = 1; p++; }
    if (!hex && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { hex = 1; p += 2; }
    for (;; p++) {
        int d = (*p >= '0' && *p <= '9') ? *p - '0' :
                hex && (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 :
                hex && (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * (hex ? 16 : 10) + d;
        any = 1;
    }
    if (any) *out = neg ? -v : v;
    return p;
}

static char *parse_float(char *p, float *out)
{
    float v = 0.0f, scale = 1.0f;
    int neg = 0, any = 0;
    p = skip_sp(p);
    if (*p == '-') { neg = 1; p++; }
    for (; *p >= '0' && *p <= '9'; p++) { v = v * 10.0f + (float)(*p - '0'); any = 1; }
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++) { scale *= 0.1f; v += (float)(*p - '0') * scale; any = 1; }
    if (any) *out = neg ? -v : v;
    return p;
}

static int key_is(char *p, const char *k, char **val)
{
    int n = slen(k);
    if (memcmp(p, k, n) == 0 && p[n] == '=') { *val = p + n + 1; return 1; }
    return 0;
}

/* first record of the scene the game is loading */
static DWORD scene_records(int st, int ar, int sc)
{
    DWORD areas = *(DWORD *)(G_LEVELS + 4 * st), scenes;
    if (IsBadReadPtr((void *)areas, 4 * (ar + 1))) return 0;
    scenes = *(DWORD *)(areas + 4 * ar);
    if (IsBadReadPtr((void *)scenes, 8 * (sc + 1))) return 0;
    return *(DWORD *)(scenes + 8 * sc);
}

/* parse the [scene] block for (st, ar, sc) from spawns.txt into g_custom; -1 = no block */
static int load_custom_scene(int st, int ar, int sc, DWORD first)
{
    static char buf[65536];
    char path[MAX_PATH + 16], *p;
    HANDLE f;
    DWORD got = 0;
    int n = -1, inBlock = 0;

    path_in_dir(path, "spawns.txt");
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;

    for (p = buf; *p; ) {
        char *line = skip_sp(p), *q;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        for (q = line; *q; q++) if (*q == '#' || *q == '\r') { *q = 0; break; }
        if (!*line) continue;
        if (line[0] == '[') {
            int s = -1, a = -1, c = -1;
            if (memcmp(line, "[scene", 6) != 0) { inBlock = 0; continue; }
            q = parse_int(line + 6, &s, 0);
            q = parse_int(q, &a, 0);
            parse_int(q, &c, 0);
            inBlock = (s == st && a == ar && c == sc);
            if (inBlock && n < 0) n = 0;
            continue;
        }
        if (!inBlock || n >= MAX_CUSTOM || n >= MAX_TABLE) continue;
        {
            BYTE *r = g_custom[n];
            DWORD like = first;
            int v;
            float fv;
            char *val, *p2, *v2;
            /* like= first, so the other keys apply on top of the copy */
            for (q = line; *q; q++) {
                if ((q == line || q[-1] == ' ' || q[-1] == '\t') && key_is(q, "like", &val)) {
                    v = 0;
                    parse_int(val, &v, 1);
                    like = (DWORD)v;
                }
            }
            if (!rec_ok(like)) { mlog("spawns.txt: bad like=%08x, line skipped", like); continue; }
            memcpy(r, (void *)like, SPAWN_REC);
            for (q = line; *q; q++) {
                if (q != line && q[-1] != ' ' && q[-1] != '\t') continue;
                v = 0x7FFFFFFF;
                if (key_is(q, "wave", &val))        { parse_int(val, &v, 0); if (v != 0x7FFFFFFF) r[1] = (BYTE)v; }
                else if (key_is(q, "model", &val))  { parse_int(val, &v, 0); if (v != 0x7FFFFFFF) r[4] = (BYTE)v; }
                else if (key_is(q, "type", &val))   { parse_int(val, &v, 0); if (v != 0x7FFFFFFF) r[0] = (BYTE)v; }
                else if (key_is(q, "diff", &val))   { parse_int(val, &v, 0); if (v != 0x7FFFFFFF) r[0x0A] = (BYTE)v; }
                else if (key_is(q, "angle", &val))  { parse_int(val, &v, 0); if (v != 0x7FFFFFFF) *(WORD *)(r + 0x18) = (WORD)v; }
                else if (key_is(q, "script", &val)) { parse_int(val, &v, 1); if (v != 0x7FFFFFFF) *(DWORD *)(r + 0x1C) = (DWORD)v; }
                else if (key_is(q, "x", &val)) { fv = *(float *)(r + 0x0C); parse_float(val, &fv); *(float *)(r + 0x0C) = fv; }
                else if (key_is(q, "y", &val)) { fv = *(float *)(r + 0x10); parse_float(val, &fv); *(float *)(r + 0x10) = fv; }
                else if (key_is(q, "z", &val)) { fv = *(float *)(r + 0x14); parse_float(val, &fv); *(float *)(r + 0x14) = fv; }
                else if (key_is(q, "floor", &val)) { fv = *(float *)(r + 0x20); parse_float(val, &fv); *(float *)(r + 0x20) = fv; }
                else if (key_is(q, "next", &val))  { parse_int(val, &v, 0); if (v != 0x7FFFFFFF) r[2] = (BYTE)v; }
                else if (key_is(q, "wait", &val))  { parse_int(val, &v, 0); if (v != 0x7FFFFFFF) r[3] = (BYTE)v; }
                else if (key_is(q, "drop", &val))  { parse_int(val, &v, 0); if (v != 0x7FFFFFFF) r[5] = (BYTE)v; }
                else if (key_is(q, "death", &val)) { parse_int(val, &v, 0); if (v != 0x7FFFFFFF) r[8] = (BYTE)v; }
                else if (key_is(q, "face", &val))  { parse_int(val, &v, 0); if (v != 0x7FFFFFFF) r[9] = (BYTE)v; }
                else if (key_is(q, "flags", &val)) { parse_int(val, &v, 1); if (v != 0x7FFFFFFF) r[0x0B] = (BYTE)v; }
            }
            /* steps= builds a new behaviour script: act:anim[:param[:turn]] separated by commas,
               with the header (reaction table, weapon) taken from the copied script unless
               react= / weapon= are given */
            for (q = line; *q; q++) {
                WORD *s = g_scripts[n], *src = *(WORD **)(r + 0x1C);
                int k = 2, react, weapon;
                if ((q != line && q[-1] != ' ' && q[-1] != '\t') || !key_is(q, "steps", &val)) continue;
                react = IsBadReadPtr(src, 4) ? 0 : src[0] & 0xFF;
                weapon = IsBadReadPtr(src, 4) ? 1 : src[1] & 0xFF;
                for (p2 = line; *p2; p2++) {
                    if (p2 != line && p2[-1] != ' ' && p2[-1] != '\t') continue;
                    if (key_is(p2, "react", &v2))  parse_int(v2, &react, 0);
                    if (key_is(p2, "weapon", &v2)) parse_int(v2, &weapon, 0);
                }
                s[0] = (WORD)react;
                s[1] = (WORD)weapon;
                while (*val && *val != ' ' && *val != '\t' && k + 4 < SCRIPT_WORDS - 1) {
                    int f[4] = { 0, 0, 0, 0 }, j;
                    for (j = 0; j < 4; j++) {
                        val = parse_int(val, &f[j], 0);
                        if (*val != ':') break;
                        val++;
                    }
                    if (f[0] < 0 || f[0] > 29) { mlog("spawns.txt: bad action %d in steps=", f[0]); break; }
                    s[k++] = (WORD)f[0]; s[k++] = (WORD)f[1]; s[k++] = (WORD)f[2]; s[k++] = (WORD)f[3];
                    if (*val != ',') break;
                    val++;
                }
                s[k] = 0xFFFF;
                *(DWORD *)(r + 0x1C) = (DWORD)s;
                break;
            }
            if (r[0] & 0x80 || r[4] > 80 || IsBadReadPtr(*(void **)(r + 0x1C), 4)) {
                mlog("spawns.txt: bad model or script on scene %d %d %d line %d, skipped", st, ar, sc, n + 1);
                continue;
            }
            n++;
        }
    }
    return n;
}

static void __cdecl SceneLoadHook(void)
{
    int st = *(short *)G_STAGE, ar = *(short *)G_AREA, sc = *(short *)G_SCENE, n, i;
    DWORD first;
    g_origSceneLoad();
    first = scene_records(st, ar, sc);
    mlog("scene %d %d %d: %d spawns (records %08x)", st, ar, sc, (int)*(short *)G_SPAWNCOUNT, first);
    if (!rec_ok(first)) first = 0x4A3FC0;      /* a known record, for like= defaults */
    n = load_custom_scene(st, ar, sc, first);
    if (n < 0) return;
    for (i = 0; i < n; i++) {
        BYTE *t = (BYTE *)G_SPAWNTAB + 8 * i;
        t[0] = 1;
        t[1] = 0xFF;
        t[2] = 0;
        *(DWORD *)(t + 4) = (DWORD)g_custom[i];
    }
    *(short *)G_SPAWNCOUNT = (short)n;
    mlog("scene %d %d %d: replaced with %d spawns from spawns.txt", st, ar, sc, n);
}

static MapEntry *map_for(Enemy *e)
{
    DWORD d = (DWORD)e->obj - CHAR_POOL;
    int slot;
    if (e->person != KIND_PERSON || (DWORD)e->obj < CHAR_POOL || d % CHAR_STRIDE) return NULL;
    slot = (int)(d / CHAR_STRIDE);
    if (slot >= CHAR_SLOTS || !g_slotRec[slot]) return NULL;
    return map_find((DWORD)g_slotRec[slot]);
}

static const char g_letters[26][2] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"
};

/* ---------------------------------------------------------------- bosses */
/*
 * Bosses are characters with +0x54E & 0x3C (0x4066A0). Their hits-taken counter is +0x54D; the
 * boss scripts compare it with a per-boss maximum (10, 15, 60 or 100) and redraw the life bar
 * 0x565A66 from it on every hit (full = 160 or 200). Each completed phrase takes a third of the
 * boss's life: just before the shot, the mod raises the counter to one below the next third, and
 * the game's own hit takes it the rest of the way, so the life bar, hit reactions and death all
 * run as normal. The maximum is worked out from the first real hit: max = hits * full / drop.
 */
#define PER_BOSS     0x54E
#define PER_BOSSHITS 0x54D
#define G_BOSSBAR    0x565A66
static char *g_bossObj;
static int   g_bossOwed;      /* thirds paid for by completed phrases (0..3) */
static int   g_bossFull, g_bossMax, g_bossWritten, g_bossLastShot, g_bossPrevCnt;
static char *g_bossLastPhrase;
static int   g_nshots;                       /* shot queue, defined with the firing code below */
static void  queue_shots(Enemy *e, int n);

static void boss_reset(char *c)
{
    g_bossObj = c;
    g_bossOwed = g_bossMax = g_bossWritten = g_bossLastShot = g_bossFull = g_bossPrevCnt = 0;
    mlog("boss %08x type %02x", (DWORD)c, (int)(BYTE)c[PER_BOSS]);
}

static int is_boss(Enemy *e)
{
    return e->person == KIND_PERSON && (e->obj[PER_BOSS] & 0x3C);
}

static int boss_need(void)
{
    if (g_bossOwed >= 3) return g_bossMax;
    return (g_bossOwed * g_bossMax + 2) / 3;
}

/* called just before a shot at the boss: move the counter to one hit short of the next third */
static void boss_prewrite(char *c)
{
    int cnt = (BYTE)c[PER_BOSSHITS], need;
    if (c != g_bossObj || !g_bossMax) return;          /* not calibrated yet: plain shot */
    need = boss_need();
    if (need - 1 > cnt) {
        c[PER_BOSSHITS] = (char)(need - 1);
        g_bossWritten = 1;
        mlog("boss %08x: hits %d -> %d (third %d of max %d)", (DWORD)c, cnt, need - 1, g_bossOwed, g_bossMax);
    }
}

static void boss_update(void)
{
    char *c = g_bossObj;
    int cnt, bar;
    Enemy *e;
    if (!c) return;
    if (c[PER_ALIVE] != 1 || !(c[PER_BOSS] & 0x3C)) return;
    cnt = (BYTE)c[PER_BOSSHITS];
    bar = *(BYTE *)G_BOSSBAR;
    if (cnt < g_bossPrevCnt) boss_reset(c);              /* same slot, new boss fight */
    g_bossPrevCnt = cnt;
    if (cnt == 0 && bar > g_bossFull) g_bossFull = bar;
    if (!g_bossMax && !g_bossWritten && cnt > 0 && g_bossFull > bar) {
        g_bossMax = (cnt * g_bossFull + (g_bossFull - bar) / 2) / (g_bossFull - bar);
        if (g_bossMax < cnt + 1) g_bossMax = cnt + 1;
        mlog("boss %08x: max hits %d (hits %d, bar %d of %d)", (DWORD)c, g_bossMax, cnt, bar, g_bossFull);
    }
    /* phrases typed but not yet paid in hits: fire another shot once the last one has settled */
    if (g_bossOwed == 0 || g_nshots > 0 || g_tick - g_bossLastShot < 20) return;
    if (g_bossMax && cnt >= boss_need() && g_bossOwed < 3) return;
    if (!g_bossMax && g_bossLastShot && g_tick - g_bossLastShot < 60) return;   /* waiting to calibrate */
    if ((e = find_enemy(c)) == NULL) return;
    g_bossLastShot = g_tick;
    queue_shots(e, 1);
}

static char *pick_phrase(DWORD avoid)
{
    int tries;
    char *w = NULL;
    if (g_ntier[2] == 0) return pick_word(MAXWORDLEN, avoid);
    for (tries = 0; tries < 200; tries++) {
        char *p = g_tier[2][rnd(g_ntier[2])], *q;
        int words = 1;
        for (q = p; *q; q++) if (*q == ' ') words++;
        if (p == g_bossLastPhrase) continue;
        if (tries < 150 && words < 2) continue;               /* prefer multi-word phrases */
        if (tries < 100 && (initial_bit(p) & avoid)) continue;
        w = p;
        break;
    }
    if (!w) w = g_tier[2][rnd(g_ntier[2])];
    g_bossLastPhrase = w;
    return w;
}

static void assign_word(Enemy *e)
{
    int slot = (int)(e - g_en), tries, t;
    DWORD avoid = initials_in_use(e);
    char *w = NULL;
    MapEntry *m;

    if (is_boss(e)) {
        if (e->obj != g_bossObj) boss_reset(e->obj);
        if (g_bossOwed >= 3) return;          /* last third paid for: no more words */
        w = pick_phrase(avoid);
    } else if (e->person != KIND_PERSON) {
        /* props, grenades, thrown objects, pickups: a single letter nobody else on screen starts with */
        for (tries = 0; tries < 60; tries++) {
            int k = rnd(26);
            if (!(avoid & (1u << k)) || tries == 59) { w = (char *)g_letters[k]; break; }
        }
    } else if ((m = map_for(e)) != NULL) {
        /* the run's pre-picked word for this appearance; a fresh pick within the same limit for
         * follow-up words (enemy survived a hit) or if it clashes with a word already on screen */
        w = m->word;
        if (!w || e->tough > 0 || (initial_bit(w) & avoid)) w = pick_word(m->maxlen, avoid);
        if (!w) w = m->word;
        t = 0;
    }

    if (!w) {
        /* not in the map: fresh enemies mostly short words, survivors medium, damage sponges long */
        t = e->tough >= 2 ? 2 : e->tough == 1 ? 1 : (rnd(10) < 7 ? 0 : 1);
        if (g_ntier[t] == 0) t = g_ntier[1] ? 1 : g_ntier[0] ? 0 : 2;
        if (g_ntier[t] == 0) return;
        for (tries = 0; tries < 40; tries++) {
            w = g_tier[t][rnd(g_ntier[t])];
            if (!(initial_bit(w) & avoid)) break;
        }
    }
    g_words[slot] = w;
    e->word = slot;
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
        if (is_boss(e)) boss_prewrite(e->obj);
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
        /* The hit spheres can report a hit even when a prop in front took the bullet. The game's
         * hit resolver (0x444970) settles that at the end of the frame, and ResolvedHook reads
         * its result: the character keeps the bullet only if nothing nearer was hit. */
        *(WORD *)G_LMB = 0;
        if (e->landTick >= g_pressTick || g_tick - e->lastSeen > 1) {
            mlog("shot at %08x hit (aim %d)", (DWORD)s->obj, s->misses);
            e->landed = 1;
            e->cooldown = SHOT_COOLDOWN;
            pop_shot();
            return;
        }
        if (++g_checkTicks < 3) return;
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
            if (e->person == KIND_PERSON && e->tough == 0) {
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
    if (is_boss(e)) {
        /* a phrase pays for a third of the boss's life; boss_update fires any follow-up shots */
        if (g_bossOwed < 3) g_bossOwed++;
        g_bossLastShot = g_tick;
        queue_shots(e, 1);
    } else
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

/* ---------------------------------------------------------------- game speed */
/*
 * The game is frame-locked: the pacer at 0x41FA80 runs one logic step and then waits until
 * 33 ms (0x21) have passed, so everything - enemy aim timers, animation, camera moves - is
 * counted in 30 fps frames. Each site below is a `cmp/add eax, imm8` with that 33; raising it
 * slows the whole game evenly. CD-audio music is unaffected.
 */
static const DWORD g_frameSites[] = {
    0x41FB64, 0x41FB6D, 0x41FB90, 0x41FB9A, 0x41FBAD,   /* frame pacer */
    0x4095D3, 0x40A410, 0x42902A                          /* frame clock resets */
};
#define NSITES      (int)(sizeof(g_frameSites) / sizeof(g_frameSites[0]))
#define SPEED_MIN   30
#define SPEED_MAX   100
#define SPEED_STEP  10
static int g_speed = 100;         /* whole-game speed, percent (tdmod.ini only) */
static int g_siteOk[NSITES];

/*
 * Enemy speed. Enemies fire when their aiming animation reaches a set keyframe (0x433D70 plays
 * the gunshot and calls 0x406C40), so there is no separate countdown: the way to give the
 * typist time is to slow the characters themselves. Each character earns `g_enemySpeed`
 * percent of a step per game frame; on frames where it hasn't earned a full step, its AI
 * (0x406720 / 0x4066A0), animation clock (0x432230) and animation events (0x433D70) are
 * skipped. Pose, drawing, the hit check and hit reactions still run every frame, so slowed
 * enemies stay visible and shootable, and the camera, props and the player run at full speed.
 */
#define ENEMY_MIN   20
#define ENEMY_MAX   100
#define G_FRAMECOUNT 0x677DD0     /* WORD, incremented once per game frame by the pacer */
static int  g_enemySpeed = 40;
static int  g_charAcc[CHAR_SLOTS];
static WORD g_charFrame[CHAR_SLOTS];
static BYTE g_charRun[CHAR_SLOTS];
static int  (__cdecl *g_origAI)(char *c);
static int  (__cdecl *g_origAIHit)(char *c);
static int  (__cdecl *g_origAnim)(char *c);
static int  (__cdecl *g_origAnimEv)(char *c);

/* 1 = this character sits out this frame's AI/animation step */
static int char_frozen(char *c)
{
    DWORD d = (DWORD)c - CHAR_POOL;
    int slot;
    WORD fr;
    if (!g_typing || g_enemySpeed >= 100) return 0;
    if ((DWORD)c < CHAR_POOL || d % CHAR_STRIDE || (slot = (int)(d / CHAR_STRIDE)) >= CHAR_SLOTS) return 0;
    fr = *(WORD *)G_FRAMECOUNT;
    if (g_charFrame[slot] != fr) {           /* decide once per frame, however often we're asked */
        g_charFrame[slot] = fr;
        g_charAcc[slot] += g_enemySpeed;
        g_charRun[slot] = g_charAcc[slot] >= 100;
        if (g_charRun[slot]) g_charAcc[slot] -= 100;
    }
    return !g_charRun[slot];
}

static int __cdecl AIHook(char *c)      { return char_frozen(c) ? 0 : g_origAI(c); }
/* 0x4066A0 is the boss AI (chosen when +0x54E & 0x3C): it runs the regular AI, then the boss's
 * shot pools and the hit test that lets the player shoot them down. On frames where a player
 * fires it always runs, so the shot isn't lost to the slow-down; the boss's own movement stays
 * slowed through the 0x406720 hook it calls. */
static int __cdecl AIHitHook(char *c)
{
    int r = 0;
    track_boss_shots();
    if (!char_frozen(c) || *(BYTE *)(G_P1 + 2) || *(BYTE *)(0x5561D0 + 2)) r = g_origAIHit(c);
    check_boss_shots();
    return r;
}
static int __cdecl AnimHook(char *c)    { return char_frozen(c) ? 0 : g_origAnim(c); }
static int __cdecl AnimEvHook(char *c)  { return char_frozen(c) ? 0 : g_origAnimEv(c); }

/* Called at the start of the character loop (0x405220), before it clears 0x12D4F54. That still
 * holds last frame's result from the hit resolver 0x444970: the character that kept the bullet,
 * or 0 if a nearer prop took it. */
#define G_HITCHAR 0x12D4F54
static void __cdecl ResolvedHook(void)
{
    char *c = *(char **)G_HITCHAR;
    Enemy *e;
    if (c && (*(BYTE *)(c + 0x5D3) & 0x10) && (e = find_enemy(c)) != 0) e->landTick = g_tick;
}

static int ini_int(const char *buf, const char *key, int lo, int hi, int def)
{
    int i, k = slen(key);
    for (i = 0; buf[i]; i++) {
        if ((i == 0 || buf[i - 1] == '\n') && memcmp(buf + i, key, k) == 0 && buf[i + k] == '=') {
            int v = 0, j = i + k + 1;
            while (buf[j] >= '0' && buf[j] <= '9') v = v * 10 + (buf[j++] - '0');
            return (v >= lo && v <= hi) ? v : def;
        }
    }
    return def;
}

static void apply_speed(void)
{
    int ms = 33 * 100 / g_speed, i;
    DWORD old;
    if (ms > 127) ms = 127;        /* imm8 is signed */
    for (i = 0; i < NSITES; i++) {
        BYTE *p = (BYTE *)g_frameSites[i];
        if (!g_siteOk[i]) continue;
        VirtualProtect(p, 3, PAGE_EXECUTE_READWRITE, &old);
        p[2] = (BYTE)ms;
        VirtualProtect(p, 3, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 3);
    }
    mlog("game speed %d%% (%d ms per frame)", g_speed, ms);
}

static void init_speed(void)
{
    char path[MAX_PATH + 16], buf[256];
    HANDLE f;
    DWORD got = 0;
    int i;
    /* only touch sites that still hold the original instruction */
    for (i = 0; i < NSITES; i++) {
        BYTE *p = (BYTE *)g_frameSites[i];
        g_siteOk[i] = p[0] == 0x83 && (p[1] == 0xC0 || p[1] == 0xF8) && p[2] == 0x21;
        if (!g_siteOk[i]) mlog("speed site %08x: unexpected bytes, left alone", g_frameSites[i]);
    }
    path_in_dir(path, "tdmod.ini");
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(f);
    }
    buf[got] = 0;
    g_speed = ini_int(buf, "speed", SPEED_MIN, SPEED_MAX, g_speed);
    g_enemySpeed = ini_int(buf, "enemy", ENEMY_MIN, ENEMY_MAX, g_enemySpeed);
    g_propLetters = ini_int(buf, "props", 0, 1, 1);
    mlog("enemy speed %d%%", g_enemySpeed);
    apply_speed();
}

static void save_speed(void)
{
    char path[MAX_PATH + 16], buf[384];
    HANDLE f;
    DWORD n;
    path_in_dir(path, "tdmod.ini");
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    n = wsprintfA(buf,
                  "# Typing mod settings\r\n"
                  "# enemy speed in percent (%d-%d): how fast characters move, aim and fire in typing mode.\r\n"
                  "# Page Up / Page Down change it in game.\r\n"
                  "enemy=%d\r\n"
                  "# whole-game speed in percent (%d-%d), slows everything including the camera\r\n"
                  "speed=%d\r\n",
                  ENEMY_MIN, ENEMY_MAX, g_enemySpeed, SPEED_MIN, SPEED_MAX, g_speed);
    WriteFile(f, buf, n, &n, NULL);
    CloseHandle(f);
}

static void change_enemy_speed(int delta)
{
    int s = g_enemySpeed + delta;
    if (s < ENEMY_MIN) s = ENEMY_MIN;
    if (s > ENEMY_MAX) s = ENEMY_MAX;
    if (s == g_enemySpeed) return;
    g_enemySpeed = s;
    mlog("enemy speed %d%%", g_enemySpeed);
    save_speed();
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
    /* F7 = item-list debug markers */
    if (m == WM_KEYDOWN && w == VK_F7) {
        if (!(l & 0x40000000)) g_debug = !g_debug;
        return 0;
    }
    /* Page Down / Page Up = slower / faster enemies */
    if (m == WM_KEYDOWN && (w == VK_NEXT || w == VK_PRIOR)) {
        change_enemy_speed(w == VK_PRIOR ? SPEED_STEP : -SPEED_STEP);
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
    boss_update();
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
    if (g_debug && g_tick - g_dbgTick <= 2) {
        /* item list markers: shape number, '*' = counts as moving (gets a letter) */
        for (i = 0; i < g_ndbg; i++) {
            char m[8];
            int n = g_dbg[i].shape >= 0x20000      /* projectile: T + its kind */
                    ? wsprintfA(m, "T%d", g_dbg[i].shape & 0xFFFF)
                    : g_dbg[i].shape >= 0x10000    /* box-tested prop: show its callback */
                    ? wsprintfA(m, "P%04x", g_dbg[i].shape & 0xFFFF)
                    : wsprintfA(m, "%d%s", g_dbg[i].shape, g_dbg[i].moving ? "*" : "");
            text_at(dc, (g_dbg[i].x + 160) * w / 320, (g_dbg[i].y + 120) * h / 240, m, n,
                    g_dbg[i].moving ? RGB(0, 255, 0) : RGB(255, 0, 255));
        }
    }
    {
        char line[128];
        int total = g_keysOk + g_keysMiss, n;
        if (g_typing)
            n = wsprintfA(line, " TYPING  acc %d%%  words %d  enemies %d%%   [F8 mouse] ",
                          total ? g_keysOk * 100 / total : 100, g_wordsDone, g_enemySpeed);
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
    static const BYTE aiPro[7] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x48, 0x04 };      /* mov eax,[esp+4]; mov ecx,[eax+4] */
    static const BYTE pushEsiPro[5] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };             /* push esi; mov esi,[esp+8] */
    static const BYTE spawnPro[6] = { 0x53, 0xB8, 0x90, 0x64, 0x55, 0x00 };         /* push ebx; mov eax,0x556490 */
    static const BYTE sph1Pro[5] = { 0x83, 0xEC, 0x0C, 0x53, 0x56 };                /* sub esp,0xC; push ebx; push esi */
    static const BYTE sph2Pro[6] = { 0x81, 0xEC, 0x8C, 0x00, 0x00, 0x00 };          /* sub esp,0x8C */
    static const BYTE itemCPro[5] = { 0x83, 0xEC, 0x18, 0x53, 0x56 };               /* sub esp,0x18; push ebx; push esi */
    static const BYTE itemBPro[5] = { 0x83, 0xEC, 0x34, 0x53, 0x56 };               /* sub esp,0x34; push ebx; push esi */
    static const BYTE pickupPro[6] = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10 };        /* push esi; push edi; mov edi,[esp+0x10] */
    static const BYTE scenePro[5] = { 0x66, 0x33, 0xC9, 0x53, 0x56 };               /* xor cx,cx; push ebx; push esi */
    static const BYTE projHitPro[5] = { 0x83, 0xEC, 0x18, 0x53, 0x56 };             /* sub esp,0x18; push ebx; push esi */
    static const BYTE charLoopPro[10] = { 0xC7, 0x05, 0x58, 0x4F, 0x2D, 0x01,
                                          0x00, 0x00, 0x00, 0x4F };           /* mov dword [0x12D4F58],0x4F000000 */
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

    /* character loop entry: pushad; call ResolvedHook; popad; original 10-byte
     * mov dword [0x12D4F58],0x4F000000; jmp back */
    p = stub(22);
    p[0] = 0x60;
    put_rel(p + 1, 0xE8, (DWORD)ResolvedHook);
    p[6] = 0x61;
    memcpy(p + 7, charLoopPro, 10);
    put_rel(p + 17, 0xE9, 0x405220 + 10);
    patch_jmp(0x405220, charLoopPro, 10, p, "hit-resolved");

    /* thrown-weapon update (push esi; mov esi,[esp+8]) and its shoot-down test (sub esp,0x18;
     * push ebx; push esi) */
    p = stub(10);
    memcpy(p, pushEsiPro, 5);
    put_rel(p + 5, 0xE9, 0x4396C0 + 5);
    g_origProjUpdate = (int (__cdecl *)(char *))p;
    patch_jmp(0x4396C0, pushEsiPro, 5, (BYTE *)ProjUpdateHook, "projectiles");

    p = stub(10);
    memcpy(p, projHitPro, 5);
    put_rel(p + 5, 0xE9, 0x439D30 + 5);
    g_origProjHit = (int (__cdecl *)(char *, char *))p;
    patch_jmp(0x439D30, projHitPro, 5, (BYTE *)ProjHitHook, "projectile-hit");

    /* scene loader (xor cx,cx; push ebx; push esi): spawns.txt overrides */
    p = stub(10);
    memcpy(p, scenePro, 5);
    put_rel(p + 5, 0xE9, 0x4424D0 + 5);
    g_origSceneLoad = (void (__cdecl *)(void))p;
    patch_jmp(0x4424D0, scenePro, 5, (BYTE *)SceneLoadHook, "scene-load");

    /* character time budget: AI, boss AI, animation clock, animation events */
    p = stub(12);
    memcpy(p, aiPro, 7);
    put_rel(p + 7, 0xE9, 0x406720 + 7);
    g_origAI = (int (__cdecl *)(char *))p;
    patch_jmp(0x406720, aiPro, 7, (BYTE *)AIHook, "char-ai");

    p = stub(10);
    memcpy(p, pushEsiPro, 5);
    put_rel(p + 5, 0xE9, 0x4066A0 + 5);
    g_origAIHit = (int (__cdecl *)(char *))p;
    patch_jmp(0x4066A0, pushEsiPro, 5, (BYTE *)AIHitHook, "char-ai-hit");

    p = stub(10);
    memcpy(p, pushEsiPro, 5);
    put_rel(p + 5, 0xE9, 0x432230 + 5);
    g_origAnim = (int (__cdecl *)(char *))p;
    patch_jmp(0x432230, pushEsiPro, 5, (BYTE *)AnimHook, "char-anim");

    p = stub(10);
    memcpy(p, loopPro, 5);                 /* sub esp,4; push ebx; push esi - same as the loop */
    put_rel(p + 5, 0xE9, 0x433D70 + 5);
    g_origAnimEv = (int (__cdecl *)(char *))p;
    patch_jmp(0x433D70, loopPro, 5, (BYTE *)AnimEvHook, "char-anim-events");

    /* sphere-tested props (grenades, boss projectiles): sub esp,0xC; push ebx; push esi */
    p = stub(10);
    memcpy(p, sph1Pro, 5);
    put_rel(p + 5, 0xE9, 0x40C430 + 5);
    g_origSphereTest = (HitTestFn)p;
    patch_jmp(0x40C430, sph1Pro, 5, (BYTE *)SphereTestHook, "sphere-props");

    /* second sphere test: sub esp,0x8C */
    p = stub(11);
    memcpy(p, sph2Pro, 6);
    put_rel(p + 6, 0xE9, 0x42D3E0 + 6);
    g_origSphereTest2 = (HitTestFn)p;
    patch_jmp(0x42D3E0, sph2Pro, 6, (BYTE *)SphereTest2Hook, "sphere-props-2");

    /* list items: circle test (sub esp,0x18; push ebx; push esi), box test (sub esp,0x34; ...) */
    p = stub(10);
    memcpy(p, itemCPro, 5);
    put_rel(p + 5, 0xE9, 0x402860 + 5);
    g_origItemCircle = (HitTestFn)p;
    patch_jmp(0x402860, itemCPro, 5, (BYTE *)ItemCircleHook, "items-circle");

    p = stub(10);
    memcpy(p, itemBPro, 5);
    put_rel(p + 5, 0xE9, 0x4029F0 + 5);
    g_origItemBox = (HitTestFn)p;
    patch_jmp(0x4029F0, itemBPro, 5, (BYTE *)ItemBoxHook, "items-box");

    /* pickups: push esi; push edi; mov edi,[esp+0x10] (esp-relative, fine in a trampoline) */
    p = stub(11);
    memcpy(p, pickupPro, 6);
    put_rel(p + 6, 0xE9, 0x434440 + 6);
    g_origPickup = (PickupFn)p;
    patch_jmp(0x434440, pickupPro, 6, (BYTE *)PickupHook, "pickups");

    /* spawn: push ebx; mov eax,0x556490 (absolute, so fine in a trampoline) */
    p = stub(11);
    memcpy(p, spawnPro, 6);
    put_rel(p + 6, 0xE9, G_SPAWN + 6);
    g_origSpawn = (int (__cdecl *)(char *))p;
    patch_jmp(G_SPAWN, spawnPro, 6, (BYTE *)SpawnHook, "spawn");

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
        load_map();
        install_hooks();
        init_speed();
    }
    return TRUE;
}
