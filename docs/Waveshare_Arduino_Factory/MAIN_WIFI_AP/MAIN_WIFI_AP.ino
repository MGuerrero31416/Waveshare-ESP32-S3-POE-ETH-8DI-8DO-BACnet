#include <HardwareSerial.h>     // Reference the ESP32 built-in serial port library
#include "WS_WIFI.h"
#include "WS_Bluetooth.h"
#include "WS_GPIO.h"
#include "WS_RS485.h"
#include "WS_CAN.h"
#include "WS_RTC.h"
#include "WS_GPIO.h"
#include "WS_DIN.h"
#include "WS_SD.h"
#include "WS_ETH.h"
#include <WiFi.h>


uint32_t Simulated_time=0;      // Analog time counting

/********************************************************  Initializing  ********************************************************/
void setup() {

  Serial.begin(115200);
  delay(1000);

  Flash_test();
  GPIO_Init();
  I2C_Init();

  RTC_Init();
  RS485_Init();
  CAN_Init();
  SD_Init();


  WIFI_Init();
      Serial.print("WiFi mode: ");
      Serial.println(WiFi.getMode());

      Serial.println("===== WiFi Information =====");

      Serial.print("SSID: ");
      Serial.println(WiFi.softAPSSID());

      Serial.print("AP IP: ");
      Serial.println(WiFi.softAPIP());

      Serial.print("Stations connected: ");
      Serial.println(WiFi.softAPgetStationNum());

      Serial.println("Open browser at:");
      Serial.print("http://");
      Serial.println(WiFi.softAPIP());

      Serial.println("============================");

      Serial.print("Stations connected: ");
      Serial.println(WiFi.softAPgetStationNum());

      Serial.print("AP IP: ");
      Serial.println(WiFi.softAPIP());

      Serial.print("STA IP: ");
      Serial.println(WiFi.localIP());

  Bluetooth_Init();

  ETH_Init();

  DIN_Init();


  Dout_Init();

  Serial.println("DONE");
}

/**********************************************************  While  **********************************************************/
void loop() {

}