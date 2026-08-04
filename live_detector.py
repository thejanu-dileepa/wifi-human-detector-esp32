import serial
import pickle
import warnings
from collections import deque

# Suppress minor warnings from Scikit-learn
warnings.filterwarnings("ignore")

SERIAL_PORT = 'COM3'
BAUD_RATE = 115200
SAMPLES_PER_PREDICTION = 30 # Because we trained with 30 data points

# 1. Load the trained AI Model
try:
    with open('wifi_model.pkl', 'rb') as f:
        model = pickle.load(f)
    print("✅ AI Model 'wifi_model.pkl' loaded successfully!")
except Exception as e:
    print("Error: Cannot load the model. Check if 'wifi_model.pkl' exists in the folder.")
    exit()

# 2. Connect to the ESP32
try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE)
    print(f"📡 Starting live data reading from {SERIAL_PORT} Port...\n")
except Exception as e:
    print("Error: Cannot open the port. Check if PlatformIO Monitor is closed.")
    exit()

# Memory (Buffer) to hold 30 data points
buffer = deque(maxlen=SAMPLES_PER_PREDICTION)

print("🔮 [AI Scanner Active] - Detecting activity in the room...")
print("-" * 50)

# 3. Continuously read data and predict live
try:
    while True:
        if ser.in_waiting > 0:
            line_str = ser.readline().decode('utf-8', errors='ignore').strip()
            
            if "RSSI:" in line_str:
                try:
                    # Extract the RSSI value
                    parts = line_str.split("RSSI: ")
                    rssi_val = int(parts[1].split(" |")[0])
                    buffer.append(rssi_val)

                    # Make a prediction once 30 data points are collected
                    if len(buffer) == SAMPLES_PER_PREDICTION:
                        
                        # Format the data for the AI [ [d1, d2, ... d30] ]
                        X_live = [list(buffer)] 
                        
                        # Live prediction
                        prediction = model.predict(X_live)[0]

                        # Print the result nicely
                        if prediction == "Empty_Room":
                            print("🟢 Empty Room - No one is present")
                        elif prediction == "Human_Walking":
                            print("🔴 Alert! (Human Walking) - Someone is walking!")
                        else:
                            print(f"🟡 Detection: {prediction}")

                        # Remove 15 old data points before the next prediction 
                        # (This creates a slight delay/gap between predictions)
                        for _ in range(15):
                            buffer.popleft()

                except ValueError:
                    pass
except KeyboardInterrupt:
    print("\nProgram stopped.")