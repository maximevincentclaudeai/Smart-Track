/* SPDX-License-Identifier: GPL-3.0-only
 *
 * smart-track: a "smart" Tag List for the XDJ-AZ.
 *
 * While you browse, a SMART button sits on the Browse screen. Tap it and an overlay lists the
 * tracks of your library that fit the track that is playing:
 *   - BPM within +/- N of the playing track (live tempo, pitch fader included)
 *   - key within +/- 1 on the Camelot wheel (or exact, or +/-1 plus relative major/minor)
 *   - same playlist(s), chosen from the playlists you tell it to watch (hip-hop, house, ...),
 *     so a catch-all playlist such as ALL is simply not watched
 * It also proposes a name for the set ("House 124 BPM 8A").
 *
 * Every criterion can be switched off in Mod Menu > SMART TRACK, and every choice is kept
 * in MODS/azmods.cfg on the USB (rows are saved by the core, the watched playlists with cfg_set_str).
 *
 * How it runs (see docs/GUIDELINES.md):
 *   - the library is read on a thread of the mod's own (lib_* are refused on the UI thread);
 *   - matching runs on that same short-lived thread, never in tick or on_paint;
 *   - the UI thread and the painter only copy a few rows out of a shared result under a tiny
 *     spin lock;
 *   - no hooks, no firmware addresses: "firmware": ["any"].
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azmod-mod.h"

#define MOD_ID "smart-track"

/* ---- layout: where the SMART button and the overlay sit (screen is 1280x800) ----------
 * The SDK cannot tell the Tag List tab from the Playlist tab (both are AZ_VIEW_BROWSE), and
 * cannot see where EP147 draws its tabs, so the button is a fixed rectangle you can move here,
 * and the overlay's top edge is a Mod Menu choice (Overlay top). */
#define CHIP_X 1040
#define CHIP_Y 6
#define CHIP_W 232
#define CHIP_H 44
#define SCREEN_W 1280
#define SCREEN_H 800
static const int OV_TOP[4] = { 0, 60, 100, 140 };

/* ---- limits ---------------------------------------------------------------------------- */
#define MAX_TRACKS 16000     /* ~120 bytes each, allocated on the worker thread */
#define MAX_PL 64            /* distinct playlist names (merged across both USB ports) */
#define PL_NAME 32
#define MAX_RES 120          /* matches kept, best first */
#define VIS_MAX 24           /* rows copied for one paint */
#define WATCH_MAX 250        /* "a;b;c" of watched playlist names, kept in two text settings */
#define WATCH_HALF 126

static const az_core_v1 *C;

/* ---- types ----------------------------------------------------------------------------- */
typedef struct {
    uint32_t id;
    uint8_t stick, key;      /* key: Camelot code, n*2 + (major ? 1 : 0), 0 = unknown */
    uint16_t pad;
    float bpm;
    uint64_t pl;             /* bit i = in playlist g_names[i] */
    char title[56];
    char artist[40];
} trk_t;

typedef struct {
    char title[56], artist[40], pl[24], key[4];
    float bpm;
    unsigned char same_key;
} res_t;

enum { ST_IDLE = 0, ST_SCAN, ST_NOLIB, ST_NOTRACK, ST_OK };

typedef struct {
    int status, deck, nres, total;
    float bpm;
    char title[64], artist[48], key[4], pls[64], name[64], flt[56], note[64];
} ctx_t;

/* ---- shared state ---------------------------------------------------------------------- */
/* Written by the worker, read by the UI thread / painter; always under st_lock(). */
static int g_lock_flag;
static ctx_t g_ctx;
static res_t g_res[MAX_RES];
static char g_names[MAX_PL][PL_NAME];
static int g_nnames;
static char g_watch[WATCH_MAX + 1];
static int g_ver;            /* bumped on every published result (atomic) */
static int g_watch_ver;      /* bumped when the watched playlists change (atomic) */

/* Worker-private: only the job thread touches these (jobs never overlap). */
static trk_t s_tracks[MAX_TRACKS];     /* .bss: pages are only touched as the library fills it */
static int s_ntracks;
static char s_names[MAX_PL][PL_NAME];
static int s_nnames;
static int s_scanned, s_nolib;
static long s_nolib_ms;

/* Atomics shared by everyone. */
static int a_pending, a_rescan, a_last_play = -1, a_want_scan;
static long a_now_ms;                         /* the UI thread's clock, for the worker */
static int a_active, a_sel;                  /* overlay open; selected row */
/* Painter -> input geometry (screen pixels). */
static int a_row_y0, a_row_h, a_vis, a_top;

/* UI-thread-only. */
static int h_bpm = -1, h_bpm_rng = -1, h_key = -1, h_key_mode = -1, h_pl = -1;
static int h_deck = -1, h_usb = -1, h_overlay = -1, h_otop = -1;
static az_surface *ov, *chip;
static long last_tick_ms;
static unsigned last_sig = 0xFFFFFFFFu;
static int seen_ver = -1, last_top = -1, last_en = -1;

static void st_lock(void)
{
    while (__atomic_exchange_n(&g_lock_flag, 1, __ATOMIC_ACQUIRE)) {
    }
}
static void st_unlock(void) { __atomic_store_n(&g_lock_flag, 0, __ATOMIC_RELEASE); }

/* A row's value, or def when the core has no rows or the row was refused. */
static int st_row(int handle, int def)
{
    if (handle < 0 || !AZ_HAS(C, row_value)) return def;
    int v = C->row_value(handle);
    return v < 0 ? def : v;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- small helpers --------------------------------------------------------------------- */
/* Copy src into dst (size n), cutting at a whole UTF-8 character. NULL src = empty. */
static void st_copy(char *dst, size_t n, const char *src)
{
    if (n == 0) return;
    if (!src) src = "";
    size_t len = strlen(src);
    if (len >= n) {
        len = n - 1;
        while (len > 0 && ((unsigned char) src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static float st_absf(float v) { return v < 0 ? -v : v; }

static int st_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char) (x + 32);
        if (y >= 'A' && y <= 'Z') y = (char) (y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* ---- keys: any of "Am", "F#m", "Bb", "Bbm", "8A", "12B" -> Camelot code ------------------ */
static int st_parse_key(const char *s)
{
    static const unsigned char MAJ[12] = { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };
    static const unsigned char MIN[12] = { 5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10 };
    if (!s) return 0;
    while (*s == ' ') s++;
    if (*s >= '0' && *s <= '9') {
        int n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s - '0');
            s++;
            if (n > 99) return 0;
        }
        if (n < 1 || n > 12) return 0;
        if (*s == 'A' || *s == 'a') return n * 2;
        if (*s == 'B' || *s == 'b') return n * 2 + 1;
        return 0;
    }
    int pc;
    switch (*s) {
    case 'C': pc = 0; break;
    case 'D': pc = 2; break;
    case 'E': pc = 4; break;
    case 'F': pc = 5; break;
    case 'G': pc = 7; break;
    case 'A': pc = 9; break;
    case 'B': pc = 11; break;
    default: return 0;
    }
    s++;
    for (;;) {
        if (*s == '#') { pc++; s++; }
        else if (*s == 'b') { pc--; s++; }
        else if (strncmp(s, "\xE2\x99\xAF", 3) == 0) { pc++; s += 3; }  /* sharp sign */
        else if (strncmp(s, "\xE2\x99\xAD", 3) == 0) { pc--; s += 3; }  /* flat sign */
        else break;
    }
    pc = ((pc % 12) + 12) % 12;
    while (*s == ' ') s++;
    int minor = (s[0] == 'm' && !(s[1] == 'a' && s[2] == 'j'));
    return minor ? MIN[pc] * 2 : MAJ[pc] * 2 + 1;
}

static void st_key_str(int code, char out[4])
{
    if (code < 2 || code > 25) { out[0] = '\0'; return; }
    snprintf(out, 4, "%d%c", code >> 1, (code & 1) ? 'B' : 'A');
}

/* mode 0 exact, 1 +/-1 on the wheel, 2 +/-1 and the relative major/minor */
static int st_key_ok(int a, int b, int mode)
{
    if (a == b) return 1;
    if (mode <= 0 || a < 2 || b < 2) return 0;
    int na = a >> 1, nb = b >> 1;
    int d = na - nb;
    if (d < 0) d = -d;
    if (d > 6) d = 12 - d;
    if ((a & 1) == (b & 1) && d == 1) return 1;
    if (mode >= 2 && na == nb) return 1;
    return 0;
}

/* ---- watched playlists: "hip-hop;house" ------------------------------------------------- */
static int st_watch_has(const char *w, const char *name)
{
    const char *p = w;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t) (e - p) : strlen(p);
        char tok[PL_NAME];
        if (len >= sizeof tok) len = sizeof tok - 1;
        memcpy(tok, p, len);
        tok[len] = '\0';
        if (tok[0] && st_ieq(tok, name)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* Does the list fit the two text settings? Tokens are never split across them. */
static int st_watch_split(const char *w, char *a, char *b)
{
    a[0] = b[0] = '\0';
    size_t la = 0, lb = 0;
    int in_b = 0;
    const char *p = w;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t) (e - p) : strlen(p);
        if (len > 0) {
            char *dst = in_b ? b : a;
            size_t *l = in_b ? &lb : &la;
            if (*l + (*l ? 1 : 0) + len > WATCH_HALF) {
                if (in_b) return -1;
                in_b = 1;
                dst = b;
                l = &lb;
                if (len > WATCH_HALF) return -1;
            }
            if (*l) dst[(*l)++] = ';';
            memcpy(dst + *l, p, len);
            *l += len;
            dst[*l] = '\0';
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* Add or remove name; 0 = done, -1 = no room. */
static int st_watch_toggle(char *w, const char *name)
{
    char out[WATCH_MAX + PL_NAME + 2];
    out[0] = '\0';
    size_t lo = 0;
    int had = 0;
    const char *p = w;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t) (e - p) : strlen(p);
        char tok[PL_NAME];
        if (len >= sizeof tok) len = sizeof tok - 1;
        memcpy(tok, p, len);
        tok[len] = '\0';
        if (tok[0]) {
            if (st_ieq(tok, name)) had = 1;
            else {
                if (lo) out[lo++] = ';';
                memcpy(out + lo, tok, len);
                lo += len;
                out[lo] = '\0';
            }
        }
        if (!e) break;
        p = e + 1;
    }
    if (!had) {
        size_t ln = strlen(name);
        if (lo + (lo ? 1 : 0) + ln > WATCH_MAX) return -1;
        if (lo) out[lo++] = ';';
        memcpy(out + lo, name, ln);
        out[lo + ln] = '\0';
    }
    char a[WATCH_HALF + 1], b[WATCH_HALF + 1];
    if (st_watch_split(out, a, b) != 0) return -1;
    st_copy(w, WATCH_MAX + 1, out);
    return 0;
}

static uint64_t st_watch_mask(const char *w, char names[][PL_NAME], int nn)
{
    uint64_t m = 0;
    for (int i = 0; i < nn; i++)
        if (st_watch_has(w, names[i])) m |= 1ull << i;
    return m;
}

static void st_watch_save(void)     /* UI thread */
{
    char w[WATCH_MAX + 1], a[WATCH_HALF + 1], b[WATCH_HALF + 1];
    st_lock();
    memcpy(w, g_watch, sizeof w);
    st_unlock();
    if (st_watch_split(w, a, b) != 0) return;
    C->cfg_set_str(MOD_ID, "watch", a);
    C->cfg_set_str(MOD_ID, "watch2", b);
}

/* ---- the library scan (worker thread only) ---------------------------------------------- */
typedef struct { int stick; int bit; } scan_ctx;
typedef struct { int stick, id, folder; char name[PL_NAME]; } plent;
static plent s_pl[160];
static int s_npl;

static int trk_cmp(const void *x, const void *y)
{
    const trk_t *a = x, *b = y;
    if (a->stick != b->stick) return (int) a->stick - (int) b->stick;
    return a->id < b->id ? -1 : (a->id > b->id ? 1 : 0);
}

/* In-place heapsort by (stick, id): glibc's qsort allocates, this does not. */
static void st_swap(trk_t *a, trk_t *b)
{
    trk_t t = *a;
    *a = *b;
    *b = t;
}

static void st_sift(trk_t *v, int root, int n)
{
    for (;;) {
        int c = 2 * root + 1;
        if (c >= n) return;
        if (c + 1 < n && trk_cmp(&v[c], &v[c + 1]) < 0) c++;
        if (trk_cmp(&v[root], &v[c]) >= 0) return;
        st_swap(&v[root], &v[c]);
        root = c;
    }
}

static void st_sort(trk_t *v, int n)
{
    for (int i = n / 2 - 1; i >= 0; i--) st_sift(v, i, n);
    for (int e = n - 1; e > 0; e--) {
        st_swap(&v[0], &v[e]);
        st_sift(v, 0, e);
    }
}

static int cb_all(const az_track *t, void *u)
{
    scan_ctx *c = u;
    if (!t || s_ntracks >= MAX_TRACKS) return 1;
    trk_t *k = &s_tracks[s_ntracks++];
    memset(k, 0, sizeof *k);
    k->id = t->id;
    k->stick = (uint8_t) c->stick;
    double b = t->bpm;
    k->bpm = (b > 0.0 && b < 1000.0) ? (float) b : 0.0f;
    k->key = (uint8_t) st_parse_key(t->key);
    st_copy(k->title, sizeof k->title, t->title);
    st_copy(k->artist, sizeof k->artist, t->artist);
    return 0;
}

static int cb_pl_list(int pl_id, int parent, const char *name, int folder, void *u)
{
    (void) parent;
    scan_ctx *c = u;
    if (s_npl >= (int) (sizeof s_pl / sizeof s_pl[0])) return 1;
    plent *p = &s_pl[s_npl++];
    p->stick = c->stick;
    p->id = pl_id;
    p->folder = folder;
    st_copy(p->name, sizeof p->name, name);
    for (char *q = p->name; *q; q++)
        if (*q == ';') *q = ',';
    return 0;
}

static int cb_pl_tracks(const az_track *t, void *u)
{
    scan_ctx *c = u;
    if (!t) return 0;
    trk_t key;
    key.stick = (uint8_t) c->stick;
    key.id = t->id;
    trk_t *k = bsearch(&key, s_tracks, (size_t) s_ntracks, sizeof key, trk_cmp);
    if (k) k->pl |= 1ull << c->bit;
    return 0;
}

static int name_slot(const char *n)
{
    for (int i = 0; i < s_nnames; i++)
        if (strcmp(s_names[i], n) == 0) return i;
    if (s_nnames >= MAX_PL) return -1;
    st_copy(s_names[s_nnames], PL_NAME, n);
    return s_nnames++;
}

static void job_scan(void)
{
    long now = __atomic_load_n(&a_now_ms, __ATOMIC_RELAXED);
    if (s_nolib && now - s_nolib_ms < 10000) return;       /* no USB library: retry every 10 s */

    st_lock();
    g_ctx.status = ST_SCAN;
    st_unlock();
    __atomic_add_fetch(&g_ver, 1, __ATOMIC_RELEASE);

    s_ntracks = 0;
    s_scanned = 0;
    s_nnames = 0;
    s_npl = 0;

    for (int stick = 0; stick < 2; stick++) {
        scan_ctx c = { stick, 0 };
        C->lib_tracks(MOD_ID, stick, 0, cb_all, &c);
    }
    if (s_ntracks == 0) {
        s_nolib = 1;
        s_nolib_ms = __atomic_load_n(&a_now_ms, __ATOMIC_RELAXED);
        C->log(MOD_ID, "no library found on either USB port");
        return;
    }
    s_nolib = 0;
    st_sort(s_tracks, s_ntracks);

    for (int stick = 0; stick < 2; stick++) {
        scan_ctx c = { stick, 0 };
        C->lib_playlists(MOD_ID, stick, cb_pl_list, &c);
    }
    for (int i = 0; i < s_npl; i++) {
        if (s_pl[i].folder || !s_pl[i].name[0]) continue;
        int slot = name_slot(s_pl[i].name);
        if (slot < 0) continue;
        scan_ctx c = { s_pl[i].stick, slot };
        C->lib_tracks(MOD_ID, s_pl[i].stick, s_pl[i].id, cb_pl_tracks, &c);
    }

    st_lock();
    memcpy(g_names, s_names, sizeof g_names);
    st_unlock();
    __atomic_store_n(&g_nnames, s_nnames, __ATOMIC_RELEASE);
    s_scanned = 1;
    C->log(MOD_ID, "indexed %d tracks, %d playlists", s_ntracks, s_nnames);
}

/* ---- picking the deck and matching (worker thread only) ---------------------------------- */
static int st_pick_deck(void)
{
    int sel = st_row(h_deck, 0);                         /* 0 = auto, 1..4 = a deck */
    if (sel >= 1 && sel <= 4) return sel - 1;
    az_deck_info di;
    int lp = __atomic_load_n(&a_last_play, __ATOMIC_RELAXED);
    if (lp >= 0 && lp < 4) {
        di.size = sizeof di;
        if (C->deck_info(lp, &di) == 0 && di.loaded == 1 && di.playing == 1) return lp;
    }
    for (int d = 0; d < 4; d++) {
        di.size = sizeof di;
        if (C->deck_info(d, &di) == 0 && di.loaded == 1 && di.playing == 1) return d;
    }
    if (lp >= 0 && lp < 4) {
        di.size = sizeof di;
        if (C->deck_info(lp, &di) == 0 && di.loaded == 1) return lp;
    }
    for (int d = 0; d < 4; d++) {
        di.size = sizeof di;
        if (C->deck_info(d, &di) == 0 && di.loaded == 1) return d;
    }
    return -1;
}

static void st_publish(const ctx_t *cx, const res_t *res, int nres)
{
    st_lock();
    g_ctx = *cx;
    if (nres > 0) memcpy(g_res, res, sizeof(res_t) * (size_t) nres);
    st_unlock();
    __atomic_add_fetch(&g_ver, 1, __ATOMIC_RELEASE);
}

static void job_match(void)
{
    static const int BPM_TOL[4] = { 3, 6, 8, 10 };
    ctx_t cx;
    res_t res[MAX_RES];
    memset(&cx, 0, sizeof cx);

    int dk = st_pick_deck();
    cx.deck = dk;
    az_deck_info di;
    di.size = sizeof di;
    if (dk < 0 || C->deck_info(dk, &di) != 0 || di.loaded != 1) {
        cx.status = ST_NOTRACK;
        st_publish(&cx, res, 0);
        return;
    }
    st_copy(cx.title, sizeof cx.title, di.title);
    st_copy(cx.artist, sizeof cx.artist, di.artist);
    float live = (di.bpm > 0.0 && di.bpm < 1000.0) ? (float) di.bpm : 0.0f;
    cx.bpm = live;

    if (!s_scanned) {
        cx.status = s_nolib ? ST_NOLIB : ST_SCAN;
        st_publish(&cx, res, 0);
        return;
    }

    /* the playing track in the index: by rekordbox id, preferring the same title when both
     * USB ports hold an id like it */
    const trk_t *cur = NULL;
    if (di.content_id) {
        for (int i = 0; i < s_ntracks; i++) {
            if (s_tracks[i].id != di.content_id) continue;
            if (!cur) cur = &s_tracks[i];
            if (di.title[0] && strncmp(s_tracks[i].title, di.title, 20) == 0) { cur = &s_tracks[i]; break; }
        }
    }
    float cur_bpm = live > 0.0f ? live : (cur ? cur->bpm : 0.0f);
    cx.bpm = cur_bpm;
    int cur_key = cur ? cur->key : 0;
    st_key_str(cur_key, cx.key);
    if (cur && !cx.title[0]) st_copy(cx.title, sizeof cx.title, cur->title);
    if (cur && !cx.artist[0]) st_copy(cx.artist, sizeof cx.artist, cur->artist);

    int f_bpm = st_row(h_bpm, 1) == 1;
    int f_key = st_row(h_key, 1) == 1;
    int f_pl = st_row(h_pl, 1) == 1;
    int ti = st_row(h_bpm_rng, 1);
    int tol = BPM_TOL[(ti < 0 || ti > 3) ? 1 : ti];
    int kmode = st_row(h_key_mode, 1);
    if (kmode < 0 || kmode > 2) kmode = 1;
    int usb = st_row(h_usb, 0);

    char w[WATCH_MAX + 1];
    st_lock();
    memcpy(w, g_watch, sizeof w);
    st_unlock();
    uint64_t wm = st_watch_mask(w, s_names, s_nnames);
    uint64_t curpl = cur ? (cur->pl & wm) : 0;

    const char *note = "";
    if (!cur) note = "Not in the library index: BPM only";
    if (f_bpm && cur_bpm <= 0.0f) { f_bpm = 0; note = "BPM unknown: BPM filter skipped"; }
    if (f_key && cur && !cur_key) { f_key = 0; note = "Key unknown: key filter skipped"; }
    if (f_key && !cur) f_key = 0;
    int use_pl = 0;
    if (f_pl) {
        if (!wm) note = "No playlist watched (Mod Menu): playlist filter off";
        else if (!curpl) note = "Playing track is in no watched playlist: playlist filter off";
        else use_pl = 1;
    }
    st_copy(cx.note, sizeof cx.note, note);

    float score[MAX_RES];
    const trk_t *hit[MAX_RES];
    int nr = 0, total = 0;
    for (int i = 0; i < s_ntracks; i++) {
        const trk_t *tk = &s_tracks[i];
        if (cur && tk->id == cur->id && tk->stick == cur->stick) continue;
        if (usb == 1 && tk->stick != 0) continue;
        if (usb == 2 && tk->stick != 1) continue;
        float db = 0.0f;
        if (f_bpm) {
            if (tk->bpm <= 0.0f) continue;
            db = st_absf(tk->bpm - cur_bpm);
            if (db > (float) tol) continue;
        } else if (cur_bpm > 0.0f && tk->bpm > 0.0f) {
            db = st_absf(tk->bpm - cur_bpm) * 0.01f;       /* only to order the list */
        }
        float kd = 0.0f;
        if (f_key) {
            if (!tk->key || !st_key_ok(cur_key, tk->key, kmode)) continue;
            kd = (tk->key == cur_key) ? 0.0f : 0.75f;
        }
        if (use_pl && !(tk->pl & curpl)) continue;
        float sc = db + kd;
        total++;
        int pos = nr;
        if (nr < MAX_RES) nr++;
        else if (sc >= score[MAX_RES - 1]) continue;
        else pos = MAX_RES - 1;
        while (pos > 0 && score[pos - 1] > sc) {
            score[pos] = score[pos - 1];
            hit[pos] = hit[pos - 1];
            pos--;
        }
        score[pos] = sc;
        hit[pos] = tk;
    }

    for (int i = 0; i < nr; i++) {
        const trk_t *tk = hit[i];
        res_t *r = &res[i];
        memset(r, 0, sizeof *r);
        st_copy(r->title, sizeof r->title, tk->title);
        st_copy(r->artist, sizeof r->artist, tk->artist);
        r->bpm = tk->bpm;
        st_key_str(tk->key, r->key);
        r->same_key = (unsigned char) (cur_key && tk->key == cur_key);
        uint64_t show = tk->pl & wm;
        if (!show) show = tk->pl;
        for (int b = 0; b < s_nnames && show; b++)
            if (show & (1ull << b)) { st_copy(r->pl, sizeof r->pl, s_names[b]); break; }
    }

    /* the playing track's watched playlists, and the proposed name */
    char pls[64] = "";
    size_t lp = 0;
    const char *first = NULL;
    for (int b = 0; b < s_nnames; b++) {
        if (!(curpl & (1ull << b))) continue;
        if (!first) first = s_names[b];
        size_t ln = strlen(s_names[b]);
        if (lp + (lp ? 2 : 0) + ln >= sizeof pls) break;
        if (lp) { pls[lp++] = ','; pls[lp++] = ' '; }
        memcpy(pls + lp, s_names[b], ln);
        lp += ln;
        pls[lp] = '\0';
    }
    st_copy(cx.pls, sizeof cx.pls, pls);

    char nm[64];
    int o = snprintf(nm, sizeof nm, "%s", (use_pl && first) ? first : "Mix");
    if (o < 0 || o >= (int) sizeof nm) o = 0;
    if (cur_bpm > 0.0f && o < (int) sizeof nm)
        o += snprintf(nm + o, sizeof nm - (size_t) o, " %d BPM", (int) (cur_bpm + 0.5f));
    if (f_key && cur_key && o > 0 && o < (int) sizeof nm)
        snprintf(nm + o, sizeof nm - (size_t) o, " %s", cx.key);
    st_copy(cx.name, sizeof cx.name, nm);

    char flt[56] = "";
    int fo = 0;
    if (f_bpm) fo += snprintf(flt + fo, sizeof flt - (size_t) fo, "BPM +/-%d  ", tol);
    if (f_key && fo < (int) sizeof flt - 1)
        fo += snprintf(flt + fo, sizeof flt - (size_t) fo, "%s  ",
                       kmode == 0 ? "KEY =" : (kmode == 1 ? "KEY +/-1" : "KEY +/-1 +REL"));
    if (use_pl && fo < (int) sizeof flt - 1) fo += snprintf(flt + fo, sizeof flt - (size_t) fo, "PLAYLIST");
    if (!f_bpm && !f_key && !use_pl) st_copy(flt, sizeof flt, "NO FILTER");
    st_copy(cx.flt, sizeof cx.flt, flt);

    cx.status = ST_OK;
    cx.nres = nr;
    cx.total = total;
    st_publish(&cx, res, nr);
}

/* ---- the job thread -------------------------------------------------------------------- */
/* One thread, started in init(). It sleeps 100 ms at a time until the UI thread asks for a job
 * (a_pending), so when nothing is open it costs next to nothing. */
static void *worker(void *arg)
{
    (void) arg;
    for (;;) {
        if (__atomic_exchange_n(&a_pending, 0, __ATOMIC_SEQ_CST)) {
            if (!s_scanned || __atomic_exchange_n(&a_rescan, 0, __ATOMIC_SEQ_CST)) job_scan();
            job_match();
        } else {
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static void st_request(void)    /* any thread */
{
    __atomic_store_n(&a_pending, 1, __ATOMIC_SEQ_CST);
}

/* ---- drawing helpers ------------------------------------------------------------------- */
static void draw_fitted(az_draw *d, int x, int y, const char *s, int px, uint32_t color, int max_w)
{
    char buf[160];
    st_copy(buf, sizeof buf, s);
    size_t len = strlen(buf);
    while (len > 0 && C->text_width(buf, px) > max_w) {
        do len--;
        while (len > 0 && ((unsigned char) buf[len] & 0xC0) == 0x80);
        buf[len] = '\0';
    }
    if (len > 0) C->draw_text(d, x, y, buf, px, color);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- the overlay ----------------------------------------------------------------------- */
static void ov_place(void)
{
    int top = OV_TOP[clampi(st_row(h_otop, 0), 0, 3)];
    C->surface_place(ov, (az_rect) { 0, top, SCREEN_W, SCREEN_H - top });
}

static void open_overlay(void)
{
    if (st_row(h_overlay, 1) != 1) return;
    __atomic_store_n(&a_active, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&a_sel, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a_top, 0, __ATOMIC_RELAXED);
    ov_place();
    C->surface_show(ov, 1);
    C->surface_focus(ov, 1);
    last_sig = 0xFFFFFFFFu;            /* force a match now */
    st_request();
    C->surface_redraw(ov);
    C->surface_redraw(chip);
}

static void close_overlay(void)
{
    __atomic_store_n(&a_active, 0, __ATOMIC_RELAXED);
    C->surface_focus(ov, 0);
    C->surface_show(ov, 0);
    C->surface_redraw(chip);
}

static void ov_paint(az_draw *d, az_rect area, void *user)
{
    (void) user;
    ctx_t cx;
    res_t vis[VIS_MAX];
    int n = 0, top = __atomic_load_n(&a_top, __ATOMIC_RELAXED);
    int sel = __atomic_load_n(&a_sel, __ATOMIC_RELAXED);

    const int row = C->row_height(d);
    const int lh = row * 3 / 4;
    int px = row * 2 / 5;
    px = clampi(px, 8, 96);
    const int pad = row / 4;
    int vmax = (area.h - 5 * lh) / lh;
    vmax = clampi(vmax, 1, VIS_MAX);

    st_lock();
    cx = g_ctx;
    n = cx.nres;
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    if (top > sel) top = sel;
    if (sel >= top + vmax) top = sel - vmax + 1;
    if (top < 0) top = 0;
    int cnt = n - top;
    if (cnt > vmax) cnt = vmax;
    if (cnt > 0) memcpy(vis, &g_res[top], sizeof(res_t) * (size_t) cnt);
    st_unlock();
    if (cnt < 0) cnt = 0;
    __atomic_store_n(&a_top, top, __ATOMIC_RELAXED);
    __atomic_store_n(&a_sel, sel, __ATOMIC_RELAXED);

    const uint32_t text = C->color(AZ_COL_TEXT), dim = C->color(AZ_COL_TEXT_DIM);
    const uint32_t hi = C->color(AZ_COL_HI), hit = C->color(AZ_COL_HI_TEXT);
    const int W = area.w - 2 * pad;
    int y = area.y;

    /* line 1: title bar (leaves room for the SMART button on the right) */
    C->fill_rect(d, area.x, y, area.w, lh, hi);
    C->draw_text(d, area.x + pad, y + (lh - px * 3 / 2) / 2, "SMART TRACK", px, hit);
    if (cx.deck >= 0) {
        char dk[16];
        snprintf(dk, sizeof dk, "DECK %d", cx.deck + 1);
        C->draw_text(d, area.x + area.w / 2, y + (lh - px * 3 / 2) / 2, dk, px, hit);
    }
    y += lh;

    if (cx.status != ST_OK) {
        const char *m = "Scanning the library...";
        if (cx.status == ST_NOLIB) m = "No rekordbox library found. Insert a USB, then tap SMART again.";
        else if (cx.status == ST_NOTRACK) m = "Load or play a track on a deck.";
        draw_fitted(d, area.x + pad, y + pad, m, px, text, W);
        if (cx.title[0]) draw_fitted(d, area.x + pad, y + pad + lh, cx.title, px, dim, W);
        return;
    }

    /* line 2: the playing track */
    char buf[160];
    snprintf(buf, sizeof buf, "%s%s%s", cx.title, cx.artist[0] ? "  -  " : "", cx.artist);
    draw_fitted(d, area.x + pad, y + (lh - px * 3 / 2) / 2, buf, px, text, W);
    y += lh;
    /* line 3: its BPM, key, playlists */
    char kb[12] = "";
    if (cx.key[0]) snprintf(kb, sizeof kb, "%s", cx.key);
    snprintf(buf, sizeof buf, "%.1f BPM   %s%s%s", (double) cx.bpm, cx.key[0] ? "KEY " : "", kb,
             cx.pls[0] ? "   in " : "");
    {
        char full[256];
        snprintf(full, sizeof full, "%s%s", buf, cx.pls);
        draw_fitted(d, area.x + pad, y + (lh - px * 3 / 2) / 2, full, px, text, W);
    }
    y += lh;
    /* line 4: proposed name, filters */
    {
        char l4[160];
        snprintf(l4, sizeof l4, "NAME: %s", cx.name);
        C->draw_text(d, area.x + pad, y + (lh - px * 3 / 2) / 2, l4, px, text);
        draw_fitted(d, area.x + area.w / 2, y + (lh - px * 3 / 2) / 2, cx.flt, px, dim, area.w / 2 - pad);
    }
    y += lh;
    C->fill_rect(d, area.x + pad, y, area.w - 2 * pad, 1, dim);
    y += 2;

    /* the list */
    const int bpm_w = C->text_width("000.0", px) + pad;
    const int key_w = C->text_width("12A", px) + pad;
    const int body = W - bpm_w - key_w;
    const int title_w = body * 55 / 100, art_w = body * 28 / 100;
    __atomic_store_n(&a_row_y0, y, __ATOMIC_RELAXED);
    __atomic_store_n(&a_row_h, lh, __ATOMIC_RELAXED);
    __atomic_store_n(&a_vis, vmax, __ATOMIC_RELAXED);

    if (n == 0) {
        draw_fitted(d, area.x + pad, y + pad, "Nothing matches. Loosen a filter in Mod Menu > SMART TRACK.",
                    px, dim, W);
        if (cx.note[0]) draw_fitted(d, area.x + pad, y + pad + lh, cx.note, px, dim, W);
        return;
    }
    for (int i = 0; i < cnt; i++) {
        const res_t *r = &vis[i];
        int is_sel = (top + i == sel);
        int ry = y + i * lh;
        if (ry + lh > area.y + area.h) break;
        if (is_sel) C->fill_rect(d, area.x, ry, area.w, lh, hi);
        uint32_t c1 = is_sel ? hit : text, c2 = is_sel ? hit : dim;
        int ty = ry + (lh - px * 3 / 2) / 2;
        int x = area.x + pad;
        char b[24];
        snprintf(b, sizeof b, "%.1f", (double) r->bpm);
        C->draw_text(d, x, ty, b, px, c1);
        x += bpm_w;
        C->draw_text(d, x, ty, r->key, px, r->same_key ? c1 : c2);
        x += key_w;
        draw_fitted(d, x, ty, r->title, px, c1, title_w - pad);
        x += title_w;
        draw_fitted(d, x, ty, r->artist, px, c2, art_w - pad);
        x += art_w;
        draw_fitted(d, x, ty, r->pl, px, c2, area.x + area.w - x - pad);
    }
    /* footer: where you are */
    int fy = y + vmax * lh;
    if (fy + lh <= area.y + area.h) {
        char f[96];
        snprintf(f, sizeof f, "%d / %d   (%d match%s)", sel + 1, n, cx.total, cx.total == 1 ? "" : "es");
        C->draw_text(d, area.x + pad, fy + (lh - px * 3 / 2) / 2, f, px, dim);
        if (cx.note[0])
            draw_fitted(d, area.x + area.w / 2, fy + (lh - px * 3 / 2) / 2, cx.note, px, dim, area.w / 2 - pad);
    }
}

static int ov_input(const az_event *ev, void *user)
{
    (void) user;
    int n;
    switch (ev->kind) {
    case AZ_EV_KNOB: {
        st_lock();
        n = g_ctx.nres;
        st_unlock();
        int s = clampi(__atomic_load_n(&a_sel, __ATOMIC_RELAXED) + (ev->dir < 0 ? -1 : 1), 0, n > 0 ? n - 1 : 0);
        __atomic_store_n(&a_sel, s, __ATOMIC_RELAXED);
        C->surface_redraw(ov);
        return 1;
    }
    case AZ_EV_TAP: {
        int y0 = __atomic_load_n(&a_row_y0, __ATOMIC_RELAXED), h = __atomic_load_n(&a_row_h, __ATOMIC_RELAXED);
        int vis = __atomic_load_n(&a_vis, __ATOMIC_RELAXED), top = __atomic_load_n(&a_top, __ATOMIC_RELAXED);
        if (h <= 0 || ev->y < y0) return 1;
        int r = (ev->y - y0) / h;
        if (r >= vis) return 1;
        st_lock();
        n = g_ctx.nres;
        st_unlock();
        if (top + r < n) __atomic_store_n(&a_sel, top + r, __ATOMIC_RELAXED);
        C->surface_redraw(ov);
        return 1;
    }
    case AZ_EV_CLICK: {
        /* the full title, since the list cuts long ones */
        char b[112];
        int sel = __atomic_load_n(&a_sel, __ATOMIC_RELAXED);
        b[0] = '\0';
        st_lock();
        if (sel >= 0 && sel < g_ctx.nres)
            snprintf(b, sizeof b, "%.50s - %.50s", g_res[sel].title, g_res[sel].artist);
        st_unlock();
        if (b[0]) C->banner(MOD_ID, b, 4000);
        return 1;
    }
    case AZ_EV_BACK:
        close_overlay();
        return 1;
    default:
        return 1;                      /* DOWN / UP / LONG inside the overlay: nothing to do */
    }
}

static const az_surface_v1 OVERLAY = {
    .size = sizeof(az_surface_v1), .layer = AZ_LAYER_OVER_EP147,
    .views = AZ_VIEW_BIT(AZ_VIEW_BROWSE), .on_paint = ov_paint, .on_input = ov_input, .user = NULL,
};

/* ---- the SMART button ------------------------------------------------------------------ */
static void chip_paint(az_draw *d, az_rect area, void *user)
{
    (void) user;
    int on = __atomic_load_n(&a_active, __ATOMIC_RELAXED);
    uint32_t bg = on ? C->color(AZ_COL_HI) : C->color(AZ_COL_BG);
    uint32_t fg = on ? C->color(AZ_COL_HI_TEXT) : C->color(AZ_COL_TEXT);
    C->fill_rect(d, area.x, area.y, area.w, area.h, bg);
    C->draw_rect(d, area.x, area.y, area.w, area.h, 2, C->color(AZ_COL_HI));
    const char *s = on ? "SMART TRACK  ON" : "SMART TRACK";
    int px = clampi(area.h * 2 / 5, 8, 96);
    int tw = C->text_width(s, px);
    C->draw_text(d, area.x + (area.w - tw) / 2, area.y + (area.h - px * 3 / 2) / 2, s, px, fg);
}

static int chip_input(const az_event *ev, void *user)
{
    (void) user;
    if (ev->kind == AZ_EV_TAP) {
        if (__atomic_load_n(&a_active, __ATOMIC_RELAXED)) close_overlay();
        else open_overlay();
    }
    return 1;
}

static const az_surface_v1 CHIP = {
    .size = sizeof(az_surface_v1), .layer = AZ_LAYER_TOP,
    .views = AZ_VIEW_BIT(AZ_VIEW_BROWSE), .on_paint = chip_paint, .on_input = chip_input, .user = NULL,
};

/* ---- the playlist picker page (Mod Menu > SMART TRACK > Playlists to watch) ------------- */
static int pk_sel, pk_top, pk_row, pk_vis = 1;
static az_rect pk_area;

static void pk_toggle(int i)
{
    char name[PL_NAME], w[WATCH_MAX + 1];
    int nn = __atomic_load_n(&g_nnames, __ATOMIC_ACQUIRE);
    if (i < 0 || i >= nn) return;
    st_lock();
    st_copy(name, sizeof name, g_names[i]);
    memcpy(w, g_watch, sizeof w);
    st_unlock();
    if (st_watch_toggle(w, name) != 0) {
        C->banner(MOD_ID, "Too many playlists selected: unselect one first", 3000);
        return;
    }
    st_lock();
    memcpy(g_watch, w, sizeof g_watch);
    st_unlock();
    __atomic_add_fetch(&g_watch_ver, 1, __ATOMIC_RELEASE);
    st_watch_save();
    C->request_redraw();
}

static void pk_draw(az_draw *d, az_rect area, void *user)
{
    (void) user;
    char nm[MAX_PL][PL_NAME], w[WATCH_MAX + 1];
    int nn = __atomic_load_n(&g_nnames, __ATOMIC_ACQUIRE);
    st_lock();
    memcpy(nm, g_names, sizeof nm);
    memcpy(w, g_watch, sizeof w);
    st_unlock();

    const int row = C->row_height(d), pad = row / 4, box = row - 2 * pad;
    int px = clampi(row / 2, 8, 96);
    const int bottom = area.y + area.h;
    int y = area.y;
    C->draw_header(d, "Playlists to watch");
    y += row;
    pk_area = area;
    pk_row = row;
    if (nn == 0) {
        if (y + row <= bottom)
            draw_fitted(d, area.x + pad, y + pad, "Reading the library... reopen in a moment.", px,
                        C->color(AZ_COL_TEXT_DIM), area.w - 2 * pad);
        return;
    }
    pk_vis = clampi((bottom - y) / row, 1, MAX_PL);
    if (pk_sel < pk_top) pk_top = pk_sel;
    if (pk_sel >= pk_top + pk_vis) pk_top = pk_sel - pk_vis + 1;
    for (int i = pk_top; i < nn && y + row <= bottom; i++) {
        C->draw_checkbox(d, area.x + pad, y + pad, box, st_watch_has(w, nm[i]), i == pk_sel);
        draw_fitted(d, area.x + 2 * pad + box, y + pad, nm[i], px, C->color(AZ_COL_TEXT),
                    area.w - 3 * pad - box);
        y += row;
    }
}

static int pk_input(const az_event *ev, void *user)
{
    (void) user;
    int nn = __atomic_load_n(&g_nnames, __ATOMIC_ACQUIRE);
    switch (ev->kind) {
    case AZ_EV_KNOB:
        pk_sel = clampi(pk_sel + (ev->dir < 0 ? -1 : 1), 0, nn > 0 ? nn - 1 : 0);
        C->request_redraw();
        return 1;
    case AZ_EV_CLICK:
        pk_toggle(pk_sel);
        return 1;
    case AZ_EV_TAP: {
        int y0 = pk_area.y + pk_row;
        if (pk_row <= 0 || ev->y < y0) return 0;
        int i = pk_top + (ev->y - y0) / pk_row;
        if (i >= nn) return 0;
        pk_sel = i;
        pk_toggle(i);
        return 1;
    }
    default:
        return 0;
    }
}

static void pk_enter(void *user)
{
    (void) user;
    pk_sel = pk_top = 0;
    st_request();                       /* reads the library if it has not been read yet */
}

static const az_screen_v1 PICKER = {
    .size = sizeof(az_screen_v1), .on_draw = pk_draw, .on_input = pk_input, .on_enter = pk_enter,
    .user = NULL,
};

/* ---- Mod Menu rows ---------------------------------------------------------------------- */
static void act_rescan(void *user)
{
    (void) user;
    __atomic_store_n(&a_rescan, 1, __ATOMIC_SEQ_CST);
    s_nolib = 0;                        /* a manual rescan does not wait out the 10 s retry */
    st_request();
    C->banner(MOD_ID, "Rescanning the library...", 2000);
}

static int add_row(int kind, const char *key, const char *label, const char *parent, const char *help,
                   unsigned flags, const char *const *choices, int n, int def, const az_screen_v1 *scr,
                   void (*act)(void *))
{
    az_row_v1 r;
    memset(&r, 0, sizeof r);
    r.size = sizeof r;
    r.key = key;
    r.label = label;
    r.kind = kind;
    r.flags = flags;
    r.parent = parent;
    r.help = help;
    r.choices = choices;
    r.n_choices = n;
    r.default_choice = def;
    r.screen = scr;
    r.on_activate = act;
    return C->register_row(MOD_ID, &r);
}

static const char *const C_BPM[] = { "+/- 3 BPM", "+/- 6 BPM", "+/- 8 BPM", "+/- 10 BPM" };
static const char *const C_KEY[] = { "Same key", "+/- 1 (Camelot)", "+/- 1 and relative" };
static const char *const C_DECK[] = { "Auto (playing)", "Deck 1", "Deck 2", "Deck 3", "Deck 4" };
static const char *const C_USB[] = { "Both USB", "USB 1 only", "USB 2 only" };
static const char *const C_TOP[] = { "Full screen", "Low", "Lower", "Lowest" };

static int add_rows(void)
{
    int ok = 1;
    h_overlay = add_row(AZ_ROW_TOGGLE, "overlay", "SMART TRACK", NULL,
                        "Shows the SMART button on the Browse screen.", AZ_ROW_DEFAULT_ON, NULL, 0, 0, NULL, NULL);
    h_otop = add_row(AZ_ROW_CHOICE, "overlay-top", "Overlay top", "overlay",
                     "Lower it if the overlay hides something you need.", 0, C_TOP, 4, 0, NULL, NULL);
    h_bpm = add_row(AZ_ROW_TOGGLE, "bpm", "Match BPM", NULL,
                    "Tracks near the playing tempo (pitch fader included).", AZ_ROW_DEFAULT_ON, NULL, 0, 0, NULL, NULL);
    h_bpm_rng = add_row(AZ_ROW_CHOICE, "bpm-range", "BPM range", "bpm", "How far from the playing BPM.",
                        0, C_BPM, 4, 1, NULL, NULL);
    h_key = add_row(AZ_ROW_TOGGLE, "key", "Match key", NULL, "Harmonic match on the Camelot wheel.",
                    AZ_ROW_DEFAULT_ON, NULL, 0, 0, NULL, NULL);
    h_key_mode = add_row(AZ_ROW_CHOICE, "key-mode", "Key range", "key", "Which keys count as compatible.",
                         0, C_KEY, 3, 1, NULL, NULL);
    h_pl = add_row(AZ_ROW_TOGGLE, "playlist", "Match playlist", NULL,
                   "Only tracks from the playing track's playlists.", AZ_ROW_DEFAULT_ON, NULL, 0, 0, NULL, NULL);
    int h_pick = add_row(AZ_ROW_SCREEN, "pl-pick", "Playlists to watch", "playlist",
                         "Pick hip-hop, house... Leave ALL out.", 0, NULL, 0, 0, &PICKER, NULL);
    h_deck = add_row(AZ_ROW_CHOICE, "deck", "Source deck", NULL, "Auto follows the deck that is playing.",
                     0, C_DECK, 5, 0, NULL, NULL);
    h_usb = add_row(AZ_ROW_CHOICE, "usb", "Search in", NULL, "Which USB ports to suggest tracks from.",
                    0, C_USB, 3, 0, NULL, NULL);
    int h_scan = add_row(AZ_ROW_ACTION, "rescan", "Rescan library", NULL,
                         "Reads the USB library again (after adding tracks or playlists).", 0, NULL, 0, 0, NULL,
                         act_rescan);
    ok = h_overlay >= 0 && h_otop >= 0 && h_bpm >= 0 && h_bpm_rng >= 0 && h_key >= 0 && h_key_mode >= 0 &&
         h_pl >= 0 && h_pick >= 0 && h_deck >= 0 && h_usb >= 0 && h_scan >= 0;
    return ok;
}

/* ---- lifecycle -------------------------------------------------------------------------- */
static void on_deck(int deck, int what, void *user)
{
    (void) user;
    if (deck < 0 || deck > 3) return;
    if (what == AZ_DECK_PLAY) __atomic_store_n(&a_last_play, deck, __ATOMIC_RELAXED);
    if (what == AZ_DECK_LOADED) __atomic_store_n(&a_want_scan, 1, __ATOMIC_RELAXED);
}

static unsigned st_sig(void)
{
    unsigned h = 2166136261u;
#define MIX(v) h = (h ^ (unsigned) (v)) * 16777619u
    int dk = st_pick_deck();
    MIX(dk + 1);
    if (dk >= 0) {
        az_deck_info di;
        di.size = sizeof di;
        if (C->deck_info(dk, &di) == 0) {
            MIX(di.loaded);
            MIX(di.content_id);
            MIX((int) (di.bpm * 2.0 + 0.5));            /* a half-BPM step is worth a new list */
        }
    }
    MIX(st_row(h_bpm, 1)); MIX(st_row(h_bpm_rng, 1));
    MIX(st_row(h_key, 1)); MIX(st_row(h_key_mode, 1));
    MIX(st_row(h_pl, 1)); MIX(st_row(h_usb, 0)); MIX(st_row(h_deck, 0));
    MIX(__atomic_load_n(&g_watch_ver, __ATOMIC_RELAXED));
#undef MIX
    return h;
}

static void tick(void)
{
    long now = now_ms();
    __atomic_store_n(&a_now_ms, now, __ATOMIC_RELAXED);
    if (now - last_tick_ms < 150) return;
    last_tick_ms = now;

    int en = st_row(h_overlay, 1) == 1;
    if (en != last_en) {
        last_en = en;
        C->surface_show(chip, en);
        if (!en && __atomic_load_n(&a_active, __ATOMIC_RELAXED)) close_overlay();
    }
    int top = st_row(h_otop, 0);
    if (top != last_top) {
        last_top = top;
        ov_place();
    }
    if (__atomic_exchange_n(&a_want_scan, 0, __ATOMIC_RELAXED) && !s_scanned && !s_nolib) st_request();

    if (!__atomic_load_n(&a_active, __ATOMIC_RELAXED)) return;
    if (C->view() != AZ_VIEW_BROWSE) { close_overlay(); return; }

    unsigned sig = st_sig();
    if (sig != last_sig) {
        last_sig = sig;
        st_request();
    }
    int ver = __atomic_load_n(&g_ver, __ATOMIC_ACQUIRE);
    if (ver != seen_ver) {
        seen_ver = ver;
        C->surface_redraw(ov);
    }
}

static int init(const az_core_v1 *core)
{
    if (!core || core->abi != AZ_MOD_ABI) return 1;
    C = core;
    if (!AZ_HAS(C, surface_create) || !AZ_HAS(C, deck_info) ||
        !AZ_HAS(C, lib_tracks) || !AZ_HAS(C, lib_playlists) || !AZ_HAS(C, thread_start) ||
        !AZ_HAS(C, on_deck_event) || !AZ_HAS(C, cfg_get_str) || !AZ_HAS(C, banner) || !AZ_HAS(C, view)) {
        C->log(MOD_ID, "needs AZ-MODS 1.2.3 or newer");
        return 2;
    }

    /* the watched playlists, kept in two text settings (127 bytes each) */
    char a[WATCH_HALF + 2], b[WATCH_HALF + 2];
    C->cfg_get_str(MOD_ID, "watch", a, (int) sizeof a, "");
    C->cfg_get_str(MOD_ID, "watch2", b, (int) sizeof b, "");
    char joined[2 * WATCH_HALF + 8];
    snprintf(joined, sizeof joined, "%s%s%s", a, (a[0] && b[0]) ? ";" : "", b);
    st_copy(g_watch, sizeof g_watch, joined);

    if (!AZ_HAS(C, register_row) || !add_rows())
        C->log(MOD_ID, "Mod Menu rows unavailable: running with default settings");
    if (C->on_deck_event(MOD_ID, on_deck, NULL) != 0) C->log(MOD_ID, "on_deck_event refused");

    ov = C->surface_create(MOD_ID, &OVERLAY);
    chip = C->surface_create(MOD_ID, &CHIP);
    if (!ov || !chip) {
        C->log(MOD_ID, "surface_create refused");
        return 4;
    }
    C->surface_place(chip, (az_rect) { CHIP_X, CHIP_Y, CHIP_W, CHIP_H });
    C->surface_show(chip, 1);
    C->surface_show(ov, 0);
    ov_place();
    if (C->thread_start(MOD_ID, "smart-scan", worker, NULL) != 0) {
        C->log(MOD_ID, "thread_start refused");
        return 5;
    }
    C->log(MOD_ID, "loaded, watching \"%s\"", g_watch);
    return 0;
}

/* No stop(): the mod creates surfaces, which the core cannot take back while the deck runs, so a
 * change of its on/off state always waits for a reload. */
static const char *const FW_TAGS[] = { "any", NULL };

static const az_mod_v1 MOD = {
    .abi = AZ_MOD_ABI,
    .size = sizeof(az_mod_v1),
    .id = MOD_ID,
    .fw_tags = FW_TAGS,
    .init = init,
    .tick = tick,
    .on_frame = NULL,
};

const az_mod_v1 *az_mod_v1_entry(void)
{
    return &MOD;
}
