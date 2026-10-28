/*
 * Hue3DS v2 - Philips Hue control for homebrew 3DS
 *
 * New in v2:
 *  - Rooms tab (L/R switches tabs): control whole rooms, incl. a
 *    synthetic "All lights" room for changing every lamp at once
 *  - One-tap presets: brightness moods + multi-color palettes
 *  - Party mode: beat detection through the 3DS microphone drives
 *    color pulses across all lights
 *  - Status line moved to the top (no longer collides with the list)
 *
 * Buttons: D-Pad select · A toggle · Y refresh · L Lights tab ·
 *          R Rooms tab · SELECT setup · START exit
 */

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <sys/stat.h>

#define CONFIG_DIR  "sdmc:/3ds/hue3ds"
#define CONFIG_PATH CONFIG_DIR "/config.txt"
#define MAX_LIGHTS  32
#define MAX_ROOMS   17            /* incl. synthetic "All lights"      */
#define RESP_SIZE   (128 * 1024)

/* ---- iOS-ish palette ------------------------------------------------ */
#define COL_BG        C2D_Color32(242, 242, 247, 255)
#define COL_CARD      C2D_Color32(255, 255, 255, 255)
#define COL_TEXT      C2D_Color32( 28,  28,  30, 255)
#define COL_TEXT2     C2D_Color32(142, 142, 147, 255)
#define COL_ACCENT    C2D_Color32(  0, 122, 255, 255)
#define COL_TRACK     C2D_Color32(229, 229, 234, 255)
#define COL_OFFDOT    C2D_Color32(209, 209, 214, 255)
#define COL_GREEN     C2D_Color32( 52, 199,  89, 255)
#define COL_PURPLE    C2D_Color32(175,  82, 222, 255)
#define COL_KNOB_EDGE C2D_Color32(200, 200, 205, 255)

typedef struct {
    char id[8];
    char name[33];
    bool on;
    int  bri, hue, sat;
} Light;

typedef struct {
    char id[8];
    char name[33];
    bool anyOn;
    int  count;                   /* number of lights in the room      */
    int  nlids;                   /* how many light ids we captured    */
    char lids[16][8];             /* light ids for palette presets     */
    int  bri, hue, sat;           /* last known group action           */
} Room;

static char  bridgeIp[64] = "";
static char  username[64] = "";
static Light lights[MAX_LIGHTS];
static Room  rooms[MAX_ROOMS];
static int   numLights = 0, numRooms = 0;
static int   tab = 0;             /* 0 = Lights, 1 = Rooms             */
static int   selL = 0, selR = 0;
static int   scrollL = 0, scrollR = 0;
static int   presetIdx[MAX_ROOMS];
static char  statusMsg[100] = "Welcome!";
static char *respBuf = NULL;

/* party mode */
static bool  party = false;
static bool  micReady = false;
static u8   *micBuf = NULL;
static float micAvg = 400.0f, micRms = 0.0f;
static int   beatCooldown = 0;
static int   bigCooldown = 0;
static int   partyLight = 0;
static int   partyColor = 0;
static int   bottomTab = 0;       /* 0 Control · 1 Scenes · 2 Party    */
static float partySens = 0.7f;    /* 0..1 beat threshold               */
static float partyGain = 0.55f;   /* 0..1 of the mic's hardware gain   */
static int   partyMode = 0;       /* 0 Pop · 1 Pulse · 2 Wave · 3 Chase */
static int   pulseDecay = 0;
static int   chaseIdx = 0;
static int   waveBase = 0;
static int   tapFlash = 0;
static int   tappedPreset = -1;

static C3D_RenderTarget *topScr, *botScr;
static C2D_TextBuf g_textBuf;

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

static Result httpDo(HTTPC_RequestMethod method, const char *url,
                     const char *body, char *out, u32 outSize)
{
    httpcContext ctx;
    Result ret;
    u32 statuscode = 0, readSize = 0, total = 0;

    out[0] = '\0';
    ret = httpcOpenContext(&ctx, method, url, 1);
    if (R_FAILED(ret)) return ret;

    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(&ctx, "User-Agent", "Hue3DS/2.0");
    httpcAddRequestHeaderField(&ctx, "Connection", "close");

    if (body) {
        httpcAddRequestHeaderField(&ctx, "Content-Type", "application/json");
        ret = httpcAddPostDataRaw(&ctx, (const u32 *)body, strlen(body));
        if (R_FAILED(ret)) { httpcCloseContext(&ctx); return ret; }
    }

    ret = httpcBeginRequest(&ctx);
    if (R_FAILED(ret)) { httpcCloseContext(&ctx); return ret; }

    ret = httpcGetResponseStatusCode(&ctx, &statuscode);
    if (R_FAILED(ret)) { httpcCloseContext(&ctx); return ret; }

    do {
        ret = httpcDownloadData(&ctx, (u8 *)out + total,
                                outSize - 1 - total, &readSize);
        total += readSize;
    } while (ret == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING &&
             total < outSize - 1);

    out[total] = '\0';
    httpcCloseContext(&ctx);
    return (statuscode == 200) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Config + helpers                                                    */
/* ------------------------------------------------------------------ */

static bool loadConfig(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return false;
    bool ok = fscanf(f, "%63s %63s", bridgeIp, username) == 2;
    char tag[16];
    if (fscanf(f, "%15s", tag) == 1 && !strcmp(tag, "party")) {
        int s = 70, m = 0, g = 55;
        if (fscanf(f, "%d %d", &s, &m) == 2) {
            partySens = s / 100.0f;
            if (partySens < 0) partySens = 0;
            if (partySens > 1) partySens = 1;
            partyMode = m & 3;
            if (fscanf(f, "%d", &g) == 1) {
                partyGain = g / 100.0f;
                if (partyGain < 0) partyGain = 0;
                if (partyGain > 1) partyGain = 1;
            }
        }
    }
    fclose(f);
    return ok && bridgeIp[0] && username[0];
}

static void saveConfig(void)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "%s %s\n", bridgeIp, username);
    fprintf(f, "party %d %d %d\n", (int)(partySens * 100), partyMode,
            (int)(partyGain * 100));
    fclose(f);
}

static bool inputText(const char *hint, const char *initial,
                      char *out, size_t size)
{
    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, size - 1);
    swkbdSetHintText(&swkbd, hint);
    if (initial && initial[0]) swkbdSetInitialText(&swkbd, initial);
    SwkbdButton btn = swkbdInputText(&swkbd, out, size);
    return btn == SWKBD_BUTTON_CONFIRM && out[0] != '\0';
}

static bool jsonFindString(const char *json, const char *key,
                           char *out, size_t size)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return false;
    size_t len = (size_t)(e - p);
    if (len >= size) len = size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static u32 hueColor(int hue, int sat, int bri)
{
    float h = (hue / 65535.0f) * 360.0f;
    float s = sat / 254.0f;
    float v = 0.35f + 0.65f * (bri / 254.0f);
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r = 0, g = 0, b = 0;
    if      (h <  60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    return C2D_Color32((u8)((r + m) * 255), (u8)((g + m) * 255),
                       (u8)((b + m) * 255), 255);
}

/* ------------------------------------------------------------------ */
/* Hue API parsing                                                     */
/* ------------------------------------------------------------------ */

static void parseLights(const char *json)
{
    numLights = 0;
    int depth = 0;
    const char *p = json;

    while (*p && numLights <= MAX_LIGHTS) {
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') { depth--; p++; continue; }
        if (*p != '"') { p++; continue; }

        const char *start = p + 1;
        const char *end = strchr(start, '"');
        if (!end) break;
        size_t len = (size_t)(end - start);

        const char *q = end + 1;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        bool isKey = (*q == ':');

        if (isKey && depth == 1) {
            if (numLights < MAX_LIGHTS) {
                Light *L = &lights[numLights++];
                snprintf(L->id, sizeof(L->id), "%.*s",
                         (int)(len < 7 ? len : 7), start);
                snprintf(L->name, sizeof(L->name), "Light %s", L->id);
                L->on = false; L->bri = 254; L->hue = 8000; L->sat = 140;
            }
        } else if (isKey && numLights > 0) {
            Light *L = &lights[numLights - 1];
            const char *v = q + 1;
            while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;

            if (depth == 2 && len == 4 && !strncmp(start, "name", 4) &&
                *v == '"') {
                const char *ve = strchr(v + 1, '"');
                if (ve) {
                    size_t nlen = (size_t)(ve - (v + 1));
                    if (nlen >= sizeof(L->name)) nlen = sizeof(L->name) - 1;
                    memcpy(L->name, v + 1, nlen);
                    L->name[nlen] = '\0';
                    p = ve + 1;
                    continue;
                }
            } else if (depth == 3 && len == 2 && !strncmp(start, "on", 2)) {
                L->on = !strncmp(v, "true", 4);
            } else if (depth == 3 && len == 3 && !strncmp(start, "bri", 3)) {
                L->bri = atoi(v);
            } else if (depth == 3 && len == 3 && !strncmp(start, "hue", 3)) {
                L->hue = atoi(v);
            } else if (depth == 3 && len == 3 && !strncmp(start, "sat", 3)) {
                L->sat = atoi(v);
            }
        }
        p = end + 1;
    }
    if (selL >= numLights) selL = numLights ? numLights - 1 : 0;
}

static void parseGroups(const char *json)
{
    /* room 0 is the synthetic "All lights" group (Hue group id 0) */
    numRooms = 1;
    strcpy(rooms[0].id, "0");
    strcpy(rooms[0].name, "All lights");
    rooms[0].anyOn = false;
    rooms[0].count = numLights;
    rooms[0].nlids = numLights < 16 ? numLights : 16;
    for (int i = 0; i < rooms[0].nlids; i++)
        strcpy(rooms[0].lids[i], lights[i].id);
    rooms[0].bri = 254; rooms[0].hue = 8000; rooms[0].sat = 140;

    int depth = 0;
    const char *p = json;

    while (*p && numRooms <= MAX_ROOMS) {
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') { depth--; p++; continue; }
        if (*p != '"') { p++; continue; }

        const char *start = p + 1;
        const char *end = strchr(start, '"');
        if (!end) break;
        size_t len = (size_t)(end - start);

        const char *q = end + 1;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        bool isKey = (*q == ':');

        if (isKey && depth == 1) {
            if (numRooms < MAX_ROOMS) {
                Room *R = &rooms[numRooms++];
                snprintf(R->id, sizeof(R->id), "%.*s",
                         (int)(len < 7 ? len : 7), start);
                snprintf(R->name, sizeof(R->name), "Room %s", R->id);
                R->anyOn = false; R->count = 0;
                R->bri = 254; R->hue = 8000; R->sat = 140;
            }
        } else if (isKey && numRooms > 1) {
            Room *R = &rooms[numRooms - 1];
            const char *v = q + 1;
            while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;

            if (depth == 2 && len == 4 && !strncmp(start, "name", 4) &&
                *v == '"') {
                const char *ve = strchr(v + 1, '"');
                if (ve) {
                    size_t nlen = (size_t)(ve - (v + 1));
                    if (nlen >= sizeof(R->name)) nlen = sizeof(R->name) - 1;
                    memcpy(R->name, v + 1, nlen);
                    R->name[nlen] = '\0';
                    p = ve + 1;
                    continue;
                }
            } else if (depth == 2 && len == 6 &&
                       !strncmp(start, "lights", 6) && *v == '[') {
                /* count + capture quoted ids until the bracket */
                const char *b = v + 1;
                int cnt = 0;
                R->nlids = 0;
                while (*b && *b != ']') {
                    if (*b == '"') {
                        const char *be = strchr(b + 1, '"');
                        if (!be) break;
                        if (R->nlids < 16) {
                            size_t il = (size_t)(be - (b + 1));
                            if (il > 7) il = 7;
                            memcpy(R->lids[R->nlids], b + 1, il);
                            R->lids[R->nlids][il] = '\0';
                            R->nlids++;
                        }
                        cnt++;
                        b = be + 1;
                    } else b++;
                }
                R->count = cnt;
                p = (*b == ']') ? b + 1 : b;
                continue;
            } else if (depth == 3 && len == 6 &&
                       !strncmp(start, "any_on", 6)) {
                R->anyOn = !strncmp(v, "true", 4);
            } else if (depth == 3 && len == 3 && !strncmp(start, "bri", 3)) {
                R->bri = atoi(v);
            } else if (depth == 3 && len == 3 && !strncmp(start, "hue", 3)) {
                R->hue = atoi(v);
            } else if (depth == 3 && len == 3 && !strncmp(start, "sat", 3)) {
                R->sat = atoi(v);
            }
        }
        p = end + 1;
    }
    /* All-lights anyOn mirrors the individual lights */
    for (int i = 0; i < numLights; i++)
        if (lights[i].on) { rooms[0].anyOn = true; break; }
    if (selR >= numRooms) selR = numRooms ? numRooms - 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Hue API actions                                                     */
/* ------------------------------------------------------------------ */

static bool apiGetPath(const char *path)
{
    char url[224];
    snprintf(url, sizeof(url), "http://%s/api/%s%s", bridgeIp, username,
             path);
    return !R_FAILED(httpDo(HTTPC_METHOD_GET, url, NULL, respBuf,
                            RESP_SIZE));
}

static void refreshAll(void)
{
    if (!apiGetPath("/lights")) {
        snprintf(statusMsg, sizeof(statusMsg),
                 "Can't reach bridge at %s", bridgeIp);
        return;
    }
    if (strstr(respBuf, "\"error\"")) {
        snprintf(statusMsg, sizeof(statusMsg),
                 "Bridge rejected key - press SELECT to re-pair");
        return;
    }
    parseLights(respBuf);
    if (apiGetPath("/groups")) parseGroups(respBuf);
    snprintf(statusMsg, sizeof(statusMsg),
             "%d light(s), %d room(s)", numLights, numRooms - 1);
}

static void putState(bool isRoom, const char *id, const char *jsonBody)
{
    char url[256];
    if (isRoom)
        snprintf(url, sizeof(url), "http://%s/api/%s/groups/%s/action",
                 bridgeIp, username, id);
    else
        snprintf(url, sizeof(url), "http://%s/api/%s/lights/%s/state",
                 bridgeIp, username, id);
    if (R_FAILED(httpDo(HTTPC_METHOD_PUT, url, jsonBody, respBuf,
                        RESP_SIZE)))
        snprintf(statusMsg, sizeof(statusMsg), "Request failed - Wi-Fi ok?");
}

/* selection helpers: the bottom controls act on light or room */
static bool selIsRoom(void) { return tab == 1; }
static const char *selId(void)
{
    return selIsRoom() ? rooms[selR].id : lights[selL].id;
}
static const char *selName(void)
{
    return selIsRoom() ? rooms[selR].name : lights[selL].name;
}
static bool selOn(void)
{
    return selIsRoom() ? rooms[selR].anyOn : lights[selL].on;
}
static void selSetOn(bool on)
{
    if (selIsRoom()) {
        rooms[selR].anyOn = on;
        if (!strcmp(rooms[selR].id, "0"))
            for (int i = 0; i < numLights; i++) lights[i].on = on;
    } else lights[selL].on = on;
}
static int  *selBri(void) { return selIsRoom() ? &rooms[selR].bri
                                               : &lights[selL].bri; }
static int  *selHue(void) { return selIsRoom() ? &rooms[selR].hue
                                               : &lights[selL].hue; }
static int  *selSat(void) { return selIsRoom() ? &rooms[selR].sat
                                               : &lights[selL].sat; }
static bool selValid(void)
{
    return selIsRoom() ? (numRooms > 0) : (numLights > 0);
}

static void sendSelected(bool withColor)
{
    if (!selValid()) return;
    char body[120];
    if (withColor)
        snprintf(body, sizeof(body),
                 "{\"on\":true,\"bri\":%d,\"hue\":%d,\"sat\":%d}",
                 *selBri(), *selHue(), *selSat());
    else
        snprintf(body, sizeof(body), "{\"on\":true,\"bri\":%d}", *selBri());
    selSetOn(true);
    putState(selIsRoom(), selId(), body);
}

static void toggleSelected(void)
{
    if (!selValid()) return;
    bool on = !selOn();
    selSetOn(on);
    putState(selIsRoom(), selId(), on ? "{\"on\":true}" : "{\"on\":false}");
    snprintf(statusMsg, sizeof(statusMsg), "%s -> %s", selName(),
             on ? "on" : "off");
}

static void allLights(bool on)
{
    char body[16];
    snprintf(body, sizeof(body), "{\"on\":%s}", on ? "true" : "false");
    putState(true, "0", body);
    for (int i = 0; i < numLights; i++) lights[i].on = on;
    for (int i = 0; i < numRooms; i++) rooms[i].anyOn = on;
    snprintf(statusMsg, sizeof(statusMsg), "All lights %s",
             on ? "on" : "off");
}

static void renameSelectedLight(void)
{
    if (tab != 0 || !numLights) return;
    Light *L = &lights[selL];
    char newName[33];
    if (!inputText("New name for this light", L->name,
                   newName, sizeof(newName)))
        return;
    char url[224], body[64];
    snprintf(url, sizeof(url), "http://%s/api/%s/lights/%s",
             bridgeIp, username, L->id);
    snprintf(body, sizeof(body), "{\"name\":\"%s\"}", newName);
    if (!R_FAILED(httpDo(HTTPC_METHOD_PUT, url, body, respBuf, RESP_SIZE))) {
        strncpy(L->name, newName, sizeof(L->name) - 1);
        L->name[sizeof(L->name) - 1] = '\0';
        snprintf(statusMsg, sizeof(statusMsg), "Renamed to %s", newName);
    }
}

typedef struct {
    const char *name;
    bool palette;
    int bri;
    int ct;              /* mired white temp when !palette             */
    const int *hues;
    int nh;
} Preset;

static const int PAL_SUNSET[] = { 1500, 6500, 54000, 58000 };
static const int PAL_TROPIC[] = { 25500, 38000, 46920, 12750 };
static const int PAL_CANDY[]  = { 56100, 50000, 46920, 60000 };
static const int PAL_FUNKY[]  = { 0, 12750, 25500, 46920, 56100, 50000 };

static const Preset PRESETS[] = {
    { "Super bright", false, 254, 230, NULL, 0 },
    { "Daylight",     false, 254, 180, NULL, 0 },
    { "Medium",       false, 150, 320, NULL, 0 },
    { "Relax",        false, 140, 450, NULL, 0 },
    { "Dimmed",       false,  70, 430, NULL, 0 },
    { "Nightlight",   false,  25, 500, NULL, 0 },
    { "Sunset",       true,  210, 0, PAL_SUNSET, 4 },
    { "Tropical",     true,  230, 0, PAL_TROPIC, 4 },
    { "Candy",        true,  230, 0, PAL_CANDY,  4 },
    { "Funky",        true,  254, 0, PAL_FUNKY,  6 },
};
#define NUM_PRESETS (int)(sizeof(PRESETS) / sizeof(PRESETS[0]))

static void applyPreset(int roomIdx, const Preset *P)
{
    Room *R = &rooms[roomIdx];
    if (!P->palette) {
        char body[96];
        snprintf(body, sizeof(body),
                 "{\"on\":true,\"bri\":%d,\"ct\":%d}", P->bri, P->ct);
        putState(true, R->id, body);
    } else if (R->nlids > 0) {
        /* paint each lamp a different color from the palette */
        for (int i = 0; i < R->nlids; i++) {
            char body[112];
            snprintf(body, sizeof(body),
                     "{\"on\":true,\"bri\":%d,\"hue\":%d,"
                     "\"sat\":254}", P->bri, P->hues[i % P->nh]);
            putState(false, R->lids[i], body);
        }
    } else {
        char body[112];
        snprintf(body, sizeof(body),
                 "{\"on\":true,\"bri\":%d,\"hue\":%d,\"sat\":254}",
                 P->bri, P->hues[0]);
        putState(true, R->id, body);
    }
    R->anyOn = true;
    if (roomIdx == 0)
        for (int i = 0; i < numLights; i++) lights[i].on = true;
    snprintf(statusMsg, sizeof(statusMsg), "%s: %s", R->name, P->name);
}

static void cyclePreset(void)
{
    int roomIdx = (tab == 1 && numRooms) ? selR : 0;
    if (!numRooms) return;
    presetIdx[roomIdx] = (presetIdx[roomIdx] + 1) % NUM_PRESETS;
    applyPreset(roomIdx, &PRESETS[presetIdx[roomIdx]]);
}

/* ------------------------------------------------------------------ */
/* Party mode (mic beat detection -> all-lights color pulses)          */
/* ------------------------------------------------------------------ */

static const int PARTY_HUES[] = { 0, 46920, 12750, 56100, 25500, 50000,
                                  6000, 38000 };

static bool partyStart(void)
{
    if (micReady) return true;
    micBuf = memalign(0x1000, 0x30000);
    if (!micBuf) return false;
    if (R_FAILED(micInit(micBuf, 0x30000))) return false;
    MICU_SetGain((u8)(partyGain * 119.0f));
    MICU_SetPower(true);
    MICU_SetAllowShellClosed(true);
    if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED,
                                    MICU_SAMPLE_RATE_16360, 0,
                                    micGetSampleDataSize(), true))) {
        micExit();
        return false;
    }
    micReady = true;
    return true;
}

static void partyStop(void)
{
    if (!micReady) return;
    MICU_StopSampling();
    micExit();
    free(micBuf);
    micBuf = NULL;
    micReady = false;
}

static void partyBeatAction(float rmsNorm)
{
    char body[128];
    int bri = 150 + (int)(104.0f * rmsNorm);
    if (bri > 254) bri = 254;

    switch (partyMode) {
    case 0: /* Pop: one random-colored lamp per beat */
        if (numLights == 0) break;
        partyLight = (partyLight + 1) % numLights;
        {
            int h = PARTY_HUES[rand() % 8] + (rand() % 4000) - 2000;
            if (h < 0) h += 65536;
            snprintf(body, sizeof(body),
                     "{\"on\":true,\"hue\":%d,\"sat\":254,\"bri\":%d,"
                     "\"transitiontime\":0}", h, bri);
            putState(false, lights[partyLight].id, body);
        }
        break;
    case 1: /* Pulse: whole room flashes bright, then breathes back down */
        partyColor = (partyColor + 1 + (rand() % 3)) % 8;
        snprintf(body, sizeof(body),
                 "{\"on\":true,\"hue\":%d,\"sat\":254,\"bri\":254,"
                 "\"transitiontime\":0}", PARTY_HUES[partyColor]);
        putState(true, "0", body);
        pulseDecay = 26;
        break;
    case 2: /* Wave: the color wheel marches across your lamps */
        waveBase = (waveBase + 7000) % 65536;
        for (int i = 0; i < numLights; i++) {
            int h = (waveBase + i * (numLights ? 65536 / numLights : 0))
                    % 65536;
            snprintf(body, sizeof(body),
                     "{\"on\":true,\"hue\":%d,\"sat\":254,\"bri\":%d,"
                     "\"transitiontime\":1}", h, bri);
            putState(false, lights[i].id, body);
        }
        break;
    case 3: /* Chase: a bright spotlight runs lamp to lamp */
        if (numLights == 0) break;
        {
            int prev = chaseIdx % numLights;
            chaseIdx = (chaseIdx + 1) % numLights;
            waveBase = (waveBase + 3000) % 65536;
            snprintf(body, sizeof(body),
                     "{\"on\":true,\"hue\":%d,\"sat\":254,\"bri\":50,"
                     "\"transitiontime\":2}", waveBase);
            putState(false, lights[prev].id, body);
            snprintf(body, sizeof(body),
                     "{\"on\":true,\"hue\":%d,\"sat\":254,\"bri\":254,"
                     "\"transitiontime\":0}", waveBase);
            putState(false, lights[chaseIdx].id, body);
        }
        break;
    }
}

static int partyModeCooldown(void)
{
    switch (partyMode) {
    case 0: return 10;            /* Pop: up to ~6/s                   */
    case 1: return 65;            /* Pulse: group commands, ~1/s       */
    case 2: return 40;            /* Wave: n light commands per beat   */
    case 3: return 14;            /* Chase: 2 commands, fast           */
    }
    return 20;
}

static void partyTick(void)
{
    if (!micReady) return;
    u32 size = micGetSampleDataSize();
    u32 off  = micGetLastSampleOffset();
    s16 *smp = (s16 *)micBuf;
    u32 total = size / 2;
    const int N = 1024;

    double acc = 0;
    for (int i = 0; i < N; i++) {
        u32 idx = (off / 2 + total - 1 - i) % total;
        float s = smp[idx];
        acc += s * s;
    }
    micRms = sqrtf((float)(acc / N));
    micAvg = 0.94f * micAvg + 0.06f * micRms;

    if (beatCooldown > 0) beatCooldown--;

    /* pulse mode: breathe back down after the flash */
    if (pulseDecay > 0 && --pulseDecay == 0) {
        char body[64];
        snprintf(body, sizeof(body),
                 "{\"bri\":70,\"transitiontime\":4}");
        putState(true, "0", body);
    }

    /* sensitivity maps to detection floor + ratio */
    float floorv = 750.0f - partySens * 620.0f;   /* 750 .. 130        */
    float ratio  = 1.65f - partySens * 0.55f;     /* 1.65 .. 1.10      */

    if (beatCooldown == 0 && micRms > micAvg * ratio &&
        micRms > floorv) {
        beatCooldown = partyModeCooldown();
        float rmsNorm = micRms / 4000.0f;
        if (rmsNorm > 1) rmsNorm = 1;
        partyBeatAction(rmsNorm);
    }
}

/* ------------------------------------------------------------------ */
/* Setup / pairing                                                     */
/* ------------------------------------------------------------------ */

typedef enum { ST_MAIN, ST_PAIR } AppState;
static AppState state = ST_MAIN;

static bool tryPair(void)
{
    char url[96];
    snprintf(url, sizeof(url), "http://%s/api", bridgeIp);
    Result rc = httpDo(HTTPC_METHOD_POST, url,
                       "{\"devicetype\":\"hue3ds#nintendo3ds\"}",
                       respBuf, RESP_SIZE);
    if (R_FAILED(rc)) {
        snprintf(statusMsg, sizeof(statusMsg),
                 "Can't reach %s - check IP and Wi-Fi", bridgeIp);
        return false;
    }
    if (jsonFindString(respBuf, "username", username, sizeof(username))) {
        saveConfig();
        snprintf(statusMsg, sizeof(statusMsg), "Paired!");
        return true;
    }
    snprintf(statusMsg, sizeof(statusMsg),
             "Link button not pressed - press it, then tap Pair");
    return false;
}

static void startSetup(void)
{
    char ip[64];
    if (inputText("Hue Bridge IP (e.g. 192.168.1.42)", bridgeIp,
                  ip, sizeof(ip))) {
        strncpy(bridgeIp, ip, sizeof(bridgeIp) - 1);
        bridgeIp[sizeof(bridgeIp) - 1] = '\0';
        state = ST_PAIR;
        snprintf(statusMsg, sizeof(statusMsg),
                 "Press the link button on the bridge, then tap Pair");
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void drawText(const char *s, float x, float y, float scale,
                     u32 col, bool center)
{
    C2D_Text t;
    C2D_TextParse(&t, g_textBuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | (center ? C2D_AlignCenter : 0),
                 x, y, 0.5f, scale, scale, col);
}

static void drawTextR(const char *s, float xRight, float y, float scale,
                      u32 col)
{
    C2D_Text t;
    C2D_TextParse(&t, g_textBuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | C2D_AlignRight, xRight, y, 0.5f,
                 scale, scale, col);
}

static void drawRRect(float x, float y, float w, float h, float r, u32 col)
{
    C2D_DrawRectSolid(x + r, y, 0.4f, w - 2 * r, h, col);
    C2D_DrawRectSolid(x, y + r, 0.4f, r, h - 2 * r, col);
    C2D_DrawRectSolid(x + w - r, y + r, 0.4f, r, h - 2 * r, col);
    C2D_DrawCircleSolid(x + r,     y + r,     0.4f, r, col);
    C2D_DrawCircleSolid(x + w - r, y + r,     0.4f, r, col);
    C2D_DrawCircleSolid(x + r,     y + h - r, 0.4f, r, col);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.4f, r, col);
}

/* ---- top screen ---- */
#define CARD_X 12
#define CARD_W 376
#define CARD_H 31
#define CARD_GAP 4
#define LIST_Y 48
#define VISIBLE 5

static void drawTabPill(float x, const char *label, bool active)
{
    drawRRect(x, 6, 62, 18, 9, active ? COL_ACCENT : COL_TRACK);
    drawText(label, x + 31, 8, 0.4f, active ? COL_CARD : COL_TEXT2, true);
}

static void drawTop(void)
{
    drawText("Hue", 14, 4, 0.72f, COL_TEXT, false);

    drawTabPill(64,  "Lights", tab == 0);
    drawTabPill(132, "Rooms",  tab == 1);

    C2D_DrawCircleSolid(384, 15, 0.5f, 5,
                        party ? COL_PURPLE : COL_GREEN);
    drawTextR(bridgeIp[0] ? bridgeIp : "not configured", 374, 8, 0.38f,
              COL_TEXT2);

    /* status line lives up here now - clear of the list */
    if (statusMsg[0])
        drawText(statusMsg, 200, 29, 0.4f, COL_TEXT2, true);

    if (state == ST_PAIR) {
        drawText("Press the round link button", 200, 90, 0.6f, COL_TEXT,
                 true);
        drawText("on your Hue Bridge,", 200, 115, 0.6f, COL_TEXT, true);
        drawText("then tap Pair below.", 200, 140, 0.6f, COL_TEXT, true);
        return;
    }

    int  count  = tab == 0 ? numLights : numRooms;
    int  sel    = tab == 0 ? selL : selR;
    int *scroll = tab == 0 ? &scrollL : &scrollR;

    if (count == 0)
        drawText("Nothing here yet - press Y to refresh", 200, 110, 0.5f,
                 COL_TEXT2, true);

    if (sel < *scroll) *scroll = sel;
    if (sel >= *scroll + VISIBLE) *scroll = sel - VISIBLE + 1;

    for (int row = 0; row < VISIBLE; row++) {
        int i = *scroll + row;
        if (i >= count) break;
        float y = LIST_Y + row * (CARD_H + CARD_GAP);

        if (i == sel)
            drawRRect(CARD_X - 2, y - 2, CARD_W + 4, CARD_H + 4, 11,
                      COL_ACCENT);
        drawRRect(CARD_X, y, CARD_W, CARD_H, 9, COL_CARD);

        if (tab == 0) {
            Light *L = &lights[i];
            u32 dot = L->on ? hueColor(L->hue, L->sat, L->bri) : COL_OFFDOT;
            C2D_DrawCircleSolid(CARD_X + 18, y + CARD_H / 2, 0.5f, 8, dot);
            drawText(L->name, CARD_X + 34, y + 7, 0.46f, COL_TEXT, false);
            char right[16];
            if (L->on)
                snprintf(right, sizeof(right), "%d%%",
                         (L->bri * 100) / 254);
            else
                snprintf(right, sizeof(right), "Off");
            drawTextR(right, CARD_X + CARD_W - 12, y + 7, 0.46f,
                      L->on ? COL_TEXT : COL_TEXT2);
        } else {
            Room *R = &rooms[i];
            u32 dot = R->anyOn ? hueColor(R->hue, R->sat, R->bri)
                               : COL_OFFDOT;
            C2D_DrawCircleSolid(CARD_X + 18, y + CARD_H / 2, 0.5f, 8, dot);
            drawText(R->name, CARD_X + 34, y + 7, 0.46f, COL_TEXT, false);
            char right[24];
            snprintf(right, sizeof(right), "%d light%s · %s", R->count,
                     R->count == 1 ? "" : "s", R->anyOn ? "On" : "Off");
            drawTextR(right, CARD_X + CARD_W - 12, y + 7, 0.42f,
                      R->anyOn ? COL_TEXT : COL_TEXT2);
        }
    }
}

/* ---- bottom screen ---- */
#define TOG_X 12
#define TOG_Y 30
#define TOG_W 296
#define TOG_H 36

#define TRK_X 24
#define TRK_W 272
#define TRK_H 18
#define BRI_Y  90
#define HUE_Y 136
#define SAT_Y 182

#define BTN_Y 206
#define BTN_H 26
#define BTN_W 68
#define BTN_GAP 8

static int activeSlider = -1;
static bool sliderDirty = false;

static float sliderFrac(int which)
{
    if (which == 3) return partySens;
    if (which == 4) return partyGain;
    if (!selValid()) return 0;
    if (which == 0) return (*selBri() - 1) / 253.0f;
    if (which == 1) return *selHue() / 65535.0f;
    return *selSat() / 254.0f;
}

static void setSliderFrac(int which, float f)
{
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    if (which == 3) { partySens = f; return; }
    if (which == 4) {
        partyGain = f;
        if (micReady) MICU_SetGain((u8)(partyGain * 119.0f));
        return;
    }
    if (!selValid()) return;
    if (which == 0) *selBri() = 1 + (int)(f * 253.0f);
    else if (which == 1) *selHue() = (int)(f * 65535.0f);
    else *selSat() = (int)(f * 254.0f);
}

static void drawSlider(int which, float y, const char *label)
{
    drawText(label, TRK_X, y - 22, 0.45f, COL_TEXT2, false);

    char val[24];
    if (selValid()) {
        if (which == 0)
            snprintf(val, sizeof(val), "%d%%", (*selBri() * 100) / 254);
        else if (which == 1)
            snprintf(val, sizeof(val), "%d\xC2\xB0",
                     (int)(*selHue() / 65535.0f * 360.0f));
        else
            snprintf(val, sizeof(val), "%d%%", (*selSat() * 100) / 254);
        drawTextR(val, TRK_X + TRK_W, y - 22, 0.45f, COL_TEXT2);
    }

    drawRRect(TRK_X, y, TRK_W, TRK_H, TRK_H / 2, COL_TRACK);
    const int SEGS = 32;
    float segW = (TRK_W - TRK_H) / (float)SEGS;
    for (int i = 0; i < SEGS; i++) {
        float f = (i + 0.5f) / SEGS;
        u32 c;
        if (which == 0) {
            int v = 40 + (int)(f * 215);
            c = C2D_Color32(v, v, (int)(v * 0.92f), 255);
        } else if (which == 1) {
            c = hueColor((int)(f * 65535), 254, 254);
        } else {
            c = hueColor(selValid() ? *selHue() : 0, (int)(f * 254), 254);
        }
        C2D_DrawRectSolid(TRK_X + TRK_H / 2 + i * segW, y + 3, 0.45f,
                          segW + 1, TRK_H - 6, c);
    }

    float f = sliderFrac(which);
    float kx = TRK_X + TRK_H / 2 + f * (TRK_W - TRK_H);
    float ky = y + TRK_H / 2;
    C2D_DrawCircleSolid(kx, ky, 0.5f, 12, COL_KNOB_EDGE);
    C2D_DrawCircleSolid(kx, ky, 0.5f, 10.5f, COL_CARD);
}

/* approximate a mired color temperature for scene previews */
static u32 ctPreview(int ct, int bri)
{
    float w = (ct - 153) / 347.0f;          /* 0 cool .. 1 warm */
    if (w < 0) w = 0;
    if (w > 1) w = 1;
    float b = 0.35f + 0.65f * (bri / 254.0f);
    int r = (int)((210 + 45 * w) * b);
    int g = (int)((225 - 35 * w) * b);
    int bl = (int)((255 - 145 * w) * b);
    return C2D_Color32(r > 255 ? 255 : r, g > 255 ? 255 : g,
                       bl > 255 ? 255 : bl, 255);
}

static void drawBottomTabs(void)
{
    static const char *TABS[3] = { "Control", "Scenes", "Party" };
    for (int i = 0; i < 3; i++) {
        float x = 8 + i * 104;
        bool act = (bottomTab == i);
        u32 bg = act ? (i == 2 ? COL_PURPLE : COL_ACCENT) : COL_TRACK;
        drawRRect(x, 4, 96, 20, 10, bg);
        drawText(TABS[i], x + 48, 7, 0.42f, act ? COL_CARD : COL_TEXT2,
                 true);
    }
}

#define SC_COLS 2
#define SC_TW 148
#define SC_TH 33
#define SC_GAP 4
#define SC_Y0 30

static void drawScenesTab(void)
{
    for (int i = 0; i < NUM_PRESETS; i++) {
        const Preset *P = &PRESETS[i];
        float x = 8 + (i % SC_COLS) * (SC_TW + 8);
        float y = SC_Y0 + (i / SC_COLS) * (SC_TH + SC_GAP);

        if (tapFlash > 0 && tappedPreset == i)
            drawRRect(x - 2, y - 2, SC_TW + 4, SC_TH + 4, 11, COL_ACCENT);
        drawRRect(x, y, SC_TW, SC_TH, 9, COL_CARD);

        /* preview swatch */
        if (P->palette) {
            float sw = 52.0f / P->nh;
            for (int k = 0; k < P->nh; k++)
                C2D_DrawRectSolid(x + 8 + k * sw, y + 6, 0.45f, sw + 1,
                                  SC_TH - 12,
                                  hueColor(P->hues[k], 254, P->bri));
        } else {
            drawRRect(x + 8, y + 6, 52, SC_TH - 12, 6,
                      ctPreview(P->ct, P->bri));
        }
        drawText(P->name, x + 68, y + 9, 0.4f, COL_TEXT, false);
    }
    const char *target = (tab == 1 && numRooms) ? rooms[selR].name
                                                : "All lights";
    char buf[48];
    snprintf(buf, sizeof(buf), "Tap to apply to: %s", target);
    drawText(buf, 160, SC_Y0 + 5 * (SC_TH + SC_GAP) + 6, 0.38f,
             COL_TEXT2, true);
}

#define PT_TOG_Y 28
#define PT_MODE_Y 64
#define PT_GAIN_Y 122
#define PT_SL_Y 162
#define PT_VU_Y 196

static const char *MODE_NAMES[4] = { "Pop", "Pulse", "Wave", "Chase" };
static const char *MODE_DESC[4] = {
    "Each beat pops one lamp a new color",
    "The whole room flashes and breathes",
    "The color wheel marches across lamps",
    "A bright spotlight chases lamp to lamp",
};

static void drawPartySlider(float y, const char *label, float frac,
                            u32 fillCol)
{
    drawText(label, TRK_X, y - 18, 0.4f, COL_TEXT2, false);
    char sv[16];
    snprintf(sv, sizeof(sv), "%d%%", (int)(frac * 100));
    drawText(sv, TRK_X + TRK_W - 30, y - 18, 0.4f, COL_TEXT2, false);
    drawRRect(TRK_X, y, TRK_W, 14, 7, COL_TRACK);
    drawRRect(TRK_X, y, 14 + (TRK_W - 14) * frac, 14, 7, fillCol);
    float kx = TRK_X + 7 + frac * (TRK_W - 14);
    C2D_DrawCircleSolid(kx, y + 7, 0.5f, 10, COL_KNOB_EDGE);
    C2D_DrawCircleSolid(kx, y + 7, 0.5f, 8.5f, COL_CARD);
}

static void drawPartyTab(void)
{
    drawRRect(12, PT_TOG_Y, 296, 30, 12,
              party ? COL_PURPLE : COL_CARD);
    drawText(party ? "Stop the party" : "Start the party", 160,
             PT_TOG_Y + 6, 0.5f, party ? COL_CARD : COL_PURPLE, true);

    for (int i = 0; i < 4; i++) {
        float x = 12 + i * 76;
        bool act = (partyMode == i);
        drawRRect(x, PT_MODE_Y, 70, 22, 9,
                  act ? COL_PURPLE : COL_CARD);
        drawText(MODE_NAMES[i], x + 35, PT_MODE_Y + 3, 0.4f,
                 act ? COL_CARD : COL_TEXT, true);
    }
    drawText(MODE_DESC[partyMode], 160, PT_MODE_Y + 26, 0.34f, COL_TEXT2,
             true);

    drawPartySlider(PT_GAIN_Y, "Mic gain (input volume)", partyGain,
                    COL_ACCENT);
    drawPartySlider(PT_SL_Y, "Beat sensitivity", partySens, COL_PURPLE);

    float frac = micRms / 4000.0f;
    if (frac > 1) frac = 1;
    drawRRect(TRK_X, PT_VU_Y, TRK_W, 12, 6, COL_TRACK);
    if (party && frac > 0.02f)
        drawRRect(TRK_X, PT_VU_Y, TRK_W * frac, 12, 6,
                  beatCooldown > partyModeCooldown() - 6 ? COL_PURPLE
                                                         : COL_ACCENT);
    drawText(party ? "Quiet room should sit near zero - lower gain if not"
                   : "Mic off - start the party to tune",
             160, PT_VU_Y + 18, 0.32f, COL_TEXT2, true);
}

static void drawButton(float x, const char *label, u32 bg, u32 fg)
{
    drawRRect(x, BTN_Y, BTN_W, BTN_H, 8, bg);
    drawText(label, x + BTN_W / 2, BTN_Y + 5, 0.42f, fg, true);
}

static void drawBottom(void)
{
    if (state == ST_PAIR) {
        drawText("Setup", 160, 16, 0.7f, COL_TEXT, true);
        drawRRect(40, 70, 240, 40, 12, COL_ACCENT);
        drawText("Pair with bridge", 160, 80, 0.55f, COL_CARD, true);
        drawRRect(40, 124, 240, 40, 12, COL_CARD);
        drawText("Change IP", 160, 134, 0.55f, COL_TEXT, true);
        if (bridgeIp[0] && username[0]) {
            drawRRect(40, 178, 240, 34, 12, COL_CARD);
            drawText("Cancel", 160, 186, 0.5f, COL_TEXT2, true);
        }
        return;
    }

    drawBottomTabs();
    if (bottomTab == 1) { drawScenesTab(); return; }
    if (bottomTab == 2) { drawPartyTab(); return; }

    drawRRect(TOG_X, TOG_Y, TOG_W, TOG_H, 12, COL_CARD);
    if (selValid()) {
        drawText(selName(), TOG_X + 14, TOG_Y + 8, 0.5f, COL_TEXT, false);
        bool on = selOn();
        u32 pill = on ? COL_GREEN : COL_TRACK;
        drawRRect(TOG_X + TOG_W - 62, TOG_Y + 7, 52, 22, 11, pill);
        drawText(on ? "On" : "Off", TOG_X + TOG_W - 36, TOG_Y + 10,
                 0.45f, on ? COL_CARD : COL_TEXT2, true);
    } else {
        drawText("Nothing selected", TOG_X + 14, TOG_Y + 8, 0.5f,
                 COL_TEXT2, false);
    }

    drawSlider(0, BRI_Y, "Brightness");
    drawSlider(1, HUE_Y, "Color");
    drawSlider(2, SAT_Y, "Saturation");

    float bx = TOG_X;
    if (tab == 0)
        drawButton(bx, "Rename", COL_CARD, COL_TEXT);
    bx += BTN_W + BTN_GAP;
    drawButton(bx, "All on", COL_CARD, COL_TEXT);
    bx += BTN_W + BTN_GAP;
    drawButton(bx, "All off", COL_CARD, COL_TEXT);
    bx += BTN_W + BTN_GAP;
    drawButton(bx, "Refresh", COL_CARD, COL_ACCENT);
}

static bool inBox(touchPosition t, float x, float y, float w, float h)
{
    return t.px >= x && t.px < x + w && t.py >= y && t.py < y + h;
}

static void toggleParty(void)
{
    if (!party) {
        if (partyStart()) {
            party = true;
            snprintf(statusMsg, sizeof(statusMsg),
                     "Party mode - listening for beats");
        } else {
            snprintf(statusMsg, sizeof(statusMsg), "Mic unavailable");
        }
    } else {
        party = false;
        partyStop();
        snprintf(statusMsg, sizeof(statusMsg), "Party mode off");
    }
}

static void handleTouch(u32 kDown, u32 kHeld, u32 kUp)
{
    touchPosition t;
    hidTouchRead(&t);

    /* slider drag/release runs regardless of tab (0-2 control,
     * 3 = party sensitivity) */
    if ((kHeld & KEY_TOUCH) && activeSlider >= 0) {
        float f = (t.px - (TRK_X + TRK_H / 2)) / (float)(TRK_W - TRK_H);
        setSliderFrac(activeSlider, f);
        sliderDirty = true;
    }
    if ((kUp & KEY_TOUCH) && activeSlider >= 0) {
        if (sliderDirty) {
            if (activeSlider >= 3) saveConfig();
            else sendSelected(activeSlider != 0);
        }
        activeSlider = -1;
        sliderDirty = false;
    }

    if (state == ST_PAIR) {
        if (kDown & KEY_TOUCH) {
            if (inBox(t, 40, 70, 240, 40)) {
                if (tryPair()) { state = ST_MAIN; refreshAll(); }
            } else if (inBox(t, 40, 124, 240, 40)) {
                startSetup();
            } else if (inBox(t, 40, 178, 240, 34) &&
                       bridgeIp[0] && username[0]) {
                state = ST_MAIN;
                statusMsg[0] = '\0';
            }
        }
        return;
    }

    if (kDown & KEY_TOUCH) {
        /* bottom tab bar */
        if (t.py < 26) {
            for (int i = 0; i < 3; i++)
                if (inBox(t, 8 + i * 104, 4, 96, 20)) bottomTab = i;
            return;
        }

        /* Scenes tab: tap a tile to apply */
        if (bottomTab == 1) {
            for (int i = 0; i < NUM_PRESETS; i++) {
                float x = 8 + (i % SC_COLS) * (SC_TW + 8);
                float y = SC_Y0 + (i / SC_COLS) * (SC_TH + SC_GAP);
                if (inBox(t, x, y, SC_TW, SC_TH) && numRooms) {
                    int roomIdx = (tab == 1) ? selR : 0;
                    applyPreset(roomIdx, &PRESETS[i]);
                    tappedPreset = i;
                    tapFlash = 30;
                }
            }
            return;
        }

        /* Party tab */
        if (bottomTab == 2) {
            if (inBox(t, 12, PT_TOG_Y, 296, 30)) {
                toggleParty();
            } else {
                for (int i = 0; i < 4; i++)
                    if (inBox(t, 12 + i * 76, PT_MODE_Y, 70, 22)) {
                        partyMode = i;
                        saveConfig();
                    }
                if (inBox(t, TRK_X - 12, PT_GAIN_Y - 8, TRK_W + 24, 30))
                    activeSlider = 4;
                else if (inBox(t, TRK_X - 12, PT_SL_Y - 8, TRK_W + 24, 30))
                    activeSlider = 3;
            }
            return;
        }

        if      (inBox(t, TRK_X - 12, BRI_Y - 10, TRK_W + 24, TRK_H + 20))
            activeSlider = 0;
        else if (inBox(t, TRK_X - 12, HUE_Y - 10, TRK_W + 24, TRK_H + 20))
            activeSlider = 1;
        else if (inBox(t, TRK_X - 12, SAT_Y - 10, TRK_W + 24, TRK_H + 20))
            activeSlider = 2;
        else if (inBox(t, TOG_X, TOG_Y, TOG_W, TOG_H)) {
            toggleSelected();
        } else if (t.py >= BTN_Y && t.py < BTN_Y + BTN_H) {
            float bx = TOG_X;
            if (inBox(t, bx, BTN_Y, BTN_W, BTN_H)) {
                if (tab == 0) renameSelectedLight();
            } else if (inBox(t, bx + (BTN_W + BTN_GAP), BTN_Y, BTN_W,
                             BTN_H)) {
                allLights(true);
            } else if (inBox(t, bx + 2 * (BTN_W + BTN_GAP), BTN_Y, BTN_W,
                             BTN_H)) {
                allLights(false);
            } else if (inBox(t, bx + 3 * (BTN_W + BTN_GAP), BTN_Y, BTN_W,
                             BTN_H)) {
                refreshAll();
            }
        }
    }

}

/* ------------------------------------------------------------------ */

int main(void)
{
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    httpcInit(4 * 1024 * 1024);
    srand(svcGetSystemTick() & 0xFFFFFFFF);

    topScr = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    botScr = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_textBuf = C2D_TextBufNew(4096);

    respBuf = malloc(RESP_SIZE);
    if (!respBuf) return 1;

    memset(presetIdx, 0, sizeof(presetIdx));

    if (loadConfig()) {
        refreshAll();
    } else {
        startSetup();
    }

    int pollTimer = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        u32 kUp   = hidKeysUp();

        if (kDown & KEY_START) break;
        if (kDown & KEY_SELECT) startSetup();

        if (state == ST_MAIN) {
            if (kDown & KEY_L) { tab = 0; }
            if (kDown & KEY_R) { tab = 1; }

            int count = tab == 0 ? numLights : numRooms;
            int *sel  = tab == 0 ? &selL : &selR;
            if (count > 0) {
                if (kDown & KEY_DUP)   *sel = (*sel + count - 1) % count;
                if (kDown & KEY_DDOWN) *sel = (*sel + 1) % count;
                if (kDown & KEY_A)     toggleSelected();
            }
            if (kDown & KEY_Y) refreshAll();
            if (kDown & KEY_X) cyclePreset();
        }

        handleTouch(kDown, kHeld, kUp);

        if (party) partyTick();
        if (tapFlash > 0) tapFlash--;

        /* periodic light refresh (paused during party mode) */
        if (!party && bridgeIp[0] && username[0] && state == ST_MAIN &&
            ++pollTimer > 1800) {
            pollTimer = 0;
            refreshAll();
        }

        C2D_TextBufClear(g_textBuf);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        C2D_TargetClear(topScr, COL_BG);
        C2D_SceneBegin(topScr);
        drawTop();

        C2D_TargetClear(botScr, COL_BG);
        C2D_SceneBegin(botScr);
        drawBottom();

        C3D_FrameEnd(0);
    }

    if (party) partyStop();
    free(respBuf);
    C2D_TextBufDelete(g_textBuf);
    C2D_Fini();
    C3D_Fini();
    httpcExit();
    gfxExit();
    return 0;
}
