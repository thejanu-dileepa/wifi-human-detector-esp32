#!/usr/bin/env python3
"""
Provision the ESP32-S3 CSI node over its serial connection.

Writes the WiFi credentials and the UDP sink settings into the node's NVS
(persistent across reboots) through the firmware's `PROV:` serial protocol.

Usage (matches the sensing-pipeline spec):

    python firmware/esp32-csi-node/provision.py --port COM9 \
        --ssid "YourWiFi" --password "secret" --target-ip 192.168.1.20

Omit --target-ip to stream CSI to the router's gateway. Optional flags:
    --udp-port 5006     UDP port the sink listens on (default 5006)
    --rate-hz 10        CSI stream rate cap in packets/second (1-100)
    --show              print the node's current configuration
    --clear             erase the stored configuration and reboot the node

Requires: pip install pyserial
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("Error: pyserial is required. Install it with: pip install pyserial")
    sys.exit(1)


def build_payload(args) -> str:
    """Build the PROV: command line from the CLI arguments."""
    fields = [
        args.ssid,
        args.password or "",
        args.target_ip or "",
        str(args.udp_port),
        str(args.rate_hz),
    ]
    for field in fields[:2]:
        if "|" in field:
            print("Error: '|' is not allowed inside the SSID or password.")
            sys.exit(1)
    return "PROV:" + "|".join(fields)


def read_for(ser, seconds: float) -> str:
    """Read whatever the node prints for a given time window."""
    deadline = time.time() + seconds
    chunks = []
    while time.time() < deadline:
        waiting = ser.in_waiting
        if waiting:
            chunks.append(ser.read(waiting).decode("utf-8", errors="ignore"))
        else:
            time.sleep(0.05)
    return "".join(chunks)


def wait_for_response(ser, timeout: float = 10.0) -> str:
    """Wait for a PROV:OK / PROV:ERR response line."""
    deadline = time.time() + timeout
    buffer = ""
    while time.time() < deadline:
        if ser.in_waiting:
            buffer += ser.read(ser.in_waiting).decode("utf-8", errors="ignore")
            for line in buffer.splitlines():
                line = line.strip()
                if line.startswith("PROV:OK"):
                    return "PROV:OK"
                if line.startswith("PROV:ERR"):
                    return line
        time.sleep(0.05)
    return "TIMEOUT"


def open_port(port: str, baud: int) -> "serial.Serial":
    try:
        ser = serial.Serial(port, baud, timeout=1)
    except Exception as exc:
        print(f"Error: cannot open {port}: {exc}")
        print("Close the PlatformIO Serial Monitor and try again.")
        sys.exit(1)
    return ser


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Provision WiFi credentials and the UDP sink for the ESP32-S3 CSI node."
    )
    parser.add_argument("--port", default="COM9", help="serial port of the node (default: COM9)")
    parser.add_argument("--baud", type=int, default=115200, help="serial baud rate (default: 115200)")
    parser.add_argument("--ssid", help="WiFi network name")
    parser.add_argument("--password", default="", help="WiFi password ('' for open networks)")
    parser.add_argument("--target-ip", default="", help="UDP sink IP (omit = stream to router gateway)")
    parser.add_argument("--udp-port", type=int, default=5006, help="sink UDP port (default: 5006)")
    parser.add_argument("--rate-hz", type=int, default=10, help="CSI stream rate cap, 1-100 Hz (default: 10)")
    parser.add_argument("--show", action="store_true", help="print the node's current configuration")
    parser.add_argument("--clear", action="store_true", help="erase stored configuration and reboot")
    args = parser.parse_args()

    if args.udp_port < 1 or args.udp_port > 65535:
        parser.error("--udp-port must be 1-65535")
    if args.rate_hz < 1 or args.rate_hz > 100:
        parser.error("--rate-hz must be 1-100")

    ser = open_port(args.port, args.baud)
    print(f"{args.port} connected at {args.baud} baud.")

    # Let any boot banner settle before sending commands.
    boot = read_for(ser, 2.0)
    if boot.strip():
        print("--- node output ---")
        print(boot.strip())
        print("-------------------")

    if args.show:
        ser.write(b"PROV:SHOW\n")
        print(read_for(ser, 2.0).strip())
        ser.close()
        return

    if args.clear:
        ser.write(b"PROV:CLEAR\n")
        response = wait_for_response(ser)
        print(f"Node responded: {response}")
        if response == "PROV:OK":
            print("Configuration erased. The node is rebooting with compiled-in defaults.")
        ser.close()
        return

    if not args.ssid:
        parser.error("--ssid is required to provision WiFi credentials")

    payload = build_payload(args)
    print(f"Sending configuration: SSID='{args.ssid}', "
          f"target={args.target_ip or '<router gateway>'}, "
          f"port={args.udp_port}, rate={args.rate_hz} Hz")

    ser.reset_input_buffer()
    ser.write(payload.encode("utf-8") + b"\n")

    response = wait_for_response(ser)
    if response == "PROV:OK":
        print("\n✅ Provisioning complete! The node is rebooting with the new settings.")
        print("   After it reconnects, CSI JSON packets stream to the configured sink")
        print(f"   on UDP port {args.udp_port}. Verify anytime with: --show")
    elif response == "TIMEOUT":
        print("\n❌ No response from the node. Make sure the firmware is flashed and")
        print("   the serial monitor is closed, then try again.")
        sys.exit(1)
    else:
        print(f"\n❌ Node rejected the configuration: {response}")
        sys.exit(1)

    ser.close()


if __name__ == "__main__":
    main()
