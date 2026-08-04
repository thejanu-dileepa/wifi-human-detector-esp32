import serial
import matplotlib.pyplot as plt
import time

# Port and Baud Rate where the ESP32 is connected
SERIAL_PORT = 'COM3' 
BAUD_RATE = 115200

# Open the serial connection
try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE)
    print(f"{SERIAL_PORT} Port connected successfully!")
except Exception as e:
    print(f"Error: Cannot open {SERIAL_PORT}.")
    print("Please close the PlatformIO Serial Monitor and run again!")
    exit()

# Prepare the Graph
plt.ion() # Interactive mode ON
fig, ax = plt.subplots(figsize=(10, 5))
y_data = []

line, = ax.plot(y_data, color='blue', linewidth=2)
ax.set_ylim(-90, -20) # Normal RSSI range (change these values if it varies)
ax.set_title('Wi-Fi Signal Strength (Movement Detection)', fontsize=14)
ax.set_ylabel('RSSI (Signal Strength)', fontsize=12)
ax.set_xlabel('Time (Packets)', fontsize=12)
plt.grid(True, linestyle='--', alpha=0.6)

print("Receiving data... (The graph will appear)")

# Continuously read data and update the graph
while True:
    try:
        if ser.in_waiting > 0:
            # Read the incoming line from the serial port
            line_str = ser.readline().decode('utf-8', errors='ignore').strip()
            
            # Process only if it contains the required RSSI Data
            if "RSSI:" in line_str:
                try:
                    # Example format: 'Router Signal Received! | RSSI: -56 | CSI Data Bytes: 256'
                    # Extract only the -56 part from this sentence
                    parts = line_str.split("RSSI: ")
                    rssi_val = int(parts[1].split(" |")[0])
                    
                    # Add value to the array (keep only the last 100 data points)
                    y_data.append(rssi_val)
                    if len(y_data) > 100:
                        y_data.pop(0) 
                        
                    # Update the graph
                    line.set_xdata(range(len(y_data)))
                    line.set_ydata(y_data)
                    ax.set_xlim(0, max(100, len(y_data)))
                    
                    fig.canvas.draw()
                    fig.canvas.flush_events()
                except ValueError:
                    pass # Ignore occasional corrupted data
    except KeyboardInterrupt:
        print("\nProgram stopped.")
        break