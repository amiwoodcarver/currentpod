/* CurrentPod - konverteringsprocessen.
 *
 * Avkodar en hel MP3-fil med mpega.library till en IFF 8SVX-fil (8 bitar,
 * mono, 12 eller 24 kHz) så att avsnittet kan spelas utan avkodning i
 * realtid, även på en maskin som inte orkar MP3. Körs med prioritet -2:
 * under både gränssnittet och nätprocessen, så den tar bara den tid som
 * blir över. På en 030 med 50 MHz tar konverteringen ungefär lika lång
 * tid som avsnittet (12 kHz) eller en och en halv gång (24 kHz).
 */
#include "currentpod.h"
#include <dos/dostags.h>
#include <proto/mpega.h>
#include <clib/alib_protos.h>
#include <utility/hooks.h>
#include <stdlib.h>

struct ConvStatus cstat;
struct MsgPort *conv_port;
static struct Process *conv_proc;
static volatile int conv_running;
struct StartMsg { struct Message msg; struct MsgPort *port; };

#define OUTBUF 32768

struct Conv {
    BPTR   in;
    LONG   inoff, insize;
    struct Hook hook;
    MPEGA_CTRL ctrl;
    MPEGA_STREAM *mps;
    WORD  *pcm[2];
    UBYTE *obuf;
    LONG   olen;
    BPTR   out;
    LONG   nsamples;
    struct MsgPort *cmdport;
};

static ULONG cv_hook(struct Hook *hook, APTR handle, MPEGA_ACCESS *a)
{
    struct Conv *c = (struct Conv *)hook->h_Data;
    switch (a->func) {
        case MPEGA_BSFUNC_OPEN: {
            UBYTE hdr[10];
            c->in = Open((STRPTR)a->data.open.stream_name, MODE_OLDFILE);
            if (!c->in) return 0;
            Seek(c->in, 0, OFFSET_END);
            c->insize = Seek(c->in, 0, OFFSET_BEGINNING);
            c->inoff = 0;
            if (Read(c->in, hdr, 10) == 10 && hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
                LONG tagsize = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) | ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
                c->inoff = tagsize + 10;
                if (hdr[5] & 0x10) c->inoff += 10;
            }
            Seek(c->in, c->inoff, OFFSET_BEGINNING);
            a->data.open.stream_size = c->insize - c->inoff;
            return 1;
        }
        case MPEGA_BSFUNC_CLOSE:
            if (c->in) { Close(c->in); c->in = 0; }
            return 0;
        case MPEGA_BSFUNC_READ: {
            LONG n;
            if (!c->in) return 0;
            n = Read(c->in, a->data.read.buffer, a->data.read.num_bytes);
            return n > 0 ? n : 0;
        }
        case MPEGA_BSFUNC_SEEK:
            if (!c->in) return 1;
            Seek(c->in, c->inoff + a->data.seek.abs_byte_seek_pos, OFFSET_BEGINNING);
            return 0;
    }
    return 0;
}

static void put32(UBYTE *p, ULONG v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put16(UBYTE *p, UWORD v) { p[0] = v >> 8; p[1] = v; }

/* FORM....8SVX VHDR(20) NAME(n) BODY(...) */
static LONG write_header(struct Conv *c, const char *name, ULONG nsamples, UWORD rate)
{
    UBYTE h[64 + 128];
    LONG nlen = strlen(name), npad = (nlen + 1) & ~1, o = 0;
    if (nlen > 120) { nlen = 120; npad = 120; }
    memcpy(h + o, "FORM", 4); put32(h + o + 4, 4 + 8 + 20 + 8 + npad + 8 + nsamples); o += 8;
    memcpy(h + o, "8SVX", 4); o += 4;
    memcpy(h + o, "VHDR", 4); put32(h + o + 4, 20); o += 8;
    put32(h + o, nsamples); put32(h + o + 4, 0); put32(h + o + 8, 0); put16(h + o + 12, rate); h[o + 14] = 1; h[o + 15] = 0; put32(h + o + 16, 0x10000); o += 20;
    memcpy(h + o, "NAME", 4); put32(h + o + 4, nlen); o += 8;
    memset(h + o, 0, npad); memcpy(h + o, name, nlen); o += npad;
    memcpy(h + o, "BODY", 4); put32(h + o + 4, nsamples); o += 8;
    if (Write(c->out, h, o) != o) return 0;
    return o;
}

static int flush_out(struct Conv *c)
{
    if (c->olen > 0) {
        if (Write(c->out, c->obuf, c->olen) != c->olen) return 0;
        c->olen = 0;
    }
    return 1;
}

static int quit_pending(struct Conv *c)
{
    struct Message *m = (struct Message *)c->cmdport->mp_MsgList.lh_Head;
    return m->mn_Node.ln_Succ != NULL;
}

static void do_convert(struct Conv *c, struct ConvCmd *cmd)
{
    MPEGA_CTRL *k = &c->ctrl;
    char tmp[PATHMAX], name[128];
    WORD div = (cmd->quality == Q_MEDIUM) ? 2 : 4;
    LONG hdrlen, freq, n;
    ULONG t0 = 0;
    cstat.state = CV_RUNNING; cstat.permille = 0; cstat.done_ms = cstat.total_ms = 0; cstat.abort = 0; cstat.errstr[0] = 0;
    cmd->error = 0; cmd->errstr[0] = 0;
    memset(k, 0, sizeof(*k));
    k->bs_access = &c->hook;
    k->check_mpeg = 1;
    k->stream_buffer_size = 32768;
    k->layer_3.force_mono = 1;
    k->layer_3.mono.freq_div = k->layer_3.stereo.freq_div = div;
    k->layer_3.mono.quality = k->layer_3.stereo.quality = 0;
    k->layer_3.mono.freq_max = k->layer_3.stereo.freq_max = 48000;
    k->layer_1_2 = k->layer_3;
    if (div == 4) k->layer_1_2.mono.freq_div = k->layer_1_2.stereo.freq_div = 2;
    log_msg("konv: %s -> %s (div %ld)", cmd->file, cmd->out, (LONG)div);
    c->mps = MPEGA_open(cmd->file, k);
    if (!c->mps) { strcpy(cmd->errstr, _(MSG_ERR_NO_MPEG_FILE)); goto fail; }
    freq = c->mps->dec_frequency;
    if (freq <= 0 || freq > 65535) { strcpy(cmd->errstr, _(MSG_ERR_ODD_RATE)); goto fail; }
    cstat.total_ms = c->mps->ms_duration;
    cstat.rate = freq;
    log_msg("konv: %ld kbit/s %ld Hz -> %ld Hz mono, %ld ms", (LONG)c->mps->bitrate, c->mps->frequency, freq, c->mps->ms_duration);
    /* tillfälligt namn: byt ändelsen så att FFS:s 30 tecken räcker */
    strlcpy_(tmp, cmd->out, sizeof(tmp));
    { char *dot = strrchr(tmp, '.'); if (dot && !strchr(dot, '/') && !strchr(dot, ':')) *dot = 0; }
    strlcat_(tmp, ".tmp", sizeof(tmp));
    c->out = Open((STRPTR)tmp, MODE_NEWFILE);
    if (!c->out) { sprintf(cmd->errstr, _(MSG_ERR_CREATE_FILE), FilePart((STRPTR)tmp)); goto fail; }
    strlcpy_(name, FilePart((STRPTR)cmd->out), sizeof(name));
    { char *dot = strrchr(name, '.'); if (dot) *dot = 0; }
    hdrlen = write_header(c, name, 0, (UWORD)freq);
    if (!hdrlen) { strcpy(cmd->errstr, _(MSG_ERR_WRITE_DISK)); goto fail; }
    c->nsamples = 0; c->olen = 0;
    {
        struct timeval tv; struct Library *tb = NULL;
        (void)tv; (void)tb;
    }
    for (;;) {
        LONG i;
        if (cstat.abort || quit_pending(c)) { strcpy(cmd->errstr, _(MSG_ERR_ABORTED)); goto fail; }
        n = MPEGA_decode_frame(c->mps, c->pcm);
        if (n < 0) break;                 /* slut */
        if (n == 0) continue;             /* hoppad ram */
        for (i = 0; i < n; i++) {
            c->obuf[c->olen++] = (UBYTE)(c->pcm[0][i] >> 8);
            if (c->olen >= OUTBUF) { if (!flush_out(c)) { strcpy(cmd->errstr, _(MSG_ERR_WRITE_DISK)); goto fail; } }
        }
        c->nsamples += n;
        cstat.done_ms = (LONG)((c->nsamples / (ULONG)freq) * 1000 + (c->nsamples % (ULONG)freq) * 1000 / (ULONG)freq);
        if (cstat.total_ms > 0) cstat.permille = cstat.done_ms >= cstat.total_ms ? 1000 : (LONG)(((ULONG)cstat.done_ms * 1000UL) / (ULONG)cstat.total_ms);
        (void)t0;
    }
    if (!flush_out(c)) { strcpy(cmd->errstr, _(MSG_ERR_WRITE_DISK)); goto fail; }
    if (c->nsamples & 1) { UBYTE z = 0; Write(c->out, &z, 1); }   /* IFF: jämn längd */
    Seek(c->out, 0, OFFSET_BEGINNING);
    if (!write_header(c, name, c->nsamples, (UWORD)freq)) { strcpy(cmd->errstr, _(MSG_ERR_WRITE_HEADER)); goto fail; }
    Close(c->out); c->out = 0;
    MPEGA_close(c->mps); c->mps = NULL;
    if (c->in) { Close(c->in); c->in = 0; }
    DeleteFile((STRPTR)cmd->out);
    if (!Rename((STRPTR)tmp, (STRPTR)cmd->out)) { strcpy(cmd->errstr, _(MSG_ERR_RENAME_TMP)); DeleteFile((STRPTR)tmp); cmd->error = 1; cstat.state = CV_ERROR; return; }
    log_msg("konv: klar, %ld samplingar", c->nsamples);
    cstat.permille = 1000;
    cstat.state = CV_DONE;
    return;
fail:
    log_msg("konv: misslyckades: %s", cmd->errstr);
    if (c->mps) { MPEGA_close(c->mps); c->mps = NULL; }
    if (c->in) { Close(c->in); c->in = 0; }
    if (c->out) { Close(c->out); c->out = 0; DeleteFile((STRPTR)tmp); }
    cmd->error = 1;
    strlcpy_(cstat.errstr, cmd->errstr, sizeof(cstat.errstr));
    cstat.state = CV_ERROR;
}

static void conv_main(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct StartMsg *sm;
    struct Conv *c;
    struct Library *lib;
    WaitPort(&me->pr_MsgPort);
    sm = (struct StartMsg *)GetMsg(&me->pr_MsgPort);
    lib = OpenLibrary((STRPTR)"mpega.library", 2);
    if (lib) MPEGABase = lib;
    c = AllocVec(sizeof(*c), MEMF_ANY | MEMF_CLEAR);
    if (c) {
        c->cmdport = CreateMsgPort();
        c->pcm[0] = AllocVec(MPEGA_PCM_SIZE * sizeof(WORD) * 2, MEMF_ANY);
        c->pcm[1] = c->pcm[0] ? c->pcm[0] + MPEGA_PCM_SIZE : NULL;
        c->obuf = AllocVec(OUTBUF, MEMF_ANY);
        c->hook.h_Entry = (HOOKFUNC)HookEntry;
        c->hook.h_SubEntry = (HOOKFUNC)cv_hook;
        c->hook.h_Data = c;
    }
    sm->port = (c && c->cmdport && c->pcm[0] && c->obuf && lib) ? c->cmdport : NULL;
    if (!sm->port) {
        ReplyMsg(&sm->msg);
        if (c) { if (c->cmdport) DeleteMsgPort(c->cmdport); if (c->pcm[0]) FreeVec(c->pcm[0]); if (c->obuf) FreeVec(c->obuf); FreeVec(c); }
        if (lib) CloseLibrary(lib);
        return;
    }
    conv_running = 1;
    ReplyMsg(&sm->msg);
    for (;;) {
        struct ConvCmd *cmd;
        WaitPort(c->cmdport);
        while ((cmd = (struct ConvCmd *)GetMsg(c->cmdport))) {
            if (cmd->cmd == CC_QUIT) {
                ReplyMsg(&cmd->msg);
                goto out;
            }
            if (cmd->cmd == CC_CONVERT) do_convert(c, cmd);
            ReplyMsg(&cmd->msg);
        }
    }
out:
    DeleteMsgPort(c->cmdport);
    FreeVec(c->pcm[0]);
    FreeVec(c->obuf);
    FreeVec(c);
    if (lib) CloseLibrary(lib);
    conv_running = 0;
}

int conv_start(void)
{
    struct StartMsg sm;
    struct MsgPort *sp = CreateMsgPort();
    if (!sp) return 0;
    memset(&cstat, 0, sizeof(cstat));
    conv_proc = CreateNewProcTags(NP_Entry, (ULONG)conv_main, NP_Name, (ULONG)"CurrentPod converter",
                                  NP_StackSize, 32768, NP_Priority, -2, TAG_DONE);
    if (!conv_proc) { DeleteMsgPort(sp); return 0; }
    memset(&sm, 0, sizeof(sm));
    sm.msg.mn_ReplyPort = sp;
    sm.msg.mn_Length = sizeof(sm);
    PutMsg(&conv_proc->pr_MsgPort, &sm.msg);
    WaitPort(sp);
    GetMsg(sp);
    DeleteMsgPort(sp);
    conv_port = sm.port;
    return conv_port != NULL;
}

void conv_send(struct ConvCmd *c)
{
    c->msg.mn_Length = sizeof(*c);
    PutMsg(conv_port, &c->msg);
}

void conv_stop(void)
{
    struct ConvCmd *c;
    if (!conv_port) return;
    cstat.abort = 1;
    c = xalloc(sizeof(*c));
    if (c) {
        struct MsgPort *rp = CreateMsgPort();
        c->cmd = CC_QUIT;
        c->msg.mn_ReplyPort = rp; c->msg.mn_Length = sizeof(*c);
        PutMsg(conv_port, &c->msg);
        WaitPort(rp); GetMsg(rp);
        DeleteMsgPort(rp);
        xfree(c);
    }
    while (conv_running) Delay(2);
    conv_port = NULL;
}
