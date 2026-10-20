/* CurrentPod - databasen: en textfil per podd i PROGDIR:Feeds */
#include "currentpod.h"
#include <stdlib.h>

struct MinList feeds;
char feeds_dir[PATHMAX];
static int next_id = 1;

void db_init(const char *dir)
{
    NewList((struct List *)&feeds);
    strlcpy_(feeds_dir, dir, sizeof(feeds_dir));
    make_dirs(feeds_dir);
}

struct Feed *db_new_feed(void)
{
    struct Feed *f = xalloc(sizeof(*f));
    if (f) NewList((struct List *)&f->episodes);
    return f;
}

struct Episode *db_new_episode(struct Feed *f)
{
    struct Episode *e = xalloc(sizeof(*e));
    if (e) {
        e->feed = f;
        if (f) { AddTail((struct List *)&f->episodes, (struct Node *)e); f->nep++; }
    }
    return e;
}

void db_free_episode(struct Episode *e)
{
    if (!e) return;
    xfree(e->guid); xfree(e->title); xfree(e->date); xfree(e->enc); xfree(e->type);
    xfree(e->link); xfree(e->desc); xfree(e->file); xfree(e->image); xfree(e->pcm);
    xfree(e);
}

void db_free_feed(struct Feed *f)
{
    struct Episode *e, *n;
    if (!f) return;
    for (e = (struct Episode *)f->episodes.mlh_Head; (n = (struct Episode *)e->node.mln_Succ); e = n)
        db_free_episode(e);
    xfree(f->name); xfree(f->url); xfree(f->site); xfree(f->author); xfree(f->desc); xfree(f->lang);
    xfree(f->image); xfree(f->etag); xfree(f->modified); xfree(f->updated); xfree(f->lasterr);
    xfree(f);
}

static void feed_path(struct Feed *f, char *out)
{
    char name[32];
    sprintf(name, "%04ld.feed", f->id);
    add_part(out, feeds_dir, name, PATHMAX);
}

void db_remove_feed(struct Feed *f)
{
    char path[PATHMAX];
    feed_path(f, path);
    DeleteFile((STRPTR)path);
    Remove((struct Node *)f);
    db_free_feed(f);
    db_save_index();
}

struct Feed *db_add_feed(const char *url, const char *name)
{
    struct Feed *f = db_new_feed();
    if (!f) return NULL;
    log_msg("db: next_id=%ld innan tilldelning", (LONG)next_id);
    f->id = next_id++;
    f->url = xstrdup(url);
    f->name = xstrdup(name ? name : _(MSG_FETCHING_NAME));
    AddTail((struct List *)&feeds, (struct Node *)f);
    db_save_index();
    return f;
}

struct Feed *db_find_url(const char *url)
{
    struct Feed *f;
    for (f = (struct Feed *)feeds.mlh_Head; f->node.mln_Succ; f = (struct Feed *)f->node.mln_Succ)
        if (f->url && strieq(f->url, url)) return f;
    return NULL;
}

struct Feed *db_find_id(int id)
{
    struct Feed *f;
    for (f = (struct Feed *)feeds.mlh_Head; f->node.mln_Succ; f = (struct Feed *)f->node.mln_Succ)
        if (f->id == id) return f;
    return NULL;
}

int db_feed_count(void)
{
    struct Feed *f; int n = 0;
    for (f = (struct Feed *)feeds.mlh_Head; f->node.mln_Succ; f = (struct Feed *)f->node.mln_Succ) n++;
    return n;
}

void db_recount(struct Feed *f)
{
    struct Episode *e;
    f->nep = f->unheard = f->hasnew = 0;
    for (e = (struct Episode *)f->episodes.mlh_Head; e->node.mln_Succ; e = (struct Episode *)e->node.mln_Succ) {
        f->nep++;
        if (e->state != EP_HEARD) f->unheard++;
        if (e->isnew) f->hasnew = 1;
    }
}

/* ------------------------------------------------------------ skriva */

static void put(struct Buf *b, const char *key, const char *val)
{
    if (!val) return;
    buf_adds(b, key); buf_adds(b, "="); buf_adds(b, val); buf_adds(b, "\n");
}

static void putl(struct Buf *b, const char *key, LONG val)
{
    char t[24];
    sprintf(t, "%ld", val);
    put(b, key, t);
}

/* beskrivningar kan ha radbrytningar: lagras med \n -> "\\n" */
static void put_multi(struct Buf *b, const char *key, const char *val)
{
    const char *p;
    if (!val) return;
    buf_adds(b, key); buf_adds(b, "=");
    for (p = val; *p; p++) {
        if (*p == '\n') buf_adds(b, "\\n");
        else if (*p == '\\') buf_adds(b, "\\\\");
        else buf_add(b, p, 1);
    }
    buf_adds(b, "\n");
}

int db_save_feed(struct Feed *f)
{
    struct Buf b = { NULL, 0, 0 };
    struct Episode *e;
    char path[PATHMAX];
    int ok;
    buf_adds(&b, "CURRENTPOD-FEED 1\n");
    put(&b, "Name", f->name);
    put(&b, "Url", f->url);
    put(&b, "Site", f->site);
    put(&b, "Author", f->author);
    put(&b, "Lang", f->lang);
    put(&b, "Image", f->image);
    putl(&b, "Apple", f->apple);
    put(&b, "ETag", f->etag);
    put(&b, "Modified", f->modified);
    put(&b, "Updated", f->updated);
    putl(&b, "AutoDownload", f->autodl);
    putl(&b, "Serial", f->serial);
    put_multi(&b, "Desc", f->desc);
    buf_adds(&b, ".\n");
    for (e = (struct Episode *)f->episodes.mlh_Head; e->node.mln_Succ; e = (struct Episode *)e->node.mln_Succ) {
        buf_adds(&b, "[E]\n");
        put(&b, "Guid", e->guid);
        put(&b, "Title", e->title);
        put(&b, "Date", e->date);
        put(&b, "Enc", e->enc);
        putl(&b, "Size", e->size);
        put(&b, "Type", e->type);
        put(&b, "Link", e->link);
        put(&b, "Image", e->image);
        putl(&b, "Dur", e->dur);
        putl(&b, "Season", e->season);
        putl(&b, "Episode", e->epnum);
        putl(&b, "EType", e->etype);
        putl(&b, "Explicit", e->explicit_);
        putl(&b, "State", e->state);
        putl(&b, "Pos", e->pos);
        putl(&b, "New", e->isnew);
        put(&b, "File", e->file);
        put(&b, "Pcm", e->pcm);
        put_multi(&b, "Desc", e->desc);
        buf_adds(&b, ".\n");
    }
    feed_path(f, path);
    ok = save_file(path, b.data, b.len);
    buf_free(&b);
    return ok;
}

int db_save_index(void)
{
    struct Buf b = { NULL, 0, 0 };
    struct Feed *f;
    char path[PATHMAX], line[64];
    int ok;
    buf_adds(&b, "CURRENTPOD-INDEX 1\n");
    for (f = (struct Feed *)feeds.mlh_Head; f->node.mln_Succ; f = (struct Feed *)f->node.mln_Succ) {
        sprintf(line, "%ld\t", f->id);
        buf_adds(&b, line);
        buf_adds(&b, f->name ? f->name : "?");
        buf_adds(&b, "\n");
    }
    add_part(path, feeds_dir, "feeds.idx", sizeof(path));
    ok = save_file(path, b.data, b.len);
    buf_free(&b);
    return ok;
}

/* ------------------------------------------------------------ läsa */

static char *unescape_multi(char *v)
{
    char *r = v, *w = v;
    while (*r) {
        if (*r == '\\' && r[1] == 'n') { *w++ = '\n'; r += 2; }
        else if (*r == '\\' && r[1] == '\\') { *w++ = '\\'; r += 2; }
        else *w++ = *r++;
    }
    *w = 0;
    return v;
}

static int load_feed_file(struct Feed *f)
{
    char path[PATHMAX];
    LONG len;
    char *data, *line, *next;
    struct Episode *e = NULL;
    feed_path(f, path);
    data = load_file(path, &len);
    if (!data) return 0;
    if (strncmp(data, "CURRENTPOD-FEED", 15)) { xfree(data); return 0; }
    for (line = strchr(data, '\n'); line; line = next) {
        char *eq;
        line++;
        next = strchr(line, '\n');
        if (next) *next = 0;
        if (!*line) continue;
        if (!strcmp(line, "[E]")) { e = db_new_episode(f); continue; }
        if (!strcmp(line, ".")) continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        {
            char *k = line, *v = eq + 1;
            if (!e) {
                if (!strcmp(k, "Name")) f->name = xstrdup(v);
                else if (!strcmp(k, "Url")) f->url = xstrdup(v);
                else if (!strcmp(k, "Site")) f->site = xstrdup(v);
                else if (!strcmp(k, "Author")) f->author = xstrdup(v);
                else if (!strcmp(k, "Lang")) f->lang = xstrdup(v);
                else if (!strcmp(k, "Image")) f->image = xstrdup(v);
                else if (!strcmp(k, "Apple")) f->apple = atol(v);
                else if (!strcmp(k, "ETag")) f->etag = xstrdup(v);
                else if (!strcmp(k, "Modified")) f->modified = xstrdup(v);
                else if (!strcmp(k, "Updated")) f->updated = xstrdup(v);
                else if (!strcmp(k, "AutoDownload")) f->autodl = atoi(v);
                else if (!strcmp(k, "Serial")) f->serial = atoi(v);
                else if (!strcmp(k, "Desc")) f->desc = xstrdup(unescape_multi(v));
            } else {
                if (!strcmp(k, "Guid")) e->guid = xstrdup(v);
                else if (!strcmp(k, "Title")) e->title = xstrdup(v);
                else if (!strcmp(k, "Date")) e->date = xstrdup(v);
                else if (!strcmp(k, "Enc")) e->enc = xstrdup(v);
                else if (!strcmp(k, "Size")) e->size = atol(v);
                else if (!strcmp(k, "Type")) e->type = xstrdup(v);
                else if (!strcmp(k, "Link")) e->link = xstrdup(v);
                else if (!strcmp(k, "Image")) e->image = xstrdup(v);
                else if (!strcmp(k, "Dur")) e->dur = atol(v);
                else if (!strcmp(k, "Season")) e->season = atol(v);
                else if (!strcmp(k, "Episode")) e->epnum = atol(v);
                else if (!strcmp(k, "EType")) e->etype = atoi(v);
                else if (!strcmp(k, "Explicit")) e->explicit_ = atoi(v);
                else if (!strcmp(k, "State")) e->state = atoi(v);
                else if (!strcmp(k, "Pos")) e->pos = atol(v);
                else if (!strcmp(k, "New")) e->isnew = atoi(v);
                else if (!strcmp(k, "File")) e->file = xstrdup(v);
                else if (!strcmp(k, "Pcm")) e->pcm = xstrdup(v);
                else if (!strcmp(k, "Desc")) e->desc = xstrdup(unescape_multi(v));
            }
        }
    }
    xfree(data);
    /* hämtade filer som försvunnit */
    for (e = (struct Episode *)f->episodes.mlh_Head; e->node.mln_Succ; e = (struct Episode *)e->node.mln_Succ) {
        if (e->state == EP_CONVERTING) e->state = EP_DOWNLOADED;   /* programmet avslutades mitt i */
        if (e->pcm && !file_exists(e->pcm)) { xfree(e->pcm); e->pcm = NULL; }
        if (e->file && (e->state == EP_DOWNLOADED || e->state == EP_HEARD || e->state == EP_PARTIAL) && !file_exists(e->file)) {
            xfree(e->file); e->file = NULL;
        }
        if (!e->file && !e->pcm && (e->state == EP_DOWNLOADED || e->state == EP_PARTIAL)) { e->state = EP_UNHEARD; e->pos = 0; }
        if (e->state == EP_QUEUED) e->state = EP_UNHEARD;
    }
    db_recount(f);
    return 1;
}

int db_load(void)
{
    char path[PATHMAX];
    char *data, *line, *next;
    int n = 0;
    add_part(path, feeds_dir, "feeds.idx", sizeof(path));
    data = load_file(path, NULL);
    if (!data) return 0;
    for (line = strchr(data, '\n'); line; line = next) {
        struct Feed *f;
        int id;
        line++;
        next = strchr(line, '\n');
        if (next) *next = 0;
        if (!*line) continue;
        id = atoi(line);
        if (id <= 0) continue;
        f = db_new_feed();
        if (!f) break;
        f->id = id;
        if (id >= next_id) next_id = id + 1;
        if (load_feed_file(f)) {
            AddTail((struct List *)&feeds, (struct Node *)f);
            n++;
        } else db_free_feed(f);
    }
    xfree(data);
    return n;
}

static struct Episode *find_episode(struct Feed *f, struct Episode *x)
{
    struct Episode *e;
    for (e = (struct Episode *)f->episodes.mlh_Head; e->node.mln_Succ; e = (struct Episode *)e->node.mln_Succ) {
        if (x->guid && e->guid && !strcmp(e->guid, x->guid)) return e;
        if (!x->guid && x->enc && e->enc && !strcmp(e->enc, x->enc)) return e;
        if (!x->guid && !x->enc && x->title && e->title && !strcmp(e->title, x->title)) return e;
    }
    return NULL;
}

/* Flytta nya avsnitt ur fresh in i dst, uppdatera metadata, behåll status.
   fresh töms på avsnitt (de som redan fanns frigörs) och ska frias av anroparen. */
int db_merge(struct Feed *dst, struct Feed *fresh, int maxep)
{
    struct Episode *e, *n;
    int added = 0;
    if (!dst->id) dst->id = next_id++;
    if (fresh->name && (!dst->name || !dst->name[0] || !strcmp(dst->name, _(MSG_FETCHING_NAME)))) xstrset(&dst->name, fresh->name);
    if (fresh->site) xstrset(&dst->site, fresh->site);
    if (fresh->author) xstrset(&dst->author, fresh->author);
    if (fresh->desc) xstrset(&dst->desc, fresh->desc);
    if (fresh->lang) xstrset(&dst->lang, fresh->lang);
    if (fresh->image) xstrset(&dst->image, fresh->image);
    dst->serial = fresh->serial;
    for (e = (struct Episode *)dst->episodes.mlh_Head; e->node.mln_Succ; e = (struct Episode *)e->node.mln_Succ)
        e->isnew = 0;
    /* fresh är i flödesordning (nyast först): gå baklänges så att AddHead ger rätt ordning */
    for (e = (struct Episode *)fresh->episodes.mlh_TailPred; (n = (struct Episode *)e->node.mln_Pred); e = n) {
        struct Episode *old;
        Remove((struct Node *)e);
        fresh->nep--;
        old = find_episode(dst, e);
        if (old) {
            if (e->title) xstrset(&old->title, e->title);
            if (e->dur && !old->dur) old->dur = e->dur;
            if (e->desc && !old->desc) xstrset(&old->desc, e->desc);
            if (e->enc && !old->enc) xstrset(&old->enc, e->enc);
            if (e->size && !old->size) old->size = e->size;
            db_free_episode(e);
        } else {
            e->feed = dst;
            e->isnew = 1;
            AddHead((struct List *)&dst->episodes, (struct Node *)e);
            added++;
        }
    }
    /* begränsa antalet: kasta äldsta som varken är hämtade eller hörda */
    db_recount(dst);
    if (maxep > 0) {
        while (dst->nep > maxep) {
            struct Episode *last = NULL;
            for (e = (struct Episode *)dst->episodes.mlh_TailPred; e->node.mln_Pred; e = (struct Episode *)e->node.mln_Pred)
                if (!e->file && e->state != EP_QUEUED) { last = e; break; }
            if (!last) break;
            Remove((struct Node *)last);
            db_free_episode(last);
            dst->nep--;
        }
        db_recount(dst);
    }
    return added;
}
