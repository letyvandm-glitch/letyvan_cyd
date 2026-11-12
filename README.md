# letyvan_cyd

Firmware ESP32 CYD (ESP32-2432S028R) pour le monitoring réseau/mining/PC de letyvan.

## Appareils

| Appareil | IP | Firmware | Description |
|----------|-----|----------|-------------|
| CYD-1 | 192.168.1.10 | `cyd_monitor_mining` | Monitoring PC (3 colonnes : TEMP/CPU/GPU, RAM, DISK, VRAM/SWAP) |
| CYD-2 | 192.168.1.11 | `cyd_hybrid` | Monitoring PC + Mining (NMMiners + Bitaxe) + Météo |

## Structure

```
cyd_monitor_mining/   — Firmware CYD-1 (PC monitor)
  cyd_monitor_mining.ino
  setup_ui.h          — Configuration WiFi/OTA via captive portal
  stratum_task.ino    — Tâche mining Stratum (optionnel)
  web_ui.h            — Interface web de configuration
  wifi_setup.ino      — Setup WiFi initial
  hybrid_config.h     — Defines de configuration
  qr_screen.h         — Affichage QR code

cyd_hybrid/           — Firmware CYD-2 (hybrid : PC + Mining + Météo)
  cyd_hybrid.ino

pc_agent.py           — Agent Python à lancer sur les PCs monitorés (port 5050)
```

## OTA Flash

Mot de passe OTA : `cyd1234`

Depuis Windows :
```bash
python espota.py -i 192.168.1.10 -f cyd_monitor_mining.ino.bin -p 3232 -a cyd1234
python espota.py -i 192.168.1.11 -f cyd_hybrid.ino.bin -p 3232 -a cyd1234
```

Depuis le mini-i5 (arduino-cli) :
```bash
~/bin/arduino-cli compile --fqbn esp32:esp32:esp32wrover cyd_monitor_mining/ --output-dir /tmp/cyd1_build/
python3 ~/.arduino15/packages/esp32/hardware/esp32/3.3.12/tools/espota.py -i 192.168.1.10 -f /tmp/cyd1_build/cyd_monitor_mining.ino.bin -p 3232 -a cyd1234
```

## Dépendances Arduino

- TFT_eSPI 2.5.43
- ArduinoJson 7.4.3
- XPT2046_Touchscreen 1.4
- ArduinoOTA (inclus esp32)
- HTTPClient (inclus esp32)

## `pc_agent.py`

Agent FastAPI (port 5050) à lancer sur les PCs Linux/Windows monitorés.
Expose : CPU%, RAM, DISK, GPU (nvidia-smi), réseau, swap (Linux), infos système.

```bash
pip install fastapi uvicorn psutil
python pc_agent.py
```
