#include "ext_gnss.h"

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

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
// - GSA: fix-mode (1/2/3) och kan senare användas för PDOP/VDOP
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

// Dela upp en NMEA-mening på kommatecken.
// Returnerar antal fält i tok[].
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

// ============================================================
// Sentence handlers
// ============================================================

// Hantera RMC-mening.
// Typiskt format:
// $GNRMC,hhmmss.ss,A,lat,N,lon,E,sog_knots,cog,...
static void handleRmc(char *s)
{
    char *tok[20] = {0};
    int n = splitCsv(s, tok, 20);

    if (n < 8)
        return;

    const char *status = tok[2];

    // A = valid, V = invalid
    if (!status || status[0] != 'A')
    {
        g_haveRmc = false;
        g_last.valid = false;
        return;
    }

    double lat = nmeaDegMinToDec(tok[3], tok[4] ? tok[4][0] : 'N');
    double lon = nmeaDegMinToDec(tok[5], tok[6] ? tok[6][0] : 'E');
    float sogKn = tok[7] ? atof(tok[7]) : 0.0f;
    float kmh = sogKn * 1.852f;

    g_last.lat = lat;
    g_last.lon = lon;
    g_last.speedKmh = kmh;

    g_haveRmc = true;
}

// Hantera GGA-mening.
// Typiskt format:
// $GNGGA,hhmmss.ss,lat,N,lon,E,fixQuality,numsats,hdop,alt,...
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

    // 0 = ingen giltig fix
    if (fixQuality <= 0)
    {
        g_haveGga = false;
        g_last.valid = false;
        return;
    }

    g_haveGga = true;
}

// Hantera GSA-mening.
// Typiskt format:
// $GNGSA,A,3,....,PDOP,HDOP,VDOP
//
// Fält:
// tok[2] = fix mode
//   1 = no fix
//   2 = 2D
//   3 = 3D
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

// Hantera en komplett NMEA-mening.
// Vi bryr oss just nu om RMC, GGA och GSA.
static void handleSentence(char *line)
{
    if (!line || line[0] != '$')
        return;

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

    // Fixa giltighet:
    // - giltig RMC
    // - giltig GGA
    // - och om vi fått GSA så ska fixMode vara minst 2
    bool gsaOk = (!g_haveGsa || g_last.fixMode >= 2);
    g_last.valid = (g_haveRmc && g_haveGga && gsaOk);
}

// ============================================================
// Public API
// ============================================================

bool extGnssBegin(int rxPin, int txPin, uint32_t baud)
{
    GNSS.begin(baud, SERIAL_8N1, rxPin, txPin);

    slen = 0;
    g_last = ExtGnssFix{};
    g_haveRmc = false;
    g_haveGga = false;
    g_haveGsa = false;

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

        // Om ett nytt '$' kommer mitt i en buffert så börjar vi om från den.
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
            // Overflow -> kasta aktuell mening
            slen = 0;
        }

        if (ch == '\n')
        {
            sbuf[slen] = 0;

            char *p = strchr(sbuf, '$');
            if (p && p[0] == '$')
            {
                // Ta bort CR/LF
                char *cr = strchr(p, '\r');
                if (cr)
                    *cr = 0;

                char *lf = strchr(p, '\n');
                if (lf)
                    *lf = 0;

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