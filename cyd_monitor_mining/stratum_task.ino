// ================================================================
// stratum_task.ino — Tâche FreeRTOS mining Bitcoin (Core 0)
// Protocole Stratum v1 + SHA256d software (DRAM+IRAM, no mutex)
// ================================================================
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "hybrid_config.h"

// ── Définitions des globales déclarées extern dans hybrid_config.h
LocalMinerStats localMiner;
SemaphoreHandle_t minerMutex = NULL;

// ── Helpers hex ─────────────────────────────────────────────────
static void hexToBytes(const char* hex, uint8_t* out, int maxOut) {
  for (int i = 0; i < maxOut && hex[2*i] && hex[2*i+1]; i++) {
    char buf[3] = {hex[2*i], hex[2*i+1], 0};
    out[i] = (uint8_t)strtoul(buf, NULL, 16);
  }
}
static void bytesToHex(const uint8_t* in, int len, char* out) {
  for (int i = 0; i < len; i++) {
    sprintf(out + 2*i, "%02x", in[i]);
  }
  out[2*len] = 0;
}
static uint32_t swab32(uint32_t v) {
  return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8)
       | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF);
}

// ── Stratum I/O ─────────────────────────────────────────────────
static bool stratumSend(WiFiClient& cli, const char* line) {
  cli.print(line);
  cli.print('\n');
  return true;
}
static int stratumReadLine(WiFiClient& cli, char* buf, int maxLen, uint32_t timeoutMs = 8000) {
  unsigned long t0 = millis();
  int i = 0;
  buf[0] = 0;
  while (millis() - t0 < timeoutMs) {
    if (!cli.connected()) return -1;
    while (cli.available() && i < maxLen - 1) {
      char c = cli.read();
      if (c == '\n') { buf[i] = 0; return i; }
      if (c != '\r') buf[i++] = c;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  buf[i] = 0;
  return i;
}

// ── SHA256 Software (DRAM data + IRAM code, zéro mutex WiFi) ────
// DRAM_ATTR = données mutables en DRAM (accès 1 cycle, pas de cache SPI)
// IRAM_ATTR = code en IRAM (zéro cache-miss SPI)
static DRAM_ATTR uint32_t _K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
  0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
  0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
  0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
  0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
  0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
  0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
  0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static DRAM_ATTR uint32_t _H0[8] = {
  0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
  0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
};

#define _ROTR(x,n) (((uint32_t)(x)>>(n))|((uint32_t)(x)<<(32-(n))))
#define _SIG0(x) (_ROTR(x,7)^_ROTR(x,18)^((x)>>3))
#define _SIG1(x) (_ROTR(x,17)^_ROTR(x,19)^((x)>>10))
#define _EP0(x)  (_ROTR(x,2)^_ROTR(x,13)^_ROTR(x,22))
#define _EP1(x)  (_ROTR(x,6)^_ROTR(x,11)^_ROTR(x,25))
#define _CH(x,y,z) (((x)&(y))^((~(x))&(z)))
#define _MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))

static void IRAM_ATTR _sha256_transform(uint32_t s[8], const uint8_t d[64]) {
  uint32_t w[64];
  int i;
  for (i = 0; i < 16; i++)
    w[i] = ((uint32_t)d[i*4]<<24)|((uint32_t)d[i*4+1]<<16)
           |((uint32_t)d[i*4+2]<<8)|(uint32_t)d[i*4+3];
  for (i = 16; i < 64; i++)
    w[i] = _SIG1(w[i-2]) + w[i-7] + _SIG0(w[i-15]) + w[i-16];
  uint32_t a=s[0],b=s[1],c=s[2],dd=s[3];
  uint32_t e=s[4],f=s[5],g=s[6],h=s[7];
  uint32_t t1,t2;
  for (i = 0; i < 64; i++) {
    t1 = h + _EP1(e) + _CH(e,f,g) + _K[i] + w[i];
    t2 = _EP0(a) + _MAJ(a,b,c);
    h=g; g=f; f=e; e=dd+t1;
    dd=c; c=b; b=a; a=t1+t2;
  }
  s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=dd;
  s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=h;
}

static void IRAM_ATTR _sha256_sw(const uint8_t* data, size_t len, uint8_t* out) {
  uint32_t state[8];
  memcpy(state, _H0, 32);
  size_t i = 0;
  for (; i + 64 <= len; i += 64)
    _sha256_transform(state, data + i);
  uint8_t block[64];
  size_t rest = len - i;
  if (rest) memcpy(block, data + i, rest);
  block[rest] = 0x80;
  if (rest < 56) {
    if (rest + 1 < 56) memset(block + rest + 1, 0, 55 - rest);
  } else {
    memset(block + rest + 1, 0, 63 - rest);
    _sha256_transform(state, block);
    memset(block, 0, 56);
  }
  uint64_t bits = (uint64_t)len * 8;
  block[56]=(uint8_t)(bits>>56); block[57]=(uint8_t)(bits>>48);
  block[58]=(uint8_t)(bits>>40); block[59]=(uint8_t)(bits>>32);
  block[60]=(uint8_t)(bits>>24); block[61]=(uint8_t)(bits>>16);
  block[62]=(uint8_t)(bits>> 8); block[63]=(uint8_t)(bits);
  _sha256_transform(state, block);
  for (i = 0; i < 8; i++) {
    out[i*4+0]=(state[i]>>24)&0xFF; out[i*4+1]=(state[i]>>16)&0xFF;
    out[i*4+2]=(state[i]>> 8)&0xFF; out[i*4+3]= state[i]      &0xFF;
  }
}

static void IRAM_ATTR sha256d(const uint8_t* data, size_t len, uint8_t* out) {
  uint8_t tmp[32];
  _sha256_sw(data, len, tmp);
  _sha256_sw(tmp, 32, out);
}

// ── Target ───────────────────────────────────────────────────────
// Convertit bits (compact) → target[32] LE
static void bitsToTarget(uint32_t bits, uint8_t* target) {
  memset(target, 0, 32);
  int exp = bits >> 24;
  uint32_t mant = bits & 0xFFFFFF;
  int pos = 32 - exp;
  if (pos >= 0 && pos + 2 < 32) {
    target[pos]   = (mant >> 16) & 0xFF;
    target[pos+1] = (mant >>  8) & 0xFF;
    target[pos+2] =  mant        & 0xFF;
  }
}
// Vérifie hash[32] < target[32] (les deux en big-endian)
static bool IRAM_ATTR hashMeetsTarget(const uint8_t* hash, const uint8_t* target) {
  // SHA256 output = big-endian : hash[0] = MSB, comparaison depuis le haut
  for (int i = 0; i < 32; i++) {
    if (hash[i] < target[i]) return true;
    if (hash[i] > target[i]) return false;
  }
  return true;
}
// Difficulty → target big-endian (index 0 = MSB)
static void difficultyToTarget(double diff, uint8_t* target) {
  memset(target, 0, 32);
  if (diff <= 0.0) { memset(target, 0xFF, 32); return; }
  // Bitcoin max_target = 0x00000000FFFF0000...0000
  // Les 8 octets de poids fort valent 0x00000000FFFF0000
  uint64_t max_top = 0x00000000FFFF0000ULL;
  uint64_t t64 = (uint64_t)((double)max_top / diff);
  for (int i = 0; i < 8; i++)
    target[i] = (uint8_t)((t64 >> (56 - i*8)) & 0xFF);
}

// ── Job courant ──────────────────────────────────────────────────
static struct {
  char     job_id[32]      = "";
  uint8_t  prev_hash[32]   = {};
  char     coinbase1_hex[512] = "";
  char     coinbase2_hex[256] = "";
  char     merkle_branch[16][65] = {};
  int      branch_count    = 0;
  uint32_t version         = 2;
  uint32_t bits            = 0x1a00ffff;
  uint32_t ntime           = 0;
  uint8_t  extranonce1[8]  = {};
  int      en1_len         = 4;
  int      en2_size        = 4;
  double   difficulty      = 1.0;
  bool     clean           = false;
  bool     valid           = false;
} job;

static uint32_t msgId = 1;

// ── Merkle root ──────────────────────────────────────────────────
static void computeMerkleRoot(const uint8_t* coinbase, int cb_len,
                               uint8_t* root) {
  sha256d(coinbase, cb_len, root);
  for (int i = 0; i < job.branch_count; i++) {
    uint8_t branch[32];
    hexToBytes(job.merkle_branch[i], branch, 32);
    uint8_t tmp[64];
    memcpy(tmp, root, 32);
    memcpy(tmp + 32, branch, 32);
    sha256d(tmp, 64, root);
  }
}

// ── Construire block header ──────────────────────────────────────
static void buildHeader(uint8_t* hdr, uint32_t nonce, uint32_t en2) {
  // version (4B LE)
  hdr[0] = job.version & 0xFF;
  hdr[1] = (job.version >> 8) & 0xFF;
  hdr[2] = (job.version >> 16) & 0xFF;
  hdr[3] = (job.version >> 24) & 0xFF;
  // prev_hash: Stratum envoie 8 mots 32-bit, chacun inversé
  for (int w = 0; w < 8; w++) {
    hdr[4 + w*4 + 0] = job.prev_hash[w*4 + 3];
    hdr[4 + w*4 + 1] = job.prev_hash[w*4 + 2];
    hdr[4 + w*4 + 2] = job.prev_hash[w*4 + 1];
    hdr[4 + w*4 + 3] = job.prev_hash[w*4 + 0];
  }
  // Coinbase = coinbase1 + extranonce1 + extranonce2 + coinbase2
  int cb1_len = strlen(job.coinbase1_hex) / 2;
  int cb2_len = strlen(job.coinbase2_hex) / 2;
  int cb_total = cb1_len + job.en1_len + job.en2_size + cb2_len;
  uint8_t coinbase[1024];
  int pos = 0;
  hexToBytes(job.coinbase1_hex, coinbase, cb1_len); pos += cb1_len;
  memcpy(coinbase + pos, job.extranonce1, job.en1_len); pos += job.en1_len;
  for (int i = 0; i < job.en2_size; i++) coinbase[pos++] = (en2 >> (i*8)) & 0xFF;
  hexToBytes(job.coinbase2_hex, coinbase + pos, cb2_len); pos += cb2_len;
  // Merkle root
  uint8_t mroot[32];
  computeMerkleRoot(coinbase, pos, mroot);
  memcpy(hdr + 36, mroot, 32);
  // ntime
  hdr[68] = job.ntime & 0xFF;
  hdr[69] = (job.ntime >> 8) & 0xFF;
  hdr[70] = (job.ntime >> 16) & 0xFF;
  hdr[71] = (job.ntime >> 24) & 0xFF;
  // bits
  hdr[72] = job.bits & 0xFF;
  hdr[73] = (job.bits >> 8) & 0xFF;
  hdr[74] = (job.bits >> 16) & 0xFF;
  hdr[75] = (job.bits >> 24) & 0xFF;
  // nonce
  hdr[76] = nonce & 0xFF;
  hdr[77] = (nonce >> 8) & 0xFF;
  hdr[78] = (nonce >> 16) & 0xFF;
  hdr[79] = (nonce >> 24) & 0xFF;
}

// ── Soumettre une share ──────────────────────────────────────────
static bool submitShare(WiFiClient& cli, uint32_t nonce, uint32_t en2) {
  char en2hex[16]; sprintf(en2hex, "%0*x", job.en2_size * 2, (unsigned)en2);
  char nhex[9];    sprintf(nhex, "%08x", (unsigned)swab32(nonce));
  char ntimehex[9]; sprintf(ntimehex, "%08x", (unsigned)job.ntime);
  char bitshex[9];  sprintf(bitshex, "%08x", (unsigned)job.bits);
  char msg[256];
  snprintf(msg, sizeof(msg),
    "{\"id\":%lu,\"method\":\"mining.submit\","
    "\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}",
    (unsigned long)msgId++,
    WORKER_NAME, job.job_id, en2hex, ntimehex, nhex);
  Serial.printf("[mine] SUBMIT nonce=%s diff=%.3f\n", nhex, job.difficulty);
  return stratumSend(cli, msg);
}

// ── Calculer la difficulté de la hash trouvée ────────────────────
static double hashToDiff(const uint8_t* hash) {
  // hash[0] = MSB (SHA256 big-endian)
  uint64_t val = 0;
  for (int i = 0; i < 8; i++) val = (val << 8) | hash[i];
  if (val == 0) return 1e15;
  return (double)0x00000000FFFF0000ULL / (double)val;
}

// ── Mise à jour best_diff ────────────────────────────────────────
static void updateBestDiff(double diff) {
  char buf[24];
  if      (diff >= 1e12) snprintf(buf, sizeof(buf), "%.3fT", diff/1e12);
  else if (diff >= 1e9)  snprintf(buf, sizeof(buf), "%.3fG", diff/1e9);
  else if (diff >= 1e6)  snprintf(buf, sizeof(buf), "%.3fM", diff/1e6);
  else if (diff >= 1e3)  snprintf(buf, sizeof(buf), "%.1fK", diff/1e3);
  else                   snprintf(buf, sizeof(buf), "%.1f",  diff);
  if (xSemaphoreTake(minerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    strncpy(localMiner.best_diff, buf, 23);
    xSemaphoreGive(minerMutex);
  }
}

// ══════════════════════════════════════════════════════════════
// Shared state Core0 (vTaskMine) <-> Core1 (vTaskHash)
// Core0 ecrit hdrCache1/target1/jobVer ; Core1 lit et hash
// Core1 ecrit shareFound/shareNonce1 ; Core0 lit et soumet
// ══════════════════════════════════════════════════════════════
static DRAM_ATTR volatile bool     g_jobReady    = false;
static DRAM_ATTR          uint8_t  g_hdrCache1[80] = {};
static DRAM_ATTR          uint8_t  g_target1[32]   = {};
static DRAM_ATTR volatile uint32_t g_jobVer      = 0;
static DRAM_ATTR volatile bool     g_shareFound  = false;
static DRAM_ATTR volatile uint32_t g_shareNonce1 = 0;
static DRAM_ATTR volatile uint64_t g_hashes1     = 0;

// ══════════════════════════════════════════════════════════════
// vTaskHash (Core 1) -- aucune operation WiFi/TCP ici
// ══════════════════════════════════════════════════════════════
void IRAM_ATTR vTaskHash(void*) {
  uint8_t  hdr[80], hash[32];
  uint8_t  lHdr[80], lTgt[32];
  uint32_t lastVer = 0xFFFFFFFF;
  uint32_t nonce   = 0;
  for (;;) {
    if (!g_jobReady) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    uint32_t v = g_jobVer;
    if (v != lastVer) {
      memcpy(lHdr, (const void*)g_hdrCache1, 80);
      memcpy(lTgt, (const void*)g_target1,   32);
      nonce   = 0;
      lastVer = v;
    }
    for (int b = 0; b < 2000; b++) {
      memcpy(hdr, lHdr, 80);
      hdr[76] =  nonce        & 0xFF;
      hdr[77] = (nonce >>  8) & 0xFF;
      hdr[78] = (nonce >> 16) & 0xFF;
      hdr[79] = (nonce >> 24) & 0xFF;
      sha256d(hdr, 80, hash);
      g_hashes1++;
      if (!g_shareFound && hashMeetsTarget(hash, lTgt)) {
        g_shareNonce1 = nonce;
        g_shareFound  = true;
      }
      nonce++;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Tâche principale mining (Core 0)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
void IRAM_ATTR vTaskMine(void* pvParams) {
  WiFiClient cli;
  char lineBuf[2048];
  uint32_t en2 = 0;
  uint64_t lastHashCount = 0;
  unsigned long lastHRupdate = 0;
  uint8_t target[32];

  for (;;) {
    // ── Attendre WiFi ────────────────────────────────────────────
    while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(2000));

    // ── Connexion stratum ────────────────────────────────────────
    Serial.printf("[mine] Connexion %s:%d\n", STRATUM_HOST, STRATUM_PORT);
    if (!cli.connect(STRATUM_HOST, STRATUM_PORT)) {
      Serial.println("[mine] Connexion KO, retry 10s");
      if (xSemaphoreTake(minerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        localMiner.connected = false;
        xSemaphoreGive(minerMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(10000));
      continue;
    }
    if (xSemaphoreTake(minerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      localMiner.connected = true;
      localMiner.up_start = millis();
      xSemaphoreGive(minerMutex);
    }

    // ── Subscribe ────────────────────────────────────────────────
    char subMsg[128];
    snprintf(subMsg, sizeof(subMsg),
      "{\"id\":%lu,\"method\":\"mining.subscribe\","
      "\"params\":[\"" DEVICE_HW "/" DEVICE_FW_VER "\",null]}",
      (unsigned long)msgId++);
    stratumSend(cli, subMsg);

    bool subscribed = false;
    int tries = 0;
    while (!subscribed && tries++ < 20) {
      int n = stratumReadLine(cli, lineBuf, sizeof(lineBuf), 3000);
      if (n <= 0) continue;
      DynamicJsonDocument doc(1024);
      if (deserializeJson(doc, lineBuf) != DeserializationError::Ok) continue;
      if (doc.containsKey("result") && doc["result"].is<JsonArray>()) {
        JsonArray res = doc["result"].as<JsonArray>();
        if (res.size() >= 3 && !res[1].isNull() && !res[2].isNull()) {
          const char* en1hex = res[1].as<const char*>();
          job.en2_size = res[2].as<int>();
          job.en1_len = strlen(en1hex) / 2;
          hexToBytes(en1hex, job.extranonce1, 8);
          subscribed = true;
          Serial.printf("[mine] Subscribe OK en1=%s en2size=%d\n", en1hex, job.en2_size);
        }
      }
    }
    if (!subscribed) { cli.stop(); vTaskDelay(pdMS_TO_TICKS(5000)); continue; }

    // ── Authorize ────────────────────────────────────────────────
    char authMsg[256];
    snprintf(authMsg, sizeof(authMsg),
      "{\"id\":%lu,\"method\":\"mining.authorize\","
      "\"params\":[\"%s\",\"%s\"]}",
      (unsigned long)msgId++, WORKER_NAME, WORKER_PASS);
    stratumSend(cli, authMsg);

    bool authorized = false;
    tries = 0;
    while (!authorized && tries++ < 20) {
      int n = stratumReadLine(cli, lineBuf, sizeof(lineBuf), 3000);
      if (n <= 0) continue;
      DynamicJsonDocument doc(512);
      if (deserializeJson(doc, lineBuf) != DeserializationError::Ok) continue;
      if (doc["result"].as<bool>() == true) {
        authorized = true;
        Serial.println("[mine] Authorize OK");
      }
    }
    if (!authorized) { cli.stop(); vTaskDelay(pdMS_TO_TICKS(5000)); continue; }

    if (xSemaphoreTake(minerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      localMiner.authorized = true;
      xSemaphoreGive(minerMutex);
    }

    // ── Boucle mining ────────────────────────────────────────────
    difficultyToTarget(job.difficulty, target);

    while (cli.connected()) {
      // Lire messages entrants (non-bloquant)
      while (cli.available()) {
        int n = stratumReadLine(cli, lineBuf, sizeof(lineBuf), 100);
        if (n <= 0) break;
        DynamicJsonDocument doc(2048);
        if (deserializeJson(doc, lineBuf) != DeserializationError::Ok) continue;

        const char* method = doc["method"] | "";
        if (strcmp(method, "mining.notify") == 0) {
          JsonArray p = doc["params"].as<JsonArray>();
          if (p.size() < 9) continue;
          bool clean = p[8].as<bool>();
          en2++;
          strncpy(job.job_id, p[0].as<const char*>(), 31);
          hexToBytes(p[1].as<const char*>(), job.prev_hash, 32);
          strncpy(job.coinbase1_hex, p[2].as<const char*>(), 511);
          strncpy(job.coinbase2_hex, p[3].as<const char*>(), 255);
          JsonArray mb = p[4].as<JsonArray>();
          job.branch_count = min((int)mb.size(), 16);
          for (int i = 0; i < job.branch_count; i++)
            strncpy(job.merkle_branch[i], mb[i].as<const char*>(), 64);
          job.version = strtoul(p[5].as<const char*>(), NULL, 16);
          job.bits    = strtoul(p[6].as<const char*>(), NULL, 16);
          job.ntime   = strtoul(p[7].as<const char*>(), NULL, 16);
          job.clean   = clean;
          job.valid   = true;
        } else if (strcmp(method, "mining.set_difficulty") == 0) {
          job.difficulty = doc["params"][0].as<double>();
          difficultyToTarget(job.difficulty, target);
          Serial.printf("[mine] Difficulty: %.1f\n", job.difficulty);
        } else if (doc.containsKey("result")) {
          // Réponse à submit
          bool ok = doc["result"].as<bool>();
          if (xSemaphoreTake(minerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (ok) localMiner.shares_ok++;
            else    localMiner.shares_ko++;
            xSemaphoreGive(minerMutex);
          }
          Serial.printf("[mine] Share %s\n", ok ? "ACCEPTED" : "REJECTED");
        }
      }

      // ── Mise a jour job -> Core1 ──────────────────────────────────
      if (job.valid) {
        static uint32_t lastEn2Cache = 0xDEADBEEF;
        static char     lastJobCache[32] = "";
        static uint8_t  hdrCache[80] = {};
        if (en2 != lastEn2Cache || strncmp(job.job_id, lastJobCache, 31) != 0) {
          buildHeader(hdrCache, 0, en2);
          lastEn2Cache = en2;
          strncpy(lastJobCache, job.job_id, 31);
          memcpy((void*)g_hdrCache1, hdrCache, 80);
          difficultyToTarget(job.difficulty, (uint8_t*)g_target1);
          g_jobReady = true;
          g_jobVer++;
        }
      }
      // ── Share trouvee par Core1 -> soumettre ─────────────────────────
      if (g_shareFound) {
        uint32_t sn = g_shareNonce1;
        g_shareFound = false;
        submitShare(cli, sn, en2);
        en2++;
        static uint8_t hdrPost[80] = {};
        buildHeader(hdrPost, 0, en2);
        memcpy((void*)g_hdrCache1, hdrPost, 80);
        g_jobVer++;
      }
      // ── Hashrate (lu depuis Core1) ────────────────────────────────────
      {
        unsigned long now = millis();
        if (now - lastHRupdate >= 5000) {
          uint64_t cur = g_hashes1;
          float mhs = (float)(cur - lastHashCount) / ((now - lastHRupdate) / 1000.0f) / 1e6f;
          if (xSemaphoreTake(minerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            localMiner.hashrate_mhs = mhs;
            localMiner.hashes = (unsigned long)cur;
            xSemaphoreGive(minerMutex);
          }
          lastHashCount = cur;
          lastHRupdate = now;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      taskYIELD();
    }

    // Déconnecté
    cli.stop();
    if (xSemaphoreTake(minerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      localMiner.connected = false;
      localMiner.authorized = false;
      localMiner.hashrate_mhs = 0;
      xSemaphoreGive(minerMutex);
    }
    Serial.println("[mine] Déconnecté, retry 10s");
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
