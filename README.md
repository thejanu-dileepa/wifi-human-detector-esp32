# ESP32 Wi‑Fi CSI Human Movement Detector (RF‑Pose)

<img width="1247" height="701" alt="image" src="https://github.com/user-attachments/assets/b700ef7b-4613-4754-9419-0dfa798e0b0a" />

An experimental "through‑wall" movement detector that extracts Wi‑Fi CSI (Channel State Information) from an ESP32 board and uses simple machine‑learning to detect movement patterns (for example: empty room vs. human walking).

**Important:** This repository provides tools for data collection, visualization, model training, and live inference. It is intended for research and learning—use responsibly and respect privacy and legal constraints in your jurisdiction.

**Highlights**
- **Real‑time CSI extraction & logging** from an ESP32 acting as a Wi‑Fi client.
- **Python tools** to visualize, collect labeled data, and run live inference.
- **Simple ML pipeline** (feature extraction + Random Forest classifier) for activity detection.

**Hardware requirements**
- ESP32 or ESP32‑S3 development board
- A standard Wi‑Fi access point (router)
- USB cable and a host PC for running the Python tools

**Software requirements**
- Python 3.8+ and `pip`
- PlatformIO + VS Code (for building/flashing the ESP32 firmware)

Install Python dependencies:

```bash
pip install -r requirements.txt
```

If `requirements.txt` does not exist, install directly:

```bash
pip install pyserial matplotlib pandas scikit-learn numpy
```

## Project layout

- [src/main.cpp](src/main.cpp) — ESP32 firmware: initializes Wi‑Fi, collects CSI (or stub data), and streams it over serial/UDP.
- [monitor.py](monitor.py) — Live plotting of incoming RSSI/CSI streams for quick inspection.
- [data_collector.py](data_collector.py) — Record labeled waveform segments into `wifi_csi_dataset.csv` for later training.
- [train_model.py](train_model.py) — Train a Random Forest model on the collected dataset and export the trained artifact.
- [live_detector.py](live_detector.py) — Load the trained model and perform real‑time inference on incoming data.
- [platformio.ini](platformio.ini) — PlatformIO project configuration for building/flashing the ESP32 firmware.

## Quick start

1. Flash the ESP32 firmware

   - Open the project in VS Code with the PlatformIO extension and build + upload the firmware in [src/main.cpp](src/main.cpp).

2. Prepare Python environment

```powershell
python -m venv .venv
.\\.venv\\Scripts\\activate
pip install -r requirements.txt
```

3. Visualize live data (for debugging)

```bash
python monitor.py --port COM3
```

4. Collect labeled samples

```bash
python data_collector.py --port COM3 --label walking --duration 30
```

5. Train model

```bash
python train_model.py --input wifi_csi_dataset.csv --output model.pkl
```

6. Run live detection

```bash
python live_detector.py --port COM3 --model model.pkl
```

Adjust `--port` to your serial device (or modify scripts to read from UDP if your firmware sends UDP). See the script help (`-h`) for additional options.

## Data format

The dataset `wifi_csi_dataset.csv` stores time‑series feature vectors and a label column. Use `data_collector.py` to generate new labeled rows consistently.

## Notes & troubleshooting

- If CSI extraction does not work on your particular ESP32 or router, the firmware may fall back to synthetic or RSSI‑only logging — check serial output.
- Use `monitor.py` first to confirm the device is streaming data before collecting training samples.
- If you see permission errors on Windows when opening serial ports, run your terminal as Administrator or adjust port permissions.

## Contributing

Contributions are welcome. Open an issue or submit a PR with improvements, bug fixes, or documentation updates.

## License

This repository does not include a license file. Add one (for example, `MIT`) if you intend to share the project publicly.
