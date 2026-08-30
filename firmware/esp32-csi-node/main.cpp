/**
 * ESP32-S3 Wi-Fi CSI Node — RF-Pose transmitter firmware
 *
 * Connects to the local router as a station, captures Channel State
 * Information (CSI) from every frame the router transmits, and streams it
 * as JSON over UDP to a sink on the LAN (default port 5006, matching the
 * RuView sensing pipeline).
 *
 * Features:
 *  - Serial provisioning (use firmware/esp32-csi-node/provision.py):
 *      PROV:<ssid>|<password>|<target-ip>|<udp-port>|<rate-hz>
 *      PROV:SHOW
 *      PROV:CLEAR
 *    Config is persisted in NVS and survives reboots.
 *  - Node-side presence fallback: ~30 s ambient calibration, then a
 *    subcarrier-amplitude variance z-score decides between
 *    "PRESENCE: IDLE" and "PRESENCE: MOVEMENT" (printed on state change).
 *  - The "Router Signal Received! | RSSI: ... | CSI Data Bytes: ..." serial
 *    line is kept so monitor.py / data_collector.py / live_detector.py work
 *    without changes.
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <mbedtls/base64.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------- Compile-time defaults (used until provisioned) ----------
#define DEFAULT_SSID        "wifi name"
#define DEFAULT_PASSWORD    "ps"
#define DEFAULT_TARGET_IP   ""      // "" -> stream to the router gateway
#define DEFAULT_UDP_PORT    5006    // RuView-compatible CSI sink port
#define DEFAULT_RATE_HZ     10

// Keepalive: 1-byte dummy UDP packets to the router so it keeps sending
// frames back to us — every received frame produces one CSI report.
#define KEEPALIVE_PORT         8080
#define KEEPALIVE_INTERVAL_MS  100

// Node-side presence fallback (spec: ~30 s ambient calibration)
#define CALIBRATION_MS         30000
#define PRESENCE_WINDOW        50     // rolling samples per variance estimate
#define PRESENCE_Z_THRESHOLD   3.0f
#define MAX_VAR_SAMPLES        256

// CSI / JSON buffers (40 MHz HT-LTF can report up to 228 subcarriers = 456 B)
#define CSI_BUF_SIZE   512
#define MAX_SUBCARRIERS 256
#define JSON_BUF_SIZE  2560
#define B64_BUF_SIZE   768

#define PROV_NS         "csi-node"
#define LOCAL_UDP_PORT  5006

// ---------- Persisted node configuration ----------
struct NodeConfig {
  char ssid[33];
  char password[65];
  char target_ip[16];   // "" -> use router gateway
  uint16_t udp_port;
  uint8_t rate_hz;
};

static NodeConfig cfg;
static Preferences prefs;

static WiFiUDP csiUdp;
static WiFiUDP keepaliveUdp;
static IPAddress targetIP;
static IPAddress gatewayIP;

static uint32_t seq = 0;
static uint32_t lastKeepaliveMs = 0;
static uint32_t lastCsiSendMs = 0;
static uint32_t csiStartMs = 0;

// ---------- Node-side presence fallback state ----------
static float ampRing[PRESENCE_WINDOW];
static size_t ringIdx = 0;
static size_t ringCount = 0;
static float varSamples[MAX_VAR_SAMPLES];
static size_t varCount = 0;
static bool calibrated = false;
static bool presenceMovement = false;
static float baselineMean = 0.0f;
static float baselineStd = 1.0f;

// ---------- Scratch buffers (shared, used only inside the CSI callback) ----------
static int8_t csiCopy[CSI_BUF_SIZE];
static uint8_t ampVals[MAX_SUBCARRIERS];
static char jsonBuf[JSON_BUF_SIZE];
static unsigned char b64Buf[B64_BUF_SIZE];

// ---------------------------------------------------------------------------
// Presence fallback: variance z-score over rolling subcarrier mean amplitude
// ---------------------------------------------------------------------------
static void presenceUpdate(float meanAmp, uint32_t now) {
  ampRing[ringIdx] = meanAmp;
  ringIdx = (ringIdx + 1) % PRESENCE_WINDOW;
  if (ringCount < PRESENCE_WINDOW) {
    ringCount++;
    return;
  }

  // Variance of the rolling window of per-packet mean amplitudes
  float sum = 0.0f;
  for (size_t i = 0; i < PRESENCE_WINDOW; i++) sum += ampRing[i];
  float mean = sum / PRESENCE_WINDOW;
  float var = 0.0f;
  for (size_t i = 0; i < PRESENCE_WINDOW; i++) {
    float d = ampRing[i] - mean;
    var += d * d;
  }
  var /= PRESENCE_WINDOW;

  if (!calibrated) {
    if (now - csiStartMs < CALIBRATION_MS && varCount < MAX_VAR_SAMPLES) {
      varSamples[varCount++] = var;
      return;
    }
    if (varCount >= 8) {
      float s = 0.0f;
      for (size_t i = 0; i < varCount; i++) s += varSamples[i];
      baselineMean = s / varCount;
      float sd = 0.0f;
      for (size_t i = 0; i < varCount; i++) {
        float d = varSamples[i] - baselineMean;
        sd += d * d;
      }
      sd = sqrtf(sd / varCount);
      baselineStd = (sd < 1.0f) ? 1.0f : sd;  // epsilon guard
      calibrated = true;
      Serial.printf("PRESENCE: Calibrated (baseline var=%.1f +/- %.1f). Monitoring.\n",
                    baselineMean, baselineStd);
    }
    return;
  }

  float z = (var - baselineMean) / baselineStd;
  bool movement = (z > PRESENCE_Z_THRESHOLD);
  if (movement != presenceMovement) {
    presenceMovement = movement;
    Serial.printf("PRESENCE: %s (z=%.1f)\n", movement ? "MOVEMENT" : "IDLE", z);
  }
}

// ---------------------------------------------------------------------------
// CSI callback: compute subcarrier amplitudes, stream JSON over UDP
// ---------------------------------------------------------------------------
void csi_callback(void *ctx, wifi_csi_info_t *data) {
  wifi_csi_info_t d = data[0];
  if (d.len < 4 || d.len > CSI_BUF_SIZE) return;

  uint32_t now = millis();

  // Copy the report out of the WiFi task buffer before working on it.
  memcpy(csiCopy, d.buf, d.len);
  if (d.first_word_invalid) {
    csiCopy[0] = csiCopy[1] = csiCopy[2] = csiCopy[3] = 0;
  }

  int n = d.len / 2;  // each subcarrier = one I/Q pair of signed bytes
  float sumAmp = 0.0f;
  for (int i = 0; i < n; i++) {
    float iv = csiCopy[2 * i];
    float qv = csiCopy[2 * i + 1];
    float amp = sqrtf(iv * iv + qv * qv);
    ampVals[i] = (uint8_t)lroundf(amp);
    sumAmp += amp;
  }
  float meanAmp = (n > 0) ? sumAmp / n : 0.0f;

  // Presence fallback runs on every received CSI report.
  presenceUpdate(meanAmp, now);

  // Rate-limit the UDP stream to the configured rate.
  if (now - lastCsiSendMs < (uint32_t)(1000 / cfg.rate_hz)) return;
  lastCsiSendMs = now;

  // Build the JSON packet.
  char *p = jsonBuf;
  p += snprintf(p, JSON_BUF_SIZE,
                "{\"type\":\"csi\",\"node\":\"esp32-csi-node\",\"seq\":%lu,"
                "\"ts_ms\":%lu,\"rssi\":%d,\"ch\":%d,\"src_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                "\"n_sc\":%d,\"fw_invalid\":%s,\"amp\":[",
                (unsigned long)seq++, (unsigned long)now,
                (int)d.rx_ctrl.rssi, (int)d.rx_ctrl.channel,
                d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
                n, d.first_word_invalid ? "true" : "false");
  for (int i = 0; i < n && (p - jsonBuf) < (JSON_BUF_SIZE - 32); i++) {
    p += snprintf(p, JSON_BUF_SIZE - (p - jsonBuf), "%u,", ampVals[i]);
  }
  if (*(p - 1) == ',') p--;  // strip trailing comma
  p += snprintf(p, 8, "]");

  // Raw I/Q pairs (base64) for phase-based processing on the sink side.
  size_t olen = 0;
  if (mbedtls_base64_encode(b64Buf, B64_BUF_SIZE, &olen,
                            (const unsigned char *)csiCopy, d.len) == 0) {
    snprintf(p, JSON_BUF_SIZE - (p - jsonBuf), ",\"iq\":\"%s\"}", (char *)b64Buf);
  } else {
    snprintf(p, JSON_BUF_SIZE - (p - jsonBuf), "}");
  }

  if (csiUdp.beginPacket(targetIP, cfg.udp_port) > 0) {
    csiUdp.write((const uint8_t *)jsonBuf, strlen(jsonBuf));
    csiUdp.endPacket();
  }

  // Backward-compatible serial line for the Python tooling.
  Serial.printf("Router Signal Received! | RSSI: %d | CSI Data Bytes: %d\n",
                (int)d.rx_ctrl.rssi, (int)d.len);
}

// ---------------------------------------------------------------------------
// Configuration persistence
// ---------------------------------------------------------------------------
static void loadConfig() {
  memset(&cfg, 0, sizeof(cfg));
  prefs.begin(PROV_NS, true);
  prefs.getString("ssid", cfg.ssid, sizeof(cfg.ssid));
  prefs.getString("pass", cfg.password, sizeof(cfg.password));
  prefs.getString("tip", cfg.target_ip, sizeof(cfg.target_ip));
  cfg.udp_port = prefs.getUShort("tport", DEFAULT_UDP_PORT);
  cfg.rate_hz = prefs.getUChar("rate", DEFAULT_RATE_HZ);
  prefs.end();

  if (cfg.ssid[0] == '\0') {
    strncpy(cfg.ssid, DEFAULT_SSID, sizeof(cfg.ssid) - 1);
    strncpy(cfg.password, DEFAULT_PASSWORD, sizeof(cfg.password) - 1);
  }
  if (cfg.rate_hz < 1 || cfg.rate_hz > 100) cfg.rate_hz = DEFAULT_RATE_HZ;
  if (cfg.udp_port < 1) cfg.udp_port = DEFAULT_UDP_PORT;
}

static void printConfig() {
  Serial.println("\n---- Current node configuration ----");
  Serial.printf("SSID        : %s\n", cfg.ssid);
  Serial.printf("Target IP   : %s\n",
                cfg.target_ip[0] ? cfg.target_ip : "<router gateway>");
  Serial.printf("UDP port    : %u\n", cfg.udp_port);
  Serial.printf("Stream rate : %u Hz\n", cfg.rate_hz);
  Serial.println("------------------------------------");
}

static void saveConfig(const char *ssid, const char *password, const char *ip,
                       uint16_t port, uint8_t rate) {
  prefs.begin(PROV_NS, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  prefs.putString("tip", ip);
  prefs.putUShort("tport", port);
  prefs.putUChar("rate", rate);
  prefs.end();
}

// ---------------------------------------------------------------------------
// Serial provisioning protocol
// ---------------------------------------------------------------------------
static void handleProvisionLine(char *line) {
  char *args = line + 5;  // skip "PROV:"

  if (strcmp(args, "SHOW") == 0) {
    printConfig();
    return;
  }

  if (strcmp(args, "CLEAR") == 0) {
    prefs.begin(PROV_NS, false);
    prefs.clear();
    prefs.end();
    Serial.println("PROV:OK");
    delay(500);
    ESP.restart();
    return;
  }

  // Format: PROV:<ssid>|<password>|<target-ip>|<udp-port>|<rate-hz>
  // '|' cannot appear inside the SSID or the password. Empty fields are
  // preserved (e.g. an empty password for an open network).
  char *fields[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  int count = 0;
  fields[count++] = args;
  for (char *c = args; *c && count < 5; c++) {
    if (*c == '|') {
      *c = '\0';
      fields[count++] = c + 1;
    }
  }
  // Anything past the 5th field is ignored.

  if (!fields[0][0]) {
    Serial.println("PROV:ERR:expected PROV:<ssid>|<password>|<target-ip>|<udp-port>|<rate-hz>");
    return;
  }

  IPAddress parsed;
  const char *ip = (count >= 3 && fields[2][0]) ? fields[2] : "";
  if (ip[0] && !parsed.fromString(ip)) {
    Serial.println("PROV:ERR:invalid target IP address");
    return;
  }

  uint16_t port = DEFAULT_UDP_PORT;
  if (count >= 4 && fields[3][0]) {
    long v = atol(fields[3]);
    if (v < 1 || v > 65535) {
      Serial.println("PROV:ERR:invalid UDP port");
      return;
    }
    port = (uint16_t)v;
  }

  uint8_t rate = DEFAULT_RATE_HZ;
  if (count == 5 && fields[4][0]) {
    long v = atol(fields[4]);
    if (v < 1 || v > 100) {
      Serial.println("PROV:ERR:invalid rate (1-100 Hz)");
      return;
    }
    rate = (uint8_t)v;
  }

  saveConfig(fields[0], fields[1], ip, port, rate);
  Serial.println("PROV:OK");
  delay(300);
  ESP.restart();
}

static void handleSerial() {
  static char line[320];
  static size_t idx = 0;

  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[idx] = '\0';
      idx = 0;
      if (strncmp(line, "PROV:", 5) == 0) handleProvisionLine(line);
      continue;
    }
    if (idx < sizeof(line) - 1) line[idx++] = c;
  }
}

// ---------------------------------------------------------------------------
// Arduino lifecycle
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n--- ESP32-S3 Wi-Fi CSI Node ---");
  loadConfig();
  printConfig();

  Serial.print("Connecting to home router...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nSuccessfully connected to the router!");

  gatewayIP = WiFi.gatewayIP();
  if (cfg.target_ip[0] && targetIP.fromString(cfg.target_ip)) {
    Serial.print("CSI sink   : ");
    Serial.println(targetIP);
  } else {
    targetIP = gatewayIP;
    Serial.print("CSI sink   : router gateway ");
    Serial.println(targetIP);
  }
  Serial.printf("CSI sink UDP port: %u @ %u Hz\n", cfg.udp_port, cfg.rate_hz);

  // Turn OFF Wi-Fi power saving to capture raw signals continuously.
  esp_wifi_set_ps(WIFI_PS_NONE);

  wifi_csi_config_t csi_config = {};
  csi_config.lltf_en = true;
  csi_config.htltf_en = true;
  csi_config.stbc_htltf2_en = true;
  csi_config.ltf_merge_en = true;
  csi_config.channel_filter_en = true;
  csi_config.manu_scale = false;
  csi_config.shift = 0;

  esp_wifi_set_csi_config(&csi_config);
  esp_wifi_set_csi_rx_cb(&csi_callback, NULL);
  esp_wifi_set_csi(true);

  csiUdp.begin(LOCAL_UDP_PORT);
  keepaliveUdp.begin(LOCAL_UDP_PORT + 1);
  csiStartMs = millis();

  Serial.println("CSI tracking active! Streaming to sink...");
  Serial.printf("PRESENCE: Calibrating for %d s - keep the room empty.\n", CALIBRATION_MS);
}

void loop() {
  handleSerial();

  uint32_t now = millis();

  // Dummy traffic toward the router keeps frames (and CSI) flowing.
  if (now - lastKeepaliveMs >= KEEPALIVE_INTERVAL_MS) {
    keepaliveUdp.beginPacket(gatewayIP, KEEPALIVE_PORT);
    keepaliveUdp.write('X');
    keepaliveUdp.endPacket();
    lastKeepaliveMs = now;
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }

  delay(10);
}
