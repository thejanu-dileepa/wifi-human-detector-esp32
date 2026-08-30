#!/usr/bin/env python3
"""
Contactless vital-signs monitor (experimental) — RuView-style CSI sink.

Listens on UDP port 5006 for the CSI JSON packets streamed by the ESP32-S3
node and estimates breathing rate and heart rate from the subcarrier
amplitude signal, following the sensing-pipeline spec:

    Breathing  : band 0.1-0.5 Hz  ->  6-30 BPM
    Heart rate : band 0.8-2.0 Hz  ->  48-120 BPM

It keeps a rolling window of the mean subcarrier amplitude, resamples it to
a fixed 20 Hz grid, then locates the dominant peak inside each band using an
FFT periodogram. A movement warning is raised when the overall signal
fluctuation is high — movement corrupts vital-sign estimation.

This is an experimental, research-grade feature: it is NOT a medical device,
and readings require independent validation before any operational use.

Usage:
    python vitals.py                 # listen on 0.0.0.0:5006
    python vitals.py --port 5006     # custom UDP port

Requires: pip install numpy
"""

import argparse
import json
import socket
import sys
import time
from collections import deque

try:
    import numpy as np
except ImportError:
    print("Error: numpy is required. Install it with: pip install numpy")
    sys.exit(1)

# ---- Analysis parameters -------------------------------------------------
SAMPLE_HZ = 20          # resampling grid (Hz)
WINDOW_SECONDS = 60     # analysis window length (keep >= 30 for 0.1 Hz)
BREATHING_BAND = (0.1, 0.5)   # Hz -> 6-30 BPM (spec)
HEART_BAND = (0.8, 2.0)       # Hz -> 48-120 BPM (spec)
PRINT_EVERY_SECONDS = 5
MOVEMENT_STD_THRESHOLD = 2.0  # detrended-signal std marking "too much motion"

RATE_HZ_MIN = {"breathing": 6.0, "heart": 48.0}


def band_peak_bpm(freqs, power, band):
    """Return the dominant periodogram peak inside a frequency band, in BPM."""
    mask = (freqs >= band[0]) & (freqs <= band[1])
    if not mask.any():
        return 0.0, 0.0
    band_freqs = freqs[mask]
    band_power = power[mask]
    idx = int(np.argmax(band_power))
    peak_hz = float(band_freqs[idx])
    total = float(np.sum(band_power)) + 1e-12
    quality = float(band_power[idx]) / (float(np.median(band_power)) + 1e-12)
    bpm = peak_hz * 60.0
    return bpm, quality


def main():
    parser = argparse.ArgumentParser(description="RuView-style CSI vital-signs sink (experimental)")
    parser.add_argument("--port", type=int, default=5006, help="UDP port to listen on (default: 5006)")
    parser.add_argument("--bind", default="0.0.0.0", help="interface to bind (default: 0.0.0.0)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind((args.bind, args.port))
    except OSError as exc:
        print(f"Error: cannot bind {args.bind}:{args.port} — {exc}")
        print("Another sink (or vitals.py) may already be listening on this port.")
        sys.exit(1)
    sock.settimeout(1.0)

    n_window = WINDOW_SECONDS * SAMPLE_HZ
    stamps = deque(maxlen=n_window * 4)   # (t, value) pairs as received
    print(f"📡 vitals.py sink listening on {args.bind}:{args.port}")
    print(f"   Waiting for CSI packets from the node... (window: {WINDOW_SECONDS}s)")
    print("   ⚠️  Experimental research feature — not a medical device.\n")

    last_print = 0.0
    first_packet_t = None
    packets = 0

    try:
        while True:
            try:
                data, _addr = sock.recvfrom(4096)
            except socket.timeout:
                continue

            try:
                pkt = json.loads(data.decode("utf-8", errors="ignore"))
            except json.JSONDecodeError:
                continue
            if pkt.get("type") != "csi":
                continue

            amp = pkt.get("amp")
            if not amp:
                continue

            now = time.time()
            if first_packet_t is None:
                first_packet_t = now
            packets += 1
            stamps.append((now, float(np.mean(amp))))

            # Print an estimate every PRINT_EVERY_SECONDS once enough time has passed.
            elapsed = now - first_packet_t
            if elapsed < min(WINDOW_SECONDS, 30) or now - last_print < PRINT_EVERY_SECONDS:
                continue
            last_print = now

            # Resample the irregular packet stream onto a fixed 20 Hz grid.
            t_arr = np.array([s[0] for s in stamps])
            v_arr = np.array([s[1] for s in stamps])
            keep = t_arr >= now - WINDOW_SECONDS
            t_arr, v_arr = t_arr[keep], v_arr[keep]
            if len(t_arr) < SAMPLE_HZ * 15:
                continue

            grid = np.arange(t_arr[0], t_arr[-1], 1.0 / SAMPLE_HZ)
            signal = np.interp(grid, t_arr, v_arr)
            signal = signal - np.mean(signal)

            # Detrend with a 2 s moving average to remove slow drift.
            kernel = np.ones(2 * SAMPLE_HZ) / (2 * SAMPLE_HZ)
            trend = np.convolve(signal, kernel, mode="same")
            detrended = signal - trend

            # FFT periodogram with a Hann window.
            windowed = detrended * np.hanning(len(detrended))
            spectrum = np.abs(np.fft.rfft(windowed))
            freqs = np.fft.rfftfreq(len(detrended), d=1.0 / SAMPLE_HZ)

            breath_bpm, breath_q = band_peak_bpm(freqs, spectrum, BREATHING_BAND)
            heart_bpm, heart_q = band_peak_bpm(freqs, spectrum, HEART_BAND)

            moving = float(np.std(detrended)) > MOVEMENT_STD_THRESHOLD
            age = f"{min(elapsed, len(grid) / SAMPLE_HZ):.0f}s of data"

            if moving:
                print(f"🏃 Movement detected ({age}) — vitals unreliable, ask the subject to sit still.")
                continue

            if breath_bpm < RATE_HZ_MIN["breathing"] and heart_bpm < RATE_HZ_MIN["heart"]:
                print(f"🟡 No confident vital peak yet ({age}). Keep the subject still and seated.")
                continue

            print(f"🫁 Breathing: {breath_bpm:5.1f} BPM (peak quality x{breath_q:5.1f})   "
                  f"💓 Heart: {heart_bpm:5.1f} BPM (peak quality x{heart_q:5.1f})   [{age}, {packets} packets]")

    except KeyboardInterrupt:
        print(f"\nStopped. Processed {packets} CSI packets.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
