#include <Arduino.h>
#include <Servo.h>
#include <Keypad.h>

#include "DoorSystem.h"

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {2,3,4,5};
byte colPins[COLS] = {6,7,8,10};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

Servo doorServo;

const byte SERVO_PIN = 9;

const String PASSWORD = "1234";

const unsigned long ENTRY_TIMEOUT = 10000;

String input = "";

bool unlocked = false;
bool doorOpen = false;

unsigned long lastKeyTime = 0;

void initDoor()
{
    doorServo.attach(SERVO_PIN);
    doorServo.write(0);
}

void handleDoor()
{
    char key = keypad.getKey();

    if(input.length() > 0 &&
       millis() - lastKeyTime > ENTRY_TIMEOUT)
    {
        input = "";
    }

    if(!key)
        return;

    lastKeyTime = millis();

    if(unlocked)
    {
        if((key == '*' || key == '#') && doorOpen)
        {
            doorServo.write(0);
            doorOpen = false;
        }

        return;
    }

    if(key == '*')
    {
        input = "";
    }
    else if(key >= '0' && key <= '9')
    {
        if(input.length() < PASSWORD.length())
            input += key;

        if(input.length() == PASSWORD.length())
        {
            if(input == PASSWORD)
            {
                doorServo.write(180);
                unlocked = true;
                doorOpen = true;
            }

            input = "";
        }
    }
}
