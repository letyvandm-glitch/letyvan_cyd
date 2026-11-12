#!/usr/bin/env python3
"""
CYD Monitor — Agent système (Windows / Linux Debian)
Expose les métriques PC via HTTP sur :8765/stats
L'ESP32 CYD poll cet endpoint toutes les 5s.
"""
import sys, os, time, platform, subprocess, json, threading, socket, concurrent.futures
from http.server import HTTPServer, BaseHTTPRequestHandler

try:
    import psutil
except ImportError:
    sys.exit("psutil manquant — pip install psutil")

IS_LINUX   = sys.platform.startswith("linux")
IS_WINDOWS = sys.platform == "win32"

SERIAL_BAUD = 115200
HTTP_PORT   = 8765             # gardé pour debug/diagnostic

# ── Températures ─────────────────────────────────────────────────

def cpu_temp_linux():
    """Lire la température CPU depuis /sys/class/thermal ou lm-sensors."""
    # Méthode 1 : psutil (si lm-sensors installé)
    try:
        temps = psutil.sensors_temperatures()
        for key in ("coretemp", "k10temp", "acpitz", "cpu_thermal"):
            if key in temps:
                entries = temps[key]
                if entries:
                    return max(e.current for e in entries)
    except Exception:
        pass

    # Méthode 2 : /sys/class/thermal
    try:
        base = "/sys/class/thermal"
        best = None
        for entry in os.listdir(base):
            if not entry.startswith("thermal_zone"):
                continue
            try:
                ttype = open(os.path.join(base, entry, "type")).read().strip()
                if any(k in ttype.lower() for k in ("cpu", "x86", "pkg", "core")):
                    raw = int(open(os.path.join(base, entry, "temp")).read().strip())
                    val = raw / 1000.0
                    if best is None or val > best:
                        best = val
            except Exception:
                pass
        if best is not None:
            return best
    except Exception:
        pass

    return -1.0


def cpu_temp_windows():
    try:
        import wmi
        w = wmi.WMI(namespace="root\\OpenHardwareMonitor")
        for s in w.Sensor():
            if "cpu" in s.Name.lower() and s.SensorType == "Temperature":
                return float(s.Value)
    except Exception:
        pass
    return -1.0


def cpu_temp():
    if IS_LINUX:   return cpu_temp_linux()
    if IS_WINDOWS: return cpu_temp_windows()
    return -1.0


# ── GPU ───────────────────────────────────────────────────────────

def gpu_stats_nvidia():
    """nvidia-smi — fonctionne Linux et Windows."""
    try:
        out = subprocess.check_output(
            ["nvidia-smi",
             "--query-gpu=utilization.gpu,temperature.gpu,memory.used,memory.total,name",
             "--format=csv,noheader,nounits"],
            timeout=3, stderr=subprocess.DEVNULL
        ).decode().strip().split(",")
        if len(out) >= 5:
            return {
                "gpu":       float(out[0]),
                "gpu_temp":  float(out[1]),
                "vram_used": float(out[2]),
                "vram_total":float(out[3]),
                "vram_pct":  float(out[2]) / max(float(out[3]), 1) * 100,
                "gpu_name":  out[4].strip(),
            }
    except Exception:
        pass
    return None


def gpu_stats_pynvml():
    """pynvml — plus rapide que nvidia-smi en subprocess."""
    try:
        import pynvml
        pynvml.nvmlInit()
        h = pynvml.nvmlDeviceGetHandleByIndex(0)
        util  = pynvml.nvmlDeviceGetUtilizationRates(h)
        temp  = pynvml.nvmlDeviceGetTemperature(h, pynvml.NVML_TEMPERATURE_GPU)
        mem   = pynvml.nvmlDeviceGetMemoryInfo(h)
        name  = pynvml.nvmlDeviceGetName(h)
        if isinstance(name, bytes):
            name = name.decode()
        return {
            "gpu":       float(util.gpu),
            "gpu_temp":  float(temp),
            "vram_used": mem.used / 1024 / 1024,
            "vram_total":mem.total / 1024 / 1024,
            "vram_pct":  mem.used / max(mem.total, 1) * 100,
            "gpu_name":  name,
        }
    except Exception:
        pass
    return None


def gpu_stats_amd_linux():
    """GPU AMD via /sys/class/drm/card*/device/"""
    try:
        for drm in os.listdir("/sys/class/drm"):
            if not drm.startswith("card") or "-" in drm:
                continue
            base = f"/sys/class/drm/{drm}/device"
            upath = f"{base}/gpu_busy_percent"
            tpath = f"{base}/hwmon/hwmon0/temp1_input"
            mused = f"{base}/mem_info_vram_used"
            mtot  = f"{base}/mem_info_vram_total"
            if not os.path.exists(upath):
                continue
            gpu = float(open(upath).read().strip())
            temp = -1.0
            vu = vt = 0.0
            try: temp = float(open(tpath).read().strip()) / 1000.0
            except Exception: pass
            try:
                vu = float(open(mused).read().strip()) / 1024 / 1024
                vt = float(open(mtot).read().strip()) / 1024 / 1024
            except Exception: pass
            return {
                "gpu": gpu, "gpu_temp": temp,
                "vram_used": vu, "vram_total": vt,
                "vram_pct": vu / max(vt, 1) * 100,
                "gpu_name": "AMD GPU",
            }
    except Exception:
        pass
    return None


def gpu_stats():
    return (gpu_stats_pynvml()
         or gpu_stats_nvidia()
         or (gpu_stats_amd_linux() if IS_LINUX else None)
         or {"gpu": 0, "gpu_temp": -1, "vram_used": 0, "vram_total": 0, "vram_pct": 0, "gpu_name": "—"})


# ── Réseau (delta KB/s) ───────────────────────────────────────────

_net_prev = None
_net_ts   = None

def net_kbps():
    global _net_prev, _net_ts
    cur = psutil.net_io_counters()
    now = time.time()
    if _net_prev is None:
        _net_prev, _net_ts = cur, now
        return 0.0, 0.0
    dt = max(now - _net_ts, 0.001)
    up = (cur.bytes_sent - _net_prev.bytes_sent) / dt / 1024
    dn = (cur.bytes_recv - _net_prev.bytes_recv) / dt / 1024
    _net_prev, _net_ts = cur, now
    return max(up, 0.0), max(dn, 0.0)


# ── Disque ────────────────────────────────────────────────────────

def disk_label(mount):
    """Nom court pour affichage SD card"""
    if mount == '/':
        return '/'
    parts = [p for p in mount.split('/') if p]
    return parts[-1][:8] if parts else mount[-8:]

def disk_stats():
    result = {"disk_pct": 0, "disk_used": 0, "disk_total": 0,
              "disk1_mount": "",
              "disk2_pct": 0, "disk2_used": 0, "disk2_total": 0, "disk2_mount": "",
              "disks": []}
    try:
        # Collecte toutes les partitions physiques (exclut tmpfs, devtmpfs, etc.)
        skip_fs = {"tmpfs", "devtmpfs", "sysfs", "proc", "cgroup", "squashfs",
                   "overlay", "nsfs", "tracefs", "debugfs", "bpf", "securityfs"}
        mounts = []
        for p in psutil.disk_partitions(all=False):
            if p.fstype in skip_fs:
                continue
            if any(p.mountpoint.startswith(x) for x in
                   ["/sys", "/proc", "/run", "/dev", "/snap", "/boot/efi"]):
                continue
            try:
                u = psutil.disk_usage(p.mountpoint)
                mounts.append((p.mountpoint, u))
            except Exception:
                pass
        # Trier par taille décroissante
        mounts.sort(key=lambda x: x[1].total, reverse=True)
        if mounts:
            m, u = mounts[0]
            result["disk_pct"]   = round(u.percent, 1)
            result["disk_used"]  = round(u.used  / 1e9, 1)
            result["disk_total"] = round(u.total / 1e9, 1)
            result["disk1_mount"] = disk_label(m)
        if len(mounts) >= 2:
            m2, u2 = mounts[1]
            result["disk2_pct"]   = round(u2.percent, 1)
            result["disk2_used"]  = round(u2.used  / 1e9, 1)
            result["disk2_total"] = round(u2.total / 1e9, 1)
            result["disk2_mount"] = m2[-12:]  # 12 chars max
        # Tableau complet pour auto-detection USB
        result["disks"] = [
            {
                "mount": disk_label(m),
                "pct":   round(u.percent, 1),
                "used":  round(u.used  / 1e9, 3) if u.used  < 1e9 else round(u.used  / 1e9, 1),
                "total": round(u.total / 1e9, 3) if u.total < 1e9 else round(u.total / 1e9, 1),
            }
            for m, u in mounts
        ]
    except Exception:
        pass
    return result


# ── CPU info ──────────────────────────────────────────────────────

def cpu_name():
    if IS_LINUX:
        try:
            for line in open("/proc/cpuinfo"):
                if "model name" in line:
                    return line.split(":", 1)[1].strip()[:47]
        except Exception:
            pass
    if IS_WINDOWS:
        try:
            import winreg
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                               r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
            return winreg.QueryValueEx(k, "ProcessorNameString")[0].strip()[:47]
        except Exception:
            pass
    return platform.processor()[:47] or "CPU"


_cpu_name_cache = None

# ── Collecte principale ───────────────────────────────────────────

_latest = {}
_lock   = threading.Lock()


def collect_loop():
    global _cpu_name_cache
    if _cpu_name_cache is None:
        _cpu_name_cache = cpu_name()

    while True:
        try:
            cpu_pct = psutil.cpu_percent(interval=1)
            mem     = psutil.virtual_memory()
            gpu     = gpu_stats()
            disk    = disk_stats()
            net_u, net_d = net_kbps()
            ct      = cpu_temp()

            stats = {
                "cpu":       round(cpu_pct, 1),
                "cpu_temp":  round(ct, 1) if ct >= 0 else None,
                "cpu_name":  _cpu_name_cache,
                "gpu":       round(gpu["gpu"], 1),
                "gpu_temp":  round(gpu["gpu_temp"], 1) if gpu["gpu_temp"] >= 0 else None,
                "gpu_name":  gpu["gpu_name"],
                "ram_pct":   round(mem.percent, 1),
                "ram_used":  round(mem.used / 1e9, 2),
                "ram_total": round(mem.total / 1e9, 1),
                "vram_pct":  round(gpu["vram_pct"], 1),
                "vram_used": round(gpu["vram_used"], 0),
                "vram_total":round(gpu["vram_total"], 0),
                **disk,
                "os_type":  "linux" if IS_LINUX else "windows",
                "net_up":   round(net_u, 1),
                "net_down": round(net_d, 1),
            }
            # Swap Linux
            if IS_LINUX:
                try:
                    swp = psutil.swap_memory()
                    stats["swap_pct"]   = round(swp.percent, 1)
                    stats["swap_used"]  = round(swp.used  / 1e9, 2)
                    stats["swap_total"] = round(swp.total / 1e9, 1)
                except Exception:
                    pass
            with _lock:
                _latest.update(stats)
        except Exception as e:
            print(f"[collect] {e}")
        time.sleep(3)



# ── Scan LAN ─────────────────────────────────────────────────────
_lan_cache = []
_lan_lock  = threading.Lock()

def _is_wifi_iface(iface):
    return os.path.isdir(f"/sys/class/net/{iface}/wireless")

def _ping_one(ip):
    try:
        subprocess.run(["ping", "-c", "1", "-W", "0.3", "-q", ip],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1)
    except Exception:
        pass

def _scan_lan():
    ips = [f"192.168.1.{i}" for i in range(1, 255)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=50) as ex:
        list(ex.map(_ping_one, ips))
    devices = []
    seen = set()
    if IS_LINUX:
        try:
            out = subprocess.check_output(["ip", "neigh"], timeout=5, text=True)
            for line in out.splitlines():
                parts = line.split()
                if len(parts) < 4:
                    continue
                ip = parts[0]
                state = parts[-1]
                if state == "FAILED" or ip in seen:
                    continue
                iface = parts[2] if len(parts) > 2 else ""
                wifi = _is_wifi_iface(iface)
                seen.add(ip)
                hostname = ""
                try:
                    hostname = socket.gethostbyaddr(ip)[0].split(".")[0]
                except Exception:
                    pass
                devices.append({
                    "ip":   ip,
                    "name": hostname[:22],
                    "wifi": wifi,
                    "ok":   state in ("REACHABLE", "DELAY", "PROBE"),
                })
        except Exception as e:
            print(f"[LAN] {e}")
    try:
        devices.sort(key=lambda d: [int(x) for x in d["ip"].split(".")])
    except Exception:
        pass
    return devices

def _lan_scan_loop():
    global _lan_cache
    while True:
        try:
            devs = _scan_lan()
            with _lan_lock:
                _lan_cache = devs
            print(f"[LAN] {len(devs)} appareils")
        except Exception as e:
            print(f"[LAN] erreur: {e}")
        time.sleep(60)

# ── Serveur HTTP ──────────────────────────────────────────────────


def _get_lan_devices():
    import socket as _sock
    devices = {}
    try:
        with open("/proc/net/arp") as f:
            for line in f.readlines()[1:]:
                parts = line.split()
                if len(parts) < 6: continue
                ip, flags, dev = parts[0], parts[2], parts[5]
                if flags == "0x0": continue
                wifi = dev.startswith("wl")
                try: name = _sock.gethostbyaddr(ip)[0].split(".")[0]
                except: name = "-"
                if ip not in devices:
                    devices[ip] = {"ip": ip, "name": name, "wifi": wifi, "ok": True}
    except Exception: pass
    return sorted(devices.values(), key=lambda d: list(map(int, d["ip"].split("."))))

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass   # silencieux

    def do_GET(self):
        if self.path == "/network":
            body = json.dumps(_get_lan_devices()).encode()
        elif self.path == "/stats":
            with _lock:
                data = dict(_latest)
            body = json.dumps(data).encode()
        else:
            self.send_response(404); self.end_headers(); return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)


def _serial_worker(port):
    """Thread par port CYD — reconnexion automatique."""
    import serial as _serial
    while True:
        try:
            with _serial.Serial(port, SERIAL_BAUD, timeout=1) as ser:
                print(f"[serial] Connecté sur {port}")
                while True:
                    with _lock:
                        data = dict(_latest)
                    if data:
                        ser.write((json.dumps(data) + "\n").encode())
                    time.sleep(3)
        except Exception as e:
            print(f"[serial] {port}: {e} — retry dans 5s")
            time.sleep(5)


_serial_started: set = set()
_serial_lock = threading.Lock()

def serial_scan_loop():
    """Détecte automatiquement tous les /dev/ttyUSB* et lance un thread par port."""
    import glob
    while True:
        ports = sorted(glob.glob("/dev/ttyUSB*"))
        with _serial_lock:
            for p in ports:
                if p not in _serial_started:
                    t = threading.Thread(target=_serial_worker, args=(p,), daemon=True)
                    t.start()
                    _serial_started.add(p)
                    print(f"[serial] Nouveau port détecté : {p}")
        time.sleep(10)


if __name__ == "__main__":
    print(f"CYD Agent — {platform.system()} {platform.release()}")

    # Collecte des métriques
    t_collect = threading.Thread(target=collect_loop, daemon=True)
    t_collect.start()
    time.sleep(2)  # premier cycle

    # Envoi série vers tous les CYD détectés automatiquement
    t_serial = threading.Thread(target=serial_scan_loop, daemon=True)
    t_lan    = threading.Thread(target=_lan_scan_loop,    daemon=True)
    t_serial.start()
    t_lan.start()

    # Serveur HTTP de diagnostic (facultatif)
    print(f"HTTP diagnostic : http://0.0.0.0:{HTTP_PORT}/stats")
    srv = HTTPServer(("0.0.0.0", HTTP_PORT), Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nArrêt.")
