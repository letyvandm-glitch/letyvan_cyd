// ================================================================
// CYD Monitor — ESP32-2432S028R (CYD 2.8" Paysage)
// 4 pages tactiles : PC | VRAM/NET | MINER | METEO
// Data : agent WiFi (PC), NMMiner API, wttr.in (météo)
// ================================================================
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <SPIFFS.h>
#include <math.h>
// WebServer DOIT être inclus après SPIFFS (qui expose fs::FS correctement)
using namespace fs;
#include <WebServer.h>
#include "hybrid_config.h"

#define OTA_HOSTNAME  "cyd-mining"
#define OTA_PASSWORD  "cyd1234"

// ── Config réseau ───────────────────────────────────────────────
#define WIFI_SSID       "RDC-2.4G"
#define WIFI_PASS       "Los_Murcianos25"
#define PC_AGENT_HOST   "192.168.001.051"  // patché par l'installateur
#define PC_AGENT_PORT   8765
// IPs de démarrage (seeds) — la découverte automatique prend le relais
#define SEED_IP_1       "192.168.1.10"
#define SEED_IP_2       "192.168.1.11"
#define SEED_IP_3       "192.168.1.13"
// Découverte auto : scan 192.168.1.SCAN_START à SCAN_END
#define SCAN_START      1
#define SCAN_END        30
#define SCAN_PER_CYCLE  3    // IPs testées par cycle de 10s
#define MAX_MINERS      8    // slots réseau (+ 1 local = 9 total)
#define WEATHER_CITY    "Thouars"

// ── Touch ───────────────────────────────────────────────────────
#define XPT_IRQ  36
#define XPT_MOSI 32
#define XPT_MISO 39
#define XPT_CLK  25
#define XPT_CS   33
#define TS_MINX  200
#define TS_MAXX  3700
#define TS_MINY  240
#define TS_MAXY  3800

// ── Écran ───────────────────────────────────────────────────────
#define SCR_W    320
#define SCR_H    240
#define TAB_H    22
#define BODY_Y   (TAB_H + 1)
#define BODY_H   (SCR_H - BODY_Y)

// ── Palette ─────────────────────────────────────────────────────
#define COL_BG      0x0000
#define COL_PANEL   0x0841
#define COL_LINE    0x18C4
#define COL_TEXT    0xEF7B
#define COL_DIM     0x528D
#define COL_CPU     0x04FF
#define COL_GPU     0xFC1B
#define COL_RAM     0x861F
#define COL_VRAM    0xFD20
#define COL_NET     0x07EA
#define COL_MINER   0xF7E0
#define COL_WEATHER 0x567D
#define COL_WARN    0xF800
#define COL_OK      0x07E0
#define COL_TRACK   0x1082
#define COL_TAB_BG  0x0A20
#define COL_TAB_ACT 0x0A4A
// ── LED RGB + Capteur lumière ───────────────────────────────
#define PIN_LED_R  4
#define PIN_LED_G  16
#define PIN_LED_B  17
#define PIN_LDR    34

// ── Pages ───────────────────────────────────────────────────────
enum Page { PAGE_PC = 0, PAGE_NET, PAGE_SCAN, PAGE_WEATHER, PAGE_COUNT };
Page currentPage = PAGE_PC;
Page prevPage    = (Page)-1;

const char* TAB_LABELS[] = { "PC", "NET", "LAN", "METEO" };

// ── Structs données ─────────────────────────────────────────────
struct PCData {
  float cpu = 0, cpu_t = 0, gpu = 0, gpu_t = 0;
  float ram = 0, ram_u = 0, ram_tot = 0;
  float vram = 0, vram_u = 0, vram_tot = 0;
  float disk = 0, disk_u = 0, disk_tot = 0;
  float net_up = 0, net_dn = 0;
  char cpu_name[48] = "—";
  char gpu_name[48] = "—";
  bool ok = false;
  bool os_linux = false;
  float swap = 0, swap_u = 0, swap_tot = 0;
  unsigned long ts = 0;
};

struct MinerDev {
  float hr_gh = 0;           // en H/s
  int   sh_ok = 0, sh_ko = 0, temp = -1;
  int   blkhits = 0;
  char  name[20] = "—";
  char  ip[20]   = "";
  char  ver[16]  = "—";
  char  best_diff[20] = "—";
  char  net_diff[16]  = "—"; // difficulty réseau BTC (ex: "92.67T")
  char  best_sess[20] = "—"; // meilleure diff session
  unsigned long up_s = 0;
  char  pool[48] = "—";
  bool  ok = false;
};

struct MinerData {
  MinerDev     dev[MAX_MINERS + 1];  // [0]=local CYD, [1..MAX_MINERS]=réseau
  int          count = 0;
  unsigned long ts = 0;
};

struct WxForecast { int maxT = 0, minT = 0, code = 113, windDeg = 0, windSpd = 0; };

struct WxData {
  float    temp = 0, feels = 0;
  int      hum = 0, code = 113;
  float    wind = 0;
  int      windDeg = 0;
  char     cond[48] = "—";
  char     city[32] = WEATHER_CITY;
  WxForecast fore[3];
  bool     ok = false;
  unsigned long ts = 0;
  int      sunrise_min = 420;   // 7h00 defaut
  int      sunset_min  = 1200;  // 20h00 defaut
};

struct BtcNetwork {
  float progress   = 0;   // % epoch actuel
  float diff_chg   = 0;   // % prochaine correction
  int   rem_blocks = 0;   // blocs restants
  float blk_min    = 0;   // temps moyen par bloc (min)
  bool  ok = false;
  unsigned long ts = 0;
};

PCData     pc;
MinerData  miner;
WxData     wx;
BtcNetwork btcnet;
// LAN scan
#define LAN_MAX 24
struct LanDevice { char ip[16]; char name[22]; bool wifi; bool ok; };
LanDevice lanDevs[LAN_MAX];
int       lanCount = 0;
int       lanHttpCode = 0;  // debug: dernier HTTP code
#define IV_LAN 30000UL
#define LAN_AGENT_HOST "192.168.1.50"
uint8_t    g_pcConn = 0;    // 0=inconnu 1=USB 2=WiFi

// ── Hardware ────────────────────────────────────────────────────
SPIClass          touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT_CS, XPT_IRQ);
TFT_eSPI          tft;
TFT_eSprite       spr(&tft);

bool wasTouched = false;
unsigned long lastTap = 0;

// ── Page MINER : scroll + scan auto ─────────────────────────────
int  minerScroll   = 0;
int  scanScroll    = 0;   // première carte visible (index dans dev[1..N])
int  scanNextOctet = SCAN_START;  // prochain octet à sonder
int  netMinerCount = 0;   // nb de miners réseau actifs dans dev[1..]

// Extrait le suffixe de WORKER_NAME (après le dernier '.') → "nm2"
static const char* workerSuffix() {
  const char* dot = strrchr(WORKER_NAME, '.');
  return dot ? dot + 1 : "cyd";
}

// ── Intervalles de poll (WiFi) ──────────────────────────────────
#define IV_MINER 10000UL
#define IV_WX    300000UL
#define IV_NET   600000UL   // stats réseau BTC toutes les 10 min

unsigned long tMiner = 0, tWx = 0, tNet = 0, tLAN = (unsigned long)-IV_LAN;

// ── Serial PC (USB) ─────────────────────────────────────────────
String _serialBuf = "";

// ── Historique net (page NET) ───────────────────────────────────
#define HIST_N 60
uint16_t histUp[HIST_N] = {0};
uint16_t histDn[HIST_N] = {0};
int      histIdx = 0;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Dessin primitives
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void drawArc(int cx, int cy, int r, int thick, float pct, uint16_t color) {
  // Arc de jauge : 210° à -30° (270° d'ouverture)
  float startA = 210.0f;
  float span   = 270.0f;
  float endA   = startA - span * constrain(pct, 0.f, 1.f);
  for (int a = (int)endA; a <= (int)startA; a++) {
    float rad = a * DEG_TO_RAD;
    float c = cosf(rad), s = -sinf(rad);
    for (int t = 0; t < thick; t++) {
      int rr = r - t;
      tft.drawPixel(cx + rr * c, cy + rr * s, color);
    }
  }
}

void drawGauge(int cx, int cy, int r, float pct, uint16_t color, const char* label, const char* val) {
  // Piste
  drawArc(cx, cy, r, 4, 1.0f, COL_TRACK);
  // Arc valeur
  drawArc(cx, cy, r, 4, pct, color);
  // Centre : label + valeur
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextSize(1);
  tft.drawString(label, cx, cy - 6);
  tft.setTextColor(color, COL_BG);
  tft.setTextSize(1);
  tft.setFreeFont(NULL);
  tft.drawString(val, cx, cy + 6);
}

// Jauge TURZX : donut coloré avec halo et valeur intérieure
void drawGaugeTX(int cx, int cy, int r, float pct, uint16_t color,
                 const char* label, const char* val, int vSize) {
  // Halo extérieur (arc fin atténué)
  uint16_t dim = ((color >> 1) & 0x7BEF);
  drawArc(cx, cy, r + 4, 2, pct, dim);
  // Cercle de fond décoratif
  tft.drawCircle(cx, cy, r + 6, 0x0841);
  tft.drawCircle(cx, cy, r - 11, 0x0841);
  // Piste grise
  drawArc(cx, cy, r, 7, 1.0f, COL_TRACK);
  // Arc valeur coloré
  drawArc(cx, cy, r, 7, pct, color);
  // Trou intérieur noir
  tft.fillCircle(cx, cy, r - 9, COL_BG);
  // Valeur (grand)
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, COL_BG);
  tft.setTextSize(vSize);
  tft.drawString(val, cx, cy - (vSize >= 2 ? 5 : 0));
  // Label (petit, dim)
  tft.setTextSize(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString(label, cx, cy + (vSize >= 2 ? 14 : 9));
}

void drawBar(int x, int y, int w, int h, float pct, uint16_t color) {
  tft.drawRect(x, y, w, h, COL_TRACK);
  int fill = (int)(w * constrain(pct, 0.f, 1.f));
  if (fill > 2) tft.fillRect(x + 1, y + 1, fill - 2, h - 2, color);
}

uint16_t tempColor(float t) {
  if (t < 60) return COL_OK;
  if (t < 80) return COL_VRAM;
  return COL_WARN;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Barre d'onglets
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void drawTabs() {
  int tw = SCR_W / PAGE_COUNT;
  for (int i = 0; i < PAGE_COUNT; i++) {
    bool act = (i == currentPage);
    int x = i * tw;
    tft.fillRect(x, 0, tw, TAB_H, act ? 0x00A4 : 0x0083);
    if (act) tft.drawFastHLine(x, TAB_H - 2, tw, COL_CPU);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(act ? COL_CPU : COL_DIM, act ? 0x00A4 : 0x0083);
    tft.setTextSize(1);
    tft.drawString(TAB_LABELS[i], x + tw / 2, TAB_H / 2);
  }
  tft.drawLine(0, TAB_H, SCR_W, TAB_H, COL_LINE);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// PAGE PC
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Charge le fond depuis SPIFFS (ligne par ligne pour économiser la RAM)
static void drawBgPC() {
  tft.fillRect(0, BODY_Y, SCR_W, BODY_H, COL_BG);

  fs::File f = SPIFFS.open("/bg_pc.raw", "r");
  if (f && f.size() == (size_t)SCR_W * SCR_H * 2) {
    uint16_t lineBuf[SCR_W];
    f.seek((uint32_t)BODY_Y * SCR_W * 2);
    bool ok = true;
    for (int y = BODY_Y; y < SCR_H && ok; y++) {
      ok = (f.read((uint8_t*)lineBuf, SCR_W * 2) == SCR_W * 2);
      if (ok) tft.pushImage(0, y, SCR_W, 1, lineBuf);
    }
    f.close();
    return;
  }
  if (f) f.close();

  // Fallback : grand halo central CPU + 2 petits halos flanquants
  const int GCY = 123;  // synchronise avec gCY de drawPagePC

  // Grand halo CPU central (cx=160)
  { uint16_t c = COL_CPU;
    tft.drawCircle(160, GCY, 56, (c >> 4) & 0x0E73);
    tft.drawCircle(160, GCY, 54, (c >> 3) & 0x1CE7);
    tft.drawCircle(160, GCY, 52, (c >> 2) & 0x39E7);
    for (int a = 0; a < 360; a += 20) {
      float rd = a * DEG_TO_RAD;
      float cs = cosf(rd), sn = sinf(rd);
      int tickLen = (a % 60 == 0) ? 6 : 3;
      tft.drawLine(160 + (int)(56*cs), GCY - (int)(56*sn),
                   160 + (int)((56+tickLen)*cs), GCY - (int)((56+tickLen)*sn),
                   (c >> 2) & 0x39E7);
    }
  }
  // Petit halo TEMP gauche (cx=42)
  { uint16_t c = COL_GPU;
    tft.drawCircle(42, GCY, 32, (c >> 3) & 0x1CE7);
    tft.drawCircle(42, GCY, 30, (c >> 2) & 0x39E7);
  }
  // Petit halo GPU% droite (cx=278)
  { uint16_t c = COL_VRAM;
    tft.drawCircle(278, GCY, 32, (c >> 3) & 0x1CE7);
    tft.drawCircle(278, GCY, 30, (c >> 2) & 0x39E7);
  }
  tft.drawFastHLine(0, GCY, SCR_W, 0x0841);
}


static void drawUSBIcon(int cx, int cy, uint16_t col) {
  tft.fillRect(cx - 7, cy - 11, 15, 22, COL_BG);
  tft.drawRect(cx - 5, cy - 8, 11, 6, col);
  tft.fillRect(cx - 2, cy - 6, 5, 2, col);
  tft.fillRect(cx - 1, cy - 2, 3, 10, col);
  tft.drawLine(cx - 4, cy + 8, cx + 4, cy + 8, col);
  tft.drawFastVLine(cx - 4, cy + 3, 6, col);
  tft.drawFastVLine(cx + 4, cy + 3, 6, col);
}
static void drawWifiIcon(int cx, int cy, uint16_t col) {
  tft.fillRect(cx - 12, cy - 7, 25, 16, COL_BG);
  tft.fillCircle(cx, cy + 6, 2, col);
  for (int ri = 1; ri <= 3; ri++) {
    int r = ri * 3 + 1;
    int px0 = -1, py0 = -1;
    for (int a = 20; a <= 160; a += 8) {
      float rad = a * DEG_TO_RAD;
      int px = cx + (int)roundf(r * cosf(rad));
      int py = cy + 6 - (int)roundf(r * sinf(rad));
      if (px0 >= 0) tft.drawLine(px0, py0, px, py, col);
      px0 = px; py0 = py;
    }
  }
}
void drawPagePC() {
  tft.fillRect(0, BODY_Y, SCR_W, BODY_H, COL_BG);
  char buf[48];
  const int Y0  = BODY_Y;      // 23
  const int SEP = Y0 + 42;     // 65 — fin header
  const int LCX = 53;          // centre col gauche
  const int CCX = 160;         // centre col centrale
  const int RCX = 267;         // centre col droite

  // ── HEADER ─────────────────────────────────────────────────────
  tft.setTextDatum(ML_DATUM); tft.setTextSize(1);
  tft.setTextColor(COL_NET, COL_BG);
  tft.drawString("NETWORK", 4, Y0 + 7);
  if (pc.ok) {
    tft.setTextColor(COL_TEXT, COL_BG);
    if (pc.net_up >= 1024) snprintf(buf, sizeof(buf), "u:%.1fMB/s", pc.net_up/1024.f);
    else                   snprintf(buf, sizeof(buf), "u:%.0fKB/s", pc.net_up);
    tft.drawString(buf, 4, Y0 + 20);
    if (pc.net_dn >= 1024) snprintf(buf, sizeof(buf), "d:%.1fMB/s", pc.net_dn/1024.f);
    else                   snprintf(buf, sizeof(buf), "d:%.0fKB/s", pc.net_dn);
    tft.drawString(buf, 4, Y0 + 32);
  } else {
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("agent hors ligne", 4, Y0 + 20);
  }
  struct tm ti;
  tft.setTextDatum(MC_DATUM);
  if (getLocalTime(&ti, 0)) {
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextSize(2);
    snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    tft.drawString(buf, CCX, Y0 + 20);
    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    snprintf(buf, sizeof(buf), "%02d/%02d", ti.tm_mday, ti.tm_mon+1);
    tft.drawString(buf, CCX, Y0 + 36);
  }
  { uint16_t ic = (g_pcConn == 1) ? 0xFFE0 : COL_NET;
    if (g_pcConn == 1) drawUSBIcon(CCX + 42, Y0 + 20, ic);
    else               drawWifiIcon(CCX + 42, Y0 + 20, ic); }
  tft.setTextDatum(MR_DATUM); tft.setTextSize(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString(PC_AGENT_HOST, SCR_W - 4, Y0 + 7);
  if (pc.ok) {
    char cname[12]; strncpy(cname, pc.cpu_name, 11); cname[11] = 0;
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString(cname, SCR_W - 4, Y0 + 32);
  }
  tft.drawLine(0, SEP, SCR_W, SEP, COL_LINE);
  tft.drawFastVLine(107, SEP, SCR_H - SEP - 1, COL_LINE);
  tft.drawFastVLine(213, SEP, SCR_H - SEP - 1, COL_LINE);

  if (!pc.ok) {
    tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_DIM, COL_BG); tft.setTextSize(1);
    tft.drawString("En attente agent...", CCX, (SEP + SCR_H)/2);
    return;
  }

  // ── GRANDE JAUGE CPU — colonne centre (r=46) ──────────────────
  const int BIG_CY = SEP + 92;  // 157
  char cpuBuf[6]; snprintf(cpuBuf, sizeof(cpuBuf), "%d", (int)pc.cpu);
  uint16_t cpuHalo = (pc.cpu >= 85.f) ? COL_WARN : (pc.cpu >= 65.f) ? 0xFD20 : COL_CPU;
  drawGaugeTX(CCX, BIG_CY, 46, pc.cpu/100.f, cpuHalo, "CPU%", cpuBuf, 3);
  tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
  tft.setTextColor(COL_DIM, COL_BG);
  char cname2[13]; strncpy(cname2, pc.cpu_name, 12); cname2[12] = 0;
  tft.drawString(cname2, CCX, BIG_CY + 56);

  // ── COLONNE GAUCHE ─────────────────────────────────────────────
  tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
  tft.setTextColor(COL_CPU, COL_BG);
  tft.drawString("TEMP", LCX, SEP + 10);
  char cpuTBuf[6]; snprintf(cpuTBuf, sizeof(cpuTBuf), "%d", (int)pc.cpu_t);
  uint16_t tcol = tempColor(pc.cpu_t);
  const int SM_CPU_CY = SEP + 48;  // 113
  drawGaugeTX(LCX, SM_CPU_CY, 28, constrain(pc.cpu_t, 0.f, 100.f)/100.f, tcol, "C", cpuTBuf, 2);
  tft.setTextColor(COL_RAM, COL_BG);
  tft.drawString("RAM", LCX, SM_CPU_CY + 38);
  char ramBuf[6]; snprintf(ramBuf, sizeof(ramBuf), "%d", (int)pc.ram);
  const int SM_RAM_CY = SM_CPU_CY + 76;  // 189
  drawGaugeTX(LCX, SM_RAM_CY, 28, pc.ram/100.f, COL_RAM, "%", ramBuf, 2);
  snprintf(buf, sizeof(buf), "%.1f/%.0fG", pc.ram_u, pc.ram_tot);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString(buf, LCX, SM_RAM_CY + 36);

  // ── COLONNE DROITE ─────────────────────────────────────────────
  tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
  tft.setTextColor(COL_NET, COL_BG);
  tft.drawString("DISK", RCX, SEP + 10);
  uint16_t dkCol = (pc.disk >= 90.f) ? COL_WARN : (pc.disk >= 70.f) ? 0xFD20 : COL_NET;
  snprintf(buf, sizeof(buf), "%.0f/%.0fG", pc.disk_u, pc.disk_tot);
  tft.setTextColor(dkCol, COL_BG);
  tft.drawString(buf, RCX, SEP + 23);
  drawBar(RCX - 42, SEP + 31, 86, 6, pc.disk/100.f, dkCol);
  tft.setTextColor(COL_GPU, COL_BG);
  tft.drawString("GPU", RCX, SEP + 50);
  char gpuBuf[6]; snprintf(gpuBuf, sizeof(gpuBuf), "%d", (int)pc.gpu);
  const int SM_GPU_CY = SEP + 90;  // 155
  drawGaugeTX(RCX, SM_GPU_CY, 28, pc.gpu/100.f, COL_GPU, "%", gpuBuf, 2);
  if (pc.gpu_t > 0) {
    snprintf(buf, sizeof(buf), "%.0fC", pc.gpu_t);
    tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
    tft.setTextColor(tempColor(pc.gpu_t), COL_BG);
    tft.drawString(buf, RCX, SM_GPU_CY + 36);
  }
  bool showSwap = pc.os_linux && pc.swap_tot > 0;
  tft.setTextColor(COL_VRAM, COL_BG);
  tft.drawString(showSwap ? "SWAP" : "VRAM", RCX, SM_GPU_CY + 50);
  if (showSwap) {
    snprintf(buf, sizeof(buf), "%.1f/%.0fG", pc.swap_u, pc.swap_tot);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString(buf, RCX, SM_GPU_CY + 62);
    drawBar(RCX - 42, SM_GPU_CY + 70, 86, 5, pc.swap/100.f, COL_VRAM);
  } else {
    snprintf(buf, sizeof(buf), "%.0f/%.0fM", pc.vram_u, pc.vram_tot);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString(buf, RCX, SM_GPU_CY + 62);
    drawBar(RCX - 42, SM_GPU_CY + 70, 86, 5, pc.vram/100.f, COL_VRAM);
  }
}


// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// PAGE NET / VRAM
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void drawPageNet() {
  tft.fillRect(0, BODY_Y, SCR_W, BODY_H, COL_BG);

  if (!pc.ok) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextSize(1);
    tft.drawString("En attente agent PC...", SCR_W / 2, SCR_H / 2);
    return;
  }

  char buf[48];
  const int LX = 4, BX = 54, BW = SCR_W - BX - 8, BH = 8, TX = SCR_W - 2;
  int ly = BODY_Y + 14;
  tft.setTextSize(1);

  // UP
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_NET, COL_BG);
  tft.drawString("UP", LX, ly);
  if (pc.net_up >= 1024) snprintf(buf, sizeof(buf), "%.2f MB/s", pc.net_up / 1024.f);
  else                   snprintf(buf, sizeof(buf), "%.1f KB/s", pc.net_up);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(buf, TX, ly); ly += 18;

  // DOWN
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_GPU, COL_BG);
  tft.drawString("DN", LX, ly);
  if (pc.net_dn >= 1024) snprintf(buf, sizeof(buf), "%.2f MB/s", pc.net_dn / 1024.f);
  else                   snprintf(buf, sizeof(buf), "%.1f KB/s", pc.net_dn);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(buf, TX, ly); ly += 22;

  // Historique
  int hx = 4, hy = ly, hw = SCR_W - 8, hh = SCR_H - hy - 4;
  tft.drawRect(hx, hy, hw, hh, COL_LINE);
  tft.setTextDatum(ML_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(COL_NET, COL_BG);
  tft.drawString("UP", hx + 2, hy - 6);
  tft.setTextColor(COL_GPU, COL_BG);
  tft.drawString("DN", hx + 22, hy - 6);

  uint16_t maxVal = 1;
  for (int i = 0; i < HIST_N; i++) {
    if (histUp[i] > maxVal) maxVal = histUp[i];
    if (histDn[i] > maxVal) maxVal = histDn[i];
  }
  int barW2 = hw / HIST_N;
  for (int i = 0; i < HIST_N; i++) {
    int xi = hx + i * barW2;
    int idx = (histIdx + i) % HIST_N;
    int hu = (int)((float)histUp[idx] / maxVal * (hh - 2));
    int hd = (int)((float)histDn[idx] / maxVal * (hh - 2));
    if (hu > 0) tft.fillRect(xi, hy + hh - 1 - hu, barW2 - 1, hu, COL_NET);
    if (hd > 0) tft.drawRect(xi, hy + hh - 1 - hd, barW2 - 1, hd, COL_GPU);
  }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Icone météo (cx,cy = centre, sz = échelle 1=petit 2=grand)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void drawWxIcon(int cx, int cy, int code, int sz, bool isNight) {
  uint16_t Y  = 0xFEA0;  // jaune soleil
  uint16_t Gr = 0x6B4D;  // gris nuage
  uint16_t Bl = COL_NET;  // bleu pluie
  uint16_t W  = 0xEF7B;  // blanc neige
  int s = sz;

  bool isSun  = (code == 113);
  bool isPart = (code == 116);
  bool isRain = (code >= 176 && code <= 284) || (code >= 293 && code <= 359);
  bool isSnow = (code == 227 || code == 230 || (code >= 323 && code <= 377 && !isRain));
  bool isThd  = (code == 200 || code >= 386);
  bool isFog  = (code >= 143 && code < 176);

  if (isSun && !isNight) {
    tft.fillCircle(cx, cy, s*5, Y);
    for (int a = 0; a < 360; a += 45) {
      float rd = a * DEG_TO_RAD;
      tft.drawLine(cx+(s*7)*cosf(rd), cy+(s*7)*sinf(rd),
                   cx+(s*10)*cosf(rd), cy+(s*10)*sinf(rd), Y);
    }
    return;
  }
  if (isSun && isNight) {
    tft.fillCircle(cx - s, cy, s*5, 0xC600);
    tft.fillCircle(cx + s*2, cy - s*2, s*5, COL_BG);
    return;
  }
  if (isPart && !isNight) {
    tft.fillCircle(cx+s*5, cy-s*5, s*3, Y);
  }
  if (isPart && isNight) {
    tft.fillCircle(cx+s*4, cy-s*5, s*3, 0xC600);
    tft.fillCircle(cx+s*5, cy-s*6, s*3, COL_BG);
  }
  // Nuage de base
  tft.fillCircle(cx-s*3, cy,    s*4, Gr);
  tft.fillCircle(cx,     cy-s*2, s*5, Gr);
  tft.fillCircle(cx+s*4, cy,    s*4, Gr);
  tft.fillRect(cx-s*3, cy, s*8, s*4, Gr);

  if (isRain) {
    for (int i = -s*3; i <= s*3; i += s*3)
      tft.drawLine(cx+i, cy+s*4, cx+i-s, cy+s*9, Bl);
  } else if (isSnow) {
    for (int i = -s*3; i <= s*3; i += s*3)
      tft.fillCircle(cx+i, cy+s*7, s, W);
  } else if (isThd) {
    tft.drawLine(cx,    cy+s*4, cx-s*2, cy+s*7, Y);
    tft.drawLine(cx-s*2, cy+s*7, cx+s,  cy+s*7, Y);
    tft.drawLine(cx+s,  cy+s*7, cx-s,  cy+s*11, Y);
  } else if (isFog) {
    for (int i = 0; i < 3; i++)
      tft.drawFastHLine(cx-s*5, cy+s*4+i*s*2, s*10, Gr);
  }
}

// PAGE MINER -- auto-decouverte, defilement vertical

static void fmtHR(char* out, int sz, float hs) {
  if      (hs >= 1e9f) snprintf(out, sz, "%.2fGH", hs/1e9f);
  else if (hs >= 1e6f) snprintf(out, sz, "%.2fMH", hs/1e6f);
  else if (hs >= 1e3f) snprintf(out, sz, "%.1fKH", hs/1e3f);
  else                 snprintf(out, sz, "%.1fH",  hs);
}

#define CARD_H       34
#define CARDS_VISIBLE 4

static void drawMinerCard(MinerDev& d, int y0, bool isLocal) {
  uint16_t bgCol = isLocal ? 0x0021 : 0x0000;
  tft.fillRect(0, y0, SCR_W - 14, CARD_H, bgCol);
  tft.drawRect(0, y0, SCR_W - 14, CARD_H, COL_LINE);
  char buf[32];
  int ly1 = y0 + 10;
  int ly2 = y0 + 24;
  tft.setTextDatum(ML_DATUM); tft.setTextSize(1);

  if (!d.ok) {
    tft.setTextColor(COL_WARN, bgCol);
    tft.drawString(d.ip[0] ? d.ip : "???", 4, ly1);
    tft.setTextColor(COL_DIM, bgCol);
    tft.drawString("hors ligne", 4, ly2);
    return;
  }

  // Ligne 1 : NOM | HR centre | B:diff droite
  tft.setTextColor(COL_MINER, bgCol);
  char nb[12]; strncpy(nb, d.name, 11); nb[11] = 0;
  tft.drawString(nb, 4, ly1);
  fmtHR(buf, sizeof(buf), d.hr_gh);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_TEXT, bgCol);
  tft.drawString(buf, (SCR_W - 14) / 2, ly1);
  tft.setTextDatum(MR_DATUM); tft.setTextColor(COL_OK, bgCol);
  char bdBuf[22]; snprintf(bdBuf, sizeof(bdBuf), "B:%s", d.best_diff[0] ? d.best_diff : "-");
  tft.drawString(bdBuf, SCR_W - 18, ly1);

  // Ligne 2 : IP | OK/KO | temp ou uptime
  tft.setTextDatum(ML_DATUM); tft.setTextColor(COL_DIM, bgCol);
  tft.drawString(d.ip, 4, ly2);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_OK, bgCol);
  snprintf(buf, sizeof(buf), "%d/%d", d.sh_ok, d.sh_ko);
  tft.drawString(buf, (SCR_W - 14) / 2, ly2);
  tft.setTextDatum(MR_DATUM);
  if (d.temp >= 0) {
    tft.setTextColor(tempColor((float)d.temp), bgCol);
    snprintf(buf, sizeof(buf), "%dC", d.temp);
  } else {
    unsigned long u = d.up_s;
    int hv = (int)(u / 3600); int mv = (int)((u % 3600) / 60);
    tft.setTextColor(COL_DIM, bgCol);
    snprintf(buf, sizeof(buf), "%dh%dm", hv, mv);
  }
  tft.drawString(buf, SCR_W - 18, ly2);
}

void drawPageScan() {
  char buf[32];
  const int Y0      = BODY_Y;
  const int HEAD_H  = 14;
  const int COLH_H  = 14;
  const int LIST_Y  = Y0 + HEAD_H + COLH_H;  // 51
  const int ROW_H   = 16;
  const int SB_X    = 305;
  const int MAX_ROWS = (SCR_H - LIST_Y) / ROW_H;  // 11

  // couleurs lignes alternees (tres sombres)
  const uint16_t ROW0 = 0x0082;   // #04101A aprox
  const uint16_t ROW1 = COL_BG;   // 0x0000
  const uint16_t HEAD_BG = 0x0203; // #040E1A

  // ── Entete ──
  tft.fillRect(0, Y0, SCR_W, HEAD_H, HEAD_BG);
  tft.setTextSize(1);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_NET, HEAD_BG);
  tft.drawString("LAN SCAN", 4, Y0 + HEAD_H/2);
  int okCnt = 0;
  for (int i = 0; i < lanCount; i++) if (lanDevs[i].ok) okCnt++;
  if (lanCount > 0)
    snprintf(buf, sizeof(buf), "%d ap  %d ok", lanCount, okCnt);
  else if (lanHttpCode != 0)
    snprintf(buf, sizeof(buf), "HTTP %d", lanHttpCode);
  else
    snprintf(buf, sizeof(buf), "attente...");
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(lanCount > 0 ? COL_DIM : COL_WARN, HEAD_BG);
  tft.drawString(buf, SB_X - 2, Y0 + HEAD_H/2);
  tft.drawFastHLine(0, Y0 + HEAD_H, SB_X, COL_LINE);

  // ── En-tetes colonnes ──
  int HY = Y0 + HEAD_H + COLH_H/2;
  tft.fillRect(0, Y0 + HEAD_H, SB_X, COLH_H, COL_BG);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("IP", 4, HY);
  tft.drawString("HOSTNAME", 100, HY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("T", 256, HY);
  tft.drawString("ST", 280, HY);
  tft.drawFastHLine(0, Y0 + HEAD_H + COLH_H, SB_X, COL_LINE);

  // ── Lignes appareils ──
  for (int i = 0; i < MAX_ROWS; i++) {
    int ry  = LIST_Y + i * ROW_H;
    uint16_t bg = (i % 2 == 0) ? ROW0 : ROW1;
    tft.fillRect(0, ry, SB_X, ROW_H, bg);
    int idx = scanScroll + i;
    if (idx >= lanCount) continue;
    LanDevice& d = lanDevs[idx];
    int ty = ry + ROW_H / 2;
    uint16_t iCol = d.ok ? COL_TEXT : COL_DIM;

    // IP
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(iCol, bg);
    tft.drawString(d.ip, 4, ty);

    // Hostname (tronque a 13 cars)
    char hbuf[15];
    strncpy(hbuf, d.name, 14); hbuf[14] = 0;
    if (d.name[13] != 0 && d.name[13] != '\0' && strlen(d.name) > 13) hbuf[13] = '>';
    tft.setTextColor(iCol, bg);
    tft.drawString(hbuf, 100, ty);

    // Type W ou E
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(d.wifi ? COL_VRAM : COL_NET, bg);
    tft.drawString(d.wifi ? "W" : "E", 256, ty);

    // Dot statut
    tft.fillCircle(272, ty, 3, d.ok ? COL_OK : COL_DIM);
    // ok/ko texte
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(d.ok ? COL_OK : COL_DIM, bg);
    tft.drawString(d.ok ? "ok" : "ko", 279, ty);
  }

  // ── Scrollbar ──
  int sbH = SCR_H - LIST_Y;
  tft.fillRect(SB_X, LIST_Y, SCR_W - SB_X, sbH, ROW1);
  tft.drawFastVLine(SB_X, Y0, SCR_H - Y0, COL_LINE);
  if (lanCount > MAX_ROWS) {
    int barH = max(10, sbH * MAX_ROWS / lanCount);
    int barY = LIST_Y + (sbH - barH) * scanScroll / max(1, lanCount - MAX_ROWS);
    tft.fillRect(SB_X + 2, barY, 6, barH, COL_DIM);
  }

  // ── Hint scroll si plus de lignes que visible ──
  if (lanCount > MAX_ROWS) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("^ v scroll", 152, SCR_H - 4);
  }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Phase lunaire : jours depuis NL (0=NL, ~14.77=PL)
static float moonPhaseDay() {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return 0.0f;
  int y = ti.tm_year + 1900, m = ti.tm_mon + 1, d = ti.tm_mday;
  int a = (14 - m) / 12;
  int yy = y + 4800 - a, mm = m + 12*a - 3;
  long jdn = (long)d + (153*mm+2)/5 + 365*yy + yy/4 - yy/100 + yy/400 - 32045;
  double diff = (double)(jdn - 2451549) - 0.5;
  double phase = fmod(diff / 29.53059, 1.0) * 29.53059;
  if (phase < 0) phase += 29.53059;
  return (float)phase;
}
// Dessiner icone phase lunaire (cx,cy = centre, r = rayon)
static void drawMoonIcon(int cx, int cy, float phase, int r) {
  tft.fillCircle(cx, cy, r + 1, COL_BG);
  if (phase < 1.5f || phase > 28.0f) {
    tft.drawCircle(cx, cy, r, 0x4208); // NL : contour sombre
    return;
  }
  tft.fillCircle(cx, cy, r, 0xFFE0); // disque jaune
  float lit = (phase < 14.77f) ? phase/14.77f : (29.53f-phase)/14.77f;
  int offset = (int)(r * (1.0f - 2.0f * lit));
  if (phase < 14.0f && offset < r)
    tft.fillCircle(cx - offset, cy, r, COL_BG);
  else if (phase > 15.5f && offset < r)
    tft.fillCircle(cx + offset, cy, r, COL_BG);
}
// Boussole direction vent (deg=0 Nord, 90 Est)
static void drawWindArrow(int cx, int cy, int deg, int r) {
  tft.fillCircle(cx, cy, r + 1, COL_BG);
  tft.drawCircle(cx, cy, r + 1, 0x4208);
  float rad = (deg - 90.0f) * 3.14159f / 180.0f;
  int x1 = cx + (int)(cosf(rad) * r);
  int y1 = cy + (int)(sinf(rad) * r);
  int x0 = cx - (int)(cosf(rad) * (r * 0.5f));
  int y0 = cy - (int)(sinf(rad) * (r * 0.5f));
  tft.drawLine(x0, y0, x1, y1, COL_NET);
  float r2 = rad + 2.36f, r3 = rad - 2.36f;
  int ax = x1 + (int)(cosf(r2) * r * 0.45f), ay = y1 + (int)(sinf(r2) * r * 0.45f);
  int bx = x1 + (int)(cosf(r3) * r * 0.45f), by = y1 + (int)(sinf(r3) * r * 0.45f);
  tft.fillTriangle(x1, y1, ax, ay, bx, by, COL_NET);
}
// PAGE METEO
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void drawPageWeather() {
  for (int y = BODY_Y; y < SCR_H; y++)
    tft.drawFastHLine(0, y, SCR_W, (y % 4 == 0) ? 0x0021 : 0x0000);

  if (!wx.ok) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_DIM, COL_BG); tft.setTextSize(1);
    tft.drawString("Chargement meteo...", SCR_W / 2, SCR_H / 2);
    return;
  }

  char buf[64];
  const int Y0 = BODY_Y + 4;

  // ── AUJOURD'HUI ─────────────────────────────────────────────
  // Ville seule sur sa ligne (taille 1, non écrasée par la température)
  tft.setTextDatum(ML_DATUM); tft.setTextSize(1);
  tft.setTextColor(COL_WEATHER, COL_BG);
  tft.drawString(wx.city, 56, Y0 + 4);
  { struct tm ti;
    tft.setTextDatum(MR_DATUM); tft.setTextSize(3);
    if (getLocalTime(&ti, 0)) {
      snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
      tft.setTextColor(COL_TEXT, COL_BG);
    } else {
      unsigned long s = millis() / 1000;
      snprintf(buf, sizeof(buf), "%02luh%02lu", s/3600, (s%3600)/60);
      tft.setTextColor(COL_DIM, COL_BG);
    }
    tft.drawString(buf, SCR_W - 4, Y0 + 28);
    tft.setTextDatum(ML_DATUM);
  }

  // Tout le reste décalé de +10px pour dégager le nom de ville
  { bool isNight = false;
    struct tm _ti; if (getLocalTime(&_ti, 0)) {
      int now_min = _ti.tm_hour * 60 + _ti.tm_min;
      isNight = (now_min < wx.sunrise_min || now_min >= wx.sunset_min);
    }
    drawWxIcon(26, Y0 + 35, wx.code, 2, isNight);
  }

  tft.setTextColor(COL_TEXT, COL_BG); tft.setTextSize(3);
  snprintf(buf, sizeof(buf), "%.0f C", wx.temp);
  tft.drawString(buf, 56, Y0 + 28);

  tft.setTextSize(1);
  tft.setTextColor(COL_DIM, COL_BG);
  char ctr[28]; strncpy(ctr, wx.cond, 27); ctr[27] = 0;
  tft.drawString(ctr, 56, Y0 + 56);
  snprintf(buf, sizeof(buf), "Res: %.0fC", wx.feels);
  tft.drawString(buf, 56, Y0 + 68);

  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(COL_NET, COL_BG);
  snprintf(buf, sizeof(buf), "%.0fkm/h", wx.wind);
  tft.drawString(buf, SCR_W - 4, Y0 + 56);
  drawWindArrow(SCR_W - 85, Y0 + 56, wx.windDeg, 9);
  tft.setTextColor(COL_VRAM, COL_BG);
  snprintf(buf, sizeof(buf), "Hum %d%%", wx.hum);
  tft.drawString(buf, SCR_W - 4, Y0 + 68);
  { float moonDay = moonPhaseDay();
    drawMoonIcon(177, Y0 + 28, moonDay, 16);
    float dtf = 14.77f - moonDay;
    if (dtf < -14.77f) dtf += 29.53f;
    if (fabsf(dtf) <= 5.0f) {
      tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
      if (fabsf(dtf) < 0.5f) {
        tft.setTextColor(0xFFE0, COL_BG);
        tft.drawString("PLEINE!", 177, Y0 + 50);
      } else {
        tft.setTextColor(dtf > 0 ? 0xFC00 : COL_DIM, COL_BG);
        snprintf(buf, sizeof(buf), "PL %+.0fj", dtf);
        tft.drawString(buf, 177, Y0 + 50);
      }
    }
  }

  const int S1 = Y0 + 82;
  tft.drawLine(0, S1, SCR_W, S1, COL_LINE);

  // ── 3 JOURS ─────────────────────────────────────────────────
  const char* days[] = { "Auj.", "Dem.", "J+2 " };
  const int CW = SCR_W / 3;  // 107px

  for (int i = 0; i < 3; i++) {
    int cx   = i * CW + CW / 2;
    int fy   = S1 + 4;

    if (i > 0) tft.drawLine(i * CW, S1, i * CW, SCR_H, COL_LINE);

    tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
    tft.setTextColor(i == 0 ? COL_CPU : COL_DIM, COL_BG);
    tft.drawString(days[i], cx, fy + 5);

    int ic   = (i == 0) ? wx.code : wx.fore[i].code;
    int maxT = (i == 0) ? (int)wx.temp : wx.fore[i].maxT;
    int minT = (i == 0) ? (int)wx.feels : wx.fore[i].minT;
    drawWxIcon(cx, fy + 35, ic, 2, false);

    tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
    tft.setTextColor(COL_VRAM, COL_BG);
    snprintf(buf, sizeof(buf), "%d C", maxT);
    tft.drawString(buf, cx, fy + 65);
    tft.setTextColor(COL_NET, COL_BG);
    snprintf(buf, sizeof(buf), "%d C", minT);
    tft.drawString(buf, cx, fy + 77);
    int windDeg = (i == 0) ? wx.windDeg : wx.fore[i].windDeg;
    int windSpd = (i == 0) ? (int)wx.wind : wx.fore[i].windSpd;
    drawWindArrow(cx, fy + 87, windDeg, 7);
    tft.setTextDatum(MC_DATUM); tft.setTextSize(1);
    tft.setTextColor(COL_NET, COL_BG);
    snprintf(buf, sizeof(buf), "%dkm/h", windSpd);
    tft.drawString(buf, cx, fy + 102);
  }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Fetch fonctions (appelées depuis tâche FreeRTOS core 0)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

SemaphoreHandle_t dataMutex;
WebServer webServer(80);

void parseSerialLine(const String& line) {
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, line)) return;
  PCData tmp;
  tmp.cpu     = doc["cpu"]       | 0.0f;
  tmp.cpu_t   = doc["cpu_temp"]  | 0.0f;
  tmp.gpu     = doc["gpu"]       | 0.0f;
  tmp.gpu_t   = doc["gpu_temp"]  | 0.0f;
  tmp.ram     = doc["ram_pct"]   | 0.0f;
  tmp.ram_u   = doc["ram_used"]  | 0.0f;
  tmp.ram_tot = doc["ram_total"] | 0.0f;
  tmp.vram    = doc["vram_pct"]  | 0.0f;
  tmp.vram_u  = doc["vram_used"] | 0.0f;
  tmp.vram_tot= doc["vram_total"]| 0.0f;
  tmp.disk    = doc["disk_pct"]  | 0.0f;
  tmp.disk_u  = doc["disk_used"] | 0.0f;
  tmp.disk_tot= doc["disk_total"]| 0.0f;
  tmp.net_up  = doc["net_up"]    | 0.0f;
  tmp.net_dn  = doc["net_down"]  | 0.0f;
  tmp.os_linux = (strcmp(doc["os_type"] | "windows", "linux") == 0);
  tmp.swap     = doc["swap_pct"]   | 0.0f;
  tmp.swap_u   = doc["swap_used"]  | 0.0f;
  tmp.swap_tot = doc["swap_total"] | 0.0f;
  strncpy(tmp.cpu_name, doc["cpu_name"] | "\u2014", 47);
  strncpy(tmp.gpu_name, doc["gpu_name"] | "\u2014", 47);
  tmp.ok = true;
  tmp.ts = millis();
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  pc = tmp;
  histUp[histIdx] = (uint16_t)min(tmp.net_up, 65535.f);
  histDn[histIdx] = (uint16_t)min(tmp.net_dn, 65535.f);
  histIdx = (histIdx + 1) % HIST_N;
  xSemaphoreGive(dataMutex);
}

void processSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      _serialBuf.trim();
      if (_serialBuf.length() > 10) { parseSerialLine(_serialBuf); g_pcConn = 1; }
      _serialBuf = "";
    } else if (_serialBuf.length() < 1023) {
      _serialBuf += c;
    }
  }
}

// ── LED RGB helpers ─────────────────────────────────────────────
static void hsvToRgb(uint8_t h, uint8_t* r, uint8_t* g, uint8_t* b) {
  uint8_t reg = h / 43, rem = (h - reg * 43) * 6;
  const uint8_t v = 160;
  uint8_t q = (v * (255 - ((255 * rem) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((255 * (255 - rem)) >> 8))) >> 8;
  switch (reg % 6) {
    case 0: *r=v; *g=t; *b=0; break;
    case 1: *r=q; *g=v; *b=0; break;
    case 2: *r=0; *g=v; *b=t; break;
    case 3: *r=0; *g=q; *b=v; break;
    case 4: *r=t; *g=0; *b=v; break;
    default:*r=v; *g=0; *b=q; break;
  }
}
static void ledRGBSet(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(PIN_LED_R, 255 - r);  // active LOW
  ledcWrite(PIN_LED_G, 255 - g);
  ledcWrite(PIN_LED_B, 255 - b);
}
static void updateMinerCount() {
  miner.count = 0;
  for (int i = 0; i <= MAX_MINERS; i++) if (miner.dev[i].ok) miner.count++;
}

void fetchMinerAuto(const char* ip) {
  // Ignorer l'IP locale
  if (strcmp(ip, WiFi.localIP().toString().c_str()) == 0) return;

  // Chercher slot existant pour cette IP, ou premier slot libre
  int slot = -1;
  for (int i = 1; i <= MAX_MINERS; i++)
    if (strcmp(miner.dev[i].ip, ip) == 0) { slot = i; break; }
  if (slot == -1)
    for (int i = 1; i <= MAX_MINERS; i++)
      if (!miner.dev[i].ok && miner.dev[i].ip[0] == '\0') { slot = i; break; }
  if (slot == -1) return;

  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s/api/system/info", ip);
  http.begin(url); http.setTimeout(3000);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<2048> doc;
    if (!deserializeJson(doc, body)) {
      MinerDev tmp;
      strncpy(tmp.ip, ip, 19);
      tmp.ok = true;

      if (doc.containsKey("miner")) {
        // Format NMMiner
        tmp.hr_gh  = (doc["miner"]["hashRate"] | 0.0f) * 1e9f;
        tmp.sh_ok  = doc["miner"]["sAccepted"]      | 0;
        tmp.sh_ko  = doc["miner"]["sRejected"]      | 0;
        tmp.up_s   = doc["miner"]["uptimeSeconds"]  | 0UL;
        tmp.blkhits= doc["miner"]["blkhits"]        | 0;
        tmp.temp   = doc["temps"]["asic"]           | -1;
        strncpy(tmp.best_diff, doc["miner"]["bestDiffEver"]    | "---", 19);
        strncpy(tmp.best_sess, doc["miner"]["bestDiffSession"] | "---", 19);
        strncpy(tmp.net_diff,  doc["miner"]["networkDiff"]     | "---", 15);
        strncpy(tmp.ver,       doc["identity"]["fwVersion"]    | "---", 15);
        strncpy(tmp.name,      doc["identity"]["hostName"]     | "NMMiner", 19);
        const char* su = doc["stratum"]["url"] | "";
        const char* at = strstr(su, "://");
        strncpy(tmp.pool, at ? at + 3 : su, 47);
      } else if (doc.containsKey("hashRate")) {
        // Format Bitaxe (601x/602x)
        tmp.hr_gh  = (doc["hashRate"] | 0.0f) * 1e9f;
        tmp.sh_ok  = doc["sharesAccepted"] | 0;
        tmp.sh_ko  = doc["sharesRejected"] | 0;
        tmp.temp   = (int)(doc["temp"] | -1.0f);
        tmp.up_s   = doc["uptimeSeconds"] | 0UL;
        strncpy(tmp.name, doc["hostname"]   | "Bitaxe", 19);
        strncpy(tmp.ver,  doc["version"]    | "---", 15);
        strncpy(tmp.pool, doc["stratumURL"] | "---", 47);
        float bd = doc["bestDiff"] | 0.0f;
        if      (bd >= 1e9f) snprintf(tmp.best_diff, 19, "%.2fG", bd/1e9f);
        else if (bd >= 1e6f) snprintf(tmp.best_diff, 19, "%.2fM", bd/1e6f);
        else if (bd >= 1e3f) snprintf(tmp.best_diff, 19, "%.1fK", bd/1e3f);
        else                 snprintf(tmp.best_diff, 19, "%.0f",  bd);
      } else {
        tmp.ok = false;  // appareil inconnu, pas un miner
      }

      if (tmp.ok) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        miner.dev[slot] = tmp;
        netMinerCount = 0;
        for (int i = 1; i <= MAX_MINERS; i++) if (miner.dev[i].ok) netMinerCount++;
        updateMinerCount();
        miner.ts = millis();
        xSemaphoreGive(dataMutex);
      }
    }
  } else if (code < 0 || code == 404) {
    // Appareil connu mais ne repond plus -> offline
    if (miner.dev[slot].ip[0]) {
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      miner.dev[slot].ok = false;
      updateMinerCount();
      xSemaphoreGive(dataMutex);
    }
  }
  http.end();
}

void fetchBtcNetwork() {
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  http.begin(cli, "https://mempool.space/api/v1/difficulty-adjustment");
  http.setTimeout(8000);
  http.addHeader("User-Agent", "CYD-Monitor/1.0");
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<64> filter;
    filter["progressPercent"]  = true;
    filter["difficultyChange"] = true;
    filter["remainingBlocks"]  = true;
    filter["timeAvg"]          = true;
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
      BtcNetwork tmp;
      tmp.progress   = doc["progressPercent"]  | 0.0f;
      tmp.diff_chg   = doc["difficultyChange"] | 0.0f;
      tmp.rem_blocks = doc["remainingBlocks"]  | 0;
      tmp.blk_min    = (doc["timeAvg"] | 0.0f) / 60000.0f;
      tmp.ok  = true;
      tmp.ts  = millis();
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      btcnet = tmp;
      xSemaphoreGive(dataMutex);
    }
  }
  http.end();
}

void scanNextMiners() {
  static const char* seeds[] = {SEED_IP_1, SEED_IP_2, SEED_IP_3};
  for (const char* s : seeds) fetchMinerAuto(s);

  // Sonder SCAN_PER_CYCLE IPs supplementaires par cycle
  for (int i = 0; i < SCAN_PER_CYCLE; i++) {
    char ip[20];
    snprintf(ip, sizeof(ip), "192.168.1.%d", scanNextOctet);
    fetchMinerAuto(ip);
    if (++scanNextOctet > SCAN_END) scanNextOctet = SCAN_START;
  }
}

void fetchWeather() {
  HTTPClient http;
  char url[80];
  snprintf(url, sizeof(url), "http://wttr.in/%s?format=j1", WEATHER_CITY);
  http.begin(url);
  http.setTimeout(8000);
  http.addHeader("User-Agent", "CYD-Monitor/1.0");
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();

    // Filtre : ne parser que les champs utiles
    StaticJsonDocument<1024> flt;
    flt["current_condition"][0]["temp_C"] = true;
    flt["current_condition"][0]["FeelsLikeC"] = true;
    flt["current_condition"][0]["humidity"] = true;
    flt["current_condition"][0]["windspeedKmph"] = true;
    flt["current_condition"][0]["winddirDegree"] = true;
    flt["current_condition"][0]["weatherCode"] = true;
    flt["current_condition"][0]["weatherDesc"][0]["value"] = true;
    flt["nearest_area"][0]["areaName"][0]["value"] = true;
    flt["weather"][0]["astronomy"][0]["sunrise"] = true;
    flt["weather"][0]["astronomy"][0]["sunset"]  = true;
    for (int d = 0; d < 3; d++) {
      flt["weather"][d]["maxtempC"] = true;
      flt["weather"][d]["mintempC"] = true;
      flt["weather"][d]["weatherCode"] = true;
      flt["weather"][d]["hourly"][4]["winddirDegree"] = true;
      flt["weather"][d]["hourly"][4]["windspeedKmph"] = true;
    }

    StaticJsonDocument<8192> doc;
    if (!deserializeJson(doc, body, DeserializationOption::Filter(flt))) {
      WxData tmp;
      JsonObject cc = doc["current_condition"][0];
      tmp.temp   = atof(cc["temp_C"]         | "0");
      tmp.feels  = atof(cc["FeelsLikeC"]     | "0");
      tmp.hum    = atoi(cc["humidity"]        | "0");
      tmp.wind   = atof(cc["windspeedKmph"]  | "0");
      tmp.windDeg = atoi(cc["winddirDegree"]  | "0");
      tmp.code   = atoi(cc["weatherCode"]    | "113");
      strncpy(tmp.cond, cc["weatherDesc"][0]["value"] | "—", 47);
      strncpy(tmp.city, doc["nearest_area"][0]["areaName"][0]["value"] | WEATHER_CITY, 31);
      for (int d = 0; d < 3; d++) {
        JsonObject day = doc["weather"][d];
        tmp.fore[d].maxT = atoi(day["maxtempC"] | "0");
        tmp.fore[d].minT = atoi(day["mintempC"] | "0");
        tmp.fore[d].code = atoi(day["weatherCode"] | "113");
        tmp.fore[d].windDeg = atoi(day["hourly"][4]["winddirDegree"] | "0");
        tmp.fore[d].windSpd = atoi(day["hourly"][4]["windspeedKmph"] | "0");
      }
      // Astronomy sunrise/sunset
      { const char* sr = doc["weather"][0]["astronomy"][0]["sunrise"] | "7:00 AM";
        const char* ss = doc["weather"][0]["astronomy"][0]["sunset"]  | "8:00 PM";
        auto parseT = [](const char* s) -> int {
          int h = atoi(s); const char* c = strchr(s, ':');
          int m = c ? atoi(c + 1) : 0;
          if (strstr(s, "PM") && h != 12) h += 12;
          if (strstr(s, "AM") && h == 12) h = 0;
          return h * 60 + m;
        };
        tmp.sunrise_min = parseT(sr);
        tmp.sunset_min  = parseT(ss);
      }
      tmp.ok = true;
      tmp.ts = millis();
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      wx = tmp;
      xSemaphoreGive(dataMutex);
    }
  }
  http.end();
}

// Fetch stats PC via WiFi HTTP (fallback série USB)
void fetchPC() {
  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s:%d/stats", PC_AGENT_HOST, PC_AGENT_PORT);
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    parseSerialLine(body);
    g_pcConn = 2;
  }
  http.end();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Stats mining local → miner.dev[0] (ce CYD lui-meme)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
void updateLocalMinerStats() {
  if (xSemaphoreTake(minerMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  LocalMinerStats s = localMiner;
  xSemaphoreGive(minerMutex);

  xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100));
  MinerDev& d = miner.dev[0];  // slot 0 = ce CYD
  d.ok    = s.authorized;
  strncpy(d.name, workerSuffix(), 19);
  strncpy(d.ip,   WiFi.localIP().toString().c_str(), 19);
  strncpy(d.ver,  DEVICE_FW_VER, 15);
  d.hr_gh = s.hashrate_mhs * 1e6f;
  d.sh_ok = (int)s.shares_ok;
  d.sh_ko = (int)s.shares_ko;
  d.up_s  = (s.up_start > 0) ? (millis() - s.up_start) / 1000UL : 0;
  strncpy(d.pool,      STRATUM_HOST, 47);
  strncpy(d.best_diff, s.best_diff,  19);
  updateMinerCount();
  miner.ts = millis();
  xSemaphoreGive(dataMutex);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Tâche FreeRTOS (core 0) — polling non-bloquant pour l'affichage
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

unsigned long tPC = 0;
#define IV_PC 5000UL   // poll PC stats WiFi toutes les 5s

// Fetch appareils reseau depuis pc_agent /network
void fetchLAN() {
  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s:8765/network", LAN_AGENT_HOST);
  http.begin(url);
  http.setTimeout(5000);
  lanHttpCode = http.GET();
  if (lanHttpCode == 200) {
    String body = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
      int n = 0;
      for (JsonObject d : doc.as<JsonArray>()) {
        if (n >= LAN_MAX) break;
        const char* ip   = d["ip"]   | "";
        const char* name = d["name"] | "";
        strncpy(lanDevs[n].ip,   ip,   15); lanDevs[n].ip[15]   = 0;
        strncpy(lanDevs[n].name, name, 21); lanDevs[n].name[21] = 0;
        lanDevs[n].wifi = d["wifi"] | false;
        lanDevs[n].ok   = d["ok"]   | false;
        n++;
      }
      lanCount = n;
    }
  }
  http.end();
}

void taskFetch(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      unsigned long now = millis();
      // PC stats via WiFi (priorité basse : fallback si série USB absent)
      if (now - tPC >= IV_PC) {
        fetchPC();
        tPC = millis();
      }
      if (now - tMiner >= IV_MINER) {
        scanNextMiners();
        updateLocalMinerStats();
        tMiner = millis();
      }
      if (now - tWx >= IV_WX) {
        fetchWeather();
        tWx = millis();
      }
      if (now - tNet >= IV_NET) {
        fetchBtcNetwork();
        tNet = millis();
      }
      if (now - tLAN >= IV_LAN) {
        fetchLAN();
        tLAN = millis();
      }
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Touch
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void handleTouch() {
  if (!ts.touched()) {
    wasTouched = false;
    return;
  }
  if (wasTouched) return;
  if (millis() - lastTap < 300) return;

  TS_Point p = ts.getPoint();
  int sx = map(p.x, TS_MINX, TS_MAXX, SCR_W, 0);
  int sy = map(p.y, TS_MINY, TS_MAXY, SCR_H, 0);

  // Debug visuel : coordonnées brutes en bas de l'écran (aide au calibrage)
  char dbg[32];
  snprintf(dbg, sizeof(dbg), "raw %d,%d  sc %d,%d", p.x, p.y, sx, sy);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString(dbg, SCR_W / 2, SCR_H - 8);

  if (sy < TAB_H) {
    // Toucher la barre d'onglets : navigation directe
    int tab = sx / (SCR_W / PAGE_COUNT);
    if (tab >= 0 && tab < PAGE_COUNT) currentPage = (Page)tab;
  } else {
    if (currentPage == PAGE_SCAN && sx >= SCR_W - 20) {
      const int MAX_R = (SCR_H - (BODY_Y + 21)) / 18;
      if (sy < SCR_H / 2) { if (scanScroll > 0) scanScroll--; }
      else { if (scanScroll + MAX_R < lanCount) scanScroll++; }
    } else {
      currentPage = (Page)((currentPage + 1) % PAGE_COUNT);
      scanScroll = 0;
    }
  }

  wasTouched = true;
  lastTap = millis();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Setup / Loop
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void drawCurrentPage() {
  tft.setRotation(3);
  drawTabs();
  switch (currentPage) {
    case PAGE_PC:      drawPagePC();      break;
    case PAGE_NET:     drawPageNet();     break;
    case PAGE_SCAN:    drawPageScan();    break;
    case PAGE_WEATHER: drawPageWeather(); break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);

  // Backlight (API LEDC core 3.x)
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, 220);
  // LED RGB init (rouge pendant boot)
  ledcAttach(PIN_LED_R, 1000, 8);
  ledcAttach(PIN_LED_G, 1000, 8);
  ledcAttach(PIN_LED_B, 1000, 8);
  ledRGBSet(255, 0, 0);

  // Affichage
  tft.init();
  tft.setRotation(3);   // MX+MV — paysage 320x240, couvre tout le GRAM, pas de MY (sans inversion couleur)
  tft.fillScreen(COL_BG);

  // Splash
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_CPU, COL_BG);
  tft.setTextSize(2);
  tft.drawString("CYD Monitor", SCR_W / 2, SCR_H / 2 - 20);
  tft.setTextSize(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("Connexion WiFi...", SCR_W / 2, SCR_H / 2 + 10);

  // Touch (SPI séparé VSPI — XPT_CS géré par la lib, pas par SPIClass)
  touchSPI.begin(XPT_CLK, XPT_MISO, XPT_MOSI);
  ts.begin(touchSPI);
  ts.setRotation(0);

  // WiFi
  WiFi.mode(WIFI_STA);
  Serial.printf("CYD_MAC:%s\n", WiFi.macAddress().c_str());
  Serial.printf("CYD_HOST:%s\n", PC_AGENT_HOST);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 60) {  // 30s max
    delay(500);
    if (tries % 10 == 0) {
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(COL_DIM, COL_BG);
      tft.setTextSize(1);
      char buf[20]; snprintf(buf, sizeof(buf), "WiFi... %ds", tries/2);
      tft.fillRect(80, SCR_H/2+10, 160, 12, COL_BG);
      tft.drawString(buf, SCR_W/2, SCR_H/2+10);
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(3600, 0, "pool.ntp.org");  // NTP UTC+1 (France hiver)
    tft.setTextColor(COL_OK, COL_BG);
    char ipbuf[32];
    snprintf(ipbuf, sizeof(ipbuf), "IP: %s", WiFi.localIP().toString().c_str());
    tft.drawString(ipbuf, SCR_W / 2, SCR_H / 2 + 24);
  } else {
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("WiFi KO — mode offline", SCR_W / 2, SCR_H / 2 + 24);
  }
  delay(1200);

  // OTA
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    tft.fillScreen(COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_MINER, COL_BG);
    tft.setTextSize(2);
    tft.drawString("OTA Update...", SCR_W / 2, SCR_H / 2 - 20);
    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("Ne pas couper l'alimentation", SCR_W / 2, SCR_H / 2 + 10);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    int pct = done * 100 / total;
    tft.fillRect(20, SCR_H / 2 + 30, 280, 12, COL_TRACK);
    tft.fillRect(20, SCR_H / 2 + 30, 280 * pct / 100, 12, COL_MINER);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextSize(1);
    char buf[12]; snprintf(buf, sizeof(buf), "%d%%", pct);
    tft.fillRect(140, SCR_H / 2 + 48, 40, 10, COL_BG);
    tft.drawString(buf, SCR_W / 2, SCR_H / 2 + 48);
  });
  ArduinoOTA.onError([](ota_error_t err) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_WARN, COL_BG);
    tft.setTextSize(1);
    tft.drawString("OTA Erreur — reboot", SCR_W / 2, SCR_H / 2 + 65);
  });
  ArduinoOTA.begin();

  // SPIFFS (image de fond)
  SPIFFS.begin(true);

  // Mutex + tâches
  dataMutex  = xSemaphoreCreateMutex();
  minerMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(taskFetch, "fetch", 20480, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(vTaskMine, "mine",  32768, NULL, 1, NULL, 0);

  // WebServer API (compatibilité scanner miner_scanner.py)
  webServer.on("/api/system/info", HTTP_GET, []() {
    if (xSemaphoreTake(minerMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
      webServer.send(503, "text/plain", "busy"); return;
    }
    LocalMinerStats s = localMiner;
    xSemaphoreGive(minerMutex);
    char json[640];
    unsigned long up = (s.up_start > 0) ? (millis() - s.up_start) / 1000UL : 0UL;
    snprintf(json, sizeof(json),
      "{\"identity\":{\"hwModel\":\"NMMiner\",\"fwVersion\":\"%s\","
      "\"hostName\":\"nm2\"},"
      "\"miner\":{\"hashRate\":%.8f,\"uptimeSeconds\":%lu,"
      "\"sAccepted\":%u,\"sRejected\":%u,\"bestDiffEver\":\"%s\"},"
      "\"stratum\":{\"url\":\"%s\","
      "\"user\":\"%s\",\"connected\":%s}}",
      DEVICE_FW_VER,
      (double)(s.hashrate_mhs / 1000.0f),
      up,
      s.shares_ok, s.shares_ko,
      s.best_diff,
      STRATUM_HOST,
      WORKER_NAME,
      s.connected ? "true" : "false");
    webServer.send(200, "application/json", json);
  });
  webServer.begin();

  tft.fillScreen(COL_BG);
  drawCurrentPage();
}

void loop() {
  ArduinoOTA.handle();
  webServer.handleClient();
  processSerial();
  handleTouch();

  // Redessiner si page changée ou toutes les 5s (rafraîchissement données)
  static unsigned long lastDraw = 0;
  bool pageChanged = (currentPage != prevPage);
  bool refresh = (millis() - lastDraw > 5000);

  if (pageChanged || refresh) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    drawCurrentPage();
    xSemaphoreGive(dataMutex);
    prevPage  = currentPage;
    lastDraw  = millis();
  }

  // ── LED RGB : rouge si services KO, arc-en-ciel si tout OK ──
  {
    static unsigned long _ledTs = 0;
    static uint8_t _hue = 0;
    if (millis() - _ledTs > 50) {
      _ledTs = millis();
      bool allOk = false;
      if (xSemaphoreTake(dataMutex, 0) == pdTRUE) {
        allOk = (WiFi.status() == WL_CONNECTED) && pc.ok
                && miner.count > 0;
        xSemaphoreGive(dataMutex);
      }
      if (!allOk) {
        ledRGBSet(220, 0, 0);
      } else {
        uint8_t r, g, b;
        hsvToRgb(_hue++, &r, &g, &b);
        ledRGBSet(r, g, b);
      }
    }
  }
  // ── LDR : luminosité écran selon lumière ambiante ─────────────
  {
    static unsigned long _ldrTs = 0;
    static bool _dimmed = false;
    if (millis() - _ldrTs > 1000) {
      _ldrTs = millis();
      int ldr = analogRead(PIN_LDR);
      // hysteresis : sombre <400 → dim, clair >700 → plein
      bool nowDark = ldr < (_dimmed ? 700 : 400);
      if (nowDark != _dimmed) {
        _dimmed = nowDark;
        ledcWrite(TFT_BL, _dimmed ? 110 : 220);
      }
    }
  }
  delay(50);
}
