/* CurrentPod - tolk för RSS 2.0 (med itunes:-taggar) och Atom 1.0.
 * En liten XML-skanner utan ambitioner: tillräcklig för poddflöden.
 */
#include "currentpod.h"
#include <stdlib.h>
#include <ctype.h>

#define MAXDEPTH 24
#define DESCMAX  3000
#define HARDMAX  300

struct Parser {
    const char *p, *end;
    char stack[MAXDEPTH][40];
    int  depth;
    int  utf8;
    struct Buf text;
    int  cdata;              /* texten kom (delvis) ur CDATA */
    struct Feed *feed;
    struct Episode *ep;
    int  in_item, in_channel, in_image, atom;
    int  maxep, nitems;
    const char *baseurl;
};

static const char *local(const char *tag)
{
    const char *c = strchr(tag, ':');
    return c ? c + 1 : tag;
}

static int is(const char *tag, const char *name)       /* lokalt namn */
{
    return !strcmp(local(tag), name);
}

static int isns(const char *tag, const char *ns, const char *name)   /* med prefix */
{
    LONG n = strlen(ns);
    return !strncmp(tag, ns, n) && tag[n] == ':' && !strcmp(tag + n + 1, name);
}

static int has_prefix(const char *tag)
{
    return strchr(tag, ':') != NULL;
}

/* hämta attributvärde ur en taggsträng "a href='x' b="y"" */
static int attr(const char *tagbody, const char *name, char *out, LONG max)
{
    const char *p = tagbody;
    LONG n = strlen(name);
    while ((p = stristr_(p, name))) {
        if ((p == tagbody || isspace((UBYTE)p[-1])) && (p[n] == '=' || isspace((UBYTE)p[n]))) {
            const char *v = p + n;
            char q;
            while (*v && *v != '=') v++;
            if (*v != '=') break;
            v++;
            while (isspace((UBYTE)*v)) v++;
            q = *v;
            if (q == '"' || q == '\'') {
                const char *e = strchr(v + 1, q);
                if (!e) break;
                strlcpy_(out, v + 1, (e - v) < max ? (e - v) : max);
            } else {
                const char *e = v;
                while (*e && !isspace((UBYTE)*e) && *e != '/' && *e != '>') e++;
                strlcpy_(out, v, (e - v + 1) < max ? (e - v + 1) : max);
            }
            xml_unescape(out);
            return 1;
        }
        p += n;
    }
    out[0] = 0;
    return 0;
}

static char *take_text(struct Parser *ps, int html)
{
    char *s;
    if (!ps->text.data) return NULL;
    s = ps->text.data;
    if (ps->utf8) utf8_to_latin1(s);
    if (html) { if (!ps->cdata) xml_unescape(s); html_strip(s); }
    else { xml_unescape(s); str_collapse_ws(s); str_trim(s); }
    if (strlen(s) > DESCMAX) s[DESCMAX] = 0;
    return s;
}

static void set_from_text(struct Parser *ps, char **dst, int html)
{
    char *s = take_text(ps, html);
    if (s && *s) xstrset(dst, s);
}

static void start_tag(struct Parser *ps, const char *name, const char *body, int selfclose)
{
    struct Feed *f = ps->feed;
    if (is(name, "rss") || is(name, "channel")) { ps->in_channel = 1; }
    else if (is(name, "feed") && ps->depth == 1) { ps->atom = 1; ps->in_channel = 1; }
    else if (is(name, "item") || (ps->atom && is(name, "entry"))) {
        ps->in_item = 1;
        ps->ep = NULL;
        if (ps->nitems < ps->maxep && ps->nitems < HARDMAX)
            ps->ep = db_new_episode(f);
        ps->nitems++;
    }
    else if (is(name, "image") && !ps->in_item && !has_prefix(name)) ps->in_image = 1;
    else if (ps->in_item && ps->ep) {
        struct Episode *e = ps->ep;
        char v[URLMAX];
        if (is(name, "enclosure") && !e->enc) {
            if (attr(body, "url", v, sizeof(v))) e->enc = xstrdup(v);
            if (attr(body, "length", v, sizeof(v))) e->size = atol(v);
            if (attr(body, "type", v, 64)) e->type = xstrdup(v);
        } else if (ps->atom && is(name, "link")) {
            char rel[32];
            attr(body, "rel", rel, sizeof(rel));
            if (strieq(rel, "enclosure") && !e->enc) {
                if (attr(body, "href", v, sizeof(v))) e->enc = xstrdup(v);
                if (attr(body, "length", v, sizeof(v))) e->size = atol(v);
                if (attr(body, "type", v, 64)) e->type = xstrdup(v);
            } else if ((!rel[0] || strieq(rel, "alternate")) && !e->link) {
                if (attr(body, "href", v, sizeof(v))) e->link = xstrdup(v);
            }
        } else if (isns(name, "itunes", "image") && !e->image) {
            if (attr(body, "href", v, sizeof(v))) e->image = xstrdup(v);
        } else if (isns(name, "media", "content") && !e->enc) {
            char t[64];
            attr(body, "type", t, sizeof(t));
            if (strnieq(t, "audio", 5) && attr(body, "url", v, sizeof(v))) {
                e->enc = xstrdup(v); e->type = xstrdup(t);
                if (attr(body, "fileSize", v, sizeof(v))) e->size = atol(v);
            }
        }
    } else if (ps->in_channel && !ps->in_item) {
        char v[URLMAX];
        if (isns(name, "itunes", "image")) {
            if (attr(body, "href", v, sizeof(v)) && !f->image) f->image = xstrdup(v);
        } else if (ps->atom && is(name, "link")) {
            char rel[32];
            attr(body, "rel", rel, sizeof(rel));
            if ((!rel[0] || strieq(rel, "alternate")) && !f->site && attr(body, "href", v, sizeof(v)))
                f->site = xstrdup(v);
        }
    }
    (void)selfclose;
}

static void end_tag(struct Parser *ps, const char *name)
{
    struct Feed *f = ps->feed;
    if (is(name, "item") || (ps->atom && is(name, "entry"))) {
        if (ps->ep) {
            struct Episode *e = ps->ep;
            if (!e->title) e->title = xstrdup("(utan titel)");
            if (!e->date) e->date = xstrdup("");
        }
        ps->ep = NULL; ps->in_item = 0;
        return;
    }
    if (is(name, "image") && !has_prefix(name)) { ps->in_image = 0; return; }
    if (ps->in_item && ps->ep) {
        struct Episode *e = ps->ep;
        if (isns(name, "itunes", "title")) set_from_text(ps, &e->title, 0);
        else if (is(name, "title") && !has_prefix(name) && !e->title) set_from_text(ps, &e->title, 0);
        else if (is(name, "pubDate") || (ps->atom && (is(name, "published") || (is(name, "updated") && !e->date)))) {
            char *s = take_text(ps, 0), iso[24];
            if (s && parse_rfc822_date(s, iso)) xstrset(&e->date, iso);
        }
        else if (is(name, "guid") || (ps->atom && is(name, "id"))) set_from_text(ps, &e->guid, 0);
        else if (is(name, "link") && !has_prefix(name) && !ps->atom) set_from_text(ps, &e->link, 0);
        else if (isns(name, "itunes", "duration")) { char *s = take_text(ps, 0); if (s) e->dur = parse_duration(s); }
        else if (isns(name, "itunes", "episode")) { char *s = take_text(ps, 0); if (s) e->epnum = atol(s); }
        else if (isns(name, "itunes", "season")) { char *s = take_text(ps, 0); if (s) e->season = atol(s); }
        else if (isns(name, "itunes", "episodeType")) {
            char *s = take_text(ps, 0);
            if (s) e->etype = strieq(s, "trailer") ? ET_TRAILER : strieq(s, "bonus") ? ET_BONUS : ET_FULL;
        }
        else if (isns(name, "itunes", "explicit")) { char *s = take_text(ps, 0); if (s) e->explicit_ = (strieq(s, "yes") || strieq(s, "true")); }
        else if (isns(name, "content", "encoded") || (ps->atom && is(name, "content"))) {
            char *s = take_text(ps, 1);
            if (s && *s && (!e->desc || strlen(s) > strlen(e->desc))) xstrset(&e->desc, s);
        }
        else if (is(name, "description") || isns(name, "itunes", "summary") || (ps->atom && is(name, "summary"))) {
            char *s = take_text(ps, 1);
            if (s && *s && (!e->desc || strlen(s) > strlen(e->desc))) xstrset(&e->desc, s);
        }
    } else if (ps->in_channel && !ps->in_item) {
        if (ps->in_image) {
            if (is(name, "url") && !f->image) set_from_text(ps, &f->image, 0);
        }
        else if (is(name, "title") && !has_prefix(name) && ps->depth <= 3) set_from_text(ps, &f->name, 0);
        else if (is(name, "link") && !has_prefix(name) && !ps->atom && ps->depth <= 3) set_from_text(ps, &f->site, 0);
        else if (isns(name, "itunes", "author")) set_from_text(ps, &f->author, 0);
        else if (is(name, "managingEditor") && !f->author) set_from_text(ps, &f->author, 0);
        else if (is(name, "language")) set_from_text(ps, &f->lang, 0);
        else if (isns(name, "itunes", "summary") || is(name, "description") || (ps->atom && is(name, "subtitle"))) {
            char *s = take_text(ps, 1);
            if (s && *s && (!f->desc || strlen(s) > strlen(f->desc))) xstrset(&f->desc, s);
        }
        else if (isns(name, "itunes", "type")) { char *s = take_text(ps, 0); if (s) f->serial = strieq(s, "serial"); }
    }
}

struct Feed *rss_parse(const char *xml, LONG len, const char *baseurl, int maxep)
{
    struct Parser ps;
    const char *p = xml, *end = xml + len;
    memset(&ps, 0, sizeof(ps));
    ps.feed = db_new_feed();
    if (!ps.feed) return NULL;
    ps.maxep = maxep > 0 ? maxep : HARDMAX;
    ps.baseurl = baseurl;
    ps.utf8 = 1;
    {
        const char *enc = stristr_(xml, "encoding=");
        if (enc && enc < xml + 200 && (strnieq(enc + 10, "iso-8859", 8) || strnieq(enc + 10, "windows-125", 11))) ps.utf8 = 0;
    }
    while (p < end) {
        if (*p == '<') {
            if (!strncmp(p, "<![CDATA[", 9)) {
                const char *e = strstr(p + 9, "]]>");
                if (!e) break;
                buf_add(&ps.text, p + 9, e - (p + 9));
                ps.cdata = 1;
                p = e + 3;
            } else if (!strncmp(p, "<!--", 4)) {
                const char *e = strstr(p + 4, "-->");
                p = e ? e + 3 : end;
            } else if (p[1] == '?' || p[1] == '!') {
                const char *e = strchr(p, '>');
                p = e ? e + 1 : end;
            } else {
                const char *e = p + 1, *q;
                char name[40], body[1024];
                int closing = (*e == '/'), selfclose = 0;
                LONG n;
                if (closing) e++;
                q = e;
                while (q < end && !isspace((UBYTE)*q) && *q != '>' && *q != '/') q++;
                n = q - e; if (n > 39) n = 39;
                memcpy(name, e, n); name[n] = 0;
                /* resten av taggen (attribut), med hänsyn till citattecken */
                {
                    const char *b = q; char quote = 0;
                    while (b < end) {
                        if (quote) { if (*b == quote) quote = 0; }
                        else if (*b == '"' || *b == '\'') quote = *b;
                        else if (*b == '>') break;
                        b++;
                    }
                    if (b > q && b[-1] == '/') selfclose = 1;
                    n = b - q; if (n > 1023) n = 1023;
                    memcpy(body, q, n); body[n] = 0;
                    p = b < end ? b + 1 : end;
                }
                if (closing) {
                    end_tag(&ps, name);
                    if (ps.depth > 0) ps.depth--;
                    ps.text.len = 0; if (ps.text.data) ps.text.data[0] = 0; ps.cdata = 0;
                } else {
                    ps.text.len = 0; if (ps.text.data) ps.text.data[0] = 0; ps.cdata = 0;
                    if (ps.depth < MAXDEPTH) strlcpy_(ps.stack[ps.depth], name, 40);
                    ps.depth++;
                    start_tag(&ps, name, body, selfclose);
                    if (selfclose) { end_tag(&ps, name); ps.depth--; }
                }
            }
        } else {
            const char *e = p;
            while (e < end && *e != '<') e++;
            if (ps.text.len < DESCMAX * 2) buf_add(&ps.text, p, e - p);
            p = e;
        }
    }
    buf_free(&ps.text);
    if (ps.ep) {
        /* dokumentet tog slut mitt i ett avsnitt (kapat flöde): släng det om ljudfilen saknas */
        if (!ps.ep->enc) { Remove((struct Node *)ps.ep); ps.feed->nep--; db_free_episode(ps.ep); ps.ep = NULL; ps.in_item = 0; }
        else end_tag(&ps, "item");
    }
    if (!ps.feed->name) {
        if (ps.nitems == 0 && ps.feed->nep == 0) { db_free_feed(ps.feed); return NULL; }
        ps.feed->name = xstrdup(_(MSG_UNNAMED_PODCAST));
    }
    /* tomma datum: ge dem dagens datum så sorteringen fungerar */
    {
        struct Episode *e;
        for (e = (struct Episode *)ps.feed->episodes.mlh_Head; e->node.mln_Succ; e = (struct Episode *)e->node.mln_Succ)
            if (e->date && !e->date[0]) {
                struct DateStamp ds; struct DateTime dt; char d[LEN_DATSTRING], t[LEN_DATSTRING];
                DateStamp(&ds);
                memset(&dt, 0, sizeof(dt));
                dt.dat_Stamp = ds; dt.dat_Format = FORMAT_CDN; dt.dat_StrDate = (STRPTR)d; dt.dat_StrTime = (STRPTR)t;
                if (DateToStr(&dt)) {
                    char iso[24];
                    sprintf(iso, "20%.2s-%.2s-%.2s %s", d + 6, d + 3, d, t);
                    xstrset(&e->date, iso);
                }
            }
    }
    db_recount(ps.feed);
    return ps.feed;
}
