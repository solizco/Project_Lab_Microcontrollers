// ============================================================
// ESP32 BLINK TEST - PlatformIO connectivity/upload sanity check
// ============================================================
#include <Arduino.h>

// Most ESP32 dev boards have an onboard LED on GPIO2, but this
// varies by board - check your board's silkscreen/datasheet and
// change this if GPIO2 doesn't blink anything.
#define LED_PIN 2

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  Serial.println("Blink test starting...");
}

void loop() {
  digitalWrite(LED_PIN, HIGH);
  Serial.println("LED ON");
  delay(500);

  digitalWrite(LED_PIN, LOW);
  Serial.println("LED OFF");
  delay(500);
}