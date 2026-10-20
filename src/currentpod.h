/* CurrentPod - poddklient för AmigaOS 3.x med MUI
 * Gemensamma typer och prototyper.
 */
#ifndef CURRENTPOD_H
#define CURRENTPOD_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>
#include <string.h>
#include <stdio.h>

#include "strings.h"      /* genererad ur CurrentPod.cd av tools/mkcatalog.py */
#define _(id) ((char *)cp_str(id))
void  cp_locale_open(const char *lang);   /* NULL: enligt Locale-inställningarna */
void  cp_locale_close(void);
const char *cp_str(LONG id);
const char *cp_month(int m);              /* "jan".."dec" enligt locale, m = 1..12 */
const char *cp_decimal(void);             /* "." eller "," enligt locale */

#define CP_VERSION  "1.0"
#define CP_DATE     "19.10.2026"
#define CP_USERAGENT "CurrentPod/" CP_VERSION " (AmigaOS 3.1; 68k)"

#define URLMAX   1024   /* avsnittsadresser med spårningsprefix kan vara långa */
#define LONGURL  4096   /* omdirigeringar landar på signerade CloudFront-adresser; Retro Hour: 2 078 tecken */
#define PATHMAX  256
#define NAMEMAX  120

/* ---------------------------------------------------------------- util.c */
APTR  xalloc(ULONG size);            /* AllocVec, nollställt; NULL om slut */
void  xfree(APTR p);
char *xstrdup(const char *s);        /* NULL -> NULL */
char *xstrndup(const char *s, LONG n);
void  xstrset(char **dst, const char *src);   /* byt ut sträng */
void  strlcpy_(char *dst, const char *src, LONG size);
void  strlcat_(char *dst, const char *src, LONG size);
int   strieq(const char *a, const char *b);
int   strnieq(const char *a, const char *b, LONG n);
char *stristr_(const char *hay, const char *needle);
void  str_trim(char *s);
void  str_collapse_ws(char *s);       /* flera blanka -> ett mellanslag */
void  utf8_to_latin1(char *s);        /* på plats, '?' för tecken > 255 */
void  html_strip(char *s);            /* ta bort taggar, avkoda entiteter, p/br -> radbryt */
void  xml_unescape(char *s);          /* &amp; &lt; &#NNN; &#xHH; ... */
LONG  url_encode_query(const char *src, char *dst, LONG dstsize);   /* Latin-1 -> UTF-8 -> %XX */
int   url_split(const char *url, char *scheme, char *host, LONG *port, char *path, LONG max);
void  url_resolve(const char *base, const char *rel, char *out, LONG max);
int   parse_rfc822_date(const char *s, char *iso);   /* -> "ÅÅÅÅ-MM-DD HH:MM:SS", 0 vid fel */
void  iso_to_display(const char *iso, char *out);     /* "12 sep 2026" */
LONG  parse_duration(const char *s);                  /* "1:02:04", "62:04", "3724" -> sekunder */
void  fmt_time(LONG secs, char *out);                 /* "1:02:04" / "12:31" */
void  fmt_size(LONG bytes, char *out);                /* "31,8 MB" */
void  safe_filename(const char *src, char *dst, LONG maxlen);
LONG  file_size(const char *path);                    /* -1 om saknas */
int   file_exists(const char *path);
int   make_dirs(const char *path);                    /* skapar alla led */
char *load_file(const char *path, LONG *len);         /* xalloc:ad, nollterminerad */
int   save_file(const char *path, const char *data, LONG len);
void  add_part(char *dst, const char *dir, const char *file, LONG max);  /* AddPart */
void  log_msg(const char *fmt, ...);                  /* PROGDIR:debug.log om DEBUG */
extern int cp_debug;
extern char log_path[PATHMAX];
int   abs_path(const char *in, char *out, LONG max);        /* PROGDIR:x -> Work:.../x */

/* dynamisk buffert */
struct Buf { char *data; LONG len, cap; };
int  buf_add(struct Buf *b, const char *data, LONG len);
int  buf_adds(struct Buf *b, const char *s);
void buf_free(struct Buf *b);

/* ---------------------------------------------------------------- db.c */
enum { EP_UNHEARD = 0, EP_QUEUED, EP_DOWNLOADED, EP_HEARD, EP_ERROR, EP_PARTIAL, EP_CONVERTING };
enum { ET_FULL = 0, ET_TRAILER, ET_BONUS };

struct Episode {
    struct MinNode node;
    struct Feed *feed;
    char *guid, *title, *date, *enc, *type, *link, *desc, *file, *image;
    char *pcm;         /* konverterad 8SVX-fil, eller NULL */
    LONG  size;        /* byte enligt flödet */
    LONG  dur;         /* sekunder, 0 = okänt */
    LONG  season, epnum;
    int   etype;       /* ET_xxx */
    int   state;       /* EP_xxx */
    LONG  pos;         /* sparad position i sekunder */
    int   isnew;       /* sedd första gången vid senaste uppdateringen */
    int   explicit_;
};

struct Feed {
    struct MinNode node;
    int   id;
    char *name, *url, *site, *author, *desc, *lang, *image, *etag, *modified, *updated, *lasterr;
    LONG  apple;       /* Apple Podcasts-ID eller 0 */
    int   autodl;
    int   serial;      /* itunes:type serial -> äldst först */
    struct MinList episodes;
    int   nep, unheard, hasnew;
    int   updating;    /* jobb pågår */
};

extern struct MinList feeds;
extern char feeds_dir[PATHMAX];

void  db_init(const char *dir);
int   db_load(void);
int   db_save_feed(struct Feed *f);
int   db_save_index(void);
struct Feed *db_new_feed(void);
void  db_free_feed(struct Feed *f);
void  db_remove_feed(struct Feed *f);     /* ur listan + filer */
struct Feed *db_add_feed(const char *url, const char *name);   /* ny podd med nytt id, i listan */
struct Feed *db_find_url(const char *url);
struct Feed *db_find_id(int id);
struct Episode *db_new_episode(struct Feed *f);
void  db_free_episode(struct Episode *e);
int   db_merge(struct Feed *dst, struct Feed *fresh, int maxep);   /* nya avsnitt in, returnerar antal nya */
void  db_recount(struct Feed *f);
int   db_feed_count(void);

/* ---------------------------------------------------------------- http.c (körs i arbetarprocessen) */
struct HttpOpts {
    const char *etag, *modified;     /* villkorliga headers (kan vara NULL) */
    LONG  range_from;                /* >0: Range: bytes=N- */
    LONG  maxbytes;                  /* tak för minnesbuffert */
    const char *tofile;              /* NULL: till minne; annars skriv till fil (append vid range) */
    volatile LONG *progress, *total; /* uppdateras under hämtning */
    volatile int *abort;
    int   verify;                    /* kontrollera certifikat */
    int   truncate_ok;               /* minnesläsning: kapa vid maxbytes i stället för att misslyckas */
};
struct HttpResult {
    int   status;                    /* HTTP-status, 0 = nätfel */
    char  errstr[96];
    char  etag[80], modified[48], ctype[64], location[LONGURL];
    LONG  clen;                      /* Content-Length (-1 okänt) */
    struct Buf body;                 /* vid minnesläsning */
    int   resumed;                   /* 206 */
    int   truncated;                 /* kroppen kapades vid maxbytes */
};
int  net_open(void);                 /* öppnar bsdsocket + AmiSSL för anropande task */
void net_close(void);
int  http_get(const char *url, struct HttpOpts *o, struct HttpResult *r);   /* följer omdirigeringar */
void http_free(struct HttpResult *r);

/* ---------------------------------------------------------------- rss.c */
struct Feed *rss_parse(const char *xml, LONG len, const char *baseurl, int maxep);

/* ---------------------------------------------------------------- json.c */
struct SearchHit {
    char name[NAMEMAX], author[NAMEMAX], feedurl[URLMAX], image[URLMAX], genre[40], released[12];
    LONG count, apple;
    int  explicit_;
};
int apple_parse_results(const char *json, struct SearchHit **hits, int max);  /* antal träffar */

/* ---------------------------------------------------------------- worker.c */
enum { JOB_FEED = 1, JOB_SEARCH, JOB_LOOKUP, JOB_DOWNLOAD, JOB_QUIT };
struct Job {
    struct Message msg;
    int   type;
    int   feedid;
    struct Episode *ep;              /* bara referens, används av huvudprogrammet */
    char  url[URLMAX];
    char  file[PATHMAX];
    char  etag[80], modified[48];
    char  term[128], country[4];
    int   maxep;
    /* resultat */
    int   error;
    char  errstr[96];
    int   status;
    struct Feed *feed;               /* JOB_FEED: tolkat flöde (ägs av mottagaren) */
    struct SearchHit *hits;          /* JOB_SEARCH/LOOKUP */
    int   nhits;
    volatile LONG bytes, total;
    volatile int abort;
    int   notmodified;
    int   truncated;                 /* JOB_FEED: flödet var större än taket, bara början lästes */
};
int  worker_start(struct MsgPort *replyport);
void worker_stop(void);
void worker_submit(struct Job *j);
extern struct MsgPort *worker_port;

/* ---------------------------------------------------------------- player.c */
enum { PC_PLAY = 1, PC_STOP, PC_PAUSE, PC_RESUME, PC_SEEK, PC_VOLUME, PC_QUIT, PC_SETQUALITY };
enum { PS_STOPPED = 0, PS_PLAYING, PS_PAUSED, PS_ERROR, PS_FINISHED };
enum { Q_AUTO = 0, Q_LOW, Q_MEDIUM, Q_HIGH, Q_FULL };

struct PlayerCmd {
    struct Message msg;
    int   cmd;
    char  file[PATHMAX];
    LONG  arg;             /* startposition i s / sökposition / volym */
    int   error;
    char  errstr[80];
};
struct PlayerStatus {     /* skrivs av spelaren, läses av GUI:t */
    volatile int  state;
    volatile LONG pos;     /* sekunder */
    volatile LONG dur;     /* sekunder */
    volatile LONG freq, channels, bitrate, quality, bits;
    volatile LONG underruns;
    volatile LONG cpu;     /* eftersläpning i ms */
    volatile LONG pcm;     /* 1: spelar en 8SVX-fil, ingen avkodning */
    char errstr[80];
    char libname[40];
};
extern struct PlayerStatus pstat;
extern struct MsgPort *player_port;
int  player_start(struct MsgPort *replyport, int quality, int bits14);
void player_send(struct PlayerCmd *c);
void player_stop(void);

/* ---------------------------------------------------------------- convert.c */
enum { CC_CONVERT = 1, CC_QUIT };
enum { CV_IDLE = 0, CV_RUNNING, CV_DONE, CV_ERROR };
struct ConvCmd {
    struct Message msg;
    int   cmd;
    char  file[PATHMAX];   /* MP3 in */
    char  out[PATHMAX];    /* 8SVX ut */
    int   quality;         /* Q_LOW (12 kHz) eller Q_MEDIUM (24 kHz), alltid mono */
    int   error;
    char  errstr[80];
};
struct ConvStatus {        /* skrivs av konverteraren, läses av GUI:t */
    volatile int  state;
    volatile LONG permille;
    volatile LONG done_ms, total_ms;
    volatile LONG rate;
    volatile int  abort;
    char errstr[80];
};
extern struct ConvStatus cstat;
int  conv_start(void);
void conv_stop(void);
void conv_send(struct ConvCmd *c);
extern struct MsgPort *conv_port;
extern struct Library *MPEGABase;

/* ---------------------------------------------------------------- gui.c / main.c */
struct Config {
    char dldir[PATHMAX];
    int  quality;          /* Q_xxx */
    int  convquality;      /* Q_LOW eller Q_MEDIUM för 8SVX */
    int  bits14;
    char country[4];
    int  maxep;
    int  verify;
    int  autoupdate;       /* minuter, 0 av */
    int  skipfwd, skipback;
    int  volume;
    int  autonext;
    char browser[PATHMAX];
};
extern struct Config cfg;
int  gui_run(void);
void cfg_load(void);
void cfg_save(void);

#endif
