/*
  Smart Home - ARDUINO B (I2C SLAVE, address 8)

  Systems on this board:
    2) Exterior lighting : ultrasonic sensor + porch LED
    4) Interior lighting : LDR + push button + indoor LED
    5) Climate           : DHT11 + 5V fan

  This board has NO LCD. Arduino A (the master) asks it for its readings
  over I2C every half second and shows them on the LCD.

  Pins (Arduino B):
    D2  -> DHT11 data
    D4  -> push button (other leg to GND)
    D5  -> indoor LED (+ through 220 ohm resistor)
    D6  -> fan control (to transistor / relay)
    D7  -> fan control 2 (only if you keep the two-pin wiring, see FAN_PIN2)
    D8  -> porch LED (+ through 220 ohm resistor)
    D9  -> ultrasonic TRIG
    D10 -> ultrasonic ECHO
    A0  -> LDR voltage divider
    A4  -> SDA  (to Arduino A A4 + LCD SDA)
    A5  -> SCL  (to Arduino A A5 + LCD SCL)
    GND -> common GND with Arduino A
*/

#include <Wire.h>
#include <DHT.h>

const byte SLAVE_ADDRESS = 8;

// ---------- Exterior lighting ----------
const int TRIG_PIN  = 9;
const int ECHO_PIN  = 10;
const int LIGHT_PIN = 8;

const int DETECTION_DISTANCE = 10;      // cm: porch LED turns on when something is closer than this
const unsigned long LIGHT_TIME = 2000;  // ms the LED stays on after the person moves away
                                        // (was 50 ms: shorter than the 100 ms between
                                        //  measurements, so the LED only flickered)

unsigned long lastDetectedTime = 0;
bool personDetected = false;
int distanceCm = 0;
unsigned long lastPing = 0;

// ---------- Interior lighting ----------
const int ldrPin = A0;
const int buttonPin = 4;
const int indoorLedPin = 5;

// LDR reading: high = bright, low = dark (LDR to 5V, 10k resistor to GND).
// Watch "LDR:" in the Serial Monitor in normal light and with the LDR covered,
// then set darkThreshold between the two values and brightThreshold a bit above it.
const int darkThreshold = 450;    // below this (dark)  -> LED on
const int brightThreshold = 550;  // above this (light) -> LED off

bool isDark = false;
bool indoorLedOn = false;
int lightLevel = 0;

// ---------- Climate ----------
#define DHTPIN 2
#define DHTTYPE DHT11
#define FAN_PIN 6
#define FAN_PIN2 7               // was D10 in your climate sketch (D10 is now ultrasonic ECHO)

const float MAX_TEMP = 27.0;            // Fan starts above this temperature (C)
const unsigned long INTERVAL = 1000;    // Update every 1 second

DHT dht(DHTPIN, DHTTYPE);
unsigned long lastUpdate = 0;
float temperature = 0;
bool dhtOk = false;
bool fanOn = false;

// ---------- Data packet sent to Arduino A ----------
// byte 0-1 : distance in cm
// byte 2   : flags (bit0 motion, bit1 indoor LED, bit2 fan, bit3 DHT ok)
// byte 3-4 : LDR reading (0-1023)
// byte 5-6 : temperature x10 (e.g. 275 = 27.5 C)
// byte 7-8 : set temperature (MAX_TEMP) x10
const byte PACKET_SIZE = 9;
volatile byte packet[PACKET_SIZE];

void setFan(bool on) {
  // both pins must change together so they never fight each other
  digitalWrite(FAN_PIN, on ? HIGH : LOW);
  digitalWrite(FAN_PIN2, on ? HIGH : LOW);
}

// Runs automatically whenever Arduino A asks for data
void onRequest() {
  Wire.write((const byte *)packet, PACKET_SIZE);
}

void updatePacket() {
  int t10 = (int)(temperature * 10);
  int set10 = (int)(MAX_TEMP * 10);
  byte flags = (personDetected ? 1 : 0) | (indoorLedOn ? 2 : 0) |
               (fanOn ? 4 : 0) | (dhtOk ? 8 : 0);

  noInterrupts();   // don't let onRequest() send a half-updated packet
  packet[0] = highByte(distanceCm);
  packet[1] = lowByte(distanceCm);
  packet[2] = flags;
  packet[3] = highByte(lightLevel);
  packet[4] = lowByte(lightLevel);
  packet[5] = highByte(t10);
  packet[6] = lowByte(t10);
  packet[7] = highByte(set10);
  packet[8] = lowByte(set10);
  interrupts();
}

void handleExteriorLight() {
  if (millis() - lastPing < 100) return;   // replaces delay(100)
  lastPing = millis();

  // Send ultrasonic pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Read echo (30 ms timeout so the loop never blocks for a full second)
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  float distance = duration * 0.034 / 2;
  distanceCm = (int)distance;

  // Someone is close (0 = no echo / out of range, so it is ignored)
  if (distance > 0 && distance <= DETECTION_DISTANCE) {
    lastDetectedTime = millis();
    digitalWrite(LIGHT_PIN, HIGH);
    personDetected = true;
  }

  // No one detected
  if (personDetected && millis() - lastDetectedTime >= LIGHT_TIME) {
    personDetected = false;
    digitalWrite(LIGHT_PIN, LOW);
  }
}

void handleInteriorLight() {
  lightLevel = analogRead(ldrPin);
  int buttonState = digitalRead(buttonPin);   // LOW = pressed

  if (lightLevel < darkThreshold) {
    isDark = true;
  } else if (lightLevel > brightThreshold) {
    isDark = false;
  }

  // LED on if dark or button pressed
  indoorLedOn = isDark || buttonState == LOW;
  digitalWrite(indoorLedPin, indoorLedOn ? HIGH : LOW);
}

void handleClimate() {
  if (millis() - lastUpdate < INTERVAL) return;
  lastUpdate = millis();

  float temp = dht.readTemperature();
  if (isnan(temp)) {
    dhtOk = false;
    fanOn = false;
    setFan(false);
    return;
  }

  dhtOk = true;
  temperature = temp;
  fanOn = (temp > MAX_TEMP);
  setFan(fanOn);
}

void setup() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(LIGHT_PIN, LOW);

  pinMode(ldrPin, INPUT);
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(indoorLedPin, OUTPUT);

  // Self-test: both LEDs blink 3 times at power-up.
  // If an LED does NOT blink, the problem is its wiring (LED, resistor, trace), not the code or the sensor.
  for (byte i = 0; i < 3; i++) {
    digitalWrite(LIGHT_PIN, HIGH); digitalWrite(indoorLedPin, HIGH); delay(250);
    digitalWrite(LIGHT_PIN, LOW);  digitalWrite(indoorLedPin, LOW);  delay(250);
  }

  pinMode(FAN_PIN, OUTPUT);
  pinMode(FAN_PIN2, OUTPUT);
  setFan(false);
  dht.begin();

  Serial.begin(9600);

  updatePacket();
  Wire.begin(SLAVE_ADDRESS);   // join the I2C bus as a slave
  Wire.onRequest(onRequest);
}

void loop() {
  handleExteriorLight();
  handleInteriorLight();
  handleClimate();
  updatePacket();

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 1000) {   // for the Serial Monitor
    lastPrint = millis();
    Serial.print("Dist: ");   Serial.print(distanceCm);
    Serial.print(" cm"); Serial.print(personDetected ? " (porch ON)" : "");
    Serial.print(" | LDR: "); Serial.print(lightLevel);
    Serial.print(isDark ? " dark" : " bright");
    Serial.print(digitalRead(buttonPin) == LOW ? " | button PRESSED" : "");
    Serial.print(" | indoor LED "); Serial.print(indoorLedOn ? "ON" : "off");
    Serial.print(" | Temp: ");
    if (dhtOk) Serial.print(temperature, 1); else Serial.print("ERR");
    Serial.print(" | Fan: ");  Serial.println(fanOn ? "ON" : "OFF");
  }
}
