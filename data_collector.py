import serial
import csv
import time

# Your Serial Port
SERIAL_PORT = 'COM3'
BAUD_RATE = 115200

# 🔴 This is where we change the recording label/scenario. 
# Change this to "Empty_Room", "Human_Walking", "Pet_Walking", etc. before running.
CURRENT_LABEL = "Human_Walking" 

# CSV File name
CSV_FILENAME = "wifi_csi_dataset.csv"
SAMPLES_PER_ROW = 30 # 30 data points per row (approx. 3 seconds of wave pattern)

try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE)
    print(f"{SERIAL_PORT} Port connected. Starting data collection...")
except Exception as e:
    print("Port Error! Check if PlatformIO Monitor is closed.")
    exit()

print(f"\n--- Starting data recording for {CURRENT_LABEL}... ---")
print("Please perform the relevant action (e.g., walk around). Press Ctrl+C to stop.\n")

# Create CSV file and write headers (only for the first time)
with open(CSV_FILENAME, mode='a', newline='') as file:
    writer = csv.writer(file)
    # Write the header only if the file is empty
    if file.tell() == 0:
        headers = [f"RSSI_{i}" for i in range(SAMPLES_PER_ROW)] + ["Label"]
        writer.writerow(headers)

    current_row = []
    rows_collected = 0

    try:
        while True:
            if ser.in_waiting > 0:
                line_str = ser.readline().decode('utf-8', errors='ignore').strip()
                
                if "RSSI:" in line_str:
                    try:
                        parts = line_str.split("RSSI: ")
                        rssi_val = int(parts[1].split(" |")[0])
                        current_row.append(rssi_val)

                        # Write to CSV once 30 data points are collected
                        if len(current_row) == SAMPLES_PER_ROW:
                            current_row.append(CURRENT_LABEL)
                            writer.writerow(current_row)
                            file.flush() # Save data to file
                            rows_collected += 1
                            print(f"[{rows_collected}] Data Row saved -> {CURRENT_LABEL}")
                            current_row = [] # Clear the row for the next set of data

                    except ValueError:
                        pass
    except KeyboardInterrupt:
        print(f"\n✅ Data recording finished. Saved {rows_collected} rows!")