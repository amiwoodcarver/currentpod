/* CurrentPod - start, tooltypes, inställningar */
#include "currentpod.h"
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/icon.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <exec/tasks.h>
#include <stdlib.h>
#include <ctype.h>

static const char ver[] = "$VER: CurrentPod " CP_VERSION " (" CP_DATE ") © 2026";
const char stack_cookie[] = "$STACK:65536";

struct Config cfg;
extern char start_url[URLMAX], start_file[PATHMAX];
extern int autoconv_test, autoplay_index;
extern int autoplay_test;
struct Library *IconBase, *UtilityBase;
struct IntuitionBase *IntuitionBase;
extern struct WBStartup *_WBenchMsg;

static const char *cfg_path = "PROGDIR:CurrentPod.prefs";
static char cfg_lang[32];            /* LANGUAGE=svenska: välj katalog oavsett Locale-inställningarna */

static void cfg_defaults(void)
{
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.dldir, "PROGDIR:Episodes");
    cfg.quality = Q_AUTO;
    cfg.convquality = Q_LOW;
    cfg.bits14 = 1;
    strcpy(cfg.country, "se");
    cfg.maxep = 50;
    cfg.verify = 0;
    cfg.autoupdate = 0;
    cfg.skipfwd = 30;
    cfg.skipback = 15;
    cfg.volume = 64;
    cfg.autonext = 1;
    strcpy(cfg.browser, "Work:Internet/IBrowse/IBrowse \"%s\"");
}

void cfg_load(void)
{
    char *data, *line, *next;
    cfg_defaults();
    data = load_file(cfg_path, NULL);
    if (!data) return;
    for (line = data; line; line = next) {
        char *eq;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        {
            char *k = line, *v = eq + 1;
            if (!strcmp(k, "DownloadDir")) strlcpy_(cfg.dldir, v, sizeof(cfg.dldir));
            else if (!strcmp(k, "Quality")) cfg.quality = atoi(v);
            else if (!strcmp(k, "ConvertQuality")) cfg.convquality = atoi(v);
            else if (!strcmp(k, "Bits14")) cfg.bits14 = atoi(v);
            else if (!strcmp(k, "Country")) strlcpy_(cfg.country, v, sizeof(cfg.country));
            else if (!strcmp(k, "MaxEpisodes")) cfg.maxep = atoi(v);
            else if (!strcmp(k, "Verify")) cfg.verify = atoi(v);
            else if (!strcmp(k, "AutoUpdate")) cfg.autoupdate = atoi(v);
            else if (!strcmp(k, "SkipForward")) cfg.skipfwd = atoi(v);
            else if (!strcmp(k, "SkipBack")) cfg.skipback = atoi(v);
            else if (!strcmp(k, "Volume")) cfg.volume = atoi(v);
            else if (!strcmp(k, "AutoNext")) cfg.autonext = atoi(v);
            else if (!strcmp(k, "Browser")) strlcpy_(cfg.browser, v, sizeof(cfg.browser));
        }
    }
    xfree(data);
}

void cfg_save(void)
{
    char buf[1024];
    sprintf(buf, "DownloadDir=%s\nQuality=%ld\nConvertQuality=%ld\nBits14=%ld\nCountry=%s\nMaxEpisodes=%ld\nVerify=%ld\nAutoUpdate=%ld\n"
                 "SkipForward=%ld\nSkipBack=%ld\nVolume=%ld\nAutoNext=%ld\nBrowser=%s\n",
            cfg.dldir, cfg.quality, cfg.convquality, cfg.bits14, cfg.country, cfg.maxep, cfg.verify, cfg.autoupdate,
            cfg.skipfwd, cfg.skipback, cfg.volume, cfg.autonext, cfg.browser);
    save_file(cfg_path, buf, strlen(buf));
}

static int quality_from_name(const char *s)
{
    if (strieq(s, "LOW") || strieq(s, "LÅG")) return Q_LOW;
    if (strieq(s, "MEDIUM") || strieq(s, "MELLAN")) return Q_MEDIUM;
    if (strieq(s, "HIGH") || strieq(s, "HÖG")) return Q_HIGH;
    if (strieq(s, "FULL")) return Q_FULL;
    return Q_AUTO;
}

static void read_tooltypes(void)
{
    struct DiskObject *dob;
    char **tt;
    char *s;
    if (!_WBenchMsg || !IconBase) return;
    dob = GetDiskObject(_WBenchMsg->sm_ArgList[0].wa_Name);
    if (!dob) return;
    tt = (char **)dob->do_ToolTypes;
    if ((s = (char *)FindToolType((UBYTE **)tt, (STRPTR)"DOWNLOADDIR"))) strlcpy_(cfg.dldir, s, sizeof(cfg.dldir));
    if ((s = (char *)FindToolType((UBYTE **)tt, (STRPTR)"QUALITY"))) cfg.quality = quality_from_name(s);
    if ((s = (char *)FindToolType((UBYTE **)tt, (STRPTR)"CONVERT"))) cfg.convquality = (quality_from_name(s) == Q_MEDIUM || atoi(s) >= 20000) ? Q_MEDIUM : Q_LOW;
    if ((s = (char *)FindToolType((UBYTE **)tt, (STRPTR)"BITS"))) cfg.bits14 = (atoi(s) == 14);
    if ((s = (char *)FindToolType((UBYTE **)tt, (STRPTR)"COUNTRY"))) strlcpy_(cfg.country, s, sizeof(cfg.country));
    if ((s = (char *)FindToolType((UBYTE **)tt, (STRPTR)"MAXEPISODES"))) cfg.maxep = atoi(s);
    if ((s = (char *)FindToolType((UBYTE **)tt, (STRPTR)"VERIFY"))) cfg.verify = atoi(s);
    if ((s = (char *)FindToolType((UBYTE **)tt, (STRPTR)"BROWSER"))) strlcpy_(cfg.browser, s, sizeof(cfg.browser));
    if (FindToolType((UBYTE **)tt, (STRPTR)"DEBUG")) cp_debug = 1;
    if ((s = (char *)FindToolType((UBYTE **)tt, (STRPTR)"LANGUAGE"))) strlcpy_(cfg_lang, s, sizeof(cfg_lang));
    if ((s = (char *)FindToolType((UBYTE **)tt, (STRPTR)"URL"))) strlcpy_(start_url, s, sizeof(start_url));
    FreeDiskObject(dob);
}

static char log_override[PATHMAX];   /* LOG=<fil>: logga någon annanstans än PROGDIR:debug.log (test) */

static void read_args(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (strnieq(argv[i], "DOWNLOADDIR=", 12)) strlcpy_(cfg.dldir, argv[i] + 12, sizeof(cfg.dldir));
        else if (strnieq(argv[i], "QUALITY=", 8)) cfg.quality = quality_from_name(argv[i] + 8);
        else if (strnieq(argv[i], "CONVERT=", 8)) cfg.convquality = (quality_from_name(argv[i] + 8) == Q_MEDIUM || atoi(argv[i] + 8) >= 20000) ? Q_MEDIUM : Q_LOW;
        else if (strnieq(argv[i], "BITS=", 5)) cfg.bits14 = (atoi(argv[i] + 5) == 14);
        else if (strnieq(argv[i], "COUNTRY=", 8)) strlcpy_(cfg.country, argv[i] + 8, sizeof(cfg.country));
        else if (strieq(argv[i], "DEBUG")) cp_debug = 1;
        else if (strieq(argv[i], "DEBUG2")) cp_debug = 2;   /* pratigare: varje tick, varje buffert */
        else if (strnieq(argv[i], "URL=", 4)) strlcpy_(start_url, argv[i] + 4, sizeof(start_url));
        else if (strnieq(argv[i], "PLAYFILE=", 9)) strlcpy_(start_file, argv[i] + 9, sizeof(start_file));
        else if (strieq(argv[i], "AUTOPLAY")) autoplay_test = 1;
        else if (strieq(argv[i], "AUTOCONVERT")) { autoplay_test = 1; autoconv_test = 1; }
        else if (strnieq(argv[i], "AUTOEP=", 7)) autoplay_index = atoi(argv[i] + 7);
        else if (strnieq(argv[i], "LOG=", 4)) strlcpy_(log_override, argv[i] + 4, sizeof(log_override));
        else if (strnieq(argv[i], "LANGUAGE=", 9)) strlcpy_(cfg_lang, argv[i] + 9, sizeof(cfg_lang));
    }
}

static int real_main(int argc, char **argv);

/* Körs vi med för liten stack (Run från ett Shell ger ofta 4 KB) byter vi
   till en egen på 64 KB; MUI och nätkoden vill ha det.

   Mellan de två StackSwap-anropen får main() inte lägga något på stacken som
   tas bort efteråt: gcc skjuter upp stackrensningen efter anrop (-fdefer-pop),
   så argument som läggs på den nya stacken plockas annars bort från den gamla
   först efter bytet tillbaka, och main() återvänder till skräp (Guru
   80000006 vid avslut). Därför görs allt arbete i stack_main(), som inte får
   inlinas och inte tar några argument, och main.c byggs med -fno-defer-pop.
   Variablerna som används efter bytet är statiska: med -fomit-frame-pointer
   adresseras lokala variabler via SP, som då pekar på den andra stacken. */
#define WANTSTACK 65536
static int main_argc; static char **main_argv; static int main_rc;
static void stack_main(void) __attribute__((noinline));
static void stack_main(void) { main_rc = real_main(main_argc, main_argv); }

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    ULONG have = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;
    (void)ver;
    (void)stack_cookie;
    if (have < WANTSTACK) {
        static struct StackSwapStruct sss;
        static UBYTE *stk;
        stk = AllocVec(WANTSTACK, MEMF_ANY);
        if (stk) {
            main_argc = argc; main_argv = argv;
            sss.stk_Lower = stk;
            sss.stk_Upper = (ULONG)stk + WANTSTACK;
            sss.stk_Pointer = (APTR)sss.stk_Upper;
            StackSwap(&sss);
            stack_main();
            StackSwap(&sss);
            FreeVec(stk);
            return main_rc;
        }
    }
    return real_main(argc, argv);
}

static int real_main(int argc, char **argv)
{
    int rc;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
    UtilityBase = OpenLibrary((STRPTR)"utility.library", 37);
    IconBase = OpenLibrary((STRPTR)"icon.library", 37);
    cfg_load();
    if (_WBenchMsg) read_tooltypes(); else read_args(argc, argv);
    {
        char dir[PATHMAX];
        if (abs_path("PROGDIR:", dir, sizeof(dir))) add_part(log_path, dir, "debug.log", sizeof(log_path));
        if (log_override[0]) strlcpy_(log_path, log_override, sizeof(log_path));
    }
    if (cp_debug) {
        char iso[32];
        DeleteFile((STRPTR)log_path);
        log_msg("CurrentPod %s startar, logg %s", CP_VERSION, log_path);
        log_msg("ctype: isalpha(F)=%ld isdigit(7)=%ld isspace(sp)=%ld tolower(A)=%ld atoi(17)=%ld",
                (LONG)(isalpha('F') != 0), (LONG)(isdigit('7') != 0), (LONG)(isspace(' ') != 0), (LONG)tolower('A'), (LONG)atoi("17"));
        parse_rfc822_date("Fri, 17 Oct 2026 10:00:16 +0000", iso);
        log_msg("datumtest: %s", iso);
    }
    cp_locale_open(cfg_lang[0] ? cfg_lang : NULL);   /* efter loggstarten så att katalogvalet syns i loggen */
    db_init("PROGDIR:Feeds");
    db_load();
    make_dirs(cfg.dldir);
    {   /* arbetarprocessen har ingen PROGDIR: */
        char dir[PATHMAX];
        if (abs_path(cfg.dldir, dir, sizeof(dir))) strlcpy_(cfg.dldir, dir, sizeof(cfg.dldir));
        log_msg("avsnitt hämtas till %s", cfg.dldir);
    }
    rc = gui_run();
    cp_locale_close();
    if (IconBase) CloseLibrary(IconBase);
    if (UtilityBase) CloseLibrary(UtilityBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
