/*
  Smart Home - ARDUINO A (I2C MASTER, owns the LCD)

  Systems on this board:
    1) Smart door      : 4x4 keypad + servo
    3) Window security : MC-38 reed sensor + arm switch + buzzer

  It also asks Arduino B (I2C address 8) for the exterior light,
  interior light and climate readings and shows everything on the LCD.

  Pins (Arduino A):
    D2-D5  -> keypad rows R1-R4
    D6,D7,D8,D10 -> keypad columns C1-C4
    D9     -> servo signal
    D11    -> MC-38 wire 1 (wire 2 to GND)          [was D2]
    D12    -> arm ON/OFF switch (other side to GND)  [was D3]
    A0     -> buzzer +  (- to GND)                   [was D4]
    D13    -> built-in "L" LED (lit when armed)
    A4     -> SDA (LCD SDA + Arduino B A4)
    A5     -> SCL (LCD SCL + Arduino B A5)
    GND    -> common GND with Arduino B

  LCD sequence:
    1) "Enter Password:" until the right password is typed (door opens)
    2) then the climate screen: current temp, set temp, fan ON/OFF

  Keypad: digits = password, * = clear while typing,
          * or # = close the door (readings stay on the LCD).
*/
#include "DoorSystem.h"
#include <Wire.h>
#include <Servo.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);  // change 0x27 to 0x3F if nothing shows
Servo doorServo;

// =====================================================================
// 1) SMART DOOR
// =====================================================================
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {2, 3, 4, 5};
byte colPins[COLS] = {6, 7, 8, 10};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

const byte SERVO_PIN = 9;
const String PASSWORD = "1234";
const unsigned long ENTRY_TIMEOUT = 10000;  // forget half-typed password after 10 s

String input = "";
bool unlocked = false;   // true after the right password: LCD shows readings
bool doorOpen = false;
unsigned long lastKeyTime = 0;

// =====================================================================
// 3) WINDOW SECURITY
// =====================================================================
const byte WINDOW_PIN = 11;
const byte ARM_PIN    = 12;
const byte BUZZER_PIN = A0;
const byte STATUS_LED = 13;

// true  = alarm when the sensor circuit is OPEN (pin reads HIGH, magnet away)
// false = alarm when the sensor circuit is CLOSED (pin reads LOW, magnet near)
const bool ALARM_WHEN_CIRCUIT_OPEN = true;

// true  = use the arm ON/OFF switch
// false = always armed (use this for testing if the switch is not wired yet)
const bool USE_ARM_SWITCH = true;

// true  = active buzzer, false = passive piezo (uses tone)
const bool BUZZER_ACTIVE = true;

const bool          ALARM_LATCH = false;  // true = keep sounding until disarmed
const unsigned int  ALARM_FREQ  = 2000;   // Hz (passive piezo only)
const unsigned long BEEP_MS     = 250;    // alarm on/off period
const unsigned long DEBOUNCE_MS = 50;     // ignore contact bounce
const bool          DEBUG       = true;   // print status to Serial Monitor

struct Input {
  byte pin;
  bool stable;              // debounced level (true = HIGH)
  bool lastRaw;
  unsigned long changedAt;

  void begin() {
    pinMode(pin, INPUT_PULLUP);
    stable = lastRaw = digitalRead(pin);
    changedAt = millis();
  }

  void update() {
    bool raw = digitalRead(pin);
    if (raw != lastRaw) {
      lastRaw = raw;
      changedAt = millis();
    } else if (raw != stable && millis() - changedAt >= DEBOUNCE_MS) {
      stable = raw;
    }
  }
};

Input windowIn = {WINDOW_PIN, true, true, 0};
Input armIn    = {ARM_PIN,    true, true, 0};

bool armed = false;
bool circuitOpen = false;     // true = pin reads HIGH (magnet away)
bool windowTriggered = false;
bool alarmLatched = false;
bool alarmActive = false;
bool beepOn = false;
unsigned long lastBeep = 0;

bool lastArmedPrinted = false;
bool lastCircuitPrinted = false;
bool firstPrint = true;

// =====================================================================
// DATA FROM ARDUINO B (exterior light, interior light, climate)
// =====================================================================
const byte SLAVE_ADDRESS = 8;
const byte PACKET_SIZE = 9;
const unsigned long POLL_MS = 500;

bool linkOk = false;
int  distanceCm = 0;
bool motion = false;
bool indoorLedOn = false;
bool fanOn = false;
bool dhtOk = false;
int  lightLevel = 0;
float temperature = 0;
float setTemp = 0;      // fan turns on above this (MAX_TEMP on Arduino B)
unsigned long lastPoll = 0;

// =====================================================================
// LCD
// =====================================================================
String shown[2] = {"", ""};           // what is on the LCD right now
String msg1 = "", msg2 = "";          // temporary message (welcome, wrong pw...)
unsigned long msgUntil = 0;

// Only rewrites a line when it changed, so the LCD doesn't flicker
void showLine(byte row, String text) {
  while (text.length() < 16) text += ' ';
  if (text.length() > 16) text = text.substring(0, 16);
  if (text != shown[row]) {
    lcd.setCursor(0, row);
    lcd.print(text);
    shown[row] = text;
  }
}

void showMessage(const String &l1, const String &l2, unsigned long ms) {
  msg1 = l1;
  msg2 = l2;
  msgUntil = millis() + ms;
}

// ---------------- Door ----------------
void handleDoor() {
  char key = keypad.getKey();

  if (input.length() > 0 && millis() - lastKeyTime > ENTRY_TIMEOUT) input = "";
  if (!key) return;
  lastKeyTime = millis();

  // Unlocked: '*' or '#' closes the door, then the readings come back
  if (unlocked) {
    if ((key == '*' || key == '#') && doorOpen) {
      doorServo.write(0);
      doorOpen = false;
      showMessage("Door Closed", "", 1500);
    }
    return;
  }

  // Locked: password entry
  if (key == '*') {                 // clear input
    input = "";
    msgUntil = 0;
  }
  else if (key >= '0' && key <= '9') {
    msgUntil = 0;                   // typing cancels any message
    if (input.length() < PASSWORD.length()) input += key;

    if (input.length() == PASSWORD.length()) {
      if (input == PASSWORD) {
        doorServo.write(180);
        doorOpen = true;
        unlocked = true;
        showMessage("Welcome Home", "Press # to close", 2500);
      } else {
        showMessage("Wrong Password", "", 2000);
      }
      input = "";
    }
  }
}

// ---------------- Window ----------------
void setBuzzer(bool on) {
  if (BUZZER_ACTIVE) {
    digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
  } else {
    if (on) tone(BUZZER_PIN, ALARM_FREQ);
    else    noTone(BUZZER_PIN);
  }
}

void handleSecurity() {
  windowIn.update();
  armIn.update();

  // switch connected to GND = LOW = armed
  if (USE_ARM_SWITCH) armed = (armIn.stable == LOW);
  else                armed = true;

  circuitOpen = (windowIn.stable == HIGH);
  windowTriggered = ALARM_WHEN_CIRCUIT_OPEN ? circuitOpen : !circuitOpen;

  if (!armed) alarmLatched = false;
  if (armed && windowTriggered) alarmLatched = true;

  alarmActive = ALARM_LATCH ? alarmLatched : (armed && windowTriggered);

  digitalWrite(STATUS_LED, armed);

  // pulsing alarm without delay()
  if (alarmActive) {
    if (millis() - lastBeep >= BEEP_MS) {
      lastBeep = millis();
      beepOn = !beepOn;
      setBuzzer(beepOn);
    }
  } else {
    beepOn = false;
    setBuzzer(false);
  }
}

void printStatus() {
  if (!DEBUG) return;
  if (firstPrint || armed != lastArmedPrinted || circuitOpen != lastCircuitPrinted) {
    Serial.print("Security: ");
    Serial.print(armed ? "ARMED" : "OFF");
    Serial.print(" | Sensor circuit: ");
    Serial.print(circuitOpen ? "OPEN (magnet away)" : "CLOSED (magnet near)");
    Serial.print(" | Alarm: ");
    Serial.println(alarmActive ? "ON" : "off");
    lastArmedPrinted = armed;
    lastCircuitPrinted = circuitOpen;
    firstPrint = false;
  }
}

// ---------------- Arduino B ----------------
void pollSlave() {
  if (millis() - lastPoll < POLL_MS) return;
  lastPoll = millis();

  byte n = Wire.requestFrom(SLAVE_ADDRESS, PACKET_SIZE);
  if (n != PACKET_SIZE) {
    while (Wire.available()) Wire.read();
    linkOk = false;
    return;
  }

  byte b[PACKET_SIZE];
  for (byte i = 0; i < PACKET_SIZE; i++) b[i] = Wire.read();

  distanceCm  = (int)((b[0] << 8) | b[1]);
  motion      = b[2] & 1;
  indoorLedOn = b[2] & 2;
  fanOn       = b[2] & 4;
  dhtOk       = b[2] & 8;
  lightLevel  = (int)((b[3] << 8) | b[4]);
  temperature = (int)((b[5] << 8) | b[6]) / 10.0;
  setTemp     = (int)((b[7] << 8) | b[8]) / 10.0;
  linkOk = true;
}

// ---------------- LCD screens ----------------
void updateLcd() {
  // Window alarm always takes over the screen
  if (alarmActive) {
    showLine(0, "!! WINDOW OPEN!!");
    showLine(1, "ALARM - disarm");
    return;
  }

  // Short messages (welcome, wrong password, door closed...)
  if (millis() < msgUntil) {
    showLine(0, msg1);
    showLine(1, msg2);
    return;
  }

  // Step 1: ask for the password
  if (!unlocked) {
    String stars = "";
    for (byte i = 0; i < input.length(); i++) stars += '#';
    showLine(0, "Enter Password:");
    showLine(1, stars);
    return;
  }

  // Step 2: climate readings
  if (!linkOk) {
    showLine(0, "Board B offline");
    showLine(1, "Check A4 A5 GND");
    return;
  }

  // Line 1: current temperature   e.g. "Temp: 25.4°C"
  String l1 = "Temp: ";
  if (dhtOk) { l1 += String(temperature, 1); l1 += (char)223; l1 += "C"; }
  else       l1 += "ERROR";
  showLine(0, l1);

  // Line 2: set temperature + fan  e.g. "Set 27°C Fan:OFF"
  String l2 = "Set ";
  l2 += String(setTemp, 0);
  l2 += (char)223;
  l2 += "C";
  l2 += fanOn ? " Fan:ON" : " Fan:OFF";
  showLine(1, l2);
}

// =====================================================================
void setup() {
  Serial.begin(9600);
  Wire.begin();                 // join the I2C bus as master

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Home");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

initDoor();

  windowIn.begin();
  armIn.begin();
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  setBuzzer(false);

  delay(1500);                  // give Arduino B time to start
  lcd.clear();
}

void loop() {
  handleDoor();
  handleSecurity();
  printStatus();
  pollSlave();
  updateLcd();
}

