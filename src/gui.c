/* CurrentPod - MUI-gränssnittet och huvudloopen */
#include "currentpod.h"
#define NO_INLINE_STDARG
#include <libraries/mui.h>
#include <libraries/gadtools.h>
#include <intuition/intuition.h>
#include <devices/timer.h>
#include <proto/muimaster.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/graphics.h>
#include <proto/diskfont.h>
#include <graphics/text.h>
#include <clib/alib_protos.h>
#include <dos/dostags.h>
#include <stdlib.h>
#include <stdarg.h>

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG)(a)<<24|(ULONG)(b)<<16|(ULONG)(c)<<8|(ULONG)(d))
#endif
static ULONG xget(Object *obj, ULONG attr) { ULONG v = 0; GetAttr(attr, obj, &v); return v; }

/* MUI:s inline-variant av MUI_NewObject tar adressen till ett varargs-argument,
   vilket inte är säkert med gcc. Vi bygger taglistan som en array i stället,
   och loggar klassnamnet i felsökningsläge. */
static Object *cp_newobject(CONST_STRPTR cl, ULONG *tags)
{
    Object *o;
    LONG n = 0;
    while (tags[n * 2] != TAG_DONE && n < 400) n++;
    o = MUI_NewObjectA(cl, (struct TagItem *)tags);
    if (cp_debug) log_msg("  %s (%ld taggar) -> %lx", cl, n, (ULONG)o);
    return o;
}
Object *MUI_NewObject(CONST_STRPTR cl, Tag tags, ...) __attribute__((noinline));
Object *MUI_NewObject(CONST_STRPTR cl, Tag tags, ...)
{
    return cp_newobject(cl, (ULONG *)&tags);
}
#define MUI_MakeObject(type, tags...) ({ULONG _tags[] = {tags}; MUI_MakeObjectA((type), (ULONG *)_tags);})
#define MUI_Request(app, win, flags, title, gadgets, format, tags...) ({ULONG _tags[] = {tags}; MUI_RequestA((app), (win), (flags), (title), (gadgets), (format), (APTR)_tags);})
char start_url[URLMAX], start_file[PATHMAX];
int autoplay_test;            /* testkrok: hämta och spela första avsnittet i första podden */
int autoconv_test;            /* testkrok: konvertera det hämtade avsnittet och spela 8SVX-filen */
int autoplay_index = 1;       /* testkrok: vilket avsnitt i listan (1 = nyast) */
static struct Episode *autoplay_ep;

struct Library *MUIMasterBase;

enum {
    ID_QUIT = 1, ID_ABOUT, ID_SEARCH_OPEN, ID_URL_OPEN, ID_UPDATE, ID_UPDATE_ALL, ID_REMOVE_FEED,
    ID_PLAY, ID_PAUSE, ID_STOP, ID_PREV, ID_NEXT, ID_SKIPF, ID_SKIPB, ID_DOWNLOAD, ID_DOWNLOAD_NEW,
    ID_MARK, ID_DELETE_FILE, ID_FEED_SELECT, ID_EP_DOUBLE, ID_SEARCH_GO, ID_SEARCH_SUB, ID_URL_OK,
    ID_VOLUME, ID_INFO, ID_FEEDINFO, ID_SAVEPREFS, ID_CANCEL_DL, ID_OPEN_LINK,
    ID_Q_AUTO, ID_Q_LOW, ID_Q_MEDIUM, ID_Q_HIGH, ID_Q_FULL, ID_BITS14, ID_AUTONEXT, ID_SEARCH_DOUBLE,
    ID_CONVERT, ID_CANCEL_CONV, ID_EP_SELECT, ID_CV_LOW, ID_CV_MEDIUM, ID_DELETE_MP3
};

static Object *app, *win, *feedlist, *eplist, *b_prev, *b_play, *b_pause, *b_next, *b_skipb, *b_skipf;
static Object *t_title, *t_time, *gauge, *vol, *status, *t_info;
static Object *b_dl, *b_playep, *b_conv;      /* knappraden under avsnittslistan */
static struct MsgPort *convport;
static struct Episode *conv_ep;               /* konverteras just nu */
static int conv_prev_state;
static Object *swin, *s_term, *s_country, *s_list, *s_sub, *s_status, *s_close;
static Object *uwin, *u_str, *u_ok, *u_cancel;
static Object *iwin, *i_text, *i_title, *i_close;
static Object *menustrip;

static struct MsgPort *jobport, *playport, *timerport;
static struct timerequest *timerio;
static int timer_open;

static struct Feed *cur_feed;         /* vald podd */
static struct Episode *play_ep;       /* spelas just nu */
static int play_starting;             /* PC_PLAY skickat, spelaren har inte svarat än (mätningen tar några sekunder) */
static struct Job *cur_job;           /* jobb hos arbetaren */
static struct MinList jobqueue;
static struct SearchHit *hits;
static int nhits;
static int quit_flag;
static int last_state = -1;
static LONG save_tick;

static const char *country_codes[] = { "se", "no", "dk", "fi", "de", "gb", "us", "fr", "nl", "pl", NULL };
static const LONG country_msgs[] = { MSG_COUNTRY_SE, MSG_COUNTRY_NO, MSG_COUNTRY_DK, MSG_COUNTRY_FI, MSG_COUNTRY_DE, MSG_COUNTRY_GB, MSG_COUNTRY_US, MSG_COUNTRY_FR, MSG_COUNTRY_NL, MSG_COUNTRY_PL };

/* ------------------------------------------------------------ listhookar */

static char feedbuf[160];
static char epcol[4][160];

static ULONG feed_display(struct Hook *h, char **array, struct Feed *f)
{
    if (!f) { array[0] = _(MSG_COL_PODCAST); array[1] = _(MSG_COL_NEW); return 0; }
    sprintf(feedbuf, "%s%s%.100s", f->hasnew ? "\33b" : "", f->updating ? "\33i" : "", f->name ? f->name : "?");
    array[0] = feedbuf;
    {
        static char n[16];
        if (f->lasterr) strcpy(n, "!");
        else if (f->unheard) sprintf(n, "%ld", f->unheard);
        else n[0] = 0;
        array[1] = n;
    }
    (void)h;
    return 0;
}

static const char *state_text(struct Episode *e)
{
    static char t[24];
    if (e == play_ep && (pstat.state == PS_PLAYING || pstat.state == PS_PAUSED)) return pstat.state == PS_PAUSED ? _(MSG_ST_PAUSED) : _(MSG_ST_PLAYING);
    switch (e->state) {
        case EP_QUEUED:
            if (cur_job && cur_job->ep == e) {
                if (cur_job->total > 0) { sprintf(t, _(MSG_ST_DOWNLOADING_PCT), (LONG)(((ULONG)cur_job->bytes / 1024UL) * 100UL / ((ULONG)cur_job->total / 1024UL + 1UL))); return t; }
                return _(MSG_ST_DOWNLOADING);
            }
            return _(MSG_ST_QUEUED);
        case EP_CONVERTING: sprintf(t, _(MSG_ST_CONVERTING), cstat.permille / 10); return t;
        case EP_DOWNLOADED: return e->pcm ? "8SVX" : _(MSG_ST_DOWNLOADED);
        case EP_PARTIAL: return e->pcm ? "8SVX" : _(MSG_ST_DOWNLOADED);   /* påbörjat avsnitt: positionen syns när man spelar */
        case EP_HEARD: return e->pcm ? _(MSG_ST_8SVX_HEARD) : (e->file ? _(MSG_ST_HEARD) : _(MSG_ST_HEARD_DELETED));
        case EP_ERROR: return _(MSG_ST_ERROR);
        default: return e->isnew ? _(MSG_ST_NEW) : (e->enc ? "" : _(MSG_ST_NOFILE));
    }
}

static ULONG ep_display(struct Hook *h, char **array, struct Episode *e)
{
    if (!e) { array[0] = _(MSG_COL_STATUS); array[1] = _(MSG_COL_DATE); array[2] = _(MSG_COL_EPISODE); array[3] = _(MSG_COL_LENGTH); return 0; }
    strlcpy_(epcol[0], state_text(e), sizeof(epcol[0]));
    iso_to_display(e->date, epcol[1]);
    {
        char pre[24] = "";
        if (e->season || e->epnum) {
            if (e->season) sprintf(pre, "S%ldE%ld ", e->season, e->epnum);
            else sprintf(pre, "%ld. ", e->epnum);
        }
        if (e->etype == ET_TRAILER) strcat(pre, "[Trailer] ");
        else if (e->etype == ET_BONUS) strcat(pre, "[Bonus] ");
        sprintf(epcol[2], "%s%s%.110s", e->enc ? "" : "\33i", pre, e->title ? e->title : "?");
    }
    if (e->dur) fmt_time(e->dur, epcol[3]);
    else if (e->size) fmt_size(e->size, epcol[3]);
    else epcol[3][0] = 0;
    array[0] = epcol[0]; array[1] = epcol[1]; array[2] = epcol[2]; array[3] = epcol[3];
    (void)h;
    return 0;
}

static char hitcol[3][140];
static ULONG hit_display(struct Hook *h, char **array, struct SearchHit *s)
{
    if (!s) { array[0] = _(MSG_COL_NAME); array[1] = _(MSG_COL_PUBLISHER); array[2] = _(MSG_COL_EPISODE); return 0; }
    sprintf(hitcol[0], "%s%.100s", s->explicit_ ? "[E] " : "", s->name);
    strlcpy_(hitcol[1], s->author, sizeof(hitcol[1]));
    sprintf(hitcol[2], "%ld", s->count);
    array[0] = hitcol[0]; array[1] = hitcol[1]; array[2] = hitcol[2];
    (void)h;
    return 0;
}

static struct Hook feed_hook, ep_hook, hit_hook;
static struct Episode *selected_episode(void);
static void update_buttons(void);
static void send_simple(int cmd, LONG arg);

/* ------------------------------------------------------------ hjälp */

static void set_status(const char *fmt, ...)
{
    static char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    set(status, MUIA_Text_Contents, buf);
}

static void redraw_feed(struct Feed *f)
{
    LONG i;
    for (i = 0; ; i++) {
        struct Feed *x;
        DoMethod(feedlist, MUIM_List_GetEntry, i, &x);
        if (!x) break;
        if (x == f) { DoMethod(feedlist, MUIM_List_Redraw, i); break; }
    }
}

static void redraw_episode(struct Episode *e)
{
    LONG i;
    if (!e || e->feed != cur_feed) return;
    for (i = 0; ; i++) {
        struct Episode *x;
        DoMethod(eplist, MUIM_List_GetEntry, i, &x);
        if (!x) break;
        if (x == e) { DoMethod(eplist, MUIM_List_Redraw, i); break; }
    }
    if (e == selected_episode()) update_buttons();
}

static void fill_episodes(struct Feed *f)
{
    struct Episode *e;
    set(eplist, MUIA_List_Quiet, TRUE);
    DoMethod(eplist, MUIM_List_Clear);
    if (f) {
        if (f->serial) {
            for (e = (struct Episode *)f->episodes.mlh_TailPred; e->node.mln_Pred; e = (struct Episode *)e->node.mln_Pred)
                DoMethod(eplist, MUIM_List_InsertSingle, e, MUIV_List_Insert_Bottom);
        } else {
            for (e = (struct Episode *)f->episodes.mlh_Head; e->node.mln_Succ; e = (struct Episode *)e->node.mln_Succ)
                DoMethod(eplist, MUIM_List_InsertSingle, e, MUIV_List_Insert_Bottom);
        }
    }
    set(eplist, MUIA_List_Quiet, FALSE);
    set(win, MUIA_Window_ActiveObject, eplist);
    if (f && f->nep) set(eplist, MUIA_List_Active, 0);   /* så att knappraden gäller något */
    update_buttons();
}

static void fill_feeds(void)
{
    struct Feed *f;
    set(feedlist, MUIA_List_Quiet, TRUE);
    DoMethod(feedlist, MUIM_List_Clear);
    for (f = (struct Feed *)feeds.mlh_Head; f->node.mln_Succ; f = (struct Feed *)f->node.mln_Succ)
        DoMethod(feedlist, MUIM_List_InsertSingle, f, MUIV_List_Insert_Bottom);
    set(feedlist, MUIA_List_Quiet, FALSE);
}

static struct Episode *selected_episode(void)
{
    struct Episode *e = NULL;
    DoMethod(eplist, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &e);
    return e;
}

static void save_feed_quiet(struct Feed *f)
{
    if (f) db_save_feed(f);
}

/* Hämta / Spela / Konvertera tänds efter det markerade avsnittets tillstånd:
   Hämta tills filen finns, Spela och Konvertera när den finns, Konvertera
   släcks igen när 8SVX-filen är gjord eller en annan konvertering pågår. */
static void update_buttons(void)
{
    struct Episode *e = selected_episode();
    int has_file = e && (e->file || e->pcm) && (e->state == EP_DOWNLOADED || e->state == EP_PARTIAL || e->state == EP_HEARD);
    int can_dl = e && e->enc && !has_file && e->state != EP_QUEUED && e->state != EP_CONVERTING;
    int can_conv = has_file && e->file && !e->pcm && !conv_ep && conv_port;
    if (b_dl) set(b_dl, MUIA_Disabled, !can_dl);
    if (b_playep) set(b_playep, MUIA_Disabled, !has_file);
    if (b_conv) set(b_conv, MUIA_Disabled, !can_conv);
}

/* ------------------------------------------------------------ jobb */

static void job_send_next(void)
{
    struct Job *j;
    if (cur_job) return;
    j = (struct Job *)RemHead((struct List *)&jobqueue);
    if (!j) return;
    cur_job = j;
    j->msg.mn_ReplyPort = jobport;
    worker_submit(j);
    if (j->type == JOB_FEED) {
        struct Feed *f = db_find_id(j->feedid);
        if (f) { f->updating = 1; redraw_feed(f); }
        if (f && f->name && strcmp(f->name, _(MSG_FETCHING_NAME))) set_status(_(MSG_FEED_FETCHING_FOR), f->name);
        else set_status(_(MSG_FEED_FETCHING), j->url);
    } else if (j->type == JOB_SEARCH) set_status(_(MSG_SEARCHING_FOR), j->term);
    else if (j->type == JOB_LOOKUP) set_status(_(MSG_LOOKING_UP), j->term);
    else if (j->type == JOB_DOWNLOAD) set_status(_(MSG_DL_STARTED), j->ep && j->ep->title ? j->ep->title : _(MSG_EPISODE_WORD));
}

static struct Job *job_new(int type)
{
    struct Job *j = xalloc(sizeof(*j));
    if (!j) return NULL;
    j->type = type;
    j->maxep = cfg.maxep;
    return j;
}

static void job_queue(struct Job *j)
{
    AddTail((struct List *)&jobqueue, (struct Node *)j);
    job_send_next();
}

static void queue_feed_update(struct Feed *f)
{
    struct Job *j;
    if (!f || f->updating) return;
    j = job_new(JOB_FEED);
    if (!j) return;
    j->feedid = f->id;
    strlcpy_(j->url, f->url, sizeof(j->url));
    if (f->etag) strlcpy_(j->etag, f->etag, sizeof(j->etag));
    if (f->modified) strlcpy_(j->modified, f->modified, sizeof(j->modified));
    f->updating = 1;
    redraw_feed(f);
    job_queue(j);
}

static void subscribe_url(const char *url)
{
    struct Feed *f;
    char u[URLMAX];
    strlcpy_(u, url, sizeof(u));
    str_trim(u);
    if (!u[0]) return;
    /* Apple-länkar: slå upp ID:t */
    if (stristr_(u, "apple.com") || strnieq(u, "itms", 4)) {
        char *id = stristr_(u, "/id");
        if (!id) id = stristr_(u, "id=");
        if (id) {
            struct Job *j = job_new(JOB_LOOKUP);
            id += (id[0] == '/') ? 3 : 3;
            if (j) {
                LONG i = 0;
                while (id[i] >= '0' && id[i] <= '9' && i < 30) { j->term[i] = id[i]; i++; }
                j->term[i] = 0;
                if (i) { job_queue(j); return; }
                xfree(j);
            }
        }
        set_status(_(MSG_NO_ID_IN_LINK));
        return;
    }
    if (strnieq(u, "podcast://", 10) || strnieq(u, "pcast://", 8) || strnieq(u, "feed://", 7)) {
        char *rest = strstr(u, "://") + 3;
        char tmp[URLMAX];
        if (strnieq(rest, "http", 4)) strlcpy_(tmp, rest, sizeof(tmp));
        else sprintf(tmp, "https://%s", rest);
        strlcpy_(u, tmp, sizeof(u));
    }
    if (!strstr(u, "://")) { char tmp[URLMAX]; sprintf(tmp, "https://%s", u); strlcpy_(u, tmp, sizeof(u)); }
    if ((f = db_find_url(u))) { set_status(_(MSG_ALREADY_SUBSCRIBED), f->name); return; }
    f = db_add_feed(u, NULL);
    if (!f) return;
    DoMethod(feedlist, MUIM_List_InsertSingle, f, MUIV_List_Insert_Bottom);
    set(feedlist, MUIA_List_Active, MUIV_List_Active_Bottom);
    queue_feed_update(f);
}

static void start_download(struct Episode *e)
{
    struct Job *j;
    char dir[PATHMAX], base[40], fname[64];
    if (!e || !e->enc) return;
    if (e->state == EP_QUEUED) return;
    if (e->file && e->state == EP_DOWNLOADED) return;
    safe_filename(e->feed->name ? e->feed->name : _(MSG_PODCAST_WORD), base, 28);
    add_part(dir, cfg.dldir, base, sizeof(dir));
    make_dirs(dir);
    safe_filename(e->title ? e->title : _(MSG_EPISODE_WORD), base, 13);   /* 10 + 1 + 13 + ".part" = 29 <= FFS 30 */
    {
        const char *ext = ".mp3";
        if (e->type && stristr_(e->type, "m4a")) ext = ".m4a";
        else if (e->type && stristr_(e->type, "mp4")) ext = ".m4a";
        else if (e->type && stristr_(e->type, "ogg")) ext = ".ogg";
        sprintf(fname, "%.10s %s%s", e->date ? e->date : "0000-00-00", base, ext);
    }
    j = job_new(JOB_DOWNLOAD);
    if (!j) return;
    j->ep = e;
    j->feedid = e->feed->id;
    strlcpy_(j->url, e->enc, sizeof(j->url));
    add_part(j->file, dir, fname, sizeof(j->file));
    xstrset(&e->file, j->file);
    e->state = EP_QUEUED;
    redraw_episode(e);
    redraw_feed(e->feed);
    job_queue(j);
}

static void update_player_panel(void);

static void play_episode(struct Episode *e)
{
    struct PlayerCmd *c;
    const char *path;
    if (!e) return;
    if (e->state == EP_CONVERTING) { set_status(_(MSG_CONVERTING_WAIT)); return; }
    if (e->pcm && !file_exists(e->pcm)) { xfree(e->pcm); e->pcm = NULL; }
    if (e->file && !file_exists(e->file)) { xfree(e->file); e->file = NULL; }
    /* finns en konverterad fil spelas den: ingen avkodning alls */
    path = e->pcm ? e->pcm : e->file;
    if (!path) {
        if (e->state == EP_DOWNLOADED || e->state == EP_PARTIAL) e->state = EP_UNHEARD;
        redraw_episode(e);
        set_status(_(MSG_DOWNLOAD_FIRST));
        return;
    }
    if (!e->pcm && e->type && (stristr_(e->type, "m4a") || stristr_(e->type, "mp4") || stristr_(e->type, "video"))) {
        set_status(_(MSG_NOT_MP3));
        return;
    }
    c = xalloc(sizeof(*c));
    if (!c) return;
    c->cmd = PC_PLAY;
    strlcpy_(c->file, path, sizeof(c->file));
    c->arg = (e->pos > 3) ? e->pos - 3 : 0;
    c->msg.mn_ReplyPort = playport;
    if (play_ep && play_ep != e) { struct Episode *old = play_ep; play_ep = NULL; redraw_episode(old); }
    play_ep = e;
    pstat.state = PS_STOPPED;
    play_starting = 1;
    player_send(c);
    set_status(_(MSG_STARTING), e->title);
    redraw_episode(e);
}

/* >, Spela, Retur, dubbelklick och menyn går alla hit: fortsätt om det markerade
   avsnittet står på paus, annars spela det. Under starten (mätningen tar några
   sekunder) ignoreras nya tryck; MUI ger dessutom både dubbelklick och Retur-
   händelse för ett Retur i listan, så utan spärren blev det två starter. */
static void play_or_resume(struct Episode *e)
{
    log_msg("gui: play_or_resume, startar=%ld tillstånd=%ld", (LONG)play_starting, (LONG)pstat.state);
    if (play_starting) return;
    if (pstat.state == PS_PAUSED && play_ep && (!e || e == play_ep)) { play_starting = 1; send_simple(PC_RESUME, 0); return; }
    if (e) play_episode(e);
}

/* ------------------------------------------------------------ konvertering till 8SVX */

static void start_conversion(struct Episode *e)
{
    struct ConvCmd *c;
    if (!e || !e->file) return;
    if (!conv_port) { set_status(_(MSG_CONVERTER_MISSING)); return; }
    if (conv_ep) { set_status(_(MSG_CONVERSION_BUSY), conv_ep->title); return; }
    if (e->pcm) { set_status(_(MSG_ALREADY_CONVERTED)); return; }
    if (e->state == EP_QUEUED) return;
    c = xalloc(sizeof(*c));
    if (!c) return;
    c->cmd = CC_CONVERT;
    c->quality = (cfg.convquality == Q_MEDIUM) ? Q_MEDIUM : Q_LOW;
    strlcpy_(c->file, e->file, sizeof(c->file));
    strlcpy_(c->out, e->file, sizeof(c->out));
    { char *dot = strrchr(c->out, '.'); if (dot && !strchr(dot, '/') && !strchr(dot, ':')) *dot = 0; }
    strlcat_(c->out, ".8svx", sizeof(c->out));
    c->msg.mn_ReplyPort = convport;
    conv_ep = e; conv_prev_state = e->state;
    e->state = EP_CONVERTING;
    conv_send(c);
    set_status(_(MSG_CONVERTING_TO), e->title);
    redraw_episode(e);
    update_buttons();
}

static void conv_done(struct ConvCmd *c)
{
    struct Episode *e = conv_ep;
    conv_ep = NULL;
    if (e) {
        e->state = conv_prev_state;
        if (c->error) set_status(_(MSG_CONVERSION_FAILED), c->errstr);
        else { xstrset(&e->pcm, c->out); set_status(_(MSG_CONVERTED), e->title); }
        save_feed_quiet(e->feed);
        redraw_episode(e);
        if (autoconv_test && e == autoplay_ep && !c->error) { log_msg("autoplay: spelar 8SVX %s", e->pcm); play_episode(e); }
    }
    update_buttons();
    xfree(c);
}

static void send_simple(int cmd, LONG arg)
{
    struct PlayerCmd *c = xalloc(sizeof(*c));
    if (!c) return;
    c->cmd = cmd; c->arg = arg;
    c->msg.mn_ReplyPort = playport;
    player_send(c);
}

static void set_quality_cmd(void)
{
    struct PlayerCmd *c = xalloc(sizeof(*c));
    if (!c) return;
    c->cmd = PC_SETQUALITY; c->arg = (cfg.quality & 0xFF) | (cfg.bits14 ? 0x100 : 0);
    c->msg.mn_ReplyPort = playport;
    player_send(c);
}

static struct Episode *next_episode(struct Episode *from, int dir)
{
    LONG i, n, start = -1;
    struct Episode *x;
    for (i = 0; ; i++) {
        DoMethod(eplist, MUIM_List_GetEntry, i, &x);
        if (!x) break;
        if (x == from) start = i;
    }
    n = i;
    if (start < 0) return NULL;
    for (i = start + dir; i >= 0 && i < n; i += dir) {
        DoMethod(eplist, MUIM_List_GetEntry, i, &x);
        if (x && x->file && (x->state == EP_DOWNLOADED || x->state == EP_PARTIAL || (x->state == EP_HEARD && dir < 0))) return x;
    }
    return NULL;
}

static void episode_finished(void)
{
    struct Episode *e = play_ep;
    if (!e) return;
    e->state = EP_HEARD;
    e->pos = 0;
    db_recount(e->feed);
    save_feed_quiet(e->feed);
    redraw_episode(e);
    redraw_feed(e->feed);
    play_ep = NULL;
    if (cfg.autonext) {
        struct Episode *n = next_episode(e, +1);
        if (n) { play_episode(n); return; }
    }
    update_player_panel();
}

/* ------------------------------------------------------------ svar från arbetaren */

static void job_done(struct Job *j)
{
    if (j->type == JOB_FEED) {
        struct Feed *f = db_find_id(j->feedid);
        if (f) {
            f->updating = 0;
            if (j->error) {
                xstrset(&f->lasterr, j->errstr);
                set_status("%.50s: %s", f->name, j->errstr);
            } else if (j->notmodified) {
                xfree(f->lasterr); f->lasterr = NULL;
                set_status(_(MSG_FEED_NO_CHANGES), f->name);
            } else if (j->feed) {
                int added = db_merge(f, j->feed, cfg.maxep);
                xstrset(&f->etag, j->etag[0] ? j->etag : NULL);
                xstrset(&f->modified, j->modified[0] ? j->modified : NULL);
                xfree(f->lasterr); f->lasterr = NULL;
                db_free_feed(j->feed);
                j->feed = NULL;
                {
                    struct DateStamp ds; struct DateTime dt; char d[LEN_DATSTRING], t[LEN_DATSTRING], u[40];
                    DateStamp(&ds);
                    memset(&dt, 0, sizeof(dt));
                    dt.dat_Stamp = ds; dt.dat_Format = FORMAT_DOS; dt.dat_StrDate = (STRPTR)d; dt.dat_StrTime = (STRPTR)t;
                    if (DateToStr(&dt)) { sprintf(u, "%s %s", d, t); xstrset(&f->updated, u); }
                }
                db_save_feed(f);
                db_save_index();
                if (j->truncated) set_status(_(MSG_FEED_NEW_TRUNC), f->name, added, (LONG)f->nep);
                else set_status(_(MSG_FEED_NEW_EPISODES), f->name, added);
                if (autoplay_test && !autoplay_ep) {
                    struct Episode *first = (struct Episode *)f->episodes.mlh_Head;
                    int n = autoplay_index;
                    while (--n > 0 && first->node.mln_Succ) first = (struct Episode *)first->node.mln_Succ;
                    if (first->node.mln_Succ && first->enc) { autoplay_ep = first; log_msg("autoplay: hämtar %s", first->title); start_download(first); }
                }
                if (f->autodl) {
                    struct Episode *e;
                    for (e = (struct Episode *)f->episodes.mlh_Head; e->node.mln_Succ; e = (struct Episode *)e->node.mln_Succ)
                        if (e->isnew && e->enc) start_download(e);
                }
            }
            redraw_feed(f);
            if (f == cur_feed) fill_episodes(f);
        }
        if (j->feed) db_free_feed(j->feed);
    } else if (j->type == JOB_SEARCH) {
        int i;
        if (j->error) { set(s_status, MUIA_Text_Contents, j->errstr); set_status(_(MSG_SEARCH_FAILED), j->errstr); }
        else {
            char t[80];
            set(s_list, MUIA_List_Quiet, TRUE);
            DoMethod(s_list, MUIM_List_Clear);
            xfree(hits); hits = j->hits; nhits = j->nhits; j->hits = NULL;
            for (i = 0; i < nhits; i++) DoMethod(s_list, MUIM_List_InsertSingle, &hits[i], MUIV_List_Insert_Bottom);
            set(s_list, MUIA_List_Quiet, FALSE);
            sprintf(t, _(MSG_HITS_SHORT), nhits);
            set(s_status, MUIA_Text_Contents, t);
            set_status(_(MSG_HITS_STATUS), nhits);
        }
    } else if (j->type == JOB_LOOKUP) {
        if (j->error) set_status(_(MSG_LOOKUP_FAILED), j->errstr);
        else if (j->nhits > 0) {
            struct Feed *f;
            subscribe_url(j->hits[0].feedurl);
            f = db_find_url(j->hits[0].feedurl);
            if (f) { f->apple = j->hits[0].apple; if (!f->name || !strcmp(f->name, _(MSG_FETCHING_NAME))) xstrset(&f->name, j->hits[0].name); }
        } else set_status(_(MSG_APPLE_UNKNOWN_ID));
        xfree(j->hits);
    } else if (j->type == JOB_DOWNLOAD) {
        struct Episode *e = j->ep;
        struct Feed *f = db_find_id(j->feedid);
        /* avsnittet kan ha försvunnit om podden togs bort under tiden */
        if (f) {
            struct Episode *x;
            int found = 0;
            for (x = (struct Episode *)f->episodes.mlh_Head; x->node.mln_Succ; x = (struct Episode *)x->node.mln_Succ)
                if (x == e) found = 1;
            if (!found) e = NULL;
        } else e = NULL;
        if (e) {
            if (j->error) {
                e->state = EP_ERROR;
                set_status(_(MSG_DL_FAILED), j->errstr);
            } else {
                e->state = EP_DOWNLOADED;
                e->size = file_size(e->file);
                if (autoplay_test && e == autoplay_ep) {
                    if (autoconv_test) { log_msg("autoplay: konverterar %s", e->title); start_conversion(e); }
                    else { log_msg("autoplay: spelar %s", e->title); play_episode(e); }
                }
                if (j->etag[0] && !stristr_(j->etag, "mpeg") && !stristr_(j->etag, "mp3") && !stristr_(j->etag, "octet"))
                    xstrset(&e->type, j->etag);
                set_status(_(MSG_DL_DONE), e->title);
            }
            db_recount(f);
            db_save_feed(f);
            redraw_episode(e);
            redraw_feed(f);
        }
    }
    xfree(j);
}

/* ------------------------------------------------------------ spelarpanel */

static void update_player_panel(void)
{
    char t[160], d[32];   /* infoRaden med "HINNER INTE MED" är 65 tecken: 64 räckte inte (stacken förstördes) */
    LONG dur = pstat.dur;
    if (play_ep && pstat.state != PS_STOPPED && pstat.state != PS_ERROR) {
        if (!dur && play_ep->dur) dur = play_ep->dur;
        fmt_time(pstat.pos, t);
        fmt_time(dur, d);
        strcat(t, " / "); strcat(t, d);
        set(t_time, MUIA_Text_Contents, t);
        set(t_title, MUIA_Text_Contents, play_ep->title);
        set(gauge, MUIA_Gauge_Current, dur ? (pstat.pos * 1000) / dur : 0);
        if (pstat.freq) {
            {
                const char *qn[5];
                qn[0] = _(MSG_Q_AUTO_SHORT); qn[1] = _(MSG_Q_LOW_SHORT); qn[2] = _(MSG_Q_MEDIUM_SHORT); qn[3] = _(MSG_Q_HIGH_SHORT); qn[4] = _(MSG_Q_FULL_SHORT);
                if (pstat.pcm)
                    sprintf(t, _(MSG_INFO_8SVX), pstat.freq, pstat.state == PS_PAUSED ? _(MSG_PAUSE_SUFFIX) : "");
                else
                sprintf(t, _(MSG_INFO_MP3), pstat.bitrate, pstat.freq, pstat.channels == 2 ? _(MSG_STEREO) : _(MSG_MONO),
                        pstat.bits, qn[pstat.quality >= 0 && pstat.quality <= 4 ? pstat.quality : 0],
                        pstat.state == PS_PAUSED ? _(MSG_PAUSE_SUFFIX) : "", pstat.cpu > 1500 ? _(MSG_LAG_SUFFIX) : "");
            }
            set(t_info, MUIA_Text_Contents, t);
        }
    } else {
        set(t_time, MUIA_Text_Contents, _(MSG_TIME_ZERO));
        set(t_title, MUIA_Text_Contents, pstat.state == PS_ERROR ? pstat.errstr : _(MSG_NOTHING_PLAYING));
        set(gauge, MUIA_Gauge_Current, 0);
        set(t_info, MUIA_Text_Contents, pstat.libname[0] ? pstat.libname : "");
    }
}

static void timer_tick(void)
{
    static int ticks;
    if (++ticks == 1 || (ticks % 30) == 0 || cp_debug > 1) log_msg("gui: tick %ld, spelartillstånd %ld pos %ld", (LONG)ticks, (LONG)pstat.state, pstat.pos);
    if (pstat.state != last_state) {
        if (pstat.state == PS_FINISHED) episode_finished();
        else if (pstat.state == PS_ERROR) { set_status(_(MSG_PLAYER_ERR), pstat.errstr); if (play_ep) { struct Episode *e = play_ep; play_ep = NULL; redraw_episode(e); } }
        else if (play_ep) redraw_episode(play_ep);
        last_state = pstat.state;
    }
    if (play_ep && (pstat.state == PS_PLAYING || pstat.state == PS_PAUSED)) {
        if (pstat.pos != play_ep->pos) {
            play_ep->pos = pstat.pos;
            if (play_ep->state == EP_DOWNLOADED || play_ep->state == EP_HEARD) play_ep->state = EP_PARTIAL;
        }
        if (++save_tick >= 15) { save_tick = 0; if (cp_debug > 1) log_msg("gui: sparar position"); save_feed_quiet(play_ep->feed); if (cp_debug > 1) log_msg("gui: sparat"); }
    }
    update_player_panel();
    if (cur_job && cur_job->type == JOB_FEED && cur_job->bytes > 0) {
        struct Feed *f = db_find_id(cur_job->feedid);
        char a[24];
        fmt_size(cur_job->bytes, a);
        set_status(_(MSG_FEED_PROGRESS), f && f->name && strcmp(f->name, _(MSG_FETCHING_NAME)) ? _(MSG_FOR_WORD) : "",
                   f && f->name && strcmp(f->name, _(MSG_FETCHING_NAME)) ? f->name : "", a, cur_job->total ? "" : "...");
    }
    if (cur_job && cur_job->type == JOB_DOWNLOAD) {
        char a[24], b[24];
        fmt_size(cur_job->bytes, a);
        if (cur_job->total > 0) { fmt_size(cur_job->total, b); set_status(_(MSG_DL_PROGRESS_TOTAL), a, b, cur_job->ep->title); }
        else set_status(_(MSG_DL_PROGRESS), a, cur_job->ep->title);
        if ((ticks % 3) == 0) redraw_episode(cur_job->ep);
    }
    if (conv_ep) {
        char t[24];
        fmt_time(cstat.done_ms / 1000, t);
        if (!(cur_job && cur_job->type == JOB_DOWNLOAD))
            set_status(_(MSG_CONVERTING_PCT), cstat.permille / 10, t, conv_ep->title);
        if ((ticks % 3) == 0) redraw_episode(conv_ep);
    }
}

/* ------------------------------------------------------------ fönster */

static Object *make_menu(void)
{
    /* etiketterna fylls i ur katalogen (nm_msgs, samma ordning); kortkommandon och ID:n här */
    static struct NewMenu nm[] = {
        { NM_TITLE, NULL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, "?", 0, 0, (APTR)ID_ABOUT },
        { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, 0, 0, 0, (APTR)ID_SAVEPREFS },
        { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, "Q", 0, 0, (APTR)ID_QUIT },
        { NM_TITLE, NULL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, "F", 0, 0, (APTR)ID_SEARCH_OPEN },
        { NM_ITEM, NULL, "S", 0, 0, (APTR)ID_URL_OPEN },
        { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, "U", 0, 0, (APTR)ID_UPDATE },
        { NM_ITEM, NULL, "A", 0, 0, (APTR)ID_UPDATE_ALL },
        { NM_ITEM, NULL, "I", 0, 0, (APTR)ID_FEEDINFO },
        { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, 0, 0, 0, (APTR)ID_REMOVE_FEED },
        { NM_TITLE, NULL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, "P", 0, 0, (APTR)ID_PLAY },
        { NM_ITEM, NULL, 0, 0, 0, (APTR)ID_PAUSE },
        { NM_ITEM, NULL, 0, 0, 0, (APTR)ID_STOP },
        { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, "D", 0, 0, (APTR)ID_DOWNLOAD },
        { NM_ITEM, NULL, 0, 0, 0, (APTR)ID_DOWNLOAD_NEW },
        { NM_ITEM, NULL, 0, 0, 0, (APTR)ID_CANCEL_DL },
        { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, "K", 0, 0, (APTR)ID_CONVERT },
        { NM_ITEM, NULL, 0, 0, 0, (APTR)ID_CANCEL_CONV },
        { NM_ITEM, NULL, 0, 0, 0, (APTR)ID_DELETE_MP3 },
        { NM_ITEM, NM_BARLABEL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, "M", 0, 0, (APTR)ID_MARK },
        { NM_ITEM, NULL, 0, 0, 0, (APTR)ID_DELETE_FILE },
        { NM_ITEM, NULL, "E", 0, 0, (APTR)ID_INFO },
        { NM_ITEM, NULL, 0, 0, 0, (APTR)ID_OPEN_LINK },
        { NM_TITLE, NULL, 0, 0, 0, 0 },
        { NM_ITEM, NULL, 0, 0, 0, 0 },
        { NM_SUB, NULL, 0, CHECKIT, ~1, (APTR)ID_Q_AUTO },
        { NM_SUB, NULL, 0, CHECKIT, ~2, (APTR)ID_Q_LOW },
        { NM_SUB, NULL, 0, CHECKIT, ~4, (APTR)ID_Q_MEDIUM },
        { NM_SUB, NULL, 0, CHECKIT, ~8, (APTR)ID_Q_HIGH },
        { NM_SUB, NULL, 0, CHECKIT, ~16, (APTR)ID_Q_FULL },
        { NM_ITEM, NULL, 0, 0, 0, 0 },
        { NM_SUB, NULL, 0, CHECKIT, ~1, (APTR)ID_CV_LOW },
        { NM_SUB, NULL, 0, CHECKIT, ~2, (APTR)ID_CV_MEDIUM },
        { NM_ITEM, NULL, 0, CHECKIT | MENUTOGGLE, 0, (APTR)ID_BITS14 },
        { NM_ITEM, NULL, 0, CHECKIT | MENUTOGGLE, 0, (APTR)ID_AUTONEXT },
        { NM_END, NULL, 0, 0, 0, 0 }
    };
    static const LONG nm_msgs[] = {
        MSG_MENU_PROJECT, MSG_MENU_ABOUT, -1, MSG_MENU_SAVE_SETTINGS, -1, MSG_MENU_QUIT,
        MSG_MENU_PODCAST, MSG_MENU_SEARCH, MSG_MENU_SUBSCRIBE_URL, -1, MSG_MENU_UPDATE, MSG_MENU_UPDATE_ALL, MSG_MENU_INFORMATION, -1, MSG_MENU_REMOVE_FEED,
        MSG_MENU_EPISODE, MSG_MENU_PLAY, MSG_MENU_PAUSE, MSG_MENU_STOP, -1, MSG_MENU_DOWNLOAD, MSG_MENU_DOWNLOAD_NEW, MSG_MENU_CANCEL_DL, -1,
        MSG_MENU_CONVERT, MSG_MENU_CANCEL_CONV, MSG_MENU_DELETE_MP3, -1, MSG_MENU_MARK, MSG_MENU_DELETE_FILE, MSG_MENU_INFORMATION, MSG_MENU_OPEN_LINK,
        MSG_MENU_SETTINGS, MSG_MENU_QUALITY, MSG_MENU_Q_AUTO, MSG_MENU_Q_LOW, MSG_MENU_Q_MEDIUM, MSG_MENU_Q_HIGH, MSG_MENU_Q_FULL,
        MSG_MENU_CONVERSION, MSG_MENU_CV_LOW, MSG_MENU_CV_MEDIUM, MSG_MENU_BITS14, MSG_MENU_AUTONEXT
    };
    int i;
    for (i = 0; nm[i].nm_Type != NM_END; i++) {
        if (nm_msgs[i] >= 0) nm[i].nm_Label = (STRPTR)_(nm_msgs[i]);
        if (nm[i].nm_UserData == (APTR)(ID_Q_AUTO + cfg.quality)) nm[i].nm_Flags |= CHECKED;
        if (nm[i].nm_UserData == (APTR)ID_BITS14 && cfg.bits14) nm[i].nm_Flags |= CHECKED;
        if (nm[i].nm_UserData == (APTR)ID_CV_LOW && cfg.convquality != Q_MEDIUM) nm[i].nm_Flags |= CHECKED;
        if (nm[i].nm_UserData == (APTR)ID_CV_MEDIUM && cfg.convquality == Q_MEDIUM) nm[i].nm_Flags |= CHECKED;
        if (nm[i].nm_UserData == (APTR)ID_AUTONEXT && cfg.autonext) nm[i].nm_Flags |= CHECKED;
    }
    return MUI_MakeObject(MUIO_MenustripNM, nm, 0);
}

/* Fönstren får helvetica 11. Utan MUI-inställningar ritar MUI allt med
 * topaz 8, utom i fönster som är för breda för skärmen (CLAUDE.md, fälla 11).
 * Texter och etiketter ärver fonten från rotgruppen; listor, knappar och
 * cykelknappar har egna fonter och får den var för sig. Saknas fonten blir
 * det som förut. */
struct Library *DiskfontBase;
static struct TextFont *ui_font;
static struct TextAttr ui_ta = { (STRPTR)"helvetica.font", 11, 0, 0 };
#define FONT_TAG (ui_font ? MUIA_Font : TAG_IGNORE), (ULONG)ui_font

static void font_open(void)
{
    if ((DiskfontBase = OpenLibrary((STRPTR)"diskfont.library", 36))) ui_font = OpenDiskFont(&ui_ta);
    log_msg("gui: helvetica 11 %s", ui_font ? "öppnad" : "saknas, MUI:s font används");
}

static void font_close(void)
{
    if (ui_font) { CloseFont(ui_font); ui_font = NULL; }
    if (DiskfontBase) { CloseLibrary(DiskfontBase); DiskfontBase = NULL; }
}

/* som SimpleButton, men med fönstrens font */
static Object *Button(const char *label)
{
    return TextObject, ButtonFrame, FONT_TAG,
        MUIA_Text_Contents, label,
        MUIA_Text_PreParse, "\33c",
        MUIA_InputMode, MUIV_InputMode_RelVerify,
        MUIA_Background, MUII_ButtonBack,
        MUIA_CycleChain, 1,
    End;
}

static Object *create_windows(void)
{
    static const char *countries[12];
    int i, ci = 0;
    for (i = 0; country_codes[i]; i++) { countries[i] = _(country_msgs[i]); if (strieq(country_codes[i], cfg.country)) ci = i; }
    countries[i] = NULL;

    feed_hook.h_Entry = (HOOKFUNC)HookEntry; feed_hook.h_SubEntry = (HOOKFUNC)feed_display;
    ep_hook.h_Entry = (HOOKFUNC)HookEntry; ep_hook.h_SubEntry = (HOOKFUNC)ep_display;
    hit_hook.h_Entry = (HOOKFUNC)HookEntry; hit_hook.h_SubEntry = (HOOKFUNC)hit_display;

    log_msg("gui: hookar satta");
    menustrip = make_menu();
    log_msg("gui: meny %lx", (ULONG)menustrip);
    {
    Object *lists, *player;
    lists = HGroup,
                    Child, VGroup, MUIA_Weight, 26,
                        Child, feedlist = ListviewObject,
                            MUIA_CycleChain, 1,
                            MUIA_Listview_List, ListObject,
                                InputListFrame, FONT_TAG,
                                MUIA_List_Format, "MIW=20 MAW=-1 BAR,MIW=3 P=\33r",
                                MUIA_List_DisplayHook, &feed_hook,
                                MUIA_List_Title, TRUE,
                            End,
                        End,
                    End,
                    Child, BalanceObject, End,
                    Child, VGroup, MUIA_Weight, 74,
                        Child, eplist = ListviewObject,
                            MUIA_CycleChain, 1,
                            MUIA_Listview_List, ListObject,
                                InputListFrame, FONT_TAG,
                                MUIA_List_Format, "MIW=10 BAR,MIW=14,MIW=30 W=150 BAR,MIW=10 P=\33r",
                                MUIA_List_DisplayHook, &ep_hook,
                                MUIA_List_Title, TRUE,
                            End,
                        End,
                        Child, HGroup,
                            Child, b_dl = Button(_(MSG_BTN_DOWNLOAD)),
                            Child, b_playep = Button(_(MSG_BTN_PLAY)),
                            Child, b_conv = Button(_(MSG_BTN_CONVERT)),
                        End,
                    End,
                End;
    log_msg("gui: listor %lx", (ULONG)lists);
    player = VGroup, GroupFrameT(_(MSG_GROUP_PLAYER)),
                    Child, HGroup,
                        Child, b_prev = Button("|<"),
                        Child, b_skipb = Button("<<"),
                        Child, b_play = Button(" > "),
                        Child, b_pause = Button("||"),
                        Child, b_skipf = Button(">>"),
                        Child, b_next = Button(">|"),
                        Child, t_title = TextObject, TextFrame, MUIA_Background, MUII_TextBack, MUIA_Text_Contents, _(MSG_NOTHING_PLAYING), MUIA_Weight, 200, End,
                        Child, t_time = TextObject, TextFrame, MUIA_Background, MUII_TextBack, MUIA_Text_Contents, _(MSG_TIME_ZERO), MUIA_Text_PreParse, "\33r", MUIA_FixWidthTxt, "0:00:00 / 0:00:00", End,
                    End,
                    Child, HGroup,
                        Child, gauge = GaugeObject, GaugeFrame, MUIA_Gauge_Horiz, TRUE, MUIA_Gauge_Max, 1000, MUIA_Gauge_Current, 0, MUIA_Weight, 300, End,
                        Child, Label(_(MSG_LABEL_VOL)),
                        Child, vol = SliderObject, MUIA_Numeric_Min, 0, MUIA_Numeric_Max, 64, MUIA_Numeric_Value, cfg.volume, MUIA_Slider_Quiet, TRUE, MUIA_Weight, 120, End,
                    End,
                    Child, t_info = TextObject, MUIA_Text_Contents, "", MUIA_Text_PreParse, "\33c", MUIA_Font, MUIV_Font_Tiny, End,
                End;
    log_msg("gui: spelarpanel %lx", (ULONG)player);
    status = TextObject, TextFrame, MUIA_Background, MUII_TextBack, MUIA_Text_Contents, "", End;
    log_msg("gui: status %lx", (ULONG)status);
    if (!lists || !player || !status) return NULL;
    win = WindowObject,
            MUIA_Window_Title, "CurrentPod " CP_VERSION,
            MUIA_Window_ID, MAKE_ID('C','P','D','1'),
            MUIA_Window_Width, MUIV_Window_Width_Screen(96),    /* avsnittstitlarna behöver bredd */
            MUIA_Window_Height, MUIV_Window_Height_Screen(80),
            WindowContents, VGroup, FONT_TAG,
                Child, lists,
                Child, player,
                Child, status,
            End,
        End;
    if (!win) {
        Object *t;
        t = WindowObject, MUIA_Window_Title, "t1", WindowContents, VGroup, Child, status, End, End;
        log_msg("gui: test status-fönster %lx", (ULONG)t); if (t) MUI_DisposeObject(t);
        t = WindowObject, MUIA_Window_Title, "t2", WindowContents, VGroup, Child, player, End, End;
        log_msg("gui: test spelar-fönster %lx", (ULONG)t); if (t) MUI_DisposeObject(t);
        t = WindowObject, MUIA_Window_Title, "t3", WindowContents, VGroup, Child, lists, End, End;
        log_msg("gui: test list-fönster %lx", (ULONG)t); if (t) MUI_DisposeObject(t);
        t = WindowObject, MUIA_Window_Title, "t4", WindowContents, VGroup, Child, TextObject, MUIA_Text_Contents, "x", End, End, End;
        log_msg("gui: test enkelt fönster %lx", (ULONG)t); if (t) MUI_DisposeObject(t);
        t = WindowObject, MUIA_Window_Title, "t5", MUIA_Window_ID, MAKE_ID('C','P','D','9'), WindowContents, VGroup, Child, TextObject, MUIA_Text_Contents, "x", End, End, End;
        log_msg("gui: test id-fönster %lx", (ULONG)t); if (t) MUI_DisposeObject(t);
        return NULL;
    }
    }
    log_msg("gui: huvudfönster %lx", (ULONG)win);
    swin = WindowObject,
            MUIA_Window_Title, _(MSG_WIN_SEARCH),
            MUIA_Window_ID, MAKE_ID('C','P','D','2'),
            WindowContents, VGroup, FONT_TAG,
                Child, HGroup,
                    Child, Label2(_(MSG_LABEL_SEARCH)),
                    Child, s_term = StringObject, StringFrame, MUIA_String_MaxLen, 100, MUIA_CycleChain, 1, End,
                    Child, Label2(_(MSG_LABEL_COUNTRY)),
                    Child, s_country = CycleObject, FONT_TAG, MUIA_Cycle_Entries, countries, MUIA_Cycle_Active, ci, End,
                End,
                Child, s_list = ListviewObject,
                    MUIA_CycleChain, 1,
                    MUIA_Listview_List, ListObject,
                        InputListFrame, FONT_TAG,
                        MUIA_List_Format, "MIW=20 MAW=-1 BAR,MIW=10 BAR,MIW=5 P=\33r",
                        MUIA_List_DisplayHook, &hit_hook,
                        MUIA_List_Title, TRUE,
                    End,
                End,
                Child, s_status = TextObject, MUIA_Text_Contents, _(MSG_SEARCH_HINT), End,
                Child, HGroup,
                    Child, s_sub = Button(_(MSG_BTN_SUBSCRIBE)),
                    Child, s_close = Button(_(MSG_BTN_CLOSE)),
                End,
            End,
        End;
    log_msg("gui: sökfönster %lx", (ULONG)swin);
    uwin = WindowObject,
            MUIA_Window_Title, _(MSG_WIN_URL),
            MUIA_Window_ID, MAKE_ID('C','P','D','3'),
            WindowContents, VGroup, FONT_TAG,
                Child, TextObject, MUIA_Text_Contents, _(MSG_URL_PROMPT), End,
                Child, u_str = StringObject, StringFrame, MUIA_String_MaxLen, URLMAX - 1, MUIA_CycleChain, 1, End,
                Child, HGroup,
                    Child, u_ok = Button(_(MSG_BTN_SUBSCRIBE)),
                    Child, u_cancel = Button(_(MSG_BTN_CANCEL)),
                End,
            End,
        End;
    log_msg("gui: adressfönster %lx", (ULONG)uwin);
    iwin = WindowObject,
            MUIA_Window_Title, _(MSG_WIN_INFO),
            MUIA_Window_ID, MAKE_ID('C','P','D','4'),
            MUIA_Window_Width, MUIV_Window_Width_Screen(50),
            MUIA_Window_Height, MUIV_Window_Height_Screen(50),
            WindowContents, VGroup, FONT_TAG,
                Child, i_title = TextObject, TextFrame, MUIA_Background, MUII_TextBack, MUIA_Text_Contents, "", End,
                Child, ListviewObject,
                    MUIA_Listview_Input, FALSE,
                    MUIA_Listview_List, i_text = FloattextObject, ReadListFrame, FONT_TAG, MUIA_Floattext_Text, "", MUIA_Floattext_Justify, FALSE, End,
                End,
                Child, i_close = Button(_(MSG_BTN_CLOSE)),
            End,
        End;
    log_msg("gui: infofönster %lx", (ULONG)iwin);
    if (!win || !swin || !uwin || !iwin) return NULL;
    app = ApplicationObject,
        MUIA_Application_Title, "CurrentPod",
        MUIA_Application_Version, "$VER: CurrentPod " CP_VERSION " (" CP_DATE ")",
        MUIA_Application_Copyright, "© 2026",
        MUIA_Application_Author, "CurrentPod",
        MUIA_Application_Description, _(MSG_APP_DESCRIPTION),
        MUIA_Application_Base, "CURRENTPOD",
        MUIA_Application_Menustrip, menustrip,
        SubWindow, win,
        SubWindow, swin,
        SubWindow, uwin,
        SubWindow, iwin,
    End;
    log_msg("gui: app %lx", (ULONG)app);
    return app;
}

static void setup_notifies(void)
{
    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, app, 2, MUIM_Application_ReturnID, ID_QUIT);
    DoMethod(feedlist, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_FEED_SELECT);
    DoMethod(eplist, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE, app, 2, MUIM_Application_ReturnID, ID_EP_DOUBLE);
    DoMethod(eplist, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_EP_SELECT);
    DoMethod(b_dl, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_DOWNLOAD);
    DoMethod(b_playep, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_PLAY);
    DoMethod(b_conv, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_CONVERT);
    DoMethod(b_prev, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_PREV);
    DoMethod(b_play, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_PLAY);
    DoMethod(b_pause, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_PAUSE);
    DoMethod(b_next, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_NEXT);
    DoMethod(b_skipb, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_SKIPB);
    DoMethod(b_skipf, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_SKIPF);
    DoMethod(vol, MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_VOLUME);
    DoMethod(win, MUIM_Notify, MUIA_Window_InputEvent, "space", app, 2, MUIM_Application_ReturnID, ID_PAUSE);
    DoMethod(win, MUIM_Notify, MUIA_Window_InputEvent, "right", app, 2, MUIM_Application_ReturnID, ID_SKIPF);
    DoMethod(win, MUIM_Notify, MUIA_Window_InputEvent, "left", app, 2, MUIM_Application_ReturnID, ID_SKIPB);
    DoMethod(win, MUIM_Notify, MUIA_Window_InputEvent, "return", app, 2, MUIM_Application_ReturnID, ID_PLAY);
    DoMethod(win, MUIM_Notify, MUIA_Window_InputEvent, "del", app, 2, MUIM_Application_ReturnID, ID_DELETE_FILE);

    DoMethod(swin, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, swin, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(s_term, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_SEARCH_GO);
    DoMethod(s_sub, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_SEARCH_SUB);
    DoMethod(s_list, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE, app, 2, MUIM_Application_ReturnID, ID_SEARCH_DOUBLE);
    DoMethod(swin, MUIM_Notify, MUIA_Window_Open, TRUE, swin, 3, MUIM_Set, MUIA_Window_ActiveObject, s_term);
    DoMethod(s_close, MUIM_Notify, MUIA_Pressed, FALSE, swin, 3, MUIM_Set, MUIA_Window_Open, FALSE);

    DoMethod(uwin, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, uwin, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(u_str, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_URL_OK);
    DoMethod(uwin, MUIM_Notify, MUIA_Window_Open, TRUE, uwin, 3, MUIM_Set, MUIA_Window_ActiveObject, u_str);
    DoMethod(u_ok, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_URL_OK);
    DoMethod(u_cancel, MUIM_Notify, MUIA_Pressed, FALSE, uwin, 3, MUIM_Set, MUIA_Window_Open, FALSE);

    DoMethod(iwin, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, iwin, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(i_close, MUIM_Notify, MUIA_Pressed, FALSE, iwin, 3, MUIM_Set, MUIA_Window_Open, FALSE);
}

static void show_info_episode(struct Episode *e)
{
    static char text[4200];
    char sz[24], du[24], d[24];
    if (!e) return;
    fmt_size(e->size, sz); fmt_time(e->dur, du); iso_to_display(e->date, d);
    sprintf(text, _(MSG_INFO_EPISODE_FMT),
            e->title ? e->title : "", d, du, sz, e->feed->name ? e->feed->name : "",
            e->desc ? e->desc : _(MSG_NO_DESC), e->enc ? e->enc : "-", e->file ? e->file : _(MSG_NOT_DOWNLOADED));
    set(i_title, MUIA_Text_Contents, e->feed->name);
    set(i_text, MUIA_Floattext_Text, text);
    set(iwin, MUIA_Window_Open, TRUE);
}

static void show_info_feed(struct Feed *f)
{
    static char text[4200];
    if (!f) return;
    sprintf(text, _(MSG_INFO_FEED_FMT),
            f->name ? f->name : "", f->author ? f->author : "", f->desc ? f->desc : _(MSG_NO_DESC),
            f->url ? f->url : "-", f->site ? f->site : "-", f->updated ? f->updated : _(MSG_NEVER), f->apple,
            f->lasterr ? _(MSG_INFO_LAST_ERROR) : "", f->lasterr ? f->lasterr : "");
    set(i_title, MUIA_Text_Contents, _(MSG_INFO_TITLE_PODCAST));
    set(i_text, MUIA_Floattext_Text, text);
    set(iwin, MUIA_Window_Open, TRUE);
}

static void open_in_browser(const char *url)
{
    char cmd[PATHMAX + URLMAX];
    const char *p = strstr(cfg.browser, "%s");
    if (!url || !url[0]) return;
    if (p) sprintf(cmd, "%.*s%s%s", (int)(p - cfg.browser), cfg.browser, url, p + 2);
    else sprintf(cmd, "%s \"%s\"", cfg.browser, url);
    SystemTags((STRPTR)cmd, SYS_Asynch, TRUE, SYS_Input, (ULONG)Open((STRPTR)"NIL:", MODE_OLDFILE), SYS_Output, (ULONG)Open((STRPTR)"NIL:", MODE_OLDFILE), NP_StackSize, 40000, TAG_DONE);
}

static void about(void)
{
    MUI_Request(app, win, 0, _(MSG_ABOUT_TITLE), "OK", _(MSG_ABOUT_BODY), (ULONG)CP_VERSION, (ULONG)CP_DATE,
                cfg.bits14 ? _(MSG_BITS14_WORD) : _(MSG_BITS8_WORD), pstat.libname[0] ? pstat.libname : "?");
}

/* ------------------------------------------------------------ timer */

static int timer_start(void)
{
    timerport = CreateMsgPort();
    if (!timerport) return 0;
    timerio = (struct timerequest *)CreateIORequest(timerport, sizeof(struct timerequest));
    if (!timerio) return 0;
    if (OpenDevice((STRPTR)"timer.device", UNIT_VBLANK, (struct IORequest *)timerio, 0)) return 0;
    timer_open = 1;
    timerio->tr_node.io_Command = TR_ADDREQUEST;
    timerio->tr_time.tv_secs = 1; timerio->tr_time.tv_micro = 0;
    SendIO((struct IORequest *)timerio);
    return 1;
}

static void timer_stop(void)
{
    if (timer_open) {
        AbortIO((struct IORequest *)timerio); WaitIO((struct IORequest *)timerio);
        CloseDevice((struct IORequest *)timerio);
    }
    if (timerio) DeleteIORequest((struct IORequest *)timerio);
    if (timerport) DeleteMsgPort(timerport);
}

/* ------------------------------------------------------------ händelser */

static void handle_id(ULONG id)
{
    struct Episode *e;
    switch (id) {
        case ID_QUIT: quit_flag = 1; break;
        case ID_ABOUT: about(); break;
        case ID_SAVEPREFS: cfg_save(); set_status(_(MSG_SETTINGS_SAVED)); break;
        case ID_SEARCH_OPEN: set(swin, MUIA_Window_Open, TRUE); break;
        case ID_URL_OPEN: set(u_str, MUIA_String_Contents, ""); set(uwin, MUIA_Window_Open, TRUE); break;
        case ID_URL_OK: {
            char *s = (char *)xget(u_str, MUIA_String_Contents);
            set(uwin, MUIA_Window_Open, FALSE);
            if (s && *s) subscribe_url(s);
            break;
        }
        case ID_SEARCH_GO: {
            char *s = (char *)xget(s_term, MUIA_String_Contents);
            struct Job *j;
            if (!s || !*s) break;
            j = job_new(JOB_SEARCH);
            if (!j) break;
            strlcpy_(j->term, s, sizeof(j->term));
            strlcpy_(j->country, country_codes[xget(s_country, MUIA_Cycle_Active)], sizeof(j->country));
            strlcpy_(cfg.country, j->country, sizeof(cfg.country));
            set(s_status, MUIA_Text_Contents, _(MSG_SEARCHING));
            job_queue(j);
            break;
        }
        case ID_SEARCH_SUB: case ID_SEARCH_DOUBLE: {
            struct SearchHit *h = NULL;
            DoMethod(s_list, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &h);
            if (h) {
                struct Feed *f;
                subscribe_url(h->feedurl);
                f = db_find_url(h->feedurl);
                if (f) { f->apple = h->apple; if (f->name && !strcmp(f->name, _(MSG_FETCHING_NAME))) { xstrset(&f->name, h->name); redraw_feed(f); } }
                set(s_status, MUIA_Text_Contents, _(MSG_SUBSCRIBING));
            }
            break;
        }
        case ID_FEED_SELECT: {
            struct Feed *f = NULL;
            DoMethod(feedlist, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &f);
            if (f != cur_feed) {
                cur_feed = f;
                if (f && f->hasnew) {
                    struct Episode *x;
                    for (x = (struct Episode *)f->episodes.mlh_Head; x->node.mln_Succ; x = (struct Episode *)x->node.mln_Succ) x->isnew = 0;
                    f->hasnew = 0;
                    redraw_feed(f);
                }
                fill_episodes(f);
            }
            break;
        }
        case ID_UPDATE: queue_feed_update(cur_feed); break;
        case ID_UPDATE_ALL: {
            struct Feed *f;
            for (f = (struct Feed *)feeds.mlh_Head; f->node.mln_Succ; f = (struct Feed *)f->node.mln_Succ) queue_feed_update(f);
            break;
        }
        case ID_FEEDINFO: show_info_feed(cur_feed); break;
        case ID_REMOVE_FEED:
            if (cur_feed && conv_ep && conv_ep->feed == cur_feed) { set_status(_(MSG_FEED_CONVERTING)); break; }
            if (cur_feed && !cur_feed->updating) {
                if (MUI_Request(app, win, 0, _(MSG_REQ_REMOVE_TITLE), _(MSG_REQ_REMOVE_GADGETS), _(MSG_REQ_REMOVE_BODY), cur_feed->name)) {
                    struct Feed *f = cur_feed;
                    LONG i;
                    if (play_ep && play_ep->feed == f) { send_simple(PC_STOP, 0); play_ep = NULL; }
                    cur_feed = NULL;
                    DoMethod(eplist, MUIM_List_Clear);
                    for (i = 0; ; i++) { struct Feed *x; DoMethod(feedlist, MUIM_List_GetEntry, i, &x); if (!x) break; if (x == f) { DoMethod(feedlist, MUIM_List_Remove, i); break; } }
                    db_remove_feed(f);
                }
            }
            break;
        case ID_EP_SELECT: update_buttons(); break;
        case ID_EP_DOUBLE:
            /* dubbelklick: spela om filen finns, annars hämta */
            log_msg("gui: dubbelklick/retur i listan");
            e = selected_episode();
            if (e) {
                if (e->file || e->pcm) play_or_resume(e);
                else if (e->enc && e->state != EP_QUEUED) { if (e->state == EP_ERROR || e->state == EP_HEARD) e->state = EP_UNHEARD; start_download(e); }
            }
            break;
        case ID_PLAY:
            log_msg("gui: spela-tryck");
            play_or_resume(selected_episode());
            break;
        case ID_CONVERT:
            e = selected_episode();
            if (e) start_conversion(e);
            break;
        case ID_CANCEL_CONV:
            if (conv_ep) { cstat.abort = 1; set_status(_(MSG_CANCELLING_CONV)); }
            break;
        case ID_CV_LOW: case ID_CV_MEDIUM:
            cfg.convquality = (id == ID_CV_MEDIUM) ? Q_MEDIUM : Q_LOW;
            set_status(_(MSG_CONV_SETTING), cfg.convquality == Q_MEDIUM ? _(MSG_KHZ24) : _(MSG_KHZ12));
            break;
        case ID_DELETE_MP3:
            e = selected_episode();
            if (e && e->file && e->pcm && e->state != EP_CONVERTING) {
                if (e == play_ep && !pstat.pcm && pstat.state != PS_STOPPED) { send_simple(PC_STOP, 0); play_ep = NULL; }
                DeleteFile((STRPTR)e->file);
                xfree(e->file); e->file = NULL;
                save_feed_quiet(e->feed);
                redraw_episode(e);
                set_status(_(MSG_MP3_DELETED));
            }
            break;
        case ID_PAUSE:
            log_msg("gui: paus-tryck, startar=%ld tillstånd=%ld", (LONG)play_starting, (LONG)pstat.state);
            if (play_starting) break;
            if (pstat.state == PS_PLAYING) send_simple(PC_PAUSE, 0);
            else play_or_resume(selected_episode());
            break;
        case ID_STOP:
            if (play_ep) { struct Episode *x = play_ep; save_feed_quiet(x->feed); send_simple(PC_STOP, 0); play_ep = NULL; redraw_episode(x); }
            break;
        case ID_SKIPF: if (play_ep && pstat.state != PS_STOPPED) send_simple(PC_SEEK, pstat.pos + cfg.skipfwd); break;
        case ID_SKIPB: if (play_ep && pstat.state != PS_STOPPED) send_simple(PC_SEEK, pstat.pos - cfg.skipback); break;
        case ID_PREV:
            if (play_ep) {
                if (pstat.pos > 10) send_simple(PC_SEEK, 0);
                else { struct Episode *n = next_episode(play_ep, -1); if (n) play_episode(n); }
            }
            break;
        case ID_NEXT:
            if (play_ep) { struct Episode *n = next_episode(play_ep, +1); if (n) play_episode(n); }
            break;
        case ID_VOLUME:
            cfg.volume = xget(vol, MUIA_Numeric_Value);
            send_simple(PC_VOLUME, cfg.volume);
            break;
        case ID_DOWNLOAD:
            e = selected_episode();
            if (e) { if (e->state == EP_ERROR || e->state == EP_HEARD) e->state = EP_UNHEARD; start_download(e); }
            break;
        case ID_DOWNLOAD_NEW:
            if (cur_feed) {
                struct Episode *x;
                for (x = (struct Episode *)cur_feed->episodes.mlh_Head; x->node.mln_Succ; x = (struct Episode *)x->node.mln_Succ)
                    if (x->state == EP_UNHEARD && x->enc) start_download(x);
            }
            break;
        case ID_CANCEL_DL:
            if (cur_job && cur_job->type == JOB_DOWNLOAD) { cur_job->abort = 1; set_status(_(MSG_CANCELLING_DL)); }
            break;
        case ID_MARK:
            e = selected_episode();
            if (e) {
                if (e->state == EP_CONVERTING) break;
                if (e->state == EP_HEARD) e->state = (e->file || e->pcm) ? EP_DOWNLOADED : EP_UNHEARD;
                else { e->state = EP_HEARD; e->pos = 0; }
                db_recount(e->feed); save_feed_quiet(e->feed);
                redraw_episode(e); redraw_feed(e->feed);
            }
            break;
        case ID_DELETE_FILE:
            e = selected_episode();
            if (e && e->state == EP_CONVERTING) { set_status(_(MSG_CONVERTING_CANCEL_FIRST)); break; }
            if (e && (e->file || e->pcm)) {
                if (e == play_ep) { send_simple(PC_STOP, 0); play_ep = NULL; }
                if (e->file) DeleteFile((STRPTR)e->file);
                if (e->pcm) DeleteFile((STRPTR)e->pcm);
                xfree(e->file); e->file = NULL;
                xfree(e->pcm); e->pcm = NULL;
                if (e->state != EP_HEARD) e->state = EP_UNHEARD;
                e->pos = 0;
                db_recount(e->feed); save_feed_quiet(e->feed);
                redraw_episode(e); redraw_feed(e->feed);
            }
            break;
        case ID_INFO: show_info_episode(selected_episode()); break;
        case ID_OPEN_LINK:
            e = selected_episode();
            if (e && e->link) open_in_browser(e->link);
            else if (cur_feed && cur_feed->site) open_in_browser(cur_feed->site);
            break;
        case ID_Q_AUTO: case ID_Q_LOW: case ID_Q_MEDIUM: case ID_Q_HIGH: case ID_Q_FULL:
            cfg.quality = id - ID_Q_AUTO;
            set_quality_cmd();
            set_status(_(MSG_QUALITY_NEXT));
            break;
        case ID_BITS14: {
            Object *item = (Object *)DoMethod(menustrip, MUIM_FindUData, ID_BITS14);
            cfg.bits14 = item ? xget(item, MUIA_Menuitem_Checked) : !cfg.bits14;
            set_quality_cmd();
            break;
        }
        case ID_AUTONEXT: {
            Object *item = (Object *)DoMethod(menustrip, MUIM_FindUData, ID_AUTONEXT);
            cfg.autonext = item ? xget(item, MUIA_Menuitem_Checked) : !cfg.autonext;
            break;
        }
    }
}

int gui_run(void)
{
    ULONG sigs = 0, jobsig, playsig, timersig, convsig;
    if (!(MUIMasterBase = OpenLibrary((STRPTR)MUIMASTER_NAME, 19))) {
        printf(_(MSG_NEED_MUI));
        return 20;
    }
    log_msg("gui: MUI öppnad");
    NewList((struct List *)&jobqueue);
    jobport = CreateMsgPort();
    playport = CreateMsgPort();
    convport = CreateMsgPort();
    if (!jobport || !playport || !convport) return 20;
    font_open();
    if (!create_windows()) {
        log_msg("gui: create_windows misslyckades");
        font_close();
        CloseLibrary(MUIMasterBase);
        return 20;
    }
    log_msg("gui: objekt skapade");
    setup_notifies();
    log_msg("gui: notifieringar klara");
    if (!worker_start(jobport)) { MUI_Request(app, NULL, 0, "CurrentPod", "OK", _(MSG_REQ_NO_WORKER)); }
    if (!player_start(playport, cfg.quality, cfg.bits14)) {
        MUI_Request(app, NULL, 0, "CurrentPod", "OK", _(MSG_REQ_NO_PLAYER), pstat.errstr[0] ? pstat.errstr : _(MSG_UNKNOWN_ERROR));
    } else send_simple(PC_VOLUME, cfg.volume);
    if (!conv_start()) log_msg("gui: konverteraren startade inte");
    log_msg("gui: arbetare, spelare (%s) och konverterare startade", pstat.libname);
    timer_start();
    fill_feeds();
    if (db_feed_count()) set(feedlist, MUIA_List_Active, 0);
    update_player_panel();
    set_status(_(MSG_WELCOME), db_feed_count());
    set(win, MUIA_Window_Open, TRUE);
    if (!xget(win, MUIA_Window_Open)) { MUI_Request(app, NULL, 0, "CurrentPod", "OK", _(MSG_REQ_NO_WINDOW)); quit_flag = 1; }
    log_msg("gui: fönstret öppet");
    if (start_url[0]) subscribe_url(start_url);
    if (start_file[0]) {
        /* testläge: spela en fil direkt, som ett låtsasavsnitt i en låtsaspodd */
        struct Feed *tf = db_new_feed();
        if (tf) {
            struct Episode *te;
            tf->name = xstrdup(_(MSG_TEST_FILE));
            te = db_new_episode(tf);
            if (te) {
                te->title = xstrdup(FilePart((STRPTR)start_file));
                te->file = xstrdup(start_file);
                te->date = xstrdup("");
                te->state = EP_DOWNLOADED;
                cur_feed = tf;
                fill_episodes(tf);
                play_episode(te);
            }
        }
    }
    jobsig = 1L << jobport->mp_SigBit;
    playsig = 1L << playport->mp_SigBit;
    timersig = timerport ? 1L << timerport->mp_SigBit : 0;
    convsig = 1L << convport->mp_SigBit;
    update_buttons();
    while (!quit_flag) {
        ULONG id = DoMethod(app, MUIM_Application_NewInput, &sigs);
        if (id == MUIV_Application_ReturnID_Quit) break;
        if (id) handle_id(id);
        if (quit_flag) break;
        if (sigs) {
            sigs = Wait(sigs | jobsig | playsig | timersig | convsig | SIGBREAKF_CTRL_C);
            if (sigs & SIGBREAKF_CTRL_C) break;
            if (sigs & convsig) {
                struct ConvCmd *c;
                while ((c = (struct ConvCmd *)GetMsg(convport))) conv_done(c);
            }
            if (sigs & jobsig) {
                struct Job *j;
                while ((j = (struct Job *)GetMsg(jobport))) {
                    if (j == cur_job) cur_job = NULL;
                    job_done(j);
                }
                job_send_next();
                if (!cur_job) { timer_tick(); }
            }
            if (sigs & playsig) {
                struct PlayerCmd *c;
                while ((c = (struct PlayerCmd *)GetMsg(playport))) {
                    log_msg("gui: spelaren svarade på kommando %ld: fel %ld %s (tillstånd %ld)", (LONG)c->cmd, (LONG)c->error, c->errstr, (LONG)pstat.state);
                    if (c->cmd == PC_PLAY || c->cmd == PC_RESUME) play_starting = 0;
                    if (c->error) set_status(_(MSG_PLAYER_ERR), c->errstr);
                    xfree(c);
                }
                timer_tick();
            }
            if (sigs & timersig) {
                if (GetMsg(timerport)) {
                    timer_tick();
                    timerio->tr_node.io_Command = TR_ADDREQUEST;
                    timerio->tr_time.tv_secs = 1; timerio->tr_time.tv_micro = 0;
                    SendIO((struct IORequest *)timerio);
                }
            }
        }
    }
    set(win, MUIA_Window_Open, FALSE);
    if (play_ep) { play_ep->pos = pstat.pos; save_feed_quiet(play_ep->feed); }
    cfg_save();
    player_stop();
    if (cur_job) { cur_job->abort = 1; }
    worker_stop();
    conv_stop();
    {
        struct ConvCmd *c;
        while ((c = (struct ConvCmd *)GetMsg(convport))) xfree(c);
        if (conv_ep) { conv_ep->state = conv_prev_state; save_feed_quiet(conv_ep->feed); conv_ep = NULL; }
    }
    {
        struct Job *j;
        while ((j = (struct Job *)GetMsg(jobport))) { if (j->feed) db_free_feed(j->feed); xfree(j->hits); xfree(j); }
        while ((j = (struct Job *)RemHead((struct List *)&jobqueue))) xfree(j);
        if (cur_job) { if (cur_job->feed) db_free_feed(cur_job->feed); xfree(cur_job->hits); xfree(cur_job); }
    }
    {
        struct PlayerCmd *c;
        while ((c = (struct PlayerCmd *)GetMsg(playport))) xfree(c);
    }
    timer_stop();
    MUI_DisposeObject(app);
    font_close();
    DeleteMsgPort(jobport);
    DeleteMsgPort(playport);
    DeleteMsgPort(convport);
    xfree(hits);
    CloseLibrary(MUIMasterBase);
    return 0;
}
