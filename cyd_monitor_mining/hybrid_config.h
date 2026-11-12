#pragma once
// ================================================================
// hybrid_config.h — Déclarations partagées cyd_hybrid + stratum_task
// ================================================================

// ── Config mining ───────────────────────────────────────────────
#define STRATUM_HOST   "solo.ckpool.org"
#define STRATUM_PORT   3333
#define WORKER_NAME    "bc1qh62leyz8mwexdg5gf7amc7rzwf3tayfs80yggy.cyd1"
#define WORKER_PASS    "x"
#define DEVICE_HW      "Monitor"
#define DEVICE_FW_VER  "hybrid-1.0"

// ── Stats mining partagées ───────────────────────────────────────
struct LocalMinerStats {
  bool     connected   = false;
  bool     authorized  = false;
  float    hashrate_mhs = 0;
  uint32_t shares_ok   = 0;
  uint32_t shares_ko   = 0;
  char     best_diff[24] = "—";
  char     pool[48]    = STRATUM_HOST;
  unsigned long up_start = 0;
  unsigned long hashes   = 0;
};

// ── Globales définies dans stratum_task.ino ─────────────────────
extern LocalMinerStats localMiner;
extern SemaphoreHandle_t minerMutex;

// ── Prototype tâche mining ───────────────────────────────────────
void vTaskMine(void* pvParams);
