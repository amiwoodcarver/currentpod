/* CurrentPod - spelarprocessen.
 *
 * Avkodar MP3 med mpega.library och matar Paula direkt genom audio.device
 * med flera köade buffertar per kanal, i 8 eller 14 bitar (14 bitar = två
 * kanaler per sida, en på volym 64 och en på volym 1). Volymen regleras
 * digitalt i avkodaren (MPEGA_scale) så att 14-bitarsläget alltid har
 * Paula på full volym.
 *
 * Alla buffertar utom ljudbuffertarna ligger i fastminne. Kommandon från
 * GUI:t kommer som meddelanden och behandlas mellan avkodade ramar.
 */
#include "currentpod.h"
#include <devices/audio.h>
#include <graphics/gfxbase.h>
#include <dos/dostags.h>
#include <exec/execbase.h>
#include <proto/graphics.h>
#include <devices/timer.h>
#include <proto/timer.h>
#include <proto/mpega.h>
#include <clib/alib_protos.h>
#include <utility/hooks.h>
#include <stdlib.h>

struct Library *MPEGABase;
struct Device *TimerBase;
static struct timerequest *timerreq;
static struct MsgPort *timerport;

static ULONG now_ms(void)
{
    struct timeval tv;
    if (!TimerBase) return 0;
    GetSysTime(&tv);
    return tv.tv_secs * 1000 + tv.tv_micro / 1000;
}
struct PlayerStatus pstat;
struct MsgPort *player_port;
static struct Process *player_proc;
static volatile int player_running;
struct StartMsg { struct Message msg; struct MsgPort *port; };

#define NSLOT      3
#define SLOTSAMP   16384          /* samplingar per kanal och buffert */
#define MINSEND    2048
#define NCHAN      4
#define STREAMBUF  32768

struct Slot {
    UBYTE *buf[NCHAN];            /* chipminne */
    struct IOAudio *io[NCHAN];
    LONG   len;                   /* samplingar */
    int    outstanding;
    int    sent[NCHAN];           /* begäran ligger hos audio.device */
};

struct Player {
    struct MsgPort *cmdport, *audioport;
    struct IOAudio *aio;          /* öppningsbegäran */
    struct IOAudio *ctl[NCHAN];   /* för PERVOL/STOP/START/FLUSH */
    struct Slot slot[NSLOT];
    int    nchan;                 /* använda Paula-kanaler: 2 eller 4 */
    int    stereo, bits14;
    int    devopen;
    MPEGA_STREAM *mps;
    MPEGA_CTRL ctrl;
    struct Hook hook;
    WORD  *pcm[2];
    LONG   pcm_n, pcm_used;
    int    eof;
    ULONG  period;
    LONG   freq;
    LONG   start_ms;
    ULONG  played;                /* samplingar sedan start_ms */
    int    quality, vol;
    int    automode;              /* kvaliteten väljs efter uppmätt avkodningshastighet */
    int    nextslot;
    LONG   frames, zeroes;
    ULONG  fill_ms, fill_n, done_n;   /* statistik: ms avkodning, antal fyllda, antal klara */
    ULONG  t_start;                   /* klockan när uppspelningen (senast) startade, ms */
    ULONG  recent_ms[4], recent_n;    /* avkodningstid för de senaste bufferterna */
    int    lowpri;                    /* prioriteten sänkt för att vi ändå inte hinner */
    char   curfile[PATHMAX];
    ULONG  paused_ms, t_pause;
    /* filåtkomst */
    BPTR   file;
    LONG   fileoff, filesize;
    /* 8SVX-läge: ren PCM från disk, ingen avkodning */
    int    pcmmode;
    LONG   pcm_body, pcm_len, pcm_pos;   /* BODY-chunkens början och längd, läst hittills */
};

static struct Player *P;          /* bara spelarprocessen rör den */

/* ------------------------------------------------------------ filhook med ID3v2-hopp */

static ULONG bs_hook(struct Hook *hook, APTR handle, MPEGA_ACCESS *a)
{
    struct Player *p = (struct Player *)hook->h_Data;
    if (cp_debug > 1) log_msg("spelare: hook func %ld", a->func);
    switch (a->func) {
        case MPEGA_BSFUNC_OPEN: {
            UBYTE hdr[10];
            p->file = Open((STRPTR)a->data.open.stream_name, MODE_OLDFILE);
            if (!p->file) return 0;
            Seek(p->file, 0, OFFSET_END);
            p->filesize = Seek(p->file, 0, OFFSET_BEGINNING);
            p->fileoff = 0;
            if (Read(p->file, hdr, 10) == 10 && hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
                LONG tagsize = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) | ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
                p->fileoff = tagsize + 10;
                if (hdr[5] & 0x10) p->fileoff += 10;   /* footer */
            }
            Seek(p->file, p->fileoff, OFFSET_BEGINNING);
            a->data.open.stream_size = p->filesize - p->fileoff;
            log_msg("spelare: fil %ld byte, ID3-hopp %ld, blockstorlek %ld", p->filesize, p->fileoff, a->data.open.buffer_size);
            return 1;
        }
        case MPEGA_BSFUNC_CLOSE:
            if (p->file) { Close(p->file); p->file = 0; }
            return 0;
        case MPEGA_BSFUNC_READ: {
            LONG n;
            if (!p->file) return 0;
            n = Read(p->file, a->data.read.buffer, a->data.read.num_bytes);
            return n > 0 ? n : 0;
        }
        case MPEGA_BSFUNC_SEEK:
            if (!p->file) return 1;
            Seek(p->file, p->fileoff + a->data.seek.abs_byte_seek_pos, OFFSET_BEGINNING);
            return 0;
    }
    return 0;
}

/* ------------------------------------------------------------ kvalitet */

static void set_quality(struct Player *p, int q)
{
    struct ExecBase *sb = (struct ExecBase *)SysBase;
    MPEGA_CTRL *c = &p->ctrl;
    int fpu = (sb->AttnFlags & (AFF_68881 | AFF_68882 | AFF_FPU40)) != 0;
    if (q == Q_AUTO) {
        if (sb->AttnFlags & AFF_68060) q = Q_FULL;
        else if (sb->AttnFlags & AFF_68040) q = Q_HIGH;
        else if ((sb->AttnFlags & AFF_68030) && fpu) q = Q_MEDIUM;
        else q = Q_LOW;
    }
    memset(c, 0, sizeof(*c));
    c->bs_access = &p->hook;
    c->check_mpeg = 1;
    c->stream_buffer_size = STREAMBUF;
    /* mpega tar mono-inställningarna för monoströmmar och stereo-inställningarna
       för stereoströmmar; force_mono mixar sedan ner. Så båda sätts lika. */
    {
        WORD div, qual, mono;
        switch (q) {
            case Q_LOW:    div = 4; qual = 0; mono = 1; break;   /* 11-12 kHz mono: 020/030 utan FPU */
            case Q_MEDIUM: div = 2; qual = 0; mono = 1; break;   /* 22-24 kHz mono: 030/50 */
            case Q_HIGH:   div = 2; qual = 1; mono = 0; break;   /* 22-24 kHz stereo: 040 */
            default:       div = 1; qual = 2; mono = 0; q = Q_FULL; break;   /* 44-48 kHz stereo: 060 */
        }
        c->layer_3.force_mono = mono;
        c->layer_3.mono.freq_div = c->layer_3.stereo.freq_div = div;
        c->layer_3.mono.quality = c->layer_3.stereo.quality = qual;
        c->layer_1_2 = c->layer_3;
        if (q == Q_LOW) { c->layer_1_2.mono.freq_div = c->layer_1_2.stereo.freq_div = 2; }
    }
    /* freq_max används bara om freq_div == 0 */
    c->layer_3.mono.freq_max = c->layer_3.stereo.freq_max = 48000;
    c->layer_1_2.mono.freq_max = c->layer_1_2.stereo.freq_max = 48000;
    p->quality = q;
    pstat.quality = q;
}

/* en nivå lägre, eller -1 om vi redan är längst ner */
static int lower_quality(int q)
{
    switch (q) {
        case Q_FULL: return Q_HIGH;
        case Q_HIGH: return Q_MEDIUM;
        case Q_MEDIUM: return Q_LOW;
        default: return -1;
    }
}

static const char *pick_library(void)
{
    /* Ett bibliotek måste öppnas under det namn som står i filen, så bygget
       installerar rätt variant (020FPU/060FPU) som LIBS:mpega.library. */
    MPEGABase = OpenLibrary((STRPTR)"mpega.library", 2);
    return MPEGABase ? "mpega.library" : NULL;
}

/* ------------------------------------------------------------ audio.device */

static void close_audio(struct Player *p)
{
    int s, c;
    for (s = 0; s < NSLOT; s++) {
        for (c = 0; c < NCHAN; c++) {
            if (p->slot[s].io[c]) {
                if (p->slot[s].sent[c]) { AbortIO((struct IORequest *)p->slot[s].io[c]); WaitIO((struct IORequest *)p->slot[s].io[c]); p->slot[s].sent[c] = 0; }
                DeleteIORequest((struct IORequest *)p->slot[s].io[c]);
                p->slot[s].io[c] = NULL;
            }
        }
        p->slot[s].outstanding = 0;
    }
    for (c = 0; c < NCHAN; c++) if (p->ctl[c]) { DeleteIORequest((struct IORequest *)p->ctl[c]); p->ctl[c] = NULL; }
    if (p->devopen) { CloseDevice((struct IORequest *)p->aio); p->devopen = 0; }
    if (p->aio) { DeleteIORequest((struct IORequest *)p->aio); p->aio = NULL; }
    /* töm svarsporten */
    while (GetMsg(p->audioport));
}

static int open_audio(struct Player *p, char *err)
{
    static UBYTE map4[] = { 0x0F };
    static UBYTE map2[] = { 0x03 };
    int s, c;
    p->nchan = (p->bits14 && !p->pcmmode) ? 4 : 2;   /* 8SVX är 8 bitar: två kanaler räcker */
    p->aio = (struct IOAudio *)CreateIORequest(p->audioport, sizeof(struct IOAudio));
    if (!p->aio) { strcpy(err, _(MSG_ERR_NO_MEMORY)); return 0; }
    p->aio->ioa_Request.io_Message.mn_Node.ln_Pri = 100;
    p->aio->ioa_Data = (p->nchan == 4) ? map4 : map2;
    p->aio->ioa_Length = 1;
    p->aio->ioa_AllocKey = 0;
    if (OpenDevice((STRPTR)"audio.device", 0, (struct IORequest *)p->aio, 0)) {
        strcpy(err, _(MSG_ERR_AUDIO_BUSY));
        DeleteIORequest((struct IORequest *)p->aio); p->aio = NULL;
        return 0;
    }
    p->devopen = 1;
    for (c = 0; c < p->nchan; c++) {
        int unit;
        /* kanal 0 och 3 är vänster, 1 och 2 höger. 8-bit: 0=V 1=H. 14-bit: hög V=0 H=1, låg V=3 H=2 */
        unit = c;
        p->ctl[c] = (struct IOAudio *)CreateIORequest(p->audioport, sizeof(struct IOAudio));
        if (!p->ctl[c]) { strcpy(err, _(MSG_ERR_NO_MEMORY)); return 0; }
        *p->ctl[c] = *p->aio;
        p->ctl[c]->ioa_Request.io_Unit = (struct Unit *)(1L << unit);
        for (s = 0; s < NSLOT; s++) {
            struct IOAudio *io = (struct IOAudio *)CreateIORequest(p->audioport, sizeof(struct IOAudio));
            if (!io) { strcpy(err, _(MSG_ERR_NO_MEMORY)); return 0; }
            *io = *p->aio;
            io->ioa_Request.io_Unit = (struct Unit *)(1L << unit);
            io->ioa_Request.io_Command = CMD_WRITE;
            io->ioa_Request.io_Flags = ADIOF_PERVOL;
            io->ioa_Cycles = 1;
            p->slot[s].io[c] = io;
        }
    }
    return 1;
}

static void send_slot(struct Player *p, int s)
{
    struct Slot *sl = &p->slot[s];
    int c;
    if (sl->len <= 0) return;
    sl->outstanding = 0;
    for (c = 0; c < p->nchan; c++) {
        struct IOAudio *io = sl->io[c];
        int lowchan = (c >= 2);
        io->ioa_Request.io_Command = CMD_WRITE;
        io->ioa_Request.io_Flags = ADIOF_PERVOL;
        io->ioa_Data = sl->buf[c];
        io->ioa_Length = sl->len;
        io->ioa_Period = p->period;
        io->ioa_Volume = lowchan ? 1 : (p->pcmmode ? p->vol : 64);   /* MP3: volymen sätts i avkodaren */
        io->ioa_Cycles = 1;
        BeginIO((struct IORequest *)io);
        sl->sent[c] = 1;
        sl->outstanding++;
    }
}

static void ctl_cmd(struct Player *p, UWORD cmd)
{
    int c;
    for (c = 0; c < p->nchan; c++) {
        p->ctl[c]->ioa_Request.io_Command = cmd;
        p->ctl[c]->ioa_Request.io_Flags = 0;
        DoIO((struct IORequest *)p->ctl[c]);
    }
}

/* ------------------------------------------------------------ avkodning */

static int cmd_pending(struct Player *p)
{
    return p->cmdport->mp_MsgList.lh_Head->ln_Succ != NULL;
}

static int decode_more(struct Player *p)
{
    LONG n;
    if (p->eof) return 0;
    for (;;) {
        n = MPEGA_decode_frame(p->mps, p->pcm);
        if (n > 0) { p->pcm_n = n; p->pcm_used = 0; p->frames++; p->zeroes = 0; return 1; }
        if (n == 0) {                    /* ram hoppad, inte slut */
            if (++p->zeroes > 500) { log_msg("spelare: 500 tomma ramar i rad, ger upp"); p->eof = 1; return 0; }
            continue;
        }
        log_msg("spelare: decode_frame gav %ld efter %ld ramar", n, p->frames);
        p->eof = 1;
        return 0;
    }
}

static LONG fill_slot_pcm(struct Player *p, int s);

/* fyll en slot ur avkodaren; returnerar antal samplingar (0 = inget mer) */
static LONG fill_slot(struct Player *p, int s)
{
    struct Slot *sl = &p->slot[s];
    LONG fill = 0;
    ULONG t0 = now_ms();
    if (p->pcmmode) return fill_slot_pcm(p, s);
    while (fill < SLOTSAMP) {
        LONG avail, take, i;
        if (p->pcm_used >= p->pcm_n) {
            if (fill >= MINSEND && cmd_pending(p)) break;
            if (!decode_more(p)) break;
        }
        avail = p->pcm_n - p->pcm_used;
        take = SLOTSAMP - fill;
        if (take > avail) take = avail;
        {
            const WORD *l = p->pcm[0] + p->pcm_used;
            const WORD *r = p->stereo ? p->pcm[1] + p->pcm_used : l;
            if (p->bits14) {
                UBYTE *hl = sl->buf[0] + fill, *hr = sl->buf[1] + fill;
                UBYTE *lr = sl->buf[2] + fill, *ll = sl->buf[3] + fill;
                if (p->stereo) {
                    for (i = 0; i < take; i++) {
                        WORD a = l[i], b = r[i];
                        hl[i] = (UBYTE)(a >> 8); ll[i] = (UBYTE)((a & 0xFF) >> 2);
                        hr[i] = (UBYTE)(b >> 8); lr[i] = (UBYTE)((b & 0xFF) >> 2);
                    }
                } else {
                    for (i = 0; i < take; i++) {
                        WORD a = l[i];
                        UBYTE h = (UBYTE)(a >> 8), lo = (UBYTE)((a & 0xFF) >> 2);
                        hl[i] = h; hr[i] = h; ll[i] = lo; lr[i] = lo;
                    }
                }
            } else {
                UBYTE *bl = sl->buf[0] + fill, *br = sl->buf[1] + fill;
                if (p->stereo) {
                    for (i = 0; i < take; i++) { bl[i] = (UBYTE)(l[i] >> 8); br[i] = (UBYTE)(r[i] >> 8); }
                } else {
                    for (i = 0; i < take; i++) { bl[i] = br[i] = (UBYTE)(l[i] >> 8); }
                }
            }
        }
        p->pcm_used += take;
        fill += take;
    }
    sl->len = fill;
    p->fill_ms += now_ms() - t0;
    p->recent_ms[p->fill_n & 3] = now_ms() - t0;
    p->fill_n++;
    if (p->fill_n <= 12 || (p->fill_n % 50) == 0)
        log_msg("spelare: slot %ld: %ld samplingar, %ld ms avkodning (snitt %ld ms per %ld ms ljud)", (LONG)s, fill,
                (LONG)(now_ms() - t0), (LONG)(p->fill_ms / p->fill_n), (LONG)(SLOTSAMP * 1000L / (p->freq ? p->freq : 1)));
    return fill;
}

/* ------------------------------------------------------------ start/stopp */

static void stop_stream(struct Player *p)
{
    if (p->automode) set_quality(p, Q_AUTO);
    close_audio(p);
    if (p->mps) { MPEGA_close(p->mps); p->mps = NULL; }
    if (p->file) { Close(p->file); p->file = 0; }
    p->eof = 0; p->pcm_n = p->pcm_used = 0;
    p->pcmmode = 0; pstat.pcm = 0;
}

static int is_8svx_name(const char *file)
{
    LONG n = strlen(file);
    return n > 5 && strieq(file + n - 5, ".8svx");
}

static void apply_volume(struct Player *p)
{
    if (p->mps) MPEGA_scale(p->mps, (p->vol * 100) / 64);
    if (p->pcmmode && p->devopen) {
        int c;
        for (c = 0; c < p->nchan; c++) {
            p->ctl[c]->ioa_Request.io_Command = ADCMD_PERVOL;
            p->ctl[c]->ioa_Request.io_Flags = 0;
            p->ctl[c]->ioa_Period = p->period;
            p->ctl[c]->ioa_Volume = p->vol;
            DoIO((struct IORequest *)p->ctl[c]);
        }
    }
}

/* ------------------------------------------------------------ 8SVX från disk */

static ULONG get32(const UBYTE *b) { return ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) | ((ULONG)b[2] << 8) | b[3]; }

static int open_pcm(struct Player *p, const char *file, LONG startsec, char *err)
{
    UBYTE h[20];
    LONG rate = 0, pos;
    struct GfxBase *gfx;
    p->file = Open((STRPTR)file, MODE_OLDFILE);
    if (!p->file) { sprintf(err, _(MSG_ERR_OPEN_FILE), FilePart((STRPTR)file)); return 0; }
    if (Read(p->file, h, 12) != 12 || memcmp(h, "FORM", 4) || memcmp(h + 8, "8SVX", 4)) { strcpy(err, _(MSG_ERR_NOT_8SVX)); return 0; }
    p->pcm_body = p->pcm_len = 0;
    pos = 12;
    for (;;) {
        ULONG len;
        if (Read(p->file, h, 8) != 8) break;
        len = get32(h + 4);
        pos += 8;
        if (!memcmp(h, "VHDR", 4) && len >= 20) {
            if (Read(p->file, h, 20) != 20) break;
            rate = (h[12] << 8) | h[13];
            pos += 20;
            Seek(p->file, pos + ((len + 1) & ~1) - 20, OFFSET_BEGINNING);
            pos += ((len + 1) & ~1) - 20;
            continue;
        }
        if (!memcmp(h, "BODY", 4)) { p->pcm_body = pos; p->pcm_len = len; break; }
        Seek(p->file, (len + 1) & ~1, OFFSET_CURRENT);
        pos += (len + 1) & ~1;
    }
    if (!p->pcm_body || rate <= 0) { strcpy(err, _(MSG_ERR_8SVX_CHUNKS)); return 0; }
    p->pcmmode = 1;
    p->stereo = 0;
    p->freq = rate;
    gfx = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 36);
    {
        ULONG clock = 3546895;
        if (gfx) { if (!(gfx->DisplayFlags & PAL)) clock = 3579545; CloseLibrary((struct Library *)gfx); }
        p->period = (clock + p->freq / 2) / p->freq;
    }
    p->frames = p->zeroes = 0;
    p->fill_ms = p->fill_n = p->done_n = 0;
    p->eof = 0; p->pcm_n = p->pcm_used = 0;
    pstat.freq = p->freq; pstat.channels = 1; pstat.bitrate = 0; pstat.bits = 8; pstat.pcm = 1;
    pstat.dur = p->pcm_len / rate;
    pstat.quality = p->quality;
    if (startsec < 0) startsec = 0;
    if (startsec * rate >= p->pcm_len) startsec = 0;
    p->pcm_pos = startsec * rate;
    Seek(p->file, p->pcm_body + p->pcm_pos, OFFSET_BEGINNING);
    p->start_ms = startsec * 1000;
    p->played = 0;
    log_msg("spelare: 8SVX %s, %ld Hz, %ld samplingar (%ld s), start %ld s", FilePart((STRPTR)file), rate, p->pcm_len, pstat.dur, startsec);
    return 1;
}

/* fyll en slot direkt från 8SVX-filen: samma 8 bitar till vänster och höger kanal */
static LONG fill_slot_pcm(struct Player *p, int s)
{
    struct Slot *sl = &p->slot[s];
    LONG want = SLOTSAMP, n = 0;
    ULONG t0 = now_ms();
    if (p->pcm_pos + want > p->pcm_len) want = p->pcm_len - p->pcm_pos;
    if (want > 0 && p->file) {
        n = Read(p->file, sl->buf[0], want);
        if (n < 0) n = 0;
        p->pcm_pos += n;
        if (n > 0) CopyMem(sl->buf[0], sl->buf[1], n);
    }
    if (n < SLOTSAMP) p->eof = 1;
    sl->len = n;
    p->fill_ms += now_ms() - t0;
    p->recent_ms[p->fill_n & 3] = now_ms() - t0;
    p->fill_n++;
    return n;
}

static int open_stream(struct Player *p, const char *file, LONG startsec, char *err)
{
    struct GfxBase *gfx;
    log_msg("spelare: öppnar %s (kvalitet %ld)", file, (LONG)p->quality);
    p->mps = MPEGA_open((char *)file, &p->ctrl);
    if (!p->mps) { sprintf(err, _(MSG_ERR_NO_MPEG_IN), FilePart((STRPTR)file)); log_msg("spelare: MPEGA_open misslyckades"); return 0; }
    log_msg("spelare: norm %ld layer %ld %ld kbit/s %ld Hz mode %ld -> %ld kanaler %ld Hz kvalitet %ld, %ld ms",
            (LONG)p->mps->norm, (LONG)p->mps->layer, (LONG)p->mps->bitrate, p->mps->frequency, (LONG)p->mps->mode,
            (LONG)p->mps->dec_channels, p->mps->dec_frequency, (LONG)p->mps->dec_quality, p->mps->ms_duration);
    p->frames = p->zeroes = 0;
    p->fill_ms = p->fill_n = p->done_n = 0;
    p->eof = 0; p->pcm_n = p->pcm_used = 0;
    p->stereo = (p->mps->dec_channels == 2);
    p->freq = p->mps->dec_frequency;
    if (p->freq <= 0) p->freq = 22050;
    gfx = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 36);
    {
        ULONG clock = 3546895;
        if (gfx) { if (!(gfx->DisplayFlags & PAL)) clock = 3579545; CloseLibrary((struct Library *)gfx); }
        p->period = (clock + p->freq / 2) / p->freq;
    }
    pstat.freq = p->freq; pstat.channels = p->stereo ? 2 : 1; pstat.bitrate = p->mps->bitrate;
    pstat.dur = p->mps->ms_duration / 1000;
    pstat.bits = p->bits14 ? 14 : 8;
    pstat.quality = p->quality;
    if (startsec > 0) {
        if (MPEGA_seek(p->mps, startsec * 1000) != 0) startsec = 0;
    }
    p->start_ms = startsec * 1000;
    p->played = 0;
    apply_volume(p);
    return 1;
}

static int start_stream(struct Player *p, const char *file, LONG startsec, char *err)
{
    int s, tries = 0;
    stop_stream(p);
    if (p->lowpri) { SetTaskPri(FindTask(NULL), 1); p->lowpri = 0; }
    strlcpy_(p->curfile, file, sizeof(p->curfile));
    p->recent_n = 0;
    if (is_8svx_name(file)) {
        /* konverterat avsnitt: ingen mätning, ingen kvalitetsnivå, bara läsa och spela */
        if (!open_pcm(p, file, startsec, err)) { stop_stream(p); return 0; }
        if (fill_slot(p, 0) <= 0) { strcpy(err, _(MSG_ERR_EMPTY_EPISODE)); stop_stream(p); return 0; }
        if (!open_audio(p, err)) { stop_stream(p); log_msg("spelare: audio.device: %s", err); return 0; }
        log_msg("spelare: audio.device öppnad för 8SVX, %ld kanaler, period %ld", (LONG)p->nchan, (LONG)p->period);
        p->nextslot = 0;
        p->t_start = now_ms(); p->paused_ms = 0;
        send_slot(p, 0);
        for (s = 1; s < NSLOT; s++) { if (fill_slot(p, s) > 0) send_slot(p, s); else break; }
        return 1;
    }
    for (;;) {
        LONG n, audio_ms;
        if (!open_stream(p, file, startsec, err)) return 0;
        /* mät första bufferten: hinner avkodaren med? annars sänk kvaliteten */
        n = fill_slot(p, 0);
        if (n <= 0) { strcpy(err, _(MSG_ERR_EMPTY_EPISODE)); stop_stream(p); return 0; }
        audio_ms = n * 1000 / p->freq;
        if (p->automode && TimerBase && p->fill_ms > (ULONG)(audio_ms * 80 / 100) && ++tries <= 3) {
            int lq = lower_quality(p->quality);
            if (lq >= 0) {
                log_msg("spelare: %ld ms avkodning för %ld ms ljud, sänker kvaliteten", (LONG)p->fill_ms, audio_ms);
                MPEGA_close(p->mps); p->mps = NULL;
                if (p->file) { Close(p->file); p->file = 0; }
                set_quality(p, lq);
                continue;
            }
        }
        break;
    }
    if (!open_audio(p, err)) { stop_stream(p); log_msg("spelare: audio.device: %s", err); return 0; }
    log_msg("spelare: audio.device öppnad, %ld kanaler, period %ld, %ld bitar", (LONG)p->nchan, (LONG)p->period, (LONG)(p->bits14 ? 14 : 8));
    p->nextslot = 0;
    p->t_start = now_ms(); p->paused_ms = 0;
    send_slot(p, 0);
    for (s = 1; s < NSLOT; s++) {
        LONG n = fill_slot(p, s);
        if (n > 0) send_slot(p, s);
        else break;
    }
    return 1;
}

static void seek_stream(struct Player *p, LONG sec)
{
    int s;
    if (!p->mps && !p->pcmmode) return;
    if (sec < 0) sec = 0;
    if (pstat.dur && sec >= pstat.dur) sec = pstat.dur - 1;
    /* kasta det som ligger i kö */
    ctl_cmd(p, CMD_FLUSH);
    for (s = 0; s < NSLOT; s++) {
        int c;
        for (c = 0; c < p->nchan; c++)
            if (p->slot[s].sent[c]) { WaitIO((struct IORequest *)p->slot[s].io[c]); p->slot[s].sent[c] = 0; }
        p->slot[s].outstanding = 0;
    }
    while (GetMsg(p->audioport));
    p->eof = 0; p->pcm_n = p->pcm_used = 0;
    if (p->pcmmode) {
        p->pcm_pos = sec * p->freq;
        if (p->pcm_pos >= p->pcm_len) p->pcm_pos = p->pcm_len;
        Seek(p->file, p->pcm_body + p->pcm_pos, OFFSET_BEGINNING);
    } else if (MPEGA_seek(p->mps, sec * 1000) != 0) { p->eof = 1; }
    p->start_ms = sec * 1000;
    p->played = 0;
    pstat.pos = sec;
    p->t_start = now_ms(); p->paused_ms = 0;
    for (s = 0; s < NSLOT; s++) {
        if (fill_slot(p, s) > 0) send_slot(p, s);
        else break;
    }
    if (pstat.state == PS_PAUSED) ctl_cmd(p, CMD_STOP);
}

/* ------------------------------------------------------------ processen */

static void handle_cmd(struct Player *p, struct PlayerCmd *c)
{
    c->error = 0; c->errstr[0] = 0;
    switch (c->cmd) {
        case PC_PLAY:
            pstat.state = PS_STOPPED;
            if (start_stream(p, c->file, c->arg, c->errstr)) { pstat.state = PS_PLAYING; pstat.pos = c->arg; }
            else { c->error = 1; pstat.state = PS_ERROR; strlcpy_(pstat.errstr, c->errstr, sizeof(pstat.errstr)); }
            break;
        case PC_STOP:
            stop_stream(p);
            pstat.state = PS_STOPPED;
            break;
        case PC_PAUSE:
            if (pstat.state == PS_PLAYING) { ctl_cmd(p, CMD_STOP); pstat.state = PS_PAUSED; p->t_pause = now_ms(); }
            break;
        case PC_RESUME:
            if (pstat.state == PS_PAUSED) { ctl_cmd(p, CMD_START); pstat.state = PS_PLAYING; p->paused_ms += now_ms() - p->t_pause; }
            break;
        case PC_SEEK:
            if (pstat.state == PS_PLAYING || pstat.state == PS_PAUSED) seek_stream(p, c->arg);
            break;
        case PC_VOLUME:
            p->vol = c->arg; if (p->vol < 0) p->vol = 0; if (p->vol > 64) p->vol = 64;
            apply_volume(p);
            break;
        case PC_SETQUALITY:
            p->automode = ((c->arg & 0xFF) == Q_AUTO);
            set_quality(p, c->arg & 0xFF);
            p->bits14 = (c->arg & 0x100) != 0;
            log_msg("spelare: kvalitet %ld, %ld bitar", (LONG)p->quality, (LONG)(p->bits14 ? 14 : 8));
            break;
    }
}

static void player_main(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct StartMsg *sm;
    struct Player *p;
    const char *lib;
    WaitPort(&me->pr_MsgPort);
    sm = (struct StartMsg *)GetMsg(&me->pr_MsgPort);
    p = AllocVec(sizeof(*p), MEMF_ANY | MEMF_CLEAR);
    if (p) {
        P = p;
        p->cmdport = CreateMsgPort();
        p->audioport = CreateMsgPort();
        p->pcm[0] = AllocVec(MPEGA_PCM_SIZE * sizeof(WORD) * 2, MEMF_ANY);
        p->pcm[1] = p->pcm[0] + MPEGA_PCM_SIZE;
        p->hook.h_Entry = (HOOKFUNC)HookEntry;
        p->hook.h_SubEntry = (HOOKFUNC)bs_hook;
        p->hook.h_Data = p;
        p->vol = 64;
        p->bits14 = 1;
        {   /* ljudbuffertarna: 3 x 4 x 16 KB chipminne, allokeras en gång */
            int s, c, ok = 1;
            for (s = 0; s < NSLOT; s++)
                for (c = 0; c < NCHAN; c++)
                    if (!(p->slot[s].buf[c] = AllocVec(SLOTSAMP, MEMF_CHIP | MEMF_CLEAR))) ok = 0;
            if (!ok) { strcpy(pstat.errstr, _(MSG_ERR_NO_CHIPMEM)); p->pcm[0] = NULL; }
        }
        set_quality(p, Q_AUTO);
        timerport = CreateMsgPort();
        if (timerport) {
            timerreq = (struct timerequest *)CreateIORequest(timerport, sizeof(struct timerequest));
            if (timerreq && !OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ, (struct IORequest *)timerreq, 0))
                TimerBase = timerreq->tr_node.io_Device;
        }
        lib = pick_library();
        if (lib) strlcpy_(pstat.libname, lib, sizeof(pstat.libname));
    }
    sm->port = (p && p->cmdport && p->audioport && p->pcm[0] && MPEGABase) ? p->cmdport : NULL;
    if (!sm->port) {
        if (!MPEGABase) strcpy(pstat.errstr, _(MSG_ERR_NO_MPEGA));
        ReplyMsg(&sm->msg);
        if (p) {
            int s2, c2;
            for (s2 = 0; s2 < NSLOT; s2++) for (c2 = 0; c2 < NCHAN; c2++) if (p->slot[s2].buf[c2]) FreeVec(p->slot[s2].buf[c2]);
            if (p->cmdport) DeleteMsgPort(p->cmdport); if (p->audioport) DeleteMsgPort(p->audioport); if (p->pcm[0]) FreeVec(p->pcm[0]); FreeVec(p);
        }
        if (MPEGABase) { CloseLibrary(MPEGABase); MPEGABase = NULL; }
        return;
    }
    player_running = 1;
    ReplyMsg(&sm->msg);
    for (;;) {
        ULONG cmdsig = 1L << p->cmdport->mp_SigBit, audsig = 1L << p->audioport->mp_SigBit;
        ULONG sigs = Wait(cmdsig | audsig);
        struct PlayerCmd *c;
        struct IOAudio *io;
        if (sigs & cmdsig) {
            while ((c = (struct PlayerCmd *)GetMsg(p->cmdport))) {
                if (c->cmd == PC_QUIT) {
                    stop_stream(p);
                    ReplyMsg(&c->msg);
                    goto out;
                }
                handle_cmd(p, c);
                ReplyMsg(&c->msg);
            }
        }
        if (sigs & audsig) {
            while ((io = (struct IOAudio *)GetMsg(p->audioport))) {
                int s, c2, found = -1;
                for (s = 0; s < NSLOT && found < 0; s++)
                    for (c2 = 0; c2 < NCHAN; c2++)
                        if (p->slot[s].io[c2] == io) { found = s; break; }
                if (found < 0) continue;
                p->slot[found].sent[c2] = 0;
                if (--p->slot[found].outstanding == 0) {
                    int any = 0;
                    p->played += p->slot[found].len;
                    p->done_n++;
                    pstat.pos = (p->start_ms + (LONG)((p->played / (ULONG)p->freq) * 1000)) / 1000;
                    for (s = 0; s < NSLOT; s++) if (p->slot[s].outstanding) any = 1;
                    if (!any && !p->eof && pstat.state == PS_PLAYING) pstat.underruns++;   /* kön tom: hack */
                    if (TimerBase && pstat.state == PS_PLAYING) {
                        /* eftersläpning: väggklocka sedan start minus spelat ljud */
                        LONG lag = (LONG)(now_ms() - p->t_start - p->paused_ms) - (LONG)((p->played / (ULONG)p->freq) * 1000 + (p->played % (ULONG)p->freq) * 1000 / (ULONG)p->freq);
                        pstat.cpu = lag;
                    }
                    if (p->done_n <= 6 || cp_debug > 1) log_msg("spelare: slot %ld klar (%lu spelade, underskott %ld, lag %ld ms)", (LONG)found, p->played, pstat.underruns, pstat.cpu);
                    if (!p->eof && (pstat.state == PS_PLAYING || pstat.state == PS_PAUSED)) {
                        if (fill_slot(p, found) > 0) send_slot(p, found);
                        /* hinner vi? medel av de senaste fyra bufferterna mot deras speltid */
                        if (p->fill_n >= 4 && TimerBase && !p->pcmmode) {
                            ULONG sum = p->recent_ms[0] + p->recent_ms[1] + p->recent_ms[2] + p->recent_ms[3];
                            ULONG audio = 4 * (ULONG)SLOTSAMP * 1000 / (ULONG)p->freq;
                            if (sum > audio * 105 / 100) {
                                int lq = p->automode ? lower_quality(p->quality) : -1;
                                if (lq >= 0) {
                                    char e2[80];
                                    LONG pos = pstat.pos;
                                    log_msg("spelare: hinner inte (%lu ms per %lu ms), byter till kvalitet %ld", sum, audio, (LONG)lq);
                                    close_audio(p);
                                    if (p->mps) { MPEGA_close(p->mps); p->mps = NULL; }
                                    if (p->file) { Close(p->file); p->file = 0; }
                                    set_quality(p, lq);
                                    p->automode = 0;   /* så att stop_stream inte nollar valet */
                                    if (open_stream(p, p->curfile, pos, e2) && open_audio(p, e2)) {
                                        int s2;
                                        p->t_start = now_ms(); p->paused_ms = 0; p->recent_n = 0; p->fill_n = 0;
                                        for (s2 = 0; s2 < NSLOT; s2++) { if (fill_slot(p, s2) > 0) send_slot(p, s2); else break; }
                                    } else { pstat.state = PS_ERROR; strlcpy_(pstat.errstr, e2, sizeof(pstat.errstr)); }
                                    p->automode = 1;
                                } else if (!p->lowpri) {
                                    /* lägsta nivån och ändå för långsamt: släpp fram GUI och nät */
                                    log_msg("spelare: hinner inte ens på lägsta nivån, sänker prioriteten");
                                    SetTaskPri(FindTask(NULL), -1);
                                    p->lowpri = 1;
                                }
                            } else if (p->lowpri && sum < audio * 80 / 100) {
                                SetTaskPri(FindTask(NULL), 1);
                                p->lowpri = 0;
                            }
                        }
                    }
                    any = 0;
                    for (s = 0; s < NSLOT; s++) if (p->slot[s].outstanding) any = 1;
                    if (!any && p->eof) { stop_stream(p); pstat.state = PS_FINISHED; }
                }
            }
        }
    }
out:
    {
        int s2, c2;
        for (s2 = 0; s2 < NSLOT; s2++) for (c2 = 0; c2 < NCHAN; c2++) if (p->slot[s2].buf[c2]) FreeVec(p->slot[s2].buf[c2]);
    }
    if (TimerBase) { CloseDevice((struct IORequest *)timerreq); TimerBase = NULL; }
    if (timerreq) DeleteIORequest((struct IORequest *)timerreq);
    if (timerport) DeleteMsgPort(timerport);
    DeleteMsgPort(p->cmdport);
    DeleteMsgPort(p->audioport);
    FreeVec(p->pcm[0]);
    FreeVec(p);
    if (MPEGABase) { CloseLibrary(MPEGABase); MPEGABase = NULL; }
    player_running = 0;
}

int player_start(struct MsgPort *replyport, int quality, int bits14)
{
    struct StartMsg sm;
    struct MsgPort *sp = CreateMsgPort();
    if (!sp) return 0;
    memset(&pstat, 0, sizeof(pstat));
    player_proc = CreateNewProcTags(NP_Entry, (ULONG)player_main, NP_Name, (ULONG)"CurrentPod player",
                                    NP_StackSize, 32768, NP_Priority, 1, TAG_DONE);
    if (!player_proc) { DeleteMsgPort(sp); return 0; }
    memset(&sm, 0, sizeof(sm));
    sm.msg.mn_ReplyPort = sp;
    sm.msg.mn_Length = sizeof(sm);
    PutMsg(&player_proc->pr_MsgPort, &sm.msg);
    WaitPort(sp);
    GetMsg(sp);
    DeleteMsgPort(sp);
    player_port = sm.port;
    if (player_port) {
        struct PlayerCmd *c = xalloc(sizeof(*c));
        if (c) {
            struct MsgPort *rp = CreateMsgPort();
            c->cmd = PC_SETQUALITY; c->arg = (quality & 0xFF) | (bits14 ? 0x100 : 0);
            c->msg.mn_ReplyPort = rp; c->msg.mn_Length = sizeof(*c);
            PutMsg(player_port, &c->msg);
            WaitPort(rp); GetMsg(rp);
            DeleteMsgPort(rp);
            xfree(c);
        }
    }
    (void)replyport;
    return player_port != NULL;
}

void player_send(struct PlayerCmd *c)
{
    c->msg.mn_Length = sizeof(*c);
    PutMsg(player_port, &c->msg);
}

void player_stop(void)
{
    struct PlayerCmd *c;
    if (!player_port) return;
    c = xalloc(sizeof(*c));
    if (c) {
        struct MsgPort *rp = CreateMsgPort();
        c->cmd = PC_QUIT;
        c->msg.mn_ReplyPort = rp; c->msg.mn_Length = sizeof(*c);
        PutMsg(player_port, &c->msg);
        WaitPort(rp); GetMsg(rp);
        DeleteMsgPort(rp);
        xfree(c);
    }
    while (player_running) Delay(2);
    player_port = NULL;
}
