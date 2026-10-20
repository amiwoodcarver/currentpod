/* CurrentPod - locale.library: inbyggd engelska, andra språk ur CurrentPod.catalog.
 * Katalogen söks i PROGDIR:Catalogs/<språk>/ och LOCALE:Catalogs/<språk>/.
 * Strängarna läses av alla processer (GetCatalogStr är bara en uppslagning),
 * men katalogen öppnas och stängs av huvudprogrammet.
 */
#define CP_STRINGS_TABLE          /* ta med den inbyggda strängtabellen ur strings.h */
#include "currentpod.h"
#include <libraries/locale.h>
#include <proto/locale.h>

struct LocaleBase *LocaleBase;
static struct Catalog *cp_catalog;
static struct Locale *cp_locale;

void cp_locale_open(const char *lang)
{
    if (!(LocaleBase = (struct LocaleBase *)OpenLibrary((STRPTR)"locale.library", 38))) return;
    cp_locale = OpenLocale(NULL);
    {
        struct TagItem tags[4];
        int n = 0;
        tags[n].ti_Tag = OC_BuiltInLanguage; tags[n++].ti_Data = (ULONG)"english";
        tags[n].ti_Tag = OC_Version;         tags[n++].ti_Data = 1;
        if (lang && lang[0]) { tags[n].ti_Tag = OC_Language; tags[n++].ti_Data = (ULONG)lang; }
        tags[n].ti_Tag = TAG_DONE; tags[n].ti_Data = 0;
        cp_catalog = OpenCatalogA(NULL, (STRPTR)"CurrentPod.catalog", tags);
    }
    log_msg("locale: katalog %s%s", cp_catalog ? "öppnad: " : "ingen (inbyggd engelska)", cp_catalog ? (char *)cp_catalog->cat_Language : "");
}

void cp_locale_close(void)
{
    if (!LocaleBase) return;
    if (cp_catalog) { CloseCatalog(cp_catalog); cp_catalog = NULL; }
    if (cp_locale) { CloseLocale(cp_locale); cp_locale = NULL; }
    CloseLibrary((struct Library *)LocaleBase); LocaleBase = NULL;
}

const char *cp_str(LONG id)
{
    const char *b = (id >= 0 && id < MSG_COUNT) ? cp_builtin[id] : "?";
    if (cp_catalog) return (const char *)GetCatalogStr(cp_catalog, id, (STRPTR)b);
    return b;
}

/* Decimaltecken och månadsnamn tas ur katalogen (inte ur Locale-strukturen), så att
   LANGUAGE=svenska ger en helt svensk vy även när Workbench står på engelska. */
const char *cp_decimal(void)
{
    return cp_str(MSG_DECIMAL_POINT);
}

const char *cp_month(int m)
{
    if (m < 1 || m > 12) m = 1;
    return cp_str(MSG_MONTH_1 + m - 1);
}
