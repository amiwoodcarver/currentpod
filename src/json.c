/* CurrentPod - minimal JSON-läsning för Apples sökgränssnitt.
 * Ingen trädbyggnad: vi letar upp "results"-arrayen och plockar fält
 * ur varje objekt på toppnivå i den.
 */
#include "currentpod.h"
#include <stdlib.h>
#include <ctype.h>

/* avkoda en JSON-sträng (p pekar på inledande citattecken); returnerar pekare efter avslutande */
static const char *json_string(const char *p, char *out, LONG max)
{
    LONG o = 0;
    if (*p != '"') { out[0] = 0; return p; }
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (*p == 'u') {
                ULONG cp = strtoul(p + 1, NULL, 16);
                char tmp[8];
                p += 5;
                if (cp < 0x80) tmp[0] = (char)cp, tmp[1] = 0;
                else if (cp < 0x800) { tmp[0] = 0xC0 | (cp >> 6); tmp[1] = 0x80 | (cp & 0x3F); tmp[2] = 0; }
                else { tmp[0] = 0xE0 | (cp >> 12); tmp[1] = 0x80 | ((cp >> 6) & 0x3F); tmp[2] = 0x80 | (cp & 0x3F); tmp[3] = 0; }
                if (o + 4 < max) { strcpy(out + o, tmp); o += strlen(tmp); }
                continue;
            }
            if (o < max - 1) {
                switch (*p) {
                    case 'n': out[o++] = '\n'; break;
                    case 't': out[o++] = ' '; break;
                    case 'r': break;
                    default: out[o++] = *p;
                }
            }
            p++;
            continue;
        }
        if (o < max - 1) out[o++] = *p;
        p++;
    }
    out[o] = 0;
    if (*p == '"') p++;
    utf8_to_latin1(out);
    return p;
}

/* hoppa över ett värde (objekt, array, sträng, tal, ...) */
static const char *json_skip(const char *p)
{
    int depth = 0;
    while (*p && isspace((UBYTE)*p)) p++;
    if (*p == '"') { char t[4]; return json_string(p, t, 0); }
    if (*p == '{' || *p == '[') {
        for (; *p; p++) {
            if (*p == '"') { char t[4]; p = json_string(p, t, 0); p--; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* läs ett objekt { "key": value, ... } och fyll en träff */
static const char *json_object(const char *p, struct SearchHit *h)
{
    while (*p && isspace((UBYTE)*p)) p++;
    if (*p != '{') return json_skip(p);
    p++;
    for (;;) {
        char key[48], val[URLMAX];
        while (*p && isspace((UBYTE)*p)) p++;
        if (*p == '}') return p + 1;
        if (*p == ',') { p++; continue; }
        if (*p != '"') return json_skip(p);
        p = json_string(p, key, sizeof(key));
        while (*p && isspace((UBYTE)*p)) p++;
        if (*p == ':') p++;
        while (*p && isspace((UBYTE)*p)) p++;
        if (*p == '"') {
            p = json_string(p, val, sizeof(val));
            if (!strcmp(key, "collectionName") || (!strcmp(key, "trackName") && !h->name[0])) strlcpy_(h->name, val, sizeof(h->name));
            else if (!strcmp(key, "artistName")) strlcpy_(h->author, val, sizeof(h->author));
            else if (!strcmp(key, "feedUrl")) strlcpy_(h->feedurl, val, sizeof(h->feedurl));
            else if (!strcmp(key, "artworkUrl100") || (!strcmp(key, "artworkUrl60") && !h->image[0])) strlcpy_(h->image, val, sizeof(h->image));
            else if (!strcmp(key, "primaryGenreName")) strlcpy_(h->genre, val, sizeof(h->genre));
            else if (!strcmp(key, "releaseDate")) strlcpy_(h->released, val, sizeof(h->released));
            else if (!strcmp(key, "collectionExplicitness")) h->explicit_ = !strcmp(val, "explicit");
        } else if (isdigit((UBYTE)*p) || *p == '-') {
            LONG v = atol(p);
            if (!strcmp(key, "trackCount")) h->count = v;
            else if (!strcmp(key, "collectionId")) h->apple = v;
            p = json_skip(p);
        } else p = json_skip(p);
    }
}

int apple_parse_results(const char *json, struct SearchHit **hits, int max)
{
    const char *p = strstr(json, "\"results\"");
    struct SearchHit *h;
    int n = 0;
    *hits = NULL;
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    h = xalloc(sizeof(*h) * max);
    if (!h) return 0;
    p++;
    while (*p && n < max) {
        while (*p && isspace((UBYTE)*p)) p++;
        if (*p == ']' || !*p) break;
        if (*p == ',') { p++; continue; }
        memset(&h[n], 0, sizeof(h[n]));
        p = json_object(p, &h[n]);
        if (h[n].feedurl[0] && h[n].name[0]) n++;
    }
    *hits = h;
    return n;
}
