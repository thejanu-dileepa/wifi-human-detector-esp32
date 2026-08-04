#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiUdp.h>
#include <math.h> // For mathematical calculations (sqrt)

// Your Wi-Fi SSID and password
const char *ssid = "....";
const char *password = ".....";

WiFiUDP udp;
IPAddress gatewayIP;

// Function triggered when Wi-Fi CSI data is received
void csi_callback(void *ctx, wifi_csi_info_t *data) {
  wifi_csi_info_t d = data[0];
  
  // Proceed only if data is received and buffer is not empty
  if (d.len > 0 && d.buf != NULL) {
    int8_t *csi_data = (int8_t *)d.buf;
    
    // Calculate the number of sub-carriers (usually 64 values)
    int subcarrier_count = d.len / 2; 

    // Loop through each sub-carrier
    for (int i = 0; i < subcarrier_count; i++) {
      int8_t imag = csi_data[i * 2];       // Imaginary value (Q)
      int8_t real = csi_data[(i * 2) + 1]; // Real value (I)
      
      // Calculate amplitude: sqrt(I^2 + Q^2)
      float amplitude = sqrt((real * real) + (imag * imag));
      
      // Send the value to the Serial Port
      Serial.print(amplitude);
      
      // Add a comma after each value except the last one
      if (i < subcarrier_count - 1) {
        Serial.print(",");
      }
    }
    // Print a new line after sending the entire wave data
    Serial.println(); 
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n--- ESP32-S3 3D Wi-Fi CSI Scanner ---");
  Serial.print("Connecting to Router...");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nSuccessfully connected!");
  gatewayIP = WiFi.gatewayIP();
  
  esp_wifi_set_ps(WIFI_PS_NONE);
  
  wifi_csi_config_t csi_config = {
      .lltf_en           = true,
      .htltf_en          = true,
      .stbc_htltf2_en    = true,
      .ltf_merge_en      = true,
      .channel_filter_en = true,
      .manu_scale        = false,
      .shift             = false
  };
  
  esp_wifi_set_csi_config(&csi_config);
  esp_wifi_set_csi_rx_cb(&csi_callback, NULL);
  esp_wifi_set_csi(true);
  
  Serial.println("CSI 3D Tracking Active! Collecting continuous data...");
}

void loop() {
  // Send a dummy UDP packet to maintain an active connection with the router
  udp.beginPacket(gatewayIP, 8080);
  udp.write('X'); 
  udp.endPacket();
  
  // Delay 100ms (Fetch data approx. 10 times per second)
  delay(100);
}