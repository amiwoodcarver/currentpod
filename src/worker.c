/* CurrentPod - arbetarprocessen: hämtar flöden, söker, laddar ner avsnitt.
 * Huvudprogrammet skickar struct Job till worker_port och får dem tillbaka
 * på sin svarsport när de är klara. Ett jobb i taget.
 */
#include "currentpod.h"
#include <dos/dostags.h>
#include <stdlib.h>

struct MsgPort *worker_port;
static struct Process *worker_proc;
static struct MsgPort *startup_port;
static volatile int worker_running;

struct StartMsg { struct Message msg; struct MsgPort *port; };

#define FEED_MAXBYTES  (3L * 1024 * 1024)   /* räcker till 200+ avsnitt; större flöden kapas, nyaste först */
#define JSON_MAXBYTES  (512L * 1024)

static void do_feed(struct Job *j)
{
    struct HttpOpts o;
    struct HttpResult r;
    memset(&o, 0, sizeof(o));
    o.etag = j->etag; o.modified = j->modified;
    o.maxbytes = FEED_MAXBYTES;
    o.truncate_ok = 1;
    o.abort = &j->abort;
    o.progress = &j->bytes; o.total = &j->total;
    o.verify = 0;
    if (!http_get(j->url, &o, &r)) {
        /* ett nätfel utan HTTP-svar (servern stängde, tidsgräns): prova en gång till */
        if (r.status == 0 && !j->abort) {
            log_msg("  flödet: %s, försöker igen om 3 s", r.errstr);
            http_free(&r);
            Delay(150);
            j->bytes = 0; j->total = 0;
            if (http_get(j->url, &o, &r)) goto got;
        }
        j->error = 1; strlcpy_(j->errstr, r.errstr, sizeof(j->errstr)); j->status = r.status;
        http_free(&r);
        return;
    }
got:
    j->status = r.status;
    if (r.status == 304) { j->notmodified = 1; http_free(&r); return; }
    strlcpy_(j->etag, r.etag, sizeof(j->etag));
    strlcpy_(j->modified, r.modified, sizeof(j->modified));
    if (!r.body.data || r.body.len < 20) { j->error = 1; strcpy(j->errstr, _(MSG_ERR_EMPTY_REPLY)); http_free(&r); return; }
    j->truncated = r.truncated;
    log_msg("  tolkar %ld byte flöde%s", r.body.len, r.truncated ? " (kapat)" : "");
    j->feed = rss_parse(r.body.data, r.body.len, j->url, j->maxep);
    log_msg("  tolkat: %ld avsnitt", j->feed ? (LONG)j->feed->nep : -1L);
    http_free(&r);
    if (!j->feed) {
        j->error = 1;
        strcpy(j->errstr, _(MSG_ERR_FEED_PARSE));
    }
}

static void do_search(struct Job *j)
{
    struct HttpOpts o;
    struct HttpResult r;
    char url[URLMAX], q[400];
    memset(&o, 0, sizeof(o));
    o.maxbytes = JSON_MAXBYTES;
    o.abort = &j->abort;
    if (j->type == JOB_LOOKUP) {
        sprintf(url, "https://itunes.apple.com/lookup?id=%s&entity=podcast", j->term);
    } else {
        url_encode_query(j->term, q, sizeof(q));
        sprintf(url, "https://itunes.apple.com/search?media=podcast&entity=podcast&limit=50&country=%s&term=%s",
                j->country[0] ? j->country : "se", q);
    }
    if (!http_get(url, &o, &r)) {
        j->error = 1; strlcpy_(j->errstr, r.errstr, sizeof(j->errstr)); j->status = r.status;
        http_free(&r);
        return;
    }
    j->status = r.status;
    if (r.body.data) j->nhits = apple_parse_results(r.body.data, &j->hits, 50);
    http_free(&r);
}

static void do_download(struct Job *j)
{
    struct HttpOpts o;
    struct HttpResult r;
    char part[PATHMAX];
    int attempt, idle = 0, ok = 0;
    LONG known_total = 0;
    /* FFS tillåter högst 30 tecken per namn: byt ut ändelsen i stället för att lägga till en */
    {
        char *dot;
        strlcpy_(part, j->file, sizeof(part));
        dot = strrchr(part, '.');
        if (dot && !strchr(dot, '/') && !strchr(dot, ':')) *dot = 0;
        strlcat_(part, ".part", sizeof(part));
    }
    /* Anslutningen kan brytas mitt i en lång hämtning (CloudFront stängde efter 19 MB i
       ett test). Då återupptas den med Range från .part-filens längd, så länge det går
       framåt; tre försök i rad utan en enda ny byte ger upp. */
    for (attempt = 1; attempt <= 60; attempt++) {
        LONG before = file_size(part), after;
        if (before < 0) before = 0;
        memset(&o, 0, sizeof(o));
        o.tofile = part;
        o.progress = &j->bytes; o.total = &j->total;
        o.abort = &j->abort;
        if (before > 0) o.range_from = before;
        if (known_total > 0 && before >= known_total) { ok = 1; break; }   /* redan komplett */
        ok = http_get(j->url, &o, &r);
        j->status = r.status;
        if (ok) break;
        if (j->total > 0) known_total = j->total;
        after = file_size(part); if (after < 0) after = 0;
        if (j->abort) { http_free(&r); break; }
        if (r.status == 416 && known_total > 0 && after >= known_total) { ok = 1; http_free(&r); break; }
        if (r.status >= 300) { http_free(&r); break; }     /* riktigt HTTP-fel: ingen idé att försöka igen */
        if (after > before) idle = 0; else idle++;
        if (idle >= 3) { http_free(&r); break; }
        log_msg("  hämtningen bröts vid %ld byte (%s), försök %ld om 3 s", after, r.errstr, (LONG)attempt + 1);
        http_free(&r);
        Delay(150);
        if (j->abort) break;
    }
    if (!ok) {
        j->error = 1;
        if (j->abort) strcpy(j->errstr, _(MSG_ERR_ABORTED));
        else strlcpy_(j->errstr, r.errstr[0] ? r.errstr : _(MSG_ERR_DOWNLOAD), sizeof(j->errstr));
        return;
    }
    if (r.ctype[0]) strlcpy_(j->etag, r.ctype, sizeof(j->etag));   /* återanvänder fältet: innehållstyp */
    http_free(&r);
    DeleteFile((STRPTR)j->file);
    if (!Rename((STRPTR)part, (STRPTR)j->file)) { j->error = 1; strcpy(j->errstr, _(MSG_ERR_RENAME_PART)); }
}

static void worker_main(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct StartMsg *sm;
    struct MsgPort *port;
    int net = 0;
    WaitPort(&me->pr_MsgPort);
    sm = (struct StartMsg *)GetMsg(&me->pr_MsgPort);
    port = CreateMsgPort();
    sm->port = port;
    ReplyMsg(&sm->msg);
    if (!port) return;
    worker_running = 1;
    for (;;) {
        struct Job *j;
        WaitPort(port);
        while ((j = (struct Job *)GetMsg(port))) {
            if (j->type == JOB_QUIT) {
                if (net) net_close();
                ReplyMsg(&j->msg);
                DeleteMsgPort(port);      /* egen port: måste tas bort av den task som skapade den */
                worker_running = 0;
                return;
            }
            j->error = 0; j->errstr[0] = 0; j->status = 0; j->feed = NULL; j->hits = NULL; j->nhits = 0; j->notmodified = 0;
            j->bytes = 0; j->total = 0;
            SetSignal(0, SIGBREAKF_CTRL_C);
            if (!net) {
                if (!net_open()) {
                    j->error = 1;
                    strcpy(j->errstr, _(MSG_ERR_NO_BSDSOCKET));
                    ReplyMsg(&j->msg);
                    continue;
                }
                net = 1;
            }
            switch (j->type) {
                case JOB_FEED: do_feed(j); break;
                case JOB_SEARCH: case JOB_LOOKUP: do_search(j); break;
                case JOB_DOWNLOAD: do_download(j); break;
            }
            ReplyMsg(&j->msg);
        }
    }
}

int worker_start(struct MsgPort *replyport)
{
    struct StartMsg sm;
    startup_port = CreateMsgPort();
    if (!startup_port) return 0;
    worker_proc = CreateNewProcTags(NP_Entry, (ULONG)worker_main, NP_Name, (ULONG)"CurrentPod net",
                                    NP_StackSize, 65536, NP_Priority, -1, TAG_DONE);
    if (!worker_proc) { DeleteMsgPort(startup_port); startup_port = NULL; return 0; }
    memset(&sm, 0, sizeof(sm));
    sm.msg.mn_ReplyPort = startup_port;
    sm.msg.mn_Length = sizeof(sm);
    PutMsg(&worker_proc->pr_MsgPort, &sm.msg);
    WaitPort(startup_port);
    GetMsg(startup_port);
    worker_port = sm.port;
    (void)replyport;
    return worker_port != NULL;
}

void worker_submit(struct Job *j)
{
    j->msg.mn_Length = sizeof(*j);
    PutMsg(worker_port, &j->msg);
}

void worker_stop(void)
{
    struct Job *q;
    if (!worker_port) return;
    q = xalloc(sizeof(*q));
    if (q) {
        struct MsgPort *rp = CreateMsgPort();
        q->type = JOB_QUIT;
        q->msg.mn_ReplyPort = rp;
        q->msg.mn_Length = sizeof(*q);
        if (worker_proc) Signal((struct Task *)worker_proc, SIGBREAKF_CTRL_C);
        PutMsg(worker_port, &q->msg);
        WaitPort(rp);
        GetMsg(rp);
        DeleteMsgPort(rp);
        xfree(q);
    }
    while (worker_running) Delay(2);
    worker_port = NULL;
    if (startup_port) { DeleteMsgPort(startup_port); startup_port = NULL; }
}
