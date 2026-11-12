#include "setup_ui.h"
#include "qr_screen.h"
// wifi_setup.ino - Captive portal premier demarrage Letyvan
// AP: "Letyvan-Setup" | 192.168.4.1
// Flux: NVS vide -> AP setup -> sauvegarde NVS -> reboot -> STA normal

#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoJson.h>


static Preferences prefs;
bool g_setupMode = false;
extern DNSServer dnsServer;

// Lire credentials depuis NVS
bool loadWifiCredentials(String &ssid, String &pass, String &pcip) {
  prefs.begin("letyvan", true);  // read-only
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  pcip = prefs.getString("pcip", "");
  prefs.end();
  return ssid.length() > 0;
}

// Sauvegarder credentials dans NVS
void saveWifiCredentials(const String &ssid, const String &pass, const String &pcip) {
  prefs.begin("letyvan", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("pcip", pcip);
  prefs.end();
}

// Effacer credentials (reset factory)
void clearWifiCredentials() {
  prefs.begin("letyvan", false);
  prefs.clear();
  prefs.end();
}

// Handler: scan WiFi -> JSON
void handleWifiScan() {
  int n = WiFi.scanNetworks(false, false, false, 300);
  String json = "[";
  for (int i = 0; i < n && i < 20; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\","
            "\"rssi\":" + String(WiFi.RSSI(i)) + ","
            "\"secure\":" + (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]";
  WiFi.scanDelete();
  webServer.send(200, "application/json", json);
}

// Handler: connexion -> sauvegarde NVS -> reboot
void handleWifiConnect() {
  if (!webServer.hasArg("plain")) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"No body\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, webServer.arg("plain"))) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"JSON invalid\"}");
    return;
  }
  String ssid = doc["ssid"] | "";
  String pass = doc["password"] | "";
  String pcip = doc["pc_ip"] | "";

  if (ssid.isEmpty()) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"SSID vide\"}");
    return;
  }

  // Sauvegarde immediate + reboot (si mdp faux, l'ESP revient en setup au reboot)
  saveWifiCredentials(ssid, pass, pcip);
  webServer.send(200, "application/json", "{\"ok\":true}");
  webServer.client().flush();
  delay(1500);
  ESP.restart();
}

// Captive portal: rediriger toutes les requetes inconnues vers /
// Captive portal handlers
void handleGenerate204() {
  // Android connectivity check -> 302 triggers captive portal notification
  webServer.sendHeader("Location", "http://192.168.4.1/", true);
  webServer.send(302, "text/plain", "");
}
void handleHotspot() {
  // iOS captive portal check
  webServer.sendHeader("Location", "http://192.168.4.1/", true);
  webServer.send(302, "text/plain", "");
}
void handleCaptivePortal() {
  webServer.sendHeader("Location", "http://192.168.4.1/", true);
  webServer.send(302, "text/plain", "");
}

// Demarrer le mode AP setup
void startSetupMode() {
  g_setupMode = true;
  Serial.println("[SETUP] Demarrage AP Letyvan-Setup");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Letyvan-Setup");
  delay(500);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("[SETUP] AP IP: ");
  Serial.println(apIP);
  // DNS: toutes requetes -> 192.168.4.1 (captive portal detection Android/iOS)
  dnsServer.start(53, "*", apIP);
  // Routes
  webServer.on("/", HTTP_GET, handleSetupRoot);
  webServer.on("/api/scan", HTTP_GET, handleWifiScan);
  webServer.on("/api/connect", HTTP_POST, handleWifiConnect);
  webServer.on("/generate_204", HTTP_GET, handleGenerate204);
  webServer.on("/hotspot-detect.html", HTTP_GET, handleHotspot);
  webServer.on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
  webServer.onNotFound(handleCaptivePortal);
  webServer.begin();
}

// Affichage TFT en mode setup (ecran de config)
void drawSetupScreen() {
  extern TFT_eSPI tft;
  tft.fillScreen(COL_BG);
  // Titre
  tft.setTextColor(0x07FF); tft.setTextSize(1);
  tft.setCursor(10, 6); tft.print("LETYVAN - PREMIER DEMARRAGE");
  tft.drawFastHLine(0, 18, 320, 0x07FF);
  // QR WiFi centre: 29x29 @ scale=6 -> 174x174, cx=160, cy=116
  drawQRBitmap(QR_WIFI_DATA, QR_WIFI_SIZE, QR_WIFI_BYTES_ROW, 160, 116, 6);
  // Texte bas
  tft.setTextColor(0x07FF); tft.setTextSize(1);
  tft.setCursor(30, 210); tft.print("Scannez pour rejoindre le WiFi");
  tft.setTextColor(COL_TEXT);
  tft.setCursor(55, 224); tft.print("Letyvan-Setup");
  tft.setTextColor(0xF81F);
  tft.setCursor(72, 234); tft.print("192.168.4.1");
}
// Handler page setup (HTML dans setup_ui.h)
void handleSetupRoot() {
  webServer.send_P(200, "text/html", (const char*)SETUP_HTML, SETUP_HTML_LEN);
}

// Handler page monitoring (HTML dans web_ui.h)
void handleRoot() {
  webServer.sendHeader("Content-Encoding", "gzip");
  webServer.sendHeader("Cache-Control", "no-cache");
  webServer.send_P(200, "text/html", (const char*)NM_INDEX_HTML_GZ, NM_INDEX_HTML_GZ_LEN);
}
