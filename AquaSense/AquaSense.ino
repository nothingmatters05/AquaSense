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

// ADAPTIVE PULSE VARIABLES
int pulseAttempts = 0;
bool pulseOn = false;
int pulseDuration = 250;  // Starts higher to bypass the dead-zone
float prePulseVol = 0;    

// IR DEBOUNCE VARIABLES
unsigned long irDebounceTimer = 0;
bool cupMissing = true; 

// HARDWARE RESET VECTOR
void(* resetFunc) (void) = 0; 

// STATES
enum State {IDLE, CALIB, WAIT_INPUT, BULK_FILL, SETTLE_WAIT, PULSE_FILL, COMPLETED, DONE, MANUAL, ERROR_NO_CUP};
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
  lcd.print("System Ready");
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
  
  // ===== IR SENSOR DEBOUNCE LOGIC =====
  int irRaw = digitalRead(IR); 
  
  if (irRaw == HIGH) {
    if (irDebounceTimer == 0) {
      irDebounceTimer = millis(); 
    } else if (millis() - irDebounceTimer > 400) {
      cupMissing = true;          
    }
  } else {
    irDebounceTimer = 0;          
    cupMissing = false;
  }

  // ===== GLOBAL CUP REMOVAL ROUTER =====
  if (cupMissing) {
    // FIX: WAIT_INPUT now safely resets to IDLE instead of alarming
    if (state == CALIB || state == WAIT_INPUT) {
      state = IDLE; 
      targetVolume = 0;
      lcd.clear();
    } 
    else if (state == BULK_FILL || state == SETTLE_WAIT || state == PULSE_FILL || state == MANUAL) {
      analogWrite(PUMP, 0);       
      targetVolume = 0;           
      digitalWrite(BLUE, LOW);    
      lcd.clear();
      lcd.print("Cup Removed!");
      state = ERROR_NO_CUP;       
    }
  }

  // ===== BLUETOOTH =====
  if (BT.available()) {
    String cmd = BT.readStringUntil('\n');
    cmd.trim();

    if (cmd == "X") { resetFunc(); }
    
    else if (cmd == "C") {
      analogWrite(PUMP, 0);
      targetVolume = 0;
      state = CALIB;
    }
    
    else if (cmd == "M") {
      state = MANUAL;
      lcd.clear();
      lcd.print("Manual Mode");
      tone(BUZZ, 1500, 150);
      digitalWrite(BLUE, HIGH);
      return;
    }
    
    else if (state == MANUAL) {
      if (cmd == "P") {
        if (cupMissing) {
          lcd.setCursor(0, 1);
          lcd.print("No Cup Present! ");
          tone(BUZZ, 500, 200);
        } else {
          lcd.setCursor(0, 1);
          lcd.print("Pumping water...");
          analogWrite(PUMP, 255); 
        }
      } 
      else if (cmd == "S") {
        analogWrite(PUMP, 0);
        lcd.setCursor(0, 1);
        lcd.print("Paused...       ");
      } 
      else if (cmd == "E") {
        analogWrite(PUMP, 0);
        lcd.clear();
        lcd.print("Finalizing...");
        
        // --- MANUAL MODE SUPER SAMPLE ---
        delay(2500); // Allow water to become completely still

        float sum = 0;
        int valid = 0;
        for (int i = 0; i < 12; i++) { // Deep average
          float d = stableDistance();
          if (d > 0) {
            sum += volumeFromDistance(d);
            valid++;
          }
        }

        float finalVol = (valid > 0) ? sum / valid : 0;
        
        BT.print("Target: Manual, Filled: ");
        BT.print(finalVol, 0);
        BT.println(" ml");

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
    else {
      targetVolume = cmd.toFloat();
    }
  }

  // ===== STATE MACHINE =====
  switch (state) {

    case IDLE:
      lcd.setCursor(0, 0);
      lcd.print("Place Cup       ");

      if (!cupMissing) { 
        tone(BUZZ, 1200, 100);
        digitalWrite(GREEN, HIGH);
        lcd.clear();
        lcd.print("Cup Detected");
        delay(1500);
        digitalWrite(GREEN, LOW);
        state = CALIB;
      }
      break;

    case ERROR_NO_CUP:
    {
      if (millis() % 500 < 250) {
        digitalWrite(RED, HIGH);
        tone(BUZZ, 800);
      } else {
        digitalWrite(RED, LOW);
        noTone(BUZZ);
      }

      if (!cupMissing) {
        digitalWrite(RED, LOW);     
        noTone(BUZZ);               
        tone(BUZZ, 1200, 100);
        digitalWrite(GREEN, HIGH);
        lcd.clear();
        lcd.print("Cup Detected");
        delay(1500);
        digitalWrite(GREEN, LOW);
        state = CALIB;
      }
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

      if (d < 0) {
        failCount++;
        analogWrite(PUMP, 0);
        if (failCount > 5) {
          lcd.clear();
          lcd.print("Sensor Fail!");
          digitalWrite(RED, HIGH);
          state = ERROR_NO_CUP; 
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
        int basePwm;
        if (err > 150) basePwm = 255;
        else if (err > 80) basePwm = 200;
        else if (err > 40) basePwm = 150;
        else if (err > 20) basePwm = 120;
        else basePwm = 100;

        int dynamicBoost = vol / 10.0; 
        int finalPwm = basePwm + dynamicBoost;
        if (finalPwm > 255) finalPwm = 255; 

        analogWrite(PUMP, finalPwm);
      }
    }
    break;

    case SETTLE_WAIT:
      if (millis() - stateTimer > 1500) {
        stateTimer = millis();
        pulseAttempts = 0;
        pulseOn = false;
        pulseDuration = 250; // FIX: Starts higher to punch through static head
        prePulseVol = 0;
        lcd.clear();
        state = PULSE_FILL;
      }
      break;

    case PULSE_FILL:
    {
      if (pulseOn) {
        if (millis() - stateTimer > pulseDuration) {
          analogWrite(PUMP, 0);       
          pulseOn = false;            
          pulseAttempts++;            
          stateTimer = millis();      
        }
        break; 
      }

      if (millis() - stateTimer > 1000) {
        float d = stableDistance();
        if (d < 0) break; 

        float vol = volumeFromDistance(d);

        lcd.setCursor(0, 0);
        lcd.print("Precision:      "); 
        lcd.setCursor(0, 1);
        lcd.print(vol, 0);
        lcd.print(" / ");
        lcd.print(targetVolume, 0);
        lcd.print("ml  ");

        if (pulseAttempts > 0) {
          float volAdded = vol - prePulseVol;
          
          // FIX: Smoother, slower adaptive ramping
          if (volAdded < 1.0 && pulseDuration < 600) {
            pulseDuration += 20; 
          } 
          else if (volAdded > 4.0 && pulseDuration > 150) {
            pulseDuration -= 15;
          }
        }

        if (vol >= (targetVolume - 1.5) || pulseAttempts >= 15) {
          analogWrite(PUMP, 0);
          
          lcd.clear();
          lcd.print("Finalizing...");
          
          // --- AUTO MODE SUPER SAMPLE ---
          delay(2000); // Final deep settle
          float finalSum = 0;
          int finalValid = 0;
          for (int i = 0; i < 10; i++) {
            float fd = stableDistance();
            if (fd > 0) {
              finalSum += volumeFromDistance(fd);
              finalValid++;
            }
          }
          float finalVol = (finalValid > 0) ? finalSum / finalValid : vol;

          BT.print("Target: ");
          BT.print(targetVolume, 0);
          BT.print(" ml, Filled: ");
          BT.println(finalVol, 0);
          
          lcd.clear();
          lcd.print("Set: "); lcd.print(targetVolume, 0); lcd.print("ml ");
          lcd.setCursor(0, 1);
          lcd.print("Got: "); lcd.print(finalVol, 0); lcd.print("ml ");
          delay(3500); 

          state = COMPLETED;
        } 
        else {
          prePulseVol = vol;      
          analogWrite(PUMP, 180);  
          pulseOn = true;
          stateTimer = millis(); 
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

    case MANUAL:
      break; 
  }
}