#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);
SoftwareSerial BT(12, 11);

// PINS
#define IR 2
#define RED 3
#define GREEN 4
#define BLUE 5
#define BUZZ 6
#define PUMP 7
#define TRIG 9
#define ECHO 10

// PARAMETERS
float emptyDistance;
float targetVolume = 0;

float radius = 4.4;
float mlPerCm = 64.94;

long duration;
unsigned long stateTimer = 0;
int pulseAttempts = 0;
bool pulseOn = false;   // NEW

// STATES
enum State {IDLE, CALIB, WAIT_INPUT, BULK_FILL, SETTLE_WAIT, PULSE_FILL, COMPLETED, DONE, MANUAL};
State state = IDLE;


// ================= ULTRASONIC =================
float readDistance() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  duration = pulseIn(ECHO, HIGH, 30000);
  float d = duration * 0.034 / 2.0;

  if (d <= 2 || d > 25) return -1;
  return d;
}


// ================= FILTER =================
float stableDistance() {
  float v[15];
  int c = 0, attempts = 0;

  while (c < 15 && attempts < 40) {
    float d = readDistance();
    if (d > 0) v[c++] = d;
    attempts++;
    delay(15);
  }

  if (c < 15) return -1;

  for (int i = 0; i < 14; i++)
    for (int j = i + 1; j < 15; j++)
      if (v[j] < v[i]) { float t = v[i]; v[i] = v[j]; v[j] = t; }

  float sum = 0;
  for (int i = 3; i < 12; i++) sum += v[i];

  return sum / 9.0;
}


// ================= VOLUME =================
float volumeFromDistance(float d) {
  float h = emptyDistance - d;
  if (h < 0) h = 0;

  float v1 = h * mlPerCm;
  float v2 = 3.1416 * radius * radius * h;

  return (v1 + v2) / 2.0;
}


// ================= STARTUP =================
void startup() {
  lcd.clear();
  lcd.print("AquaSense");

  for (int i = 0; i < 3; i++) {
    digitalWrite(GREEN, HIGH);
    tone(BUZZ, 1500, 100);
    delay(200);
    digitalWrite(GREEN, LOW);
    delay(200);
  }

  lcd.clear();
  lcd.print("Ultrasonic");
  lcd.setCursor(0, 1);
  lcd.print("Dispensing Sys");
  delay(2000);
}


// ================= SETUP =================
void setup() {
  pinMode(IR, INPUT);
  pinMode(RED, OUTPUT);
  pinMode(GREEN, OUTPUT);
  pinMode(BLUE, OUTPUT);
  pinMode(BUZZ, OUTPUT);
  pinMode(PUMP, OUTPUT);
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);

  lcd.init();
  lcd.backlight();
  BT.begin(9600);

  startup();
}


// ================= LOOP =================
void loop() {
  int ir = digitalRead(IR);

  // ===== BLUETOOTH =====
  if (BT.available()) {
    String cmd = BT.readStringUntil('\n');
    cmd.trim();

    if (cmd == "M") {
      state = MANUAL;
      lcd.clear();
      lcd.print("Manual Mode");
      tone(BUZZ, 1500, 150);
      digitalWrite(BLUE, HIGH);
      delay(1500);
      lcd.clear();
      return;
    }

    if (state == MANUAL) {
      if (cmd == "P") analogWrite(PUMP, 200);
      else if (cmd == "S") analogWrite(PUMP, 0);

      else if (cmd == "E") {
        analogWrite(PUMP, 0);

        lcd.clear();
        lcd.print("Measuring...");
        delay(2000);

        float sum = 0;
        int valid = 0;

        for (int i = 0; i < 10; i++) {
          float d = stableDistance();
          if (d > 0) {
            sum += volumeFromDistance(d);
            valid++;
          }
          delay(100);
        }

        float finalVol = (valid > 0) ? sum / valid : 0;

        lcd.clear();
        lcd.print("Manual Fill:");
        lcd.setCursor(0, 1);
        lcd.print(finalVol, 0);
        lcd.print(" ml");

        tone(BUZZ, 2000, 200);
        delay(3000);

        digitalWrite(BLUE, LOW);
        state = IDLE;
        lcd.clear();
      }
      return;
    }

    targetVolume = cmd.toFloat();
  }


  switch (state) {

    case IDLE:
      lcd.setCursor(0, 0);
      lcd.print("Place Cup     ");

      if (ir == HIGH) {
        tone(BUZZ, 1200, 100);
        digitalWrite(GREEN, HIGH);

        lcd.clear();
        lcd.print("Cup Detected");
        delay(1500);

        digitalWrite(GREEN, LOW);
        state = CALIB;
      }
      break;


    case CALIB:
      lcd.setCursor(0, 0);
      lcd.print("Calibrating...");
      emptyDistance = stableDistance();

      if (emptyDistance < 15.5 || emptyDistance > 16.3 || emptyDistance < 0) {
        lcd.setCursor(0, 1);
        lcd.print("Out of range ");
        digitalWrite(RED, HIGH);
        delay(500);
        digitalWrite(RED, LOW);
      } else {
        lcd.clear();
        lcd.print("Calibration OK");
        tone(BUZZ, 2000, 150);
        delay(1500);
        state = WAIT_INPUT;
      }
      break;


    case WAIT_INPUT:
      lcd.setCursor(0, 0);
      lcd.print("Send Volume BT");
      if (targetVolume > 0) {
        lcd.clear();
        state = BULK_FILL;
        digitalWrite(BLUE, HIGH);
      }
      break;


    case BULK_FILL:
    {
      static int failCount = 0;

      float stopOffset = 10;
      float effectiveTarget = targetVolume - stopOffset;

      float d = stableDistance();

      // 🔴 SENSOR FAIL SAFETY
      if (d < 0) {
        failCount++;
        analogWrite(PUMP, 0);

        if (failCount > 5) {
          lcd.clear();
          lcd.print("Sensor Fail!");
          digitalWrite(RED, HIGH);
          state = DONE;
        }
        return;
      } else {
        failCount = 0;
      }

      float vol = volumeFromDistance(d);
      float h = emptyDistance - d;

      lcd.setCursor(0, 0);
      lcd.print("H:");
      lcd.print(h, 1);
      lcd.print("cm   ");

      lcd.setCursor(0, 1);
      lcd.print(vol, 0);
      lcd.print("ml   ");

      float err = effectiveTarget - vol;

      if (err <= 5) {
        analogWrite(PUMP, 0);
        stateTimer = millis();
        lcd.clear();
        lcd.print("Settling...");
        state = SETTLE_WAIT;
      } else {
        int pwm;
        if (err > 150) pwm = 255;
        else if (err > 80) pwm = 200;
        else if (err > 40) pwm = 150;
        else if (err > 20) pwm = 120;
        else pwm = 100;

        analogWrite(PUMP, pwm);
      }
    }
    break;


    case SETTLE_WAIT:
      if (millis() - stateTimer > 1500) {
        stateTimer = millis();
        pulseAttempts = 0;
        pulseOn = false;
        lcd.clear();
        state = PULSE_FILL;
      }
      break;


    // ---------------- PRECISION PULSE FILL ----------------
    case PULSE_FILL:
    {
      // PHASE 1: If the pump is pulsing, let it finish the 150ms kick.
      if (pulseOn) {
        if (millis() - stateTimer > 150) {
          analogWrite(PUMP, 0);       // Turn pump off
          pulseOn = false;            // End pulse phase
          pulseAttempts++;            // Count this attempt
          stateTimer = millis();      // Start the settling cooldown timer
        }
        break; // Exit the loop here so we don't read the sensor while pumping
      }

      // PHASE 2: Wait for water to settle (800ms), THEN read and decide.
      if (millis() - stateTimer > 800) {
        float d = stableDistance();
        if (d < 0) break; // Error handling: try again next loop

        float vol = volumeFromDistance(d);

        lcd.setCursor(0, 0);
        lcd.print("Precision:      "); 
        lcd.setCursor(0, 1);
        lcd.print(vol, 0);
        lcd.print(" / ");
        lcd.print(targetVolume, 0);
        lcd.print("ml  ");

        // The exact logic you wanted: Target reached OR tried 4 times
        if (vol >= (targetVolume - 2.0) || pulseAttempts >= 4) {
          analogWrite(PUMP, 0);
          
          // Send final precision data back to the phone
          BT.print("Target: ");
          BT.print(targetVolume, 0);
          BT.print(" ml, Filled: ");
          BT.println(vol, 0);
          
          delay(1000); // Show the final precision on the LCD for 1 second
          state = COMPLETED;
        } 
        // If not done and under 4 attempts, trigger the next pulse
        else {
          analogWrite(PUMP, 150);
          pulseOn = true;
          stateTimer = millis(); // Start the 150ms pulse timer
        }
      }
    }
    break;

    case COMPLETED:
      lcd.clear();
      lcd.print("Completed!");
      digitalWrite(GREEN, HIGH);
      digitalWrite(BLUE, LOW);
      tone(BUZZ, 2000, 300);
      delay(3000);
      digitalWrite(GREEN, LOW);
      state = DONE;
      break;


    case DONE:
      targetVolume = 0;
      lcd.clear();
      state = IDLE;
      break;
  }
}