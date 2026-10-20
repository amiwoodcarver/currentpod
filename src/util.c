/* CurrentPod - hjälpfunktioner: minne, strängar, UTF-8, URL, datum, filer */
#include "currentpod.h"
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>

int cp_debug = 0;

/* ------------------------------------------------------------ minne */

APTR xalloc(ULONG size)
{
    return AllocVec(size ? size : 1, MEMF_ANY | MEMF_CLEAR);
}

void xfree(APTR p)
{
    if (p) FreeVec(p);
}

char *xstrdup(const char *s)
{
    char *d;
    if (!s) return NULL;
    d = xalloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

char *xstrndup(const char *s, LONG n)
{
    char *d;
    if (!s) return NULL;
    d = xalloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}

void xstrset(char **dst, const char *src)
{
    char *n = xstrdup(src);
    xfree(*dst);
    *dst = n;
}

/* ------------------------------------------------------------ strängar */

void strlcpy_(char *dst, const char *src, LONG size)
{
    LONG n;
    if (size <= 0) return;
    if (!src) { dst[0] = 0; return; }
    n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void strlcat_(char *dst, const char *src, LONG size)
{
    LONG l = strlen(dst);
    if (l < size - 1) strlcpy_(dst + l, src, size - l);
}

int strieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((UBYTE)*a) != tolower((UBYTE)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

int strnieq(const char *a, const char *b, LONG n)
{
    while (n-- > 0) {
        if (tolower((UBYTE)*a) != tolower((UBYTE)*b)) return 0;
        if (!*a) return 1;
        a++; b++;
    }
    return 1;
}

char *stristr_(const char *hay, const char *needle)
{
    LONG n = strlen(needle);
    if (!n) return (char *)hay;
    for (; *hay; hay++)
        if (strnieq(hay, needle, n)) return (char *)hay;
    return NULL;
}

void str_trim(char *s)
{
    char *p = s, *e;
    while (*p && isspace((UBYTE)*p)) p++;
    e = p + strlen(p);
    while (e > p && isspace((UBYTE)e[-1])) e--;
    memmove(s, p, e - p);
    s[e - p] = 0;
}

void str_collapse_ws(char *s)
{
    char *d = s;
    int sp = 0;
    for (; *s; s++) {
        if (isspace((UBYTE)*s) && *s != '\n') {
            if (!sp) *d++ = ' ';
            sp = 1;
        } else {
            *d++ = *s;
            sp = (*s == '\n');
        }
    }
    *d = 0;
}

/* UTF-8 -> ISO-8859-1 på plats. Ogiltiga sekvenser lämnas som de är. */
void utf8_to_latin1(char *s)
{
    UBYTE *r = (UBYTE *)s, *w = (UBYTE *)s;
    while (*r) {
        UBYTE c = *r;
        if (c < 0x80) { *w++ = c; r++; }
        else if ((c & 0xE0) == 0xC0 && (r[1] & 0xC0) == 0x80) {
            ULONG cp = ((c & 0x1F) << 6) | (r[1] & 0x3F);
            *w++ = cp < 256 ? (UBYTE)cp : '?';
            r += 2;
        } else if ((c & 0xF0) == 0xE0 && (r[1] & 0xC0) == 0x80 && (r[2] & 0xC0) == 0x80) {
            ULONG cp = ((c & 0x0F) << 12) | ((r[1] & 0x3F) << 6) | (r[2] & 0x3F);
            /* några vanliga tecken utanför Latin-1 */
            if (cp == 0x2019 || cp == 0x2018) *w++ = '\'';
            else if (cp == 0x201C || cp == 0x201D || cp == 0x201E) *w++ = '"';
            else if (cp == 0x2013 || cp == 0x2014) *w++ = '-';
            else if (cp == 0x2026) { *w++ = '.'; *w++ = '.'; *w++ = '.'; }
            else if (cp == 0x20AC) *w++ = 'E';
            else if (cp == 0x2022) *w++ = '*';
            else if (cp == 0x00A0 || cp == 0x2009 || cp == 0x200B) *w++ = ' ';
            else *w++ = '?';
            r += 3;
        } else if ((c & 0xF8) == 0xF0 && (r[1] & 0xC0) == 0x80 && (r[2] & 0xC0) == 0x80 && (r[3] & 0xC0) == 0x80) {
            *w++ = '?';            /* emoji och liknande */
            r += 4;
        } else { *w++ = c; r++; }
    }
    *w = 0;
}

static const struct { const char *name; UBYTE ch; } entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "nbsp", ' ' }, { "aring", 0xE5 }, { "auml", 0xE4 }, { "ouml", 0xF6 },
    { "Aring", 0xC5 }, { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "eacute", 0xE9 },
    { "egrave", 0xE8 }, { "uuml", 0xFC }, { "Uuml", 0xDC }, { "szlig", 0xDF },
    { "ndash", '-' }, { "mdash", '-' }, { "hellip", '.' }, { "rsquo", '\'' },
    { "lsquo", '\'' }, { "rdquo", '"' }, { "ldquo", '"' }, { "copy", 0xA9 },
    { "deg", 0xB0 }, { "euro", 'E' }, { NULL, 0 }
};

/* Avkoda XML/HTML-entiteter på plats. Numeriska entiteter > 255 blir '?',
   utom de vanligaste typografiska. Resultatet är Latin-1 om källan var det;
   kör utf8_to_latin1 före för UTF-8-texter. */
void xml_unescape(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            char *semi = strchr(r, ';');
            if (semi && semi - r <= 10) {
                LONG cp = -1;
                if (r[1] == '#') {
                    cp = (r[2] == 'x' || r[2] == 'X') ? strtol(r + 3, NULL, 16) : strtol(r + 2, NULL, 10);
                } else {
                    int i;
                    for (i = 0; entities[i].name; i++) {
                        LONG n = strlen(entities[i].name);
                        if (semi - r - 1 == n && !strncmp(r + 1, entities[i].name, n)) { cp = entities[i].ch; break; }
                    }
                }
                if (cp >= 0) {
                    if (cp < 256) *w++ = (char)cp;
                    else if (cp == 0x2019 || cp == 0x2018) *w++ = '\'';
                    else if (cp == 0x201C || cp == 0x201D) *w++ = '"';
                    else if (cp == 0x2013 || cp == 0x2014) *w++ = '-';
                    else if (cp == 0x2026) { *w++ = '.'; *w++ = '.'; *w++ = '.'; }
                    else *w++ = '?';
                    r = semi + 1;
                    continue;
                }
            }
        }
        *w++ = *r++;
    }
    *w = 0;
}

/* Ta bort HTML ur en beskrivning: <p>, <br>, <li> blir radbrytningar,
   andra taggar försvinner, entiteter avkodas, blanka slås ihop. */
void html_strip(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '<') {
            char *e = strchr(r, '>');
            if (!e) break;
            if (strnieq(r, "<p", 2) || strnieq(r, "<br", 3) || strnieq(r, "<li", 3) ||
                strnieq(r, "</p", 3) || strnieq(r, "<div", 4) || strnieq(r, "<h", 2) || strnieq(r, "</h", 3))
                *w++ = '\n';
            r = e + 1;
        } else *w++ = *r++;
    }
    *w = 0;
    xml_unescape(s);
    str_collapse_ws(s);
    /* max två radbrytningar i rad */
    r = w = s;
    {
        int nl = 0;
        for (; *r; r++) {
            if (*r == '\n') { if (++nl > 2) continue; }
            else nl = 0;
            *w++ = *r;
        }
        *w = 0;
    }
    str_trim(s);
}

/* Latin-1-sträng -> UTF-8 -> procentkodad för query-delen av en URL */
LONG url_encode_query(const char *src, char *dst, LONG dstsize)
{
    static const char hex[] = "0123456789ABCDEF";
    LONG o = 0;
    for (; *src && o < dstsize - 7; src++) {
        UBYTE c = (UBYTE)*src;
        UBYTE u[2]; int n = 1, i;
        if (c == ' ') { dst[o++] = '+'; continue; }
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') { dst[o++] = c; continue; }
        if (c < 0x80) { u[0] = c; }
        else { u[0] = 0xC0 | (c >> 6); u[1] = 0x80 | (c & 0x3F); n = 2; }
        for (i = 0; i < n; i++) {
            dst[o++] = '%'; dst[o++] = hex[u[i] >> 4]; dst[o++] = hex[u[i] & 15];
        }
    }
    dst[o] = 0;
    return o;
}

/* Dela upp en URL. path får alltid minst "/" */
int url_split(const char *url, char *scheme, char *host, LONG *port, char *path, LONG max)
{
    const char *p, *h, *e;
    LONG n;
    p = strstr(url, "://");
    if (!p) return 0;
    n = p - url; if (n >= max) return 0;
    memcpy(scheme, url, n); scheme[n] = 0;
    h = p + 3;
    e = h;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    n = e - h; if (n >= max || n == 0) return 0;
    memcpy(host, h, n); host[n] = 0;
    *port = strieq(scheme, "https") ? 443 : 80;
    {
        char *c = strrchr(host, ':');
        if (c) { *port = atol(c + 1); *c = 0; }
    }
    if (*e == 0 || *e == '#') strlcpy_(path, "/", max);
    else if (*e == '?') { path[0] = '/'; strlcpy_(path + 1, e, max - 1); }
    else strlcpy_(path, e, max);
    {
        char *f = strchr(path, '#');
        if (f) *f = 0;
    }
    return 1;
}

/* Gör en relativ Location-adress absolut utifrån base */
void url_resolve(const char *base, const char *rel, char *out, LONG max)
{
    char scheme[16], host[NAMEMAX], path[LONGURL];
    LONG port;
    if (strstr(rel, "://")) { strlcpy_(out, rel, max); return; }
    if (!url_split(base, scheme, host, &port, path, sizeof(path))) { strlcpy_(out, rel, max); return; }
    if (rel[0] == '/' && rel[1] == '/') { sprintf(out, "%s:%s", scheme, rel); return; }
    if (rel[0] == '/') {
        if ((port == 80 && strieq(scheme, "http")) || (port == 443 && strieq(scheme, "https")))
            sprintf(out, "%s://%s%s", scheme, host, rel);
        else
            sprintf(out, "%s://%s:%ld%s", scheme, host, port, rel);
        return;
    }
    {
        char *q = strchr(path, '?'); if (q) *q = 0;
        q = strrchr(path, '/'); if (q) q[1] = 0;
        if ((port == 80 && strieq(scheme, "http")) || (port == 443 && strieq(scheme, "https")))
            sprintf(out, "%s://%s%s%s", scheme, host, path, rel);
        else
            sprintf(out, "%s://%s:%ld%s%s", scheme, host, port, path, rel);
    }
}

/* ------------------------------------------------------------ datum och tid */

static const char *months[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };

/* "Fri, 12 Sep 2026 06:00:02 GMT", "12 Sep 2026 06:00:02 +0200", eller ISO "2026-09-12T06:00:02Z" */
int parse_rfc822_date(const char *s, char *iso)
{
    int d = 0, m = -1, y = 0, hh = 0, mm = 0, ss = 0, i;
    const char *p = s;
    while (*p == ' ') p++;
    if (isdigit((UBYTE)p[0]) && isdigit((UBYTE)p[1]) && isdigit((UBYTE)p[2]) && isdigit((UBYTE)p[3]) && p[4] == '-') {
        y = atoi(p); m = atoi(p + 5) - 1; d = atoi(p + 8);
        if (p[10] == 'T' || p[10] == ' ') { hh = atoi(p + 11); mm = atoi(p + 14); ss = atoi(p + 17); }
    } else {
        if (isalpha((UBYTE)*p)) { while (*p && *p != ' ') p++; while (*p == ' ') p++; }   /* veckodag */
        d = atoi(p);
        while (*p && *p != ' ') p++; while (*p == ' ') p++;
        for (i = 0; i < 12; i++) if (strnieq(p, months[i], 3)) { m = i; break; }
        while (*p && *p != ' ') p++; while (*p == ' ') p++;
        y = atoi(p);
        if (y < 100) y += (y < 70) ? 2000 : 1900;
        while (*p && *p != ' ') p++; while (*p == ' ') p++;
        if (isdigit((UBYTE)*p)) { hh = atoi(p); if (p[2] == ':') { mm = atoi(p + 3); if (p[5] == ':') ss = atoi(p + 6); } }
    }
    if (m < 0 || m > 11 || d < 1 || d > 31 || y < 1990 || y > 2100) return 0;
    sprintf(iso, "%04ld-%02ld-%02ld %02ld:%02ld:%02ld", y, m + 1, d, hh, mm, ss);
    return 1;
}

void iso_to_display(const char *iso, char *out)
{
    int m;
    if (!iso || strlen(iso) < 10) { strcpy(out, "-"); return; }
    m = atoi(iso + 5) - 1;
    if (m < 0 || m > 11) m = 0;
    sprintf(out, "%ld %s %.4s", atoi(iso + 8), cp_month(m + 1), iso);
}

LONG parse_duration(const char *s)
{
    LONG parts[3] = { 0, 0, 0 }, n = 0;
    const char *p = s;
    if (!s) return 0;
    while (*p == ' ') p++;
    if (!*p) return 0;
    while (n < 3) {
        parts[n++] = atol(p);
        while (*p && *p != ':') p++;
        if (*p != ':') break;
        p++;
    }
    if (n == 1) return parts[0];
    if (n == 2) return parts[0] * 60 + parts[1];
    return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

void fmt_time(LONG secs, char *out)
{
    if (secs < 0) secs = 0;
    if (secs >= 3600) sprintf(out, "%ld:%02ld:%02ld", secs / 3600, (secs / 60) % 60, secs % 60);
    else sprintf(out, "%ld:%02ld", secs / 60, secs % 60);
}

void fmt_size(LONG bytes, char *out)
{
    if (bytes < 0) { strcpy(out, "?"); return; }
    if (bytes >= 1024 * 1024) sprintf(out, "%ld%s%ld MB", bytes / (1024 * 1024), cp_decimal(), ((bytes % (1024 * 1024)) * 10) / (1024 * 1024));
    else if (bytes >= 1024) sprintf(out, "%ld kB", bytes / 1024);
    else sprintf(out, "%ld B", bytes);
}

/* Filnamn ur titel: ersätt tecken AmigaDOS inte gillar, korta till maxlen */
void safe_filename(const char *src, char *dst, LONG maxlen)
{
    LONG o = 0;
    for (; *src && o < maxlen; src++) {
        UBYTE c = (UBYTE)*src;
        if (c == '/' || c == ':' || c == '#' || c == '?' || c == '*' || c == '"' || c == '|' || c < 32) c = '_';
        if (c == '\'' || c == '\\') continue;
        dst[o++] = c;
    }
    dst[o] = 0;
    str_trim(dst);
    if (!dst[0]) strcpy(dst, _(MSG_EPISODE_WORD));
}

/* ------------------------------------------------------------ filer */

LONG file_size(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    LONG size = -1;
    if (l) {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        if (fib) {
            if (Examine(l, fib) && fib->fib_DirEntryType < 0) size = fib->fib_Size;
            FreeDosObject(DOS_FIB, fib);
        }
        UnLock(l);
    }
    return size;
}

int file_exists(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) { UnLock(l); return 1; }
    return 0;
}

int make_dirs(const char *path)
{
    char tmp[PATHMAX];
    char *p;
    BPTR l;
    strlcpy_(tmp, path, sizeof(tmp));
    p = strchr(tmp, ':');
    p = p ? p + 1 : tmp;
    for (;;) {
        char *s = strchr(p, '/');
        if (s) *s = 0;
        if (*p) {
            l = Lock((STRPTR)tmp, ACCESS_READ);
            if (l) UnLock(l);
            else {
                l = CreateDir((STRPTR)tmp);
                if (!l) return 0;
                UnLock(l);
            }
        }
        if (!s) break;
        *s = '/';
        p = s + 1;
    }
    return 1;
}

char *load_file(const char *path, LONG *len)
{
    LONG size = file_size(path);
    char *d;
    BPTR f;
    if (size < 0) return NULL;
    d = xalloc(size + 1);
    if (!d) return NULL;
    f = Open((STRPTR)path, MODE_OLDFILE);
    if (!f) { xfree(d); return NULL; }
    if (Read(f, d, size) != size) { Close(f); xfree(d); return NULL; }
    Close(f);
    d[size] = 0;
    if (len) *len = size;
    return d;
}

int save_file(const char *path, const char *data, LONG len)
{
    char tmp[PATHMAX];
    BPTR f;
    sprintf(tmp, "%s.tmp", path);
    f = Open((STRPTR)tmp, MODE_NEWFILE);
    if (!f) return 0;
    if (Write(f, (APTR)data, len) != len) { Close(f); DeleteFile((STRPTR)tmp); return 0; }
    Close(f);
    DeleteFile((STRPTR)path);
    if (!Rename((STRPTR)tmp, (STRPTR)path)) return 0;
    return 1;
}

void add_part(char *dst, const char *dir, const char *file, LONG max)
{
    strlcpy_(dst, dir, max);
    AddPart((STRPTR)dst, (STRPTR)file, max);
}

char log_path[PATHMAX] = "PROGDIR:debug.log";

/* Gör en sökväg absolut (PROGDIR: finns bara i huvudprocessen) */
int abs_path(const char *in, char *out, LONG max)
{
    BPTR l = Lock((STRPTR)in, ACCESS_READ);
    if (!l) { strlcpy_(out, in, max); return 0; }
    if (!NameFromLock(l, (STRPTR)out, max)) strlcpy_(out, in, max);
    UnLock(l);
    return 1;
}

static struct SignalSemaphore log_sem;
static int log_sem_ready;

void log_msg(const char *fmt, ...)
{
    va_list ap;
    static char line[LONGURL + 512];      /* adresser kan vara 4 KB; bufferten delas av alla tasks, därav semaforen */
    BPTR f;
    if (!cp_debug) return;
    if (!log_sem_ready) { InitSemaphore(&log_sem); log_sem_ready = 1; }   /* första anropet sker i huvudtasken före processerna */
    ObtainSemaphore(&log_sem);
    va_start(ap, fmt);
    vsprintf(line, fmt, ap);
    va_end(ap);
    f = Open((STRPTR)log_path, MODE_READWRITE);
    if (f) {
        Seek(f, 0, OFFSET_END);
        Write(f, line, strlen(line));
        Write(f, "\n", 1);
        Close(f);
    }
    ReleaseSemaphore(&log_sem);
}

/* ------------------------------------------------------------ Buf */

int buf_add(struct Buf *b, const char *data, LONG len)
{
    if (b->len + len + 1 > b->cap) {
        LONG ncap = b->cap ? b->cap * 2 : 4096;
        char *n;
        while (ncap < b->len + len + 1) ncap *= 2;
        n = xalloc(ncap);
        if (!n) return 0;
        if (b->data) { memcpy(n, b->data, b->len); xfree(b->data); }
        b->data = n;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = 0;
    return 1;
}

int buf_adds(struct Buf *b, const char *s)
{
    return buf_add(b, s, strlen(s));
}

void buf_free(struct Buf *b)
{
    xfree(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}
