# ESP32 Wi-Fi CSI Human Movement Detector (RF-Pose)

<img width="1247" height="701" alt="image" src="https://github.com/user-attachments/assets/b700ef7b-4613-4754-9419-0dfa798e0b0a" />

**Turn ordinary WiFi into a presence sensor.** Every WiFi router already fills your space with radio waves — when people move or breathe, they disturb those waves in measurable ways. This project captures those disturbances using **Channel State Information (CSI)** from a low-cost **ESP32-S3** and turns them into actionable data: who's there, what they're doing, and (experimentally) how they're breathing. No cameras, no wearables.

The firmware implements a **RuView-compatible CSI node**: it streams CSI as JSON over UDP to a sink on your LAN (default port **5006**), is provisioned over serial with `provision.py`, and runs a node-side presence fallback so it works even without the PC-side tooling.

## 🚀 What it senses

| | Capability | How | Output |
|---|------------|-----|--------|
| 👤 | **Presence / occupancy** | Subcarrier-amplitude variance z-score with ~30 s ambient calibration, on-device | `PRESENCE: MOVEMENT / IDLE` over serial, < 1 s latency |
| 🚶 | **Activity recognition** | Random Forest classifier over 30-sample RSSI windows (PC-side ML) | `Empty_Room`, `Human_Walking`, or any label you record |
| 📡 | **Raw CSI streaming** | Full subcarrier I/Q + amplitude per received frame, JSON over UDP :5006 | Any sink can consume it (e.g. `vitals.py`, your own scripts) |
| 🫁 | **Vital signs** *(experimental)* | Band-limited periodogram on the CSI stream: breathing 0.1–0.5 Hz → 6–30 BPM, heart 0.8–2.0 Hz → 48–120 BPM | `vitals.py` console output |

## 🛠️ Hardware Requirements

| Option | Hardware | Cost | Full CSI | Notes |
|--------|----------|------|----------|-------|
| **ESP32-S3 node** (recommended) | ESP32-S3 DevKitC + 2.4 GHz router | ~$9 | Yes | Dual-core, sufficient for continuous CSI DSP |
| ESP32-C3 / original ESP32 | — | — | — | **Not supported** — single-core, insufficient for CSI DSP |
| Extra node (optional) | 2nd ESP32-S3 | +$9 | Yes | A single node has limited spatial resolution; 2+ nodes improve coverage |

Also required: a PC/laptop for serial provisioning, data collection, and ML; a Micro-USB / USB-C data cable; and a standard home WiFi router (channels 1/6/11 are ideal if you deploy multiple nodes).

## 🔬 How It Works

```
WiFi Router → radio waves pass through room → hit human body → scatter
    ↓
ESP32-S3 node (station mode) receives router frames → CSI report per frame
(56–114 subcarriers, lltf + htltf + STBC, channel filter on, power-save OFF)
    ↓
Node-side: amplitude-variance presence fallback (~30 s calibration)
Node-side: JSON CSI packets → UDP stream → sink PC :5006
    ↓
PC tooling:
  monitor.py        → live RSSI waveform
  data_collector.py → labeled CSV dataset (Empty_Room / Human_Walking / ...)
  train_model.py    → Random Forest model (wifi_model.pkl)
  live_detector.py  → real-time activity prediction
  vitals.py         → (experimental) breathing + heart rate from CSI
```

The router keeps transmitting because the node sends 1-byte "keepalive" UDP packets to the gateway 10× per second — every frame the router sends back produces one CSI report.

## 💻 Software & Libraries

* **IDE:** VS Code + **PlatformIO** extension
* **C++ Framework:** Arduino for ESP32 (`esp_wifi.h`, `WiFi.h`, `Preferences.h`)
* **Python 3.x**

```bash
pip install pyserial matplotlib pandas scikit-learn numpy
```

## 🚀 Quick Start

### 1. Build & flash the node

With PlatformIO:

```bash
pio run -t upload                # build + flash over the attached COM port
```

Or flash the built binaries with esptool directly:

```bash
python -m esptool --chip esp32s3 --port COM9 --baud 460800 write_flash \
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 firmware.bin
# (binaries are in .pio/build/esp32-s3-devkitc-1/ after a build)
```

### 2. Provision WiFi credentials and the sink IP

```bash
python firmware/esp32-csi-node/provision.py --port COM9 \
  --ssid "YourWiFi" --password "secret" --target-ip 192.168.1.20
```

* `--target-ip` is the PC that runs the sink tools. Omit it to stream CSI to the router's gateway.
* `--udp-port 5006` (default) and `--rate-hz 10` (default) tune the stream.
* `--show` prints the stored configuration; `--clear` erases it.

Settings are persisted in NVS and survive reboots. Until provisioned, the firmware falls back to the SSID/password compiled into `firmware/esp32-csi-node/main.cpp` (`DEFAULT_SSID` / `DEFAULT_PASSWORD`).

### 3. Watch it work

Open the PlatformIO Serial Monitor (115200 baud) to see:

```
Router Signal Received! | RSSI: -56 | CSI Data Bytes: 228
PRESENCE: Calibrating for 30000 ms - keep the room empty.
PRESENCE: Calibrated (baseline var=12.3 +/- 1.8). Monitoring.
PRESENCE: MOVEMENT (z=4.2)
PRESENCE: IDLE (z=0.6)
```

### 4. Run the PC-side tools

| Script | Purpose |
|--------|---------|
| `python monitor.py` | Live RSSI waveform (Matplotlib) |
| `python data_collector.py` | Record labeled rows into `wifi_csi_dataset.csv` — change `CURRENT_LABEL` between recordings (e.g. `Empty_Room`, `Human_Walking`, `Pet_Walking`) |
| `python train_model.py` | Train the Random Forest classifier → `wifi_model.pkl` |
| `python live_detector.py` | Real-time activity prediction from the serial stream |
| `python vitals.py` | Listen on UDP :5006 and print (experimental) breathing/heart-rate estimates |

All serial tools default to `COM8` — edit the `SERIAL_PORT` constant at the top of each script.

## 📡 CSI UDP protocol (port 5006)

The node streams one JSON object per CSI report (rate-limited to the configured `rate-hz`):

```json
{
  "type": "csi",
  "node": "esp32-csi-node",
  "seq": 42,
  "ts_ms": 123456,
  "rssi": -56,
  "ch": 6,
  "src_mac": "aa:bb:cc:dd:ee:ff",
  "n_sc": 114,
  "fw_invalid": false,
  "amp": [42, 37, ...],
  "iq": "base64-encoded signed I/Q pairs"
}
```

* `amp` — per-subcarrier amplitude, `sqrt(I² + Q²)`, one entry per subcarrier (`n_sc` entries; 20 MHz HT-LTF reports ≈ 114 subcarriers, legacy LTF ≈ 56).
* `iq` — the raw driver buffer, base64: pairs of signed bytes `(I, Q)` per subcarrier, for phase-based processing.
* `fw_invalid` — when `true`, the first four bytes of `iq` were invalid on this chip (already zeroed in `amp`).

Write your own sink in a few lines: bind UDP port 5006, `json.loads()` each datagram, skip packets where `type != "csi"`.

## 📂 Project Structure

```text
WiFi_CSI_Transmitter/
│
├── firmware/
│   └── esp32-csi-node/
│       ├── main.cpp             # ESP32-S3 CSI node: CSI capture, UDP JSON stream,
│       │                        #   NVS config, serial provisioning, presence fallback
│       └── provision.py         # WiFi credentials + sink IP provisioning over serial
├── monitor.py                   # Real-time RSSI waveform graphing
├── data_collector.py            # Labeled data recording into CSV
├── train_model.py               # Random Forest training
├── live_detector.py             # Real-time activity prediction
├── vitals.py                    # Experimental breathing/heart-rate sink on UDP :5006
├── wifi_csi_dataset.csv         # Recorded dataset
├── wifi_model.pkl               # Trained model
└── platformio.ini               # PlatformIO configuration (src_dir = firmware/esp32-csi-node)
```

## 🚧 Limitations (beta software)

> - A **single node has limited spatial resolution** — deploy 2+ nodes (ideally on channels 1/6/11) for better room coverage.
> - The current ML classifier uses **RSSI windows only**; the full subcarrier CSI is streamed to UDP :5006 for richer processing but not yet used by `train_model.py`.
> - **Vital signs are experimental** and not validated — quality degrades with movement, distance, and multipath changes.
>
> **Safety boundary:** this is a research/prototype project, not a medical device, emergency system, or safety-certified control. Any vital-sign or occupancy output requires independent validation on the exact hardware, room, and subjects before operational use.

## 🤝 Contributing

Bug reports and data-collection findings are welcome — open an issue with your hardware setup, router model, and observed behavior.
