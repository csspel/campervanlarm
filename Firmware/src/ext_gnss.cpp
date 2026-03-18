#include "ext_gnss.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// ============================================================
// Extern GNSS via UART
// ------------------------------------------------------------
// Den här modulen:
// - läser NMEA-data från extern GNSS över UART
// - parser ut de meningar vi bryr oss om
// - håller senaste kända fix i RAM
//
// Vi använder nu:
// - RMC: lat, lon, fart, giltighetsstatus
// - GGA: fix-quality, satelliter, HDOP, höjd
// - GSA: fix-mode (1/2/3)
//
// Förbättringar i denna version:
// - NMEA-checksum valideras innan mening används
// - enkel plausibility-check för lat/lon
// - lite robustare radslutshantering
// ============================================================

static HardwareSerial GNSS(2);

// Senaste kända fix
static ExtGnssFix g_last;

// Flaggar om vi fått användbara meningar
static bool g_haveRmc = false;
static bool g_haveGga = false;
static bool g_haveGsa = false;

// Enkel buffert för en NMEA-mening
static char sbuf[200];
static int slen = 0;

// ============================================================
// Helpers
// ============================================================

// Konvertera NMEA-format ddmm.mmmm / dddmm.mmmm till decimalgrader.
static double nmeaDegMinToDec(const char *dm, char hemi)
{
    if (!dm || !dm[0])
        return 0.0;

    double v = atof(dm);
    int deg = (int)(v / 100.0);
    double minutes = v - (deg * 100.0);
    double dec = (double)deg + (minutes / 60.0);

    if (hemi == 'S' || hemi == 'W')
        dec = -dec;

    return dec;
}

// Enkel plausibility-check av koordinater.
static bool latLonPlausible(double lat, double lon)
{
    if (lat < -90.0 || lat > 90.0)
        return false;

    if (lon < -180.0 || lon > 180.0)
        return false;

    return true;
}

// Dela upp en NMEA-mening på kommatecken.
static int splitCsv(char *s, char **tok, int maxTok)
{
    int n = 0;

    for (char *p = s; p && n < maxTok;)
    {
        tok[n++] = p;

        char *c = strchr(p, ',');
        if (!c)
            break;

        *c = 0;
        p = c + 1;
    }

    return n;
}

// Konvertera en hex-teckenuppsättning till nibble.
static int hexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    return -1;
}

// ------------------------------------------------------------
// Validera NMEA-checksum.
//
// Format:
//   $.....*HH
//
// XOR räknas över alla tecken mellan '$' och '*'.
// ------------------------------------------------------------
static bool nmeaChecksumOk(const char *line)
{
    if (!line || line[0] != '$')
        return false;

    const char *star = strchr(line, '*');
    if (!star)
        return false;

    // Måste finnas två hextecken efter '*'
    if (!star[1] || !star[2])
        return false;

    int hi = hexNibble(star[1]);
    int lo = hexNibble(star[2]);
    if (hi < 0 || lo < 0)
        return false;

    uint8_t expected = (uint8_t)((hi << 4) | lo);

    uint8_t sum = 0;
    for (const char *p = line + 1; p < star; ++p)
    {
        sum ^= (uint8_t)(*p);
    }

    return sum == expected;
}

// ------------------------------------------------------------
// Trimma bort CR/LF i slutet av strängen.
// ------------------------------------------------------------
static void trimLineEnd(char *s)
{
    if (!s)
        return;

    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n'))
    {
        s[len - 1] = 0;
        --len;
    }
}

// ------------------------------------------------------------
// För NMEA-hantering vill vi ofta parsa meningen utan checksum-delen,
// alltså allt före '*'.
// Denna funktion skriver 0 på '*' om sådan finns.
// ------------------------------------------------------------
static void stripChecksumPart(char *s)
{
    if (!s)
        return;

    char *star = strchr(s, '*');
    if (star)
        *star = 0;
}

// ------------------------------------------------------------
// Rensa parserstatus och senaste fix.
// Bra vid wake/start av ny burst så vi inte råkar använda
// gammal data som om den vore ny.
// ------------------------------------------------------------
void extGnssClearLatest()
{
    slen = 0;
    g_last = ExtGnssFix{};
    g_haveRmc = false;
    g_haveGga = false;
    g_haveGsa = false;
}

// ============================================================
// Sentence handlers
// ============================================================

// Hantera RMC-mening.
static void handleRmc(char *s)
{
    char *tok[20] = {0};
    int n = splitCsv(s, tok, 20);

    if (n < 8)
        return;

    const char *status = tok[2];

    if (!status || status[0] != 'A')
    {
        g_haveRmc = false;
        g_last.valid = false;
        return;
    }

    double lat = nmeaDegMinToDec(tok[3], tok[4] ? tok[4][0] : 'N');
    double lon = nmeaDegMinToDec(tok[5], tok[6] ? tok[6][0] : 'E');

    if (!latLonPlausible(lat, lon))
    {
        g_haveRmc = false;
        g_last.valid = false;
        return;
    }

    float sogKn = tok[7] ? atof(tok[7]) : 0.0f;
    float kmh = sogKn * 1.852f;

    g_last.lat = lat;
    g_last.lon = lon;
    g_last.speedKmh = kmh;

    g_haveRmc = true;
}

// Hantera GGA-mening.
static void handleGga(char *s)
{
    char *tok[20] = {0};
    int n = splitCsv(s, tok, 20);

    if (n < 10)
        return;

    int fixQuality = tok[6] ? atoi(tok[6]) : 0;
    int sats = tok[7] ? atoi(tok[7]) : 0;
    float hdop = tok[8] ? atof(tok[8]) : 99.0f;
    float alt = tok[9] ? atof(tok[9]) : 0.0f;

    g_last.fixQuality = (uint8_t)fixQuality;
    g_last.sats = sats;
    g_last.hdop = hdop;
    g_last.altM = alt;

    if (fixQuality <= 0)
    {
        g_haveGga = false;
        g_last.valid = false;
        return;
    }

    g_haveGga = true;
}

// Hantera GSA-mening.
static void handleGsa(char *s)
{
    char *tok[20] = {0};
    int n = splitCsv(s, tok, 20);

    if (n < 3)
        return;

    int fixMode = tok[2] ? atoi(tok[2]) : 0;
    g_last.fixMode = (uint8_t)fixMode;

    if (fixMode >= 2)
    {
        g_haveGsa = true;
    }
    else
    {
        g_haveGsa = false;
        g_last.valid = false;
    }
}

// Hantera komplett NMEA-mening.
// Viktigt:
// - checksum måste vara korrekt
// - sedan strippar vi "*HH" innan CSV-split
static void handleSentence(char *line)
{
    if (!line || line[0] != '$')
        return;

    trimLineEnd(line);

    if (!nmeaChecksumOk(line))
    {
        return;
    }

    stripChecksumPart(line);

    if (strlen(line) < 6)
        return;

    // Vi använder talker-oberoende matchning på suffix:
    // $GPRMC, $GNRMC, $GARMC ... -> "RMC"
    if (strncmp(line + 3, "RMC", 3) == 0)
    {
        handleRmc(line);
    }
    else if (strncmp(line + 3, "GGA", 3) == 0)
    {
        handleGga(line);
    }
    else if (strncmp(line + 3, "GSA", 3) == 0)
    {
        handleGsa(line);
    }

    // valid sätts bara om:
    // - vi har giltig RMC
    // - vi har giltig GGA
    // - och om GSA finns så måste den visa minst 2D fix
    bool gsaOk = (!g_haveGsa || g_last.fixMode >= 2);
    g_last.valid = (g_haveRmc && g_haveGga && gsaOk);
}

// ============================================================
// Public API
// ============================================================

bool extGnssBegin(int rxPin, int txPin, uint32_t baud)
{
    GNSS.begin(baud, SERIAL_8N1, rxPin, txPin);

    extGnssClearLatest();

    return true;
}

void extGnssEnd()
{
    GNSS.end();
}

void extGnssPoll()
{
    while (GNSS.available())
    {
        char ch = (char)GNSS.read();

        // Om en ny '$' kommer mitt i en redan påbörjad buffert, så försöker vi
        // avsluta den gamla som "best effort" och börjar sedan om med den nya.
        if (ch == '$' && slen > 0)
        {
            sbuf[slen] = 0;

            char *p = strchr(sbuf, '$');
            if (!p)
                p = sbuf;

            if (p[0] == '$')
            {
                handleSentence(p);
            }

            slen = 0;
        }

        if (slen < (int)sizeof(sbuf) - 1)
        {
            sbuf[slen++] = ch;
        }
        else
        {
            // Overflow: kasta aktuell rad och börja om.
            slen = 0;
        }

        // NMEA-rad komplett när LF kommer.
        if (ch == '\n')
        {
            sbuf[slen] = 0;

            char *p = strchr(sbuf, '$');
            if (p && p[0] == '$')
            {
                trimLineEnd(p);
                handleSentence(p);
            }

            slen = 0;
        }
    }
}

bool extGnssGetLatest(ExtGnssFix &out)
{
    out = g_last;
    return out.valid;
}