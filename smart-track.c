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
#define MOD_VERSION "0.3.0"

/* ---- layout (screen is 1280x800) ---------------------------------------------------------
 * The SDK cannot tell the Tag List tab from the Playlist tab (both are AZ_VIEW_BROWSE) and does
 * not say where EP147 draws its tabs. So the DJ calibrates once (Mod Menu > Smart Track >
 * Calibrate Tag List tab: tap the tab) and a touch filter, which sees touches before EP147,
 * opens the overlay when that tab is touched. The SMART button below is the fallback. */
#define CHIP_X 1040
#define CHIP_Y 6
#define CHIP_W 232
#define CHIP_H 44
#define CAL_X 816            /* the SET TAG LIST TAB button, left of the SMART button */
#define CAL_Y 6
#define CAL_W 216
#define CAL_H 44
#define SCREEN_W 1280
#define SCREEN_H 800

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
    int f_bpm, f_key, f_pl, tol, kmode;       /* the filters actually applied */
} ctx_t;

/* ---- shared state ---------------------------------------------------------------------- */
/* Written by the worker, read by the UI thread / painter; always under st_lock(). */
static int g_lock_flag;
static ctx_t g_ctx;
static res_t g_res[MAX_RES];
static char g_names[MAX_PL][PL_NAME];
static int g_nnames;
static char g_watch[WATCH_MAX + 1];
static int g_plcount[MAX_PL];
static int g_scan_state;     /* 0 not read, 1 reading, 2 no library, 3 ready (atomic) */
static int g_ntracks_pub;
static int g_ver;            /* bumped on every published result (atomic) */
static int g_watch_ver;      /* bumped when the watched playlists change (atomic) */

/* Worker-private: only the job thread touches these (jobs never overlap). */
static trk_t s_tracks[MAX_TRACKS];     /* .bss: pages are only touched as the library fills it */
static int s_ntracks;
static char s_names[MAX_PL][PL_NAME];
static int s_nnames;
static int s_plcount[MAX_PL];
static int s_scanned, s_nolib;
static long s_nolib_ms;

/* Atomics shared by everyone. */
static int a_pending, a_rescan, a_last_play = -1, a_want_scan;
static long a_now_ms;                         /* the UI thread's clock, for the worker */
static int a_active, a_sel;                  /* overlay open; selected row */
static int a_calib, a_calib_done, a_req;     /* calibration; touch filter -> tick: 1 open, 2 close */
static int a_tab_x = -1, a_tab_y = -1;       /* the Tag List tab's touch point, -1 = not calibrated */
static int a_tab_on, a_hw = 100;             /* open from the tab; half width of its touch zone */
static long a_calib_t, a_pk_ms;
static int a_ticks, a_touch_n, a_touch_x, a_touch_y, a_touch_view = -1;   /* the HUD's facts */
/* Painter -> input geometry (screen pixels). */
static int a_row_y0, a_row_h, a_vis, a_top;
/* The overlay's second page (playlists to watch) and its touch zones. */
static int a_page, a_psel, a_ptop, a_pvis, a_prow_y0, a_prow_h;
static int a_pb_x, a_pb_y, a_pb_w, a_pb_h;      /* page 1: the "< RESULTS" button */
static int a_pl_x, a_pl_y, a_pl_w, a_pl_h;      /* page 0: the PLAYLIST button (w = 0: none) */
static int a_gut_x, a_gut_w;                    /* the UP / DN buttons (w = 0: none) */

/* UI-thread-only. */
static int h_bpm = -1, h_bpm_rng = -1, h_key = -1, h_key_mode = -1, h_pl = -1;
static int h_diag = -1, h_deck = -1, h_usb = -1, h_overlay = -1, h_otop = -1, h_open = -1, h_tabw = -1;
static int a_focus;
static az_surface *ov, *chip, *hud, *cal;
static long last_tick_ms;
static unsigned last_sig = 0xFFFFFFFFu;
static int seen_ver = -1, last_top = -1, last_chip = -1, last_hud = -1, last_cal = -1, last_cal_sig = -1,
    banner_done;
static unsigned last_hud_sig = 0xFFFFFFFFu;

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
    if (k) {
        if (!(k->pl & (1ull << c->bit))) s_plcount[c->bit]++;
        k->pl |= 1ull << c->bit;
    }
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

    __atomic_store_n(&g_scan_state, 1, __ATOMIC_RELEASE);
    st_lock();
    g_ctx.status = ST_SCAN;
    st_unlock();
    __atomic_add_fetch(&g_ver, 1, __ATOMIC_RELEASE);

    s_ntracks = 0;
    s_scanned = 0;
    s_nnames = 0;
    memset(s_plcount, 0, sizeof s_plcount);
    s_npl = 0;

    for (int stick = 0; stick < 2; stick++) {
        scan_ctx c = { stick, 0 };
        C->lib_tracks(MOD_ID, stick, 0, cb_all, &c);
    }
    if (s_ntracks == 0) {
        s_nolib = 1;
        __atomic_store_n(&g_scan_state, 2, __ATOMIC_RELEASE);
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
    memcpy(g_plcount, s_plcount, sizeof g_plcount);
    st_unlock();
    __atomic_store_n(&g_nnames, s_nnames, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ntracks_pub, s_ntracks, __ATOMIC_RELEASE);
    __atomic_store_n(&g_scan_state, 3, __ATOMIC_RELEASE);
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
    cx.f_bpm = f_bpm;
    cx.f_key = f_key;
    cx.f_pl = use_pl;
    cx.tol = tol;
    cx.kmode = kmode;
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

/* t = 0..256: 0 gives a, 256 gives b */
static uint32_t mix(uint32_t a, uint32_t b, int t)
{
    uint32_t r = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (int) ((a >> sh) & 255), cb = (int) ((b >> sh) & 255);
        r |= (uint32_t) (ca + (cb - ca) * t / 256) << sh;
    }
    return r;
}

/* text centred vertically in a band of height h */
static void text_in(az_draw *d, int x, int y, int h, const char *s, int px, uint32_t col)
{
    C->draw_text(d, x, y + (h - px * 3 / 2) / 2, s, px, col);
}

static void fitted_in(az_draw *d, int x, int y, int h, const char *s, int px, uint32_t col, int w)
{
    draw_fitted(d, x, y + (h - px * 3 / 2) / 2, s, px, col, w);
}

/* A small label box: filled when on, outlined and dim when off. Returns its width. */
static int pill(az_draw *d, int x, int y, int h, const char *s, int px, int on, uint32_t hi, uint32_t hit,
                uint32_t dim)
{
    int w = C->text_width(s, px) + h;
    if (on) C->fill_rect(d, x, y, w, h, hi);
    else C->draw_rect(d, x, y, w, h, 2, dim);
    text_in(d, x + h / 2, y, h, s, px, on ? hit : dim);
    return w;
}

/* ---- the overlay ----------------------------------------------------------------------- */
static const int OV_OFF[4] = { 34, 48, 64, 84 };    /* px under the Tag List tab's touch point */
static const int TAB_HW[3] = { 70, 100, 140 };      /* half width of the tab's touch zone */
#define TAB_HH 34                                   /* half height of the tab bar's band */
#define DEF_TAB_Y 70

static void pk_toggle(int i);

static void ov_place(void)
{
    int ty = __atomic_load_n(&a_tab_y, __ATOMIC_RELAXED);
    if (ty < 0) ty = DEF_TAB_Y;
    int top = clampi(ty + OV_OFF[clampi(st_row(h_otop, 1), 0, 3)], 0, SCREEN_H - 240);
    C->surface_place(ov, (az_rect) { 0, top, SCREEN_W, SCREEN_H - top });
}

static void open_overlay(void)
{
    if (st_row(h_overlay, 1) != 1) return;
    __atomic_store_n(&a_active, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&a_sel, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a_page, 0, __ATOMIC_RELAXED);
    ov_place();
    C->surface_show(ov, 1);
    a_focus = (C->surface_focus(ov, 1) == 1);      /* tick retries until the core grants it */
    last_sig = 0xFFFFFFFFu;                        /* force a match now */
    st_request();
    C->surface_redraw(ov);
    C->surface_redraw(chip);
}

static void close_overlay(void)
{
    __atomic_store_n(&a_active, 0, __ATOMIC_RELAXED);
    C->surface_focus(ov, 0);
    a_focus = 0;
    C->surface_show(ov, 0);
    C->surface_redraw(chip);
}

/* UP / DN touch buttons over the right end of a list: the way to scroll when the knob is not ours */
static void scroll_btns(az_draw *d, int x, int y0, int h, int w, int px)
{
    const uint32_t hi = C->color(AZ_COL_HI), text = C->color(AZ_COL_TEXT), bg = C->color(AZ_COL_BG);
    int bh = h / 2 - 4;
    if (bh > w * 3 / 2) bh = w * 3 / 2;
    if (bh < 12) return;
    static const char *const LBL[2] = { "UP", "DN" };
    for (int i = 0; i < 2; i++) {
        int by = i == 0 ? y0 : y0 + h - bh;
        C->fill_rect(d, x, by, w, bh, bg);
        C->draw_rect(d, x, by, w, bh, 2, hi);
        int tw = C->text_width(LBL[i], px);
        C->draw_text(d, x + (w - tw) / 2, by + (bh - px * 3 / 2) / 2, LBL[i], px, text);
    }
}

/* page 1 of the overlay: tick the playlists to watch, right on the Browse screen */
static void pick_paint(az_draw *d, az_rect area)
{
    char nm[MAX_PL][PL_NAME], w[WATCH_MAX + 1], buf[160];
    int cnt[MAX_PL];
    int nn = __atomic_load_n(&g_nnames, __ATOMIC_ACQUIRE);
    st_lock();
    memcpy(nm, g_names, sizeof nm);
    memcpy(cnt, g_plcount, sizeof cnt);
    memcpy(w, g_watch, sizeof w);
    st_unlock();

    const uint32_t text = C->color(AZ_COL_TEXT), dim = C->color(AZ_COL_TEXT_DIM);
    const uint32_t hi = C->color(AZ_COL_HI), hit = C->color(AZ_COL_HI_TEXT), bg = C->color(AZ_COL_BG);
    const int row = C->row_height(d), pad = row / 4, box = row - 2 * pad;
    const int px = clampi(row * 2 / 5, 8, 96), px_s = clampi(row * 28 / 100, 8, 96);
    const int bottom = area.y + area.h;
    int y = area.y;
    C->fill_rect(d, area.x, y, area.w, area.h, bg);

    /* top bar: back to the results, and the title */
    const int bh = row * 4 / 5, bw = C->text_width("< RESULTS", px_s) + row;
    C->fill_rect(d, area.x + pad, y + pad / 2, bw, bh, hi);
    text_in(d, area.x + pad + row / 2, y + pad / 2, bh, "< RESULTS", px_s, hit);
    C->draw_text(d, area.x + pad + bw + pad, y + pad / 2 + (bh - px * 3 / 2) / 2, "Playlists to watch", px, text);
    __atomic_store_n(&a_pb_x, area.x, __ATOMIC_RELAXED);
    __atomic_store_n(&a_pb_y, area.y, __ATOMIC_RELAXED);
    __atomic_store_n(&a_pb_w, bw + 2 * pad, __ATOMIC_RELAXED);
    __atomic_store_n(&a_pb_h, row, __ATOMIC_RELAXED);
    y += row;
    __atomic_store_n(&a_gut_w, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a_prow_h, 0, __ATOMIC_RELAXED);

    if (nn == 0) {
        int st = __atomic_load_n(&g_scan_state, __ATOMIC_ACQUIRE);
        const char *m = "Reading the library...";
        if (st == 2) m = "No rekordbox library found. Insert a USB, then use Rescan library.";
        else if (st == 3) m = "The library has no playlists.";
        draw_fitted(d, area.x + pad, y + pad, m, px, text, area.w - 2 * pad);
        return;
    }
    int on = 0;
    for (int i = 0; i < nn; i++) on += st_watch_has(w, nm[i]);
    if (on) snprintf(buf, sizeof buf, "%d watched: only tracks from the playing track's watched playlists are suggested.", on);
    else snprintf(buf, sizeof buf, "Tap the playlists to search in (hip-hop, house...). Leave ALL unticked.");
    draw_fitted(d, area.x + pad, y + (row * 3 / 4 - px_s * 3 / 2) / 2, buf, px_s, dim, area.w - 2 * pad);
    y += row * 3 / 4;

    const int vis = clampi((bottom - y) / row, 1, MAX_PL);
    const int gut = nn > vis ? clampi(row * 3 / 2, 40, 80) : 0;
    int sel = __atomic_load_n(&a_psel, __ATOMIC_RELAXED), top = __atomic_load_n(&a_ptop, __ATOMIC_RELAXED);
    if (sel >= nn) sel = nn - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    if (top < 0) top = 0;
    __atomic_store_n(&a_psel, sel, __ATOMIC_RELAXED);
    __atomic_store_n(&a_ptop, top, __ATOMIC_RELAXED);
    __atomic_store_n(&a_pvis, vis, __ATOMIC_RELAXED);
    __atomic_store_n(&a_prow_y0, y, __ATOMIC_RELAXED);
    __atomic_store_n(&a_prow_h, row, __ATOMIC_RELAXED);
    const int list_y = y;
    for (int i = top; i < nn && i < top + vis && y + row <= bottom; i++) {
        const int is_sel = (i == sel);
        if (is_sel) C->fill_rect(d, area.x, y, area.w - gut, row, hi);
        C->draw_checkbox(d, area.x + pad, y + pad, box, st_watch_has(w, nm[i]), is_sel);
        snprintf(buf, sizeof buf, "%d tracks", cnt[i]);
        int cw = C->text_width(buf, px_s);
        draw_fitted(d, area.x + 2 * pad + box, y + (row - px * 3 / 2) / 2, nm[i], px, is_sel ? hit : text,
                    area.w - gut - 4 * pad - box - cw);
        C->draw_text(d, area.x + area.w - gut - pad - cw, y + (row - px_s * 3 / 2) / 2, buf, px_s,
                     is_sel ? hit : dim);
        y += row;
    }
    if (gut) {
        scroll_btns(d, area.x + area.w - gut + 2, list_y, vis * row, gut - 4, px);
        __atomic_store_n(&a_gut_x, area.x + area.w - gut, __ATOMIC_RELAXED);
        __atomic_store_n(&a_gut_w, gut, __ATOMIC_RELAXED);
    }
}

static void ov_paint(az_draw *d, az_rect area, void *user)
{
    (void) user;
    ctx_t cx;
    res_t vis[VIS_MAX];
    int top = __atomic_load_n(&a_top, __ATOMIC_RELAXED);
    int sel = __atomic_load_n(&a_sel, __ATOMIC_RELAXED);

    if (__atomic_load_n(&a_page, __ATOMIC_RELAXED) == 1) {
        pick_paint(d, area);
        return;
    }
    __atomic_store_n(&a_pl_w, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a_gut_w, 0, __ATOMIC_RELAXED);

    const int row = C->row_height(d);
    const int pad = row * 3 / 10;
    const int card_h = row * 5 / 2, flt_h = row * 9 / 10, col_h = row * 3 / 5;
    const int lh = row * 4 / 5, foot_h = row * 7 / 10;
    int vmax = (area.h - card_h - flt_h - col_h - foot_h) / lh;
    vmax = clampi(vmax, 1, VIS_MAX);

    st_lock();
    cx = g_ctx;
    int n = cx.nres;
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
    const uint32_t hi = C->color(AZ_COL_HI), hit = C->color(AZ_COL_HI_TEXT), bg = C->color(AZ_COL_BG);
    const uint32_t card = mix(bg, text, 22), zebra = mix(bg, text, 12), rail = mix(bg, text, 50);
    const int px_s = clampi(row * 27 / 100, 10, 96);      /* small labels */
    const int px_m = clampi(row * 36 / 100, 10, 96);      /* body */
    const int px_l = clampi(row * 52 / 100, 10, 96);      /* the playing title */
    const int px_b = clampi(row * 95 / 100, 10, 96);      /* the BPM figure */
    const int X0 = area.x + pad, X1 = area.x + area.w - pad;
    int y = area.y;

    /* ---- card: the playing track ---- */
    C->fill_rect(d, area.x, y, area.w, card_h, card);
    C->fill_rect(d, area.x, y, pad / 2 + 2, card_h, hi);
    char buf[200];
    if (cx.status != ST_OK) {
        const char *m = "Reading the library...";
        if (cx.status == ST_NOLIB) m = "No rekordbox library found. Insert a USB, then use Rescan library.";
        else if (cx.status == ST_NOTRACK) m = "Load or play a track on a deck.";
        text_in(d, X0 + pad, y, px_s * 3 / 2 + pad, "SMART TRACK", px_s, dim);
        fitted_in(d, X0 + pad, y + pad, card_h - pad, m, px_l, text, X1 - X0 - pad);
        {   /* the playlists can still be chosen */
            int bh = px_s * 3 / 2 + pad, bw = C->text_width("PLAYLISTS >", px_s) + row;
            C->fill_rect(d, X1 - bw, y + pad / 2, bw, bh, hi);
            text_in(d, X1 - bw + row / 2, y + pad / 2, bh, "PLAYLISTS >", px_s, hit);
            __atomic_store_n(&a_pl_x, X1 - bw - pad, __ATOMIC_RELAXED);
            __atomic_store_n(&a_pl_y, y, __ATOMIC_RELAXED);
            __atomic_store_n(&a_pl_w, bw + 2 * pad, __ATOMIC_RELAXED);
            __atomic_store_n(&a_pl_h, bh + pad, __ATOMIC_RELAXED);
        }
        return;
    }
    snprintf(buf, sizeof buf, "NOW PLAYING   DECK %d", cx.deck + 1);
    C->draw_text(d, X0 + pad, y + pad / 2, buf, px_s, dim);
    int ty = y + pad / 2 + px_s * 3 / 2;
    int left_w = area.w * 58 / 100 - pad;
    draw_fitted(d, X0 + pad, ty, cx.title, px_l, text, left_w);
    ty += px_l * 3 / 2;
    draw_fitted(d, X0 + pad, ty, cx.artist, px_m, dim, left_w);
    ty += px_m * 3 / 2;
    if (cx.pls[0]) {
        snprintf(buf, sizeof buf, "in %s", cx.pls);
        draw_fitted(d, X0 + pad, ty, buf, px_s, dim, left_w);
    }
    {   /* right block: BPM figure, key badge */
        int rx = area.x + area.w * 62 / 100;
        snprintf(buf, sizeof buf, "%.1f", (double) cx.bpm);
        C->draw_text(d, rx, y + pad / 2, buf, px_b, text);
        C->draw_text(d, rx + C->text_width(buf, px_b) + pad / 2, y + pad / 2 + px_b * 3 / 2 - px_m * 3 / 2 - 4,
                     "BPM", px_m, dim);
        int by = y + pad / 2 + px_b * 3 / 2 + 2, bh = px_m * 3 / 2 + 6;
        if (cx.key[0]) {
            int bw = C->text_width(cx.key, px_m) + pad * 2;
            C->fill_rect(d, rx, by, bw, bh, hi);
            text_in(d, rx + pad, by, bh, cx.key, px_m, hit);
            C->draw_text(d, rx + bw + pad / 2, by + (bh - px_s * 3 / 2) / 2, "KEY", px_s, dim);
        } else {
            C->draw_text(d, rx, by + (bh - px_s * 3 / 2) / 2, "KEY unknown", px_s, dim);
        }
    }
    y += card_h;

    /* ---- filter row: what is applied, and the name to give the set ---- */
    {
        int fy = y + flt_h / 6, fh = flt_h * 2 / 3, fx = X0;
        const char *km = cx.kmode == 0 ? "KEY =" : (cx.kmode == 1 ? "KEY +/-1" : "KEY +/-1 +REL");
        snprintf(buf, sizeof buf, "BPM +/-%d", cx.tol);
        fx += pill(d, fx, fy, fh, buf, px_s, cx.f_bpm, hi, hit, dim) + pad / 2;
        fx += pill(d, fx, fy, fh, km, px_s, cx.f_key, hi, hit, dim) + pad / 2;
        int pw = pill(d, fx, fy, fh, "PLAYLIST >", px_s, cx.f_pl, hi, hit, dim);
        __atomic_store_n(&a_pl_x, fx - pad, __ATOMIC_RELAXED);
        __atomic_store_n(&a_pl_y, y, __ATOMIC_RELAXED);
        __atomic_store_n(&a_pl_w, pw + 2 * pad, __ATOMIC_RELAXED);
        __atomic_store_n(&a_pl_h, flt_h, __ATOMIC_RELAXED);
        fx += pw;
        int nx = area.x + area.w * 52 / 100;
        C->draw_text(d, nx, fy + (fh - px_s * 3 / 2) / 2, "SET NAME", px_s, dim);
        int bx = nx + C->text_width("SET NAME", px_s) + pad / 2;
        C->draw_rect(d, bx, fy, X1 - bx, fh, 2, hi);
        fitted_in(d, bx + pad / 2, fy, fh, cx.name, px_m, text, X1 - bx - pad);
    }
    y += flt_h;

    /* ---- list: column heads ---- */
    const int sbw = 8;
    const int gutw = n > vmax ? clampi(row * 3 / 2, 40, 80) : 0;     /* UP / DN buttons */
    const int w_bpm = C->text_width("000.0", px_m) + pad;
    const int w_dif = C->text_width("+00.0", px_m) + pad;
    const int w_key = C->text_width("12A", px_m) + pad * 3;
    const int rest = (X1 - sbw - gutw) - (X0 + w_bpm + w_dif + w_key);
    const int w_tit = rest * 50 / 100, w_art = rest * 28 / 100;
    const int c_bpm = X0, c_dif = c_bpm + w_bpm, c_key = c_dif + w_dif, c_tit = c_key + w_key;
    const int c_art = c_tit + w_tit, c_pl = c_art + w_art;
    C->fill_rect(d, area.x, y, area.w, col_h, bg);
    text_in(d, c_bpm, y, col_h, "BPM", px_s, dim);
    text_in(d, c_dif, y, col_h, "DIFF", px_s, dim);
    text_in(d, c_key, y, col_h, "KEY", px_s, dim);
    text_in(d, c_tit, y, col_h, "TITLE", px_s, dim);
    text_in(d, c_art, y, col_h, "ARTIST", px_s, dim);
    text_in(d, c_pl, y, col_h, "PLAYLIST", px_s, dim);
    C->fill_rect(d, X0, y + col_h - 2, X1 - X0, 1, rail);
    y += col_h;

    __atomic_store_n(&a_row_y0, y, __ATOMIC_RELAXED);
    __atomic_store_n(&a_row_h, lh, __ATOMIC_RELAXED);
    __atomic_store_n(&a_vis, vmax, __ATOMIC_RELAXED);

    if (n == 0) {
        fitted_in(d, X0, y, lh * 2, "Nothing matches. Loosen a filter in Mod Menu > Smart Track.", px_m, text,
                  X1 - X0);
        if (cx.note[0]) fitted_in(d, X0, y + lh * 2, lh, cx.note, px_s, dim, X1 - X0);
    }
    for (int i = 0; i < cnt; i++) {
        const res_t *r = &vis[i];
        const int is_sel = (top + i == sel);
        const int ry = y + i * lh;
        if (is_sel) C->fill_rect(d, area.x, ry, area.w - sbw - gutw, lh, hi);
        else if (i & 1) C->fill_rect(d, area.x, ry, area.w - sbw - gutw, lh, zebra);
        const uint32_t c1 = is_sel ? hit : text, c2 = is_sel ? hit : dim;
        snprintf(buf, sizeof buf, "%.1f", (double) r->bpm);
        fitted_in(d, c_bpm, ry, lh, buf, px_m, c1, w_bpm);
        float df = r->bpm - cx.bpm;
        snprintf(buf, sizeof buf, "%+.1f", (double) df);
        fitted_in(d, c_dif, ry, lh, buf, px_m, c2, w_dif);
        {   /* key: filled badge when it is the same key, outlined otherwise */
            int bh = lh - 8, bw = C->text_width("12A", px_m) + pad * 2, by = ry + 4;
            uint32_t edge = is_sel ? hit : hi;
            if (r->same_key) {
                C->fill_rect(d, c_key, by, bw, bh, edge);
                text_in(d, c_key + pad, by, bh, r->key, px_m, is_sel ? hi : hit);
            } else {
                C->draw_rect(d, c_key, by, bw, bh, 2, edge);
                text_in(d, c_key + pad, by, bh, r->key, px_m, c1);
            }
        }
        fitted_in(d, c_tit, ry, lh, r->title, px_m, c1, w_tit - pad);
        fitted_in(d, c_art, ry, lh, r->artist, px_m, c2, w_art - pad);
        fitted_in(d, c_pl, ry, lh, r->pl, px_s, c2, X1 - sbw - gutw - c_pl);
    }
    if (gutw) {
        scroll_btns(d, area.x + area.w - sbw - gutw + 2, y, vmax * lh, gutw - 4, px_m);
        __atomic_store_n(&a_gut_x, area.x + area.w - sbw - gutw, __ATOMIC_RELAXED);
        __atomic_store_n(&a_gut_w, gutw, __ATOMIC_RELAXED);
    }
    /* scrollbar */
    if (n > vmax) {
        int th = vmax * lh;
        int bh2 = clampi(th * vmax / n, 24, th);
        int by2 = y + (th - bh2) * top / (n - vmax);
        C->fill_rect(d, area.x + area.w - sbw + 2, y, 4, th, zebra);
        C->fill_rect(d, area.x + area.w - sbw + 2, by2, 4, bh2, hi);
    }
    /* footer */
    int fy = y + vmax * lh;
    if (n > 0) {
        snprintf(buf, sizeof buf, "%d / %d   (%d match%s)", sel + 1, n, cx.total, cx.total == 1 ? "" : "es");
        fitted_in(d, X0, fy, foot_h, buf, px_s, dim, area.w / 3);
    }
    if (cx.note[0]) fitted_in(d, area.x + area.w * 36 / 100, fy, foot_h, cx.note, px_s, dim, area.w * 30 / 100);
    {
        const char *hint = "KNOB or UP/DN scroll   CLICK details   BACK close";
        int hw = C->text_width(hint, px_s);
        text_in(d, X1 - hw, fy, foot_h, hint, px_s, dim);
    }
}

static int in_box(int x, int y, int bx, int by, int bw, int bh)
{
    return bw > 0 && bh > 0 && x >= bx && x < bx + bw && y >= by && y < by + bh;
}

/* Move the selection of the page that is showing by delta rows (clamped). */
static void ov_move(int delta)
{
    int n;
    if (__atomic_load_n(&a_page, __ATOMIC_RELAXED) == 1) {
        n = __atomic_load_n(&g_nnames, __ATOMIC_ACQUIRE);
        __atomic_store_n(&a_psel, clampi(__atomic_load_n(&a_psel, __ATOMIC_RELAXED) + delta, 0, n > 0 ? n - 1 : 0),
                         __ATOMIC_RELAXED);
    } else {
        st_lock();
        n = g_ctx.nres;
        st_unlock();
        __atomic_store_n(&a_sel, clampi(__atomic_load_n(&a_sel, __ATOMIC_RELAXED) + delta, 0, n > 0 ? n - 1 : 0),
                         __ATOMIC_RELAXED);
    }
    C->surface_redraw(ov);
}

static int ov_input(const az_event *ev, void *user)
{
    (void) user;
    int n;
    const int page = __atomic_load_n(&a_page, __ATOMIC_RELAXED);
    switch (ev->kind) {
    case AZ_EV_KNOB:
        ov_move(ev->dir < 0 ? -1 : 1);
        return 1;
    case AZ_EV_TAP: {
        if (!a_focus && C->surface_focus(ov, 1) == 1) a_focus = 1;     /* a touch is a chance to get the knob */
        const int gx = __atomic_load_n(&a_gut_x, __ATOMIC_RELAXED), gw = __atomic_load_n(&a_gut_w, __ATOMIC_RELAXED);
        const int y0 = page == 1 ? __atomic_load_n(&a_prow_y0, __ATOMIC_RELAXED)
                                 : __atomic_load_n(&a_row_y0, __ATOMIC_RELAXED);
        const int h = page == 1 ? __atomic_load_n(&a_prow_h, __ATOMIC_RELAXED)
                                : __atomic_load_n(&a_row_h, __ATOMIC_RELAXED);
        const int vis = page == 1 ? __atomic_load_n(&a_pvis, __ATOMIC_RELAXED)
                                  : __atomic_load_n(&a_vis, __ATOMIC_RELAXED);
        if (page == 1) {
            if (in_box(ev->x, ev->y, __atomic_load_n(&a_pb_x, __ATOMIC_RELAXED),
                       __atomic_load_n(&a_pb_y, __ATOMIC_RELAXED), __atomic_load_n(&a_pb_w, __ATOMIC_RELAXED),
                       __atomic_load_n(&a_pb_h, __ATOMIC_RELAXED))) {
                __atomic_store_n(&a_page, 0, __ATOMIC_RELAXED);
                C->surface_redraw(ov);
                return 1;
            }
        } else if (in_box(ev->x, ev->y, __atomic_load_n(&a_pl_x, __ATOMIC_RELAXED),
                          __atomic_load_n(&a_pl_y, __ATOMIC_RELAXED), __atomic_load_n(&a_pl_w, __ATOMIC_RELAXED),
                          __atomic_load_n(&a_pl_h, __ATOMIC_RELAXED))) {
            __atomic_store_n(&a_page, 1, __ATOMIC_RELAXED);
            C->surface_redraw(ov);
            return 1;
        }
        if (h <= 0 || ev->y < y0) return 1;
        if (gw > 0 && ev->x >= gx && ev->x < gx + gw) {                /* UP / DN: one page */
            int step = vis > 1 ? vis - 1 : 1;
            ov_move(ev->y < y0 + vis * h / 2 ? -step : step);
            return 1;
        }
        int r = (ev->y - y0) / h;
        if (r >= vis) return 1;
        if (page == 1) {
            n = __atomic_load_n(&g_nnames, __ATOMIC_ACQUIRE);
            int i = __atomic_load_n(&a_ptop, __ATOMIC_RELAXED) + r;
            if (i < n) {
                __atomic_store_n(&a_psel, i, __ATOMIC_RELAXED);
                pk_toggle(i);
            }
            C->surface_redraw(ov);
            return 1;
        }
        st_lock();
        n = g_ctx.nres;
        st_unlock();
        if (__atomic_load_n(&a_top, __ATOMIC_RELAXED) + r < n)
            __atomic_store_n(&a_sel, __atomic_load_n(&a_top, __ATOMIC_RELAXED) + r, __ATOMIC_RELAXED);
        C->surface_redraw(ov);
        return 1;
    }
    case AZ_EV_CLICK: {
        if (page == 1) {
            pk_toggle(__atomic_load_n(&a_psel, __ATOMIC_RELAXED));
            C->surface_redraw(ov);
            return 1;
        }
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
        if (page == 1) {
            __atomic_store_n(&a_page, 0, __ATOMIC_RELAXED);
            C->surface_redraw(ov);
        } else {
            close_overlay();
        }
        return 1;
    default:
        return 1;                      /* DOWN / UP / LONG inside the overlay: nothing to do */
    }
}

static const az_surface_v1 OVERLAY = {
    .size = sizeof(az_surface_v1), .layer = AZ_LAYER_OVER_EP147,
    .views = AZ_VIEW_BIT(AZ_VIEW_BROWSE), .on_paint = ov_paint, .on_input = ov_input, .user = NULL,
};

/* ---- the SMART button (fallback, and until the tab is calibrated) ------------------------ */
static void chip_paint(az_draw *d, az_rect area, void *user)
{
    (void) user;
    int on = __atomic_load_n(&a_active, __ATOMIC_RELAXED);
    uint32_t bg = on ? C->color(AZ_COL_HI) : C->color(AZ_COL_BG);
    uint32_t fg = on ? C->color(AZ_COL_HI_TEXT) : C->color(AZ_COL_TEXT);
    C->fill_rect(d, area.x, area.y, area.w, area.h, bg);
    C->draw_rect(d, area.x, area.y, area.w, area.h, 2, C->color(AZ_COL_HI));
    const char *s = on ? "SMART  ON" : "SMART TRACK";
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

/* ---- the SET TAG LIST TAB button: calibration without the Mod Menu ----------------------- */
static void cal_paint(az_draw *d, az_rect area, void *user)
{
    (void) user;
    int calib = __atomic_load_n(&a_calib, __ATOMIC_RELAXED);
    int set = __atomic_load_n(&a_tab_x, __ATOMIC_RELAXED) >= 0;
    uint32_t bg = calib ? C->color(AZ_COL_HI) : C->color(AZ_COL_BG);
    uint32_t fg = calib ? C->color(AZ_COL_HI_TEXT) : C->color(AZ_COL_TEXT);
    C->fill_rect(d, area.x, area.y, area.w, area.h, bg);
    C->draw_rect(d, area.x, area.y, area.w, area.h, 2, C->color(AZ_COL_HI));
    const char *s = calib ? "NOW TOUCH THE TAG LIST TAB" : (set ? "TAG LIST TAB SET - REDO" : "SET TAG LIST TAB");
    int px = clampi(area.h * 3 / 10, 8, 96);
    while (px > 8 && C->text_width(s, px) > area.w - 8) px--;
    int tw = C->text_width(s, px);
    C->draw_text(d, area.x + (area.w - tw) / 2, area.y + (area.h - px * 3 / 2) / 2, s, px, fg);
}

static int cal_input(const az_event *ev, void *user)
{
    (void) user;
    if (ev->kind == AZ_EV_TAP) {
        if (__atomic_load_n(&a_calib, __ATOMIC_RELAXED)) {
            __atomic_store_n(&a_calib, 0, __ATOMIC_RELEASE);          /* tap again: cancel */
        } else {
            __atomic_store_n(&a_calib_t, __atomic_load_n(&a_now_ms, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
            __atomic_store_n(&a_calib, 1, __ATOMIC_RELEASE);
            if (__atomic_load_n(&a_active, __ATOMIC_RELAXED)) close_overlay();
        }
        C->surface_redraw(cal);
    }
    return 1;
}

static const az_surface_v1 CAL = {
    .size = sizeof(az_surface_v1), .layer = AZ_LAYER_TOP,
    .views = AZ_VIEW_BIT(AZ_VIEW_BROWSE), .on_paint = cal_paint, .on_input = cal_input, .user = NULL,
};

/* ---- the Tag List tab: a touch filter (bus thread: atomics only, always PASS) -------------- */
static int touch_fn(const az_touch *t, void *user)
{
    static int prev_down;
    (void) user;
    if (!t || t->slot != 0) return AZ_TOUCH_PASS;
    int edge = t->down && !prev_down;
    prev_down = t->down ? 1 : 0;
    if (edge) {                                                  /* for the HUD */
        __atomic_store_n(&a_touch_x, t->x, __ATOMIC_RELAXED);
        __atomic_store_n(&a_touch_y, t->y, __ATOMIC_RELAXED);
        __atomic_store_n(&a_touch_view, C->view(), __ATOMIC_RELAXED);
        __atomic_add_fetch(&a_touch_n, 1, __ATOMIC_RELAXED);
    }
    if (!edge || C->view() != AZ_VIEW_BROWSE) return AZ_TOUCH_PASS;
    if (t->y < CAL_Y + CAL_H + 6 && t->x >= CAL_X) return AZ_TOUCH_PASS;      /* our own buttons */

    if (__atomic_load_n(&a_calib, __ATOMIC_RELAXED)) {          /* the DJ is teaching us the tab */
        __atomic_store_n(&a_tab_x, t->x, __ATOMIC_RELAXED);
        __atomic_store_n(&a_tab_y, t->y, __ATOMIC_RELAXED);
        __atomic_store_n(&a_calib, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&a_calib_done, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&a_req, 1, __ATOMIC_RELEASE);
        return AZ_TOUCH_PASS;
    }
    int tx = __atomic_load_n(&a_tab_x, __ATOMIC_RELAXED), tyy = __atomic_load_n(&a_tab_y, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&a_tab_on, __ATOMIC_RELAXED) || tx < 0) return AZ_TOUCH_PASS;
    int dy = t->y - tyy, dx = t->x - tx;
    if (dy < 0) dy = -dy;
    if (dx < 0) dx = -dx;
    if (dy > TAB_HH) return AZ_TOUCH_PASS;                      /* not the tab bar */
    /* on the Tag List tab: open; on another tab: close. EP147 still switches tabs as usual. */
    __atomic_store_n(&a_req, dx <= __atomic_load_n(&a_hw, __ATOMIC_RELAXED) ? 1 : 2, __ATOMIC_RELEASE);
    return AZ_TOUCH_PASS;
}

static void act_calibrate(void *user)
{
    (void) user;
    __atomic_store_n(&a_calib_t, __atomic_load_n(&a_now_ms, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&a_calib, 1, __ATOMIC_RELEASE);
    C->banner(MOD_ID, "Close the menu, open Browse, then tap the TAG LIST tab", 6000);
}

/* ---- the status strip (Diagnostics): which version runs, and what it sees ---------------- */
static void hud_paint(az_draw *d, az_rect area, void *user)
{
    (void) user;
    static const char *const SCAN[4] = { "not read", "reading", "NO LIBRARY", "ok" };
    char l1[160], l2[160];
    int st = clampi(__atomic_load_n(&g_scan_state, __ATOMIC_ACQUIRE), 0, 3);
    snprintf(l1, sizeof l1, "Smart Track v%s   library: %s   %d tracks   %d playlists   tick %d", MOD_VERSION,
             SCAN[st], __atomic_load_n(&g_ntracks_pub, __ATOMIC_ACQUIRE),
             __atomic_load_n(&g_nnames, __ATOMIC_ACQUIRE), __atomic_load_n(&a_ticks, __ATOMIC_RELAXED));
    int tx = __atomic_load_n(&a_tab_x, __ATOMIC_RELAXED), ty = __atomic_load_n(&a_tab_y, __ATOMIC_RELAXED);
    char tab[32];
    if (tx < 0) snprintf(tab, sizeof tab, "not set");
    else snprintf(tab, sizeof tab, "%d,%d", tx, ty);
    snprintf(l2, sizeof l2, "tab %s   last touch #%d at %d,%d (screen %d)   overlay %s   knob %s", tab,
             __atomic_load_n(&a_touch_n, __ATOMIC_RELAXED), __atomic_load_n(&a_touch_x, __ATOMIC_RELAXED),
             __atomic_load_n(&a_touch_y, __ATOMIC_RELAXED), __atomic_load_n(&a_touch_view, __ATOMIC_RELAXED),
             __atomic_load_n(&a_active, __ATOMIC_RELAXED) ? "open" : "closed", a_focus ? "OURS" : "not ours");
    const uint32_t text = C->color(AZ_COL_TEXT), bg = C->color(AZ_COL_BG);
    C->fill_rect(d, area.x, area.y, area.w, area.h, mix(bg, text, 30));
    int px = clampi(area.h * 5 / 16, 8, 96);
    C->draw_text(d, area.x + 8, area.y + 3, l1, px, text);
    C->draw_text(d, area.x + 8, area.y + 3 + px * 3 / 2, l2, px, text);
}

static int hud_input(const az_event *ev, void *user)
{
    (void) ev;
    (void) user;
    return 0;                          /* a status strip: touches go through to EP147 */
}

static const az_surface_v1 HUD = {
    .size = sizeof(az_surface_v1), .layer = AZ_LAYER_TOP,
    .views = AZ_VIEW_BIT(AZ_VIEW_BROWSE), .on_paint = hud_paint, .on_input = hud_input, .user = NULL,
};

/* ---- the playlist picker page (Mod Menu > Smart Track > Playlists to watch) ------------- */
static int pk_sel, pk_top, pk_row, pk_list_y, pk_vis = 1;

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
        C->banner(MOD_ID, "Too many playlists selected: untick one first", 3000);
        return;
    }
    st_lock();
    memcpy(g_watch, w, sizeof g_watch);
    st_unlock();
    __atomic_add_fetch(&g_watch_ver, 1, __ATOMIC_RELEASE);
    st_watch_save();
    C->request_redraw();
    if (ov) C->surface_redraw(ov);
}

static void pk_draw(az_draw *d, az_rect area, void *user)
{
    (void) user;
    char nm[MAX_PL][PL_NAME], w[WATCH_MAX + 1];
    int cnt[MAX_PL];
    int nn = __atomic_load_n(&g_nnames, __ATOMIC_ACQUIRE);
    st_lock();
    memcpy(nm, g_names, sizeof nm);
    memcpy(cnt, g_plcount, sizeof cnt);
    memcpy(w, g_watch, sizeof w);
    st_unlock();
    __atomic_store_n(&a_pk_ms, __atomic_load_n(&a_now_ms, __ATOMIC_RELAXED), __ATOMIC_RELAXED);

    const uint32_t text = C->color(AZ_COL_TEXT), dim = C->color(AZ_COL_TEXT_DIM);
    const uint32_t hi = C->color(AZ_COL_HI), hit = C->color(AZ_COL_HI_TEXT);
    const int row = C->row_height(d), pad = row / 4, box = row - 2 * pad;
    const int px = clampi(row * 2 / 5, 8, 96), px_s = clampi(row * 28 / 100, 8, 96);
    const int bottom = area.y + area.h;
    int y = area.y;
    C->draw_header(d, "Playlists to watch");
    y += row;
    pk_row = row;

    char buf[160];
    if (nn == 0) {
        int st = __atomic_load_n(&g_scan_state, __ATOMIC_ACQUIRE);
        const char *m = "Reading the library...";
        if (st == 2) m = "No rekordbox library found. Insert a USB, then use Rescan library.";
        else if (st == 3) m = "The library has no playlists.";
        draw_fitted(d, area.x + pad, y + pad, m, px, text, area.w - 2 * pad);
        pk_list_y = y + row;
        return;
    }
    int on = 0;
    for (int i = 0; i < nn; i++) on += st_watch_has(w, nm[i]);
    if (on) snprintf(buf, sizeof buf, "%d watched. Smart Track only suggests tracks from the playing track's watched playlists.", on);
    else snprintf(buf, sizeof buf, "Tick the playlists to search in (hip-hop, house...) and leave ALL unticked. None ticked = no playlist filter.");
    draw_fitted(d, area.x + pad, y + (row * 3 / 4 - px_s * 3 / 2) / 2, buf, px_s, dim, area.w - 2 * pad);
    y += row * 3 / 4;
    pk_list_y = y;

    pk_vis = clampi((bottom - y) / row, 1, MAX_PL);
    if (pk_sel >= nn) pk_sel = nn - 1;
    if (pk_sel < pk_top) pk_top = pk_sel;
    if (pk_sel >= pk_top + pk_vis) pk_top = pk_sel - pk_vis + 1;
    if (pk_top < 0) pk_top = 0;
    for (int i = pk_top; i < nn && y + row <= bottom; i++) {
        const int is_sel = (i == pk_sel);
        if (is_sel) C->fill_rect(d, area.x, y, area.w, row, hi);
        C->draw_checkbox(d, area.x + pad, y + pad, box, st_watch_has(w, nm[i]), is_sel);
        snprintf(buf, sizeof buf, "%d tracks", cnt[i]);
        int cw = C->text_width(buf, px_s);
        draw_fitted(d, area.x + 2 * pad + box, y + (row - px * 3 / 2) / 2, nm[i], px, is_sel ? hit : text,
                    area.w - 4 * pad - box - cw);
        C->draw_text(d, area.x + area.w - pad - cw, y + (row - px_s * 3 / 2) / 2, buf, px_s, is_sel ? hit : dim);
        y += row;
    }
}

static int pk_input(const az_event *ev, void *user)
{
    (void) user;
    int nn = __atomic_load_n(&g_nnames, __ATOMIC_ACQUIRE);
    switch (ev->kind) {
    case AZ_EV_KNOB:
        if (nn == 0) return 0;
        pk_sel = clampi(pk_sel + (ev->dir < 0 ? -1 : 1), 0, nn - 1);
        C->request_redraw();
        return 1;
    case AZ_EV_CLICK:
        if (nn == 0) return 0;
        pk_toggle(pk_sel);
        return 1;
    case AZ_EV_TAP: {
        if (pk_row <= 0 || ev->y < pk_list_y) return 0;
        int i = pk_top + (ev->y - pk_list_y) / pk_row;
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

static const char *const C_OPEN[] = { "Tag List tab", "SMART button", "Both" };
static const char *const C_TABW[] = { "Narrow", "Normal", "Wide" };
static const char *const C_TOP[] = { "Tight", "Normal", "Loose", "Looser" };
static const char *const C_BPM[] = { "+/- 3 BPM", "+/- 6 BPM", "+/- 8 BPM", "+/- 10 BPM" };
static const char *const C_KEY[] = { "Same key", "+/- 1 (Camelot)", "+/- 1 and relative" };
static const char *const C_DECK[] = { "Auto (playing)", "Deck 1", "Deck 2", "Deck 3", "Deck 4" };
static const char *const C_USB[] = { "Both USB", "USB 1 only", "USB 2 only" };

static int add_rows(void)
{
    h_overlay = add_row(AZ_ROW_TOGGLE, "overlay", "Smart Track", NULL,
                        "Suggests tracks that fit the playing one.", AZ_ROW_DEFAULT_ON, NULL, 0, 0, NULL, NULL);
    h_open = add_row(AZ_ROW_CHOICE, "open-with", "Open with", "overlay",
                     "Touching the Tag List tab (calibrate it first), the SMART button, or both.", 0, C_OPEN, 3, 2,
                     NULL, NULL);
    int h_cal = add_row(AZ_ROW_ACTION, "calibrate", "Calibrate Tag List tab", NULL,
                        "Then close the menu, open Browse and touch the TAG LIST tab once.", 0, NULL, 0, 0, NULL,
                        act_calibrate);
    h_tabw = add_row(AZ_ROW_CHOICE, "tab-width", "Tab touch width", "overlay",
                     "How wide a zone around that point counts as the Tag List tab.", 0, C_TABW, 3, 1, NULL, NULL);
    h_otop = add_row(AZ_ROW_CHOICE, "overlay-top", "Overlay position", "overlay",
                     "Distance under the tab bar. Raise it if the overlay covers the tabs.", 0, C_TOP, 4, 1, NULL,
                     NULL);
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
    int h_pick = add_row(AZ_ROW_SCREEN, "pl-pick", "Playlists to watch", NULL,
                         "Pick hip-hop, house... Leave ALL out.", 0, NULL, 0, 0, &PICKER, NULL);
    h_deck = add_row(AZ_ROW_CHOICE, "deck", "Source deck", NULL, "Auto follows the deck that is playing.",
                     0, C_DECK, 5, 0, NULL, NULL);
    h_usb = add_row(AZ_ROW_CHOICE, "usb", "Search in", NULL, "Which USB ports to suggest tracks from.",
                    0, C_USB, 3, 0, NULL, NULL);
    h_diag = add_row(AZ_ROW_TOGGLE, "diagnostics", "Status strip", NULL,
                     "A small strip on Browse: version, library, touches. Turn off once it works.",
                     AZ_ROW_DEFAULT_ON, NULL, 0, 0, NULL, NULL);
    int h_scan = add_row(AZ_ROW_ACTION, "rescan", "Rescan library", NULL,
                         "Reads the USB library again (after adding tracks or playlists).", 0, NULL, 0, 0, NULL,
                         act_rescan);
    return h_overlay >= 0 && h_open >= 0 && h_cal >= 0 && h_tabw >= 0 && h_otop >= 0 && h_bpm >= 0 &&
           h_bpm_rng >= 0 && h_key >= 0 && h_key_mode >= 0 && h_pl >= 0 && h_pick >= 0 && h_deck >= 0 &&
           h_usb >= 0 && h_diag >= 0 && h_scan >= 0;
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
    __atomic_add_fetch(&a_ticks, 1, __ATOMIC_RELAXED);
    if (!banner_done) {
        banner_done = 1;
        C->banner(MOD_ID, "Smart Track " MOD_VERSION " loaded", 3000);
    }

    /* what the touch filter saw: open on the Tag List tab, close on another tab */
    int rq = __atomic_exchange_n(&a_req, 0, __ATOMIC_ACQ_REL);
    int active = __atomic_load_n(&a_active, __ATOMIC_RELAXED);
    if (rq == 1 && !active) open_overlay();
    else if (rq == 2 && active) close_overlay();

    if (now - last_tick_ms < 100) return;
    last_tick_ms = now;

    int en = st_row(h_overlay, 1) == 1;
    int mode = st_row(h_open, 2);                       /* 0 tab, 1 button, 2 both */
    __atomic_store_n(&a_tab_on, en && mode != 1, __ATOMIC_RELAXED);
    __atomic_store_n(&a_hw, TAB_HW[clampi(st_row(h_tabw, 1), 0, 2)], __ATOMIC_RELAXED);
    int chip_on = en && (mode != 0 || __atomic_load_n(&a_tab_x, __ATOMIC_RELAXED) < 0);
    if (chip_on != last_chip) {
        last_chip = chip_on;
        C->surface_show(chip, chip_on);
    }
    if (en != last_cal) {
        last_cal = en;
        C->surface_show(cal, en);
    }
    {
        int cs = __atomic_load_n(&a_calib, __ATOMIC_RELAXED) * 2 + (__atomic_load_n(&a_tab_x, __ATOMIC_RELAXED) >= 0);
        if (cs != last_cal_sig) {
            last_cal_sig = cs;
            C->surface_redraw(cal);
        }
    }
    if (!en && __atomic_load_n(&a_active, __ATOMIC_RELAXED)) close_overlay();

    int hud_on = st_row(h_diag, 1) == 1;
    if (hud_on != last_hud) {
        last_hud = hud_on;
        C->surface_show(hud, hud_on);
    }
    if (hud_on) {
        unsigned hs = (unsigned) (__atomic_load_n(&a_ticks, __ATOMIC_RELAXED) / 30);
        hs = hs * 31u + (unsigned) __atomic_load_n(&g_scan_state, __ATOMIC_RELAXED);
        hs = hs * 31u + (unsigned) __atomic_load_n(&g_ntracks_pub, __ATOMIC_RELAXED);
        hs = hs * 31u + (unsigned) __atomic_load_n(&g_nnames, __ATOMIC_RELAXED);
        hs = hs * 31u + (unsigned) __atomic_load_n(&a_touch_n, __ATOMIC_RELAXED);
        hs = hs * 31u + (unsigned) __atomic_load_n(&a_active, __ATOMIC_RELAXED);
        hs = hs * 31u + (unsigned) a_focus + (unsigned) __atomic_load_n(&a_tab_x, __ATOMIC_RELAXED);
        if (hs != last_hud_sig) {
            last_hud_sig = hs;
            C->surface_redraw(hud);
        }
    }

    if (__atomic_load_n(&a_calib, __ATOMIC_RELAXED) &&
        now - __atomic_load_n(&a_calib_t, __ATOMIC_RELAXED) > 60000)
        __atomic_store_n(&a_calib, 0, __ATOMIC_RELAXED);        /* gave up waiting */
    if (__atomic_exchange_n(&a_calib_done, 0, __ATOMIC_ACQ_REL)) {
        C->cfg_set(MOD_ID, "tab_x", __atomic_load_n(&a_tab_x, __ATOMIC_RELAXED));
        C->cfg_set(MOD_ID, "tab_y", __atomic_load_n(&a_tab_y, __ATOMIC_RELAXED));
        C->banner(MOD_ID, "Tag List tab saved", 2500);
    }
    int pos = st_row(h_otop, 1) * 10000 + __atomic_load_n(&a_tab_y, __ATOMIC_RELAXED) + 1;
    if (pos != last_top) {
        last_top = pos;
        ov_place();
    }
    if (__atomic_exchange_n(&a_want_scan, 0, __ATOMIC_RELAXED) && !s_scanned && !s_nolib) st_request();

    /* a fresh result: repaint the overlay, and the playlist page if it is open */
    int ver = __atomic_load_n(&g_ver, __ATOMIC_ACQUIRE);
    if (ver != seen_ver) {
        seen_ver = ver;
        if (__atomic_load_n(&a_active, __ATOMIC_RELAXED)) C->surface_redraw(ov);
        if (now - __atomic_load_n(&a_pk_ms, __ATOMIC_RELAXED) < 1500) C->request_redraw();
    }

    if (!__atomic_load_n(&a_active, __ATOMIC_RELAXED)) return;
    if (C->view() != AZ_VIEW_BROWSE) { close_overlay(); return; }
    if (!a_focus && C->surface_focus(ov, 1) == 1) {     /* the knob is ours once the core grants it */
        a_focus = 1;
        C->log(MOD_ID, "overlay has the knob");
    }

    unsigned sig = st_sig();
    if (sig != last_sig) {
        last_sig = sig;
        st_request();
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

    /* the Tag List tab's touch point, saved by the calibration */
    if (AZ_HAS(C, cfg_get)) {
        a_tab_x = C->cfg_get(MOD_ID, "tab_x", -1);
        a_tab_y = C->cfg_get(MOD_ID, "tab_y", -1);
        if (a_tab_x < 0 || a_tab_y < 0) a_tab_x = a_tab_y = -1;
    }

    if (!AZ_HAS(C, register_row) || !add_rows())
        C->log(MOD_ID, "Mod Menu rows unavailable: running with default settings");
    if (C->on_deck_event(MOD_ID, on_deck, NULL) != 0) C->log(MOD_ID, "on_deck_event refused");
    if (!AZ_HAS(C, touch_filter) || C->touch_filter(MOD_ID, touch_fn, NULL) != 0)
        C->log(MOD_ID, "touch_filter refused: use the SMART button");

    ov = C->surface_create(MOD_ID, &OVERLAY);
    chip = C->surface_create(MOD_ID, &CHIP);
    hud = C->surface_create(MOD_ID, &HUD);
    cal = C->surface_create(MOD_ID, &CAL);
    if (!ov || !chip || !hud || !cal) {
        C->log(MOD_ID, "surface_create refused");
        return 4;
    }
    C->surface_place(chip, (az_rect) { CHIP_X, CHIP_Y, CHIP_W, CHIP_H });
    C->surface_show(chip, 1);
    C->surface_show(ov, 0);
    C->surface_place(cal, (az_rect) { CAL_X, CAL_Y, CAL_W, CAL_H });
    C->surface_show(cal, 0);                        /* tick shows it */
    C->surface_place(hud, (az_rect) { 0, 0, 800, 52 });
    C->surface_show(hud, 0);                        /* tick shows it when Diagnostics is on */
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
