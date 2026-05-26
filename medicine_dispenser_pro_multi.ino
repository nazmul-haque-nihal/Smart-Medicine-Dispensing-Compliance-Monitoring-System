/*
 * Professional Medicine Dispenser - Multi-Dose Support
 * - Multiple doses per medicine per day
 * - Non-blocking time entry with backspace, timeout, validation
 * - Fixed IR sensor logic (HIGH=empty, LOW=detected)
 * - Servo safety with limits and emergency stop
 * - Proper state machine with error recovery
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <Keypad.h>
#include <EEPROM.h>

// ==================== CONFIGURATION ====================
#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

#define SERVO1_PIN 10
#define SERVO2_PIN 11
#define SERVO_MIN_ANGLE -70
#define SERVO_MAX_ANGLE 0
#define SERVO_SPEED_MS 80  // Much slower for stability (50° range @ 80ms = 4 seconds)

#define BUZZER_PIN 12

#define IR1_PIN A1
#define IR2_PIN A0
#define IR_DEBOUNCE_MS 50

#define EEPROM_MAGIC 42
#define EEPROM_MAGIC_ADDR 20

#define INPUT_TIMEOUT_MS 30000
#define MENU_TIMEOUT_MS 60000

// ==================== DOSE CONFIGURATION ====================
#define MAX_DOSES 4  // Max 4 doses per medicine per day

// ==================== HARDWARE ====================
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
Servo servo1;
Servo servo2;

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {2, 3, 4, 5};
byte colPins[COLS] = {6, 7, 8, 9};
Keypad keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ==================== GLOBAL STATE ====================
int hour = 0;
int minute = 0;

// Multiple dose times for each medicine
struct DoseTime {
  int hour;
  int minute;
  bool taken;
  bool missed;
};

DoseTime med1Doses[MAX_DOSES] = {
  {8, 0, false, false},
  {0, 0, false, false},  // Empty slots
  {0, 0, false, false},
  {0, 0, false, false}
};

DoseTime med2Doses[MAX_DOSES] = {
  {20, 0, false, false},
  {0, 0, false, false},  // Empty slots
  {0, 0, false, false},
  {0, 0, false, false}
};

int med1Count = 1;  // Number of active doses for med1
int med2Count = 1;  // Number of active doses for med2

unsigned long lastMinuteCheck = 0;

// IR Sensor state tracking
struct IRState {
  bool previouslyDetected;
  unsigned long debounceTimer;
  bool currentState;
};
IRState ir1State = {false, 0, false};
IRState ir2State = {false, 0, false};

// Time entry state
struct TimeEntry {
  bool active;
  char input[8];  // Changed from String
  int inputLength;  // Track actual length
  int maxLength;
  int targetMed;
  int doseIndex;  // Which dose we're editing
  unsigned long timeout;
  bool isManualTime;
  bool isMedTime;
};
TimeEntry timeEntry = {false, "", 0, 0, 0, 0, 0, false, false};

// Menu state
enum MenuState {
  MENU_NONE,
  MENU_MAIN,
  MENU_MED_SELECT,
  MENU_DOSE_SELECT,
  MENU_TIME_ENTRY,
  MENU_DOSE_ACTION,
  MENU_TEST_SELECT,
  MENU_OPEN_SELECT,
  MENU_OPEN_ACTIVE
};
MenuState menuState = MENU_NONE;
unsigned long menuTimeout = 0;
int menuSelection = 0;
int menuOpenedServo = 0;

// ==================== STATE MACHINE ====================
enum State {
  NORMAL,
  ALERTING_MED1,
  ALERTING_MED2,
  ALERTING_BOTH,
  OPENING,
  WAITING_TAKE,
  TAKEN_COUNTDOWN,
  WAITING_RETURN,
  RETURNED_COUNTDOWN,
  CLOSING,
  COMPLETE
};

State state = NORMAL;
State lastDisplayState = NORMAL;  // Track last display state for optimization
int activeMed = 0;
int activeDoseIndex = 0;  // Which dose is being dispensed
unsigned long stateTimer = 0;
int countdownVal = 0;
bool buzzerOn = false;
unsigned long buzzerTimer = 0;
char lastDisplay[34];  // 2 lines of 16 chars + 2 newlines/null
bool handRemoved = false;  // Track hand removal for return detection

// Display cycling for NORMAL state
unsigned long displayCycleTimer = 0;
int displayMedIndex = 1;
int lastDisplayMedIndex = 0;  // Track last displayed medicine for refresh

// Message system for non-blocking display
struct Message {
  bool active;
  char line1[17];  // 16 chars + null terminator
  char line2[17];
  unsigned long endTime;
};
Message message = {false, "", "", 0};

// Keypad debouncing
unsigned long lastKeyTime = 0;
char lastKey = '\0';

// ==================== SETUP ====================
void setup() {
  Serial.begin(9600);
  Serial.setTimeout(100);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Med Dispenser");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");
  delay(1000);

  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  servo1.write(90);
  servo2.write(90);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(IR1_PIN, INPUT);
  pinMode(IR2_PIN, INPUT);

  // Initialize IR states
  ir1State.previouslyDetected = false;
  ir1State.currentState = (digitalRead(IR1_PIN) == LOW);
  ir2State.previouslyDetected = false;
  ir2State.currentState = (digitalRead(IR2_PIN) == LOW);
  ir1State.debounceTimer = millis();
  ir2State.debounceTimer = millis();

  loadEEPROM();

  lcd.clear();
  lcd.print("Waiting for time");
  lcd.setCursor(0, 1);
  lcd.print("via Serial...");

  lastMinuteCheck = millis();
  displayCycleTimer = millis();
}

// ==================== MAIN LOOP ====================
void loop() {
  checkTimeSync();
  updateLocalClock();
  updateIRState();

  // Handle message display - cancel immediately during active states
  if (message.active) {
    if (state != NORMAL) {
      message.active = false;  // Cancel message during active states
    } else if (millis() >= message.endTime) {
      message.active = false;  // Message expired
    }

    // If message still active, show it and skip rest of loop
    if (message.active) {
      updateDisplay();
      return;
    }
  }

  if (timeEntry.active) {
    handleTimeEntry();
    return;
  }

  if (menuState != MENU_NONE) {
    checkMenuTimeout();
    handleMenu();
    return;
  }

  handleKeypad();
  handleState();
  updateDisplay();

  // Conservative delays for maximum stability during demonstration
  if (state == NORMAL) {
    delay(100);  // Long delay saves power, prevents brownout
  } else if (state == WAITING_TAKE || state == WAITING_RETURN) {
    delay(80);  // Medium delay during IR monitoring
  } else if (state == ALERTING_MED1 || state == ALERTING_MED2 || state == ALERTING_BOTH) {
    delay(30);  // Shorter during alert (buzzer needs precise timing)
  } else {
    delay(50);  // Standard delay for other states
  }
}

// ==================== IR SENSOR HANDLING ====================
void updateIRState() {
  ir1State.currentState = (digitalRead(IR1_PIN) == LOW); // LOW = detected
  ir2State.currentState = (digitalRead(IR2_PIN) == LOW);
}

bool getIRTransition(int med) {
  IRState* ir = (med == 1) ? &ir1State : &ir2State;

  // Detect LOW transition (hand detected) - LOW to HIGH is not a transition
  if (ir->currentState && !ir->previouslyDetected) {
    ir->debounceTimer = millis();
    ir->previouslyDetected = ir->currentState;
    return true;
  }

  // Update state when it changes
  if (ir->currentState != ir->previouslyDetected) {
    ir->previouslyDetected = ir->currentState;
  }

  return false;
}

bool getIRReleased(int med) {
  IRState* ir = (med == 1) ? &ir1State : &ir2State;

  // Detect HIGH transition (hand removed) - only after debounce period
  if (!ir->currentState && ir->previouslyDetected) {
    if (millis() - ir->debounceTimer >= IR_DEBOUNCE_MS) {
      ir->previouslyDetected = ir->currentState;
      return true;
    }
  }

  // Update debounce timer when IR is detected
  if (ir->currentState) {
    ir->debounceTimer = millis();
  }

  // Update state when it changes
  if (ir->currentState != ir->previouslyDetected) {
    ir->previouslyDetected = ir->currentState;
  }

  return false;
}

bool isIRDetected(int med) {
  IRState* ir = (med == 1) ? &ir1State : &ir2State;
  return ir->currentState; // LOW = detected
}

// ==================== TIME SYNC ====================
void checkTimeSync() {
  if (Serial.available()) {
    char t[10];
    int len = Serial.readBytesUntil('\n', t, sizeof(t) - 1);
    t[len] = '\0';

    // Trim whitespace
    while (len > 0 && (t[len-1] == ' ' || t[len-1] == '\r' || t[len-1] == '\n')) {
      len--;
      t[len] = '\0';
    }

    if (len == 5 && t[2] == ':') {
      int h = (t[0] - '0') * 10 + (t[1] - '0');
      int m = (t[3] - '0') * 10 + (t[4] - '0');
      if (h >= 0 && h < 24 && m >= 0 && m < 60) {
        hour = h;
        minute = m;
        lastMinuteCheck = millis();
        // No display message - silent sync
      }
    }
  }
}

void updateLocalClock() {
  if (millis() - lastMinuteCheck >= 60000) {
    lastMinuteCheck = millis();
    minute++;
    if (minute >= 60) {
      minute = 0;
      hour++;
      if (hour >= 24) {
        hour = 0;
        resetDaily();
      }
    }
  }
}

// ==================== TIME ENTRY SYSTEM ====================
void startTimeEntry(int maxLength, int med, int doseIdx, bool isManual, bool isMed) {
  timeEntry.active = true;
  memset(timeEntry.input, 0, sizeof(timeEntry.input));
  timeEntry.inputLength = 0;
  timeEntry.maxLength = maxLength;
  timeEntry.targetMed = med;
  timeEntry.doseIndex = doseIdx;
  timeEntry.timeout = millis() + INPUT_TIMEOUT_MS;
  timeEntry.isManualTime = isManual;
  timeEntry.isMedTime = isMed;

  lcd.clear();
  if (isMed) {
    lcd.print("Med");
    lcd.print(med);
    if (doseIdx >= 0) {
      lcd.print("-D");
      lcd.print(doseIdx + 1);
    }
    lcd.print(" HHMM:");
  } else {
    lcd.print("Time HHMM:");
  }
  lcd.setCursor(0, 1);
  lcd.print("_#:back *:exit");
  updateTimeEntryDisplay();
}

void handleTimeEntry() {
  // Check timeout
  if (millis() > timeEntry.timeout) {
    cancelTimeEntry("Timeout!");
    return;
  }

  char k = getDebouncedKey();
  if (!k) return;

  // Reset timeout on any key
  timeEntry.timeout = millis() + INPUT_TIMEOUT_MS;

  if (k == '*') {
    cancelTimeEntry("Cancelled!");
  }
  else if (k == '#') {
    // Backspace
    if (timeEntry.inputLength > 0) {
      timeEntry.inputLength--;
      timeEntry.input[timeEntry.inputLength] = '\0';
      updateTimeEntryDisplay();
    }
  }
  else if (isdigit(k)) {
    if (timeEntry.inputLength < timeEntry.maxLength) {
      timeEntry.input[timeEntry.inputLength] = k;
      timeEntry.inputLength++;
      timeEntry.input[timeEntry.inputLength] = '\0';
      updateTimeEntryDisplay();

      // Validate as we go
      if (!validatePartialTime(timeEntry.input, timeEntry.inputLength)) {
        lcd.setCursor(timeEntry.inputLength - 1, 1);
        lcd.print("!");
        delay(200);
        timeEntry.inputLength--;
        timeEntry.input[timeEntry.inputLength] = '\0';
        updateTimeEntryDisplay();
      }

      // Auto-complete when full
      if (timeEntry.inputLength == timeEntry.maxLength) {
        completeTimeEntry();
      }
    }
  }
}

bool validatePartialTime(const char* input, int len) {
  if (len >= 1) {
    int firstDigit = input[0] - '0';
    if (firstDigit > 2) return false;
  }

  if (len >= 2) {
    int hours = (input[0] - '0') * 10 + (input[1] - '0');
    if (hours > 23) return false;
  }

  if (len >= 3) {
    int thirdDigit = input[2] - '0';
    if (thirdDigit > 5) return false;
  }

  if (len == 4) {
    int minutes = (input[2] - '0') * 10 + (input[3] - '0');
    if (minutes > 59) return false;
  }

  return true;
}

void updateTimeEntryDisplay() {
  lcd.setCursor(0, 1);
  for (int i = 0; i < 4; i++) {
    if (i < timeEntry.inputLength) {
      lcd.print(timeEntry.input[i]);
    } else {
      lcd.print("_");
    }
  }
}

void completeTimeEntry() {
  int h = (timeEntry.input[0] - '0') * 10 + (timeEntry.input[1] - '0');
  int m = (timeEntry.input[2] - '0') * 10 + (timeEntry.input[3] - '0');

  if (h >= 0 && h < 24 && m >= 0 && m < 60) {
    if (timeEntry.isMedTime) {
      DoseTime* doses = (timeEntry.targetMed == 1) ? med1Doses : med2Doses;
      int* count = (timeEntry.targetMed == 1) ? &med1Count : &med2Count;

      doses[timeEntry.doseIndex].hour = h;
      doses[timeEntry.doseIndex].minute = m;

      // Update active dose count
      *count = 0;
      for (int i = 0; i < MAX_DOSES; i++) {
        if (doses[i].hour != 0 || doses[i].minute != 0) {
          (*count)++;
        }
      }

      saveEEPROM();
      char msgLine1[17];
      snprintf(msgLine1, sizeof(msgLine1), "Med %d D%d Set", timeEntry.targetMed, timeEntry.doseIndex + 1);
      char msgLine2[6];
      formatTime(msgLine2, h, m);
      showMessage(msgLine1, msgLine2, 1500);
    } else {
      hour = h;
      minute = m;
      lastMinuteCheck = millis();
      char timeStr[6];
      formatTime(timeStr, h, m);
      showMessage("Time Set!", timeStr, 1000);
    }
    beep(1);
  } else {
    showMessage("Invalid!", "Retry", 1000);
    timeEntry.timeout = millis() + INPUT_TIMEOUT_MS;
    return;
  }

  timeEntry.active = false;
}

void cancelTimeEntry(const char* msg) {
  timeEntry.active = false;
  showMessage(msg, "", 500);
  state = NORMAL;
}

// ==================== MENU SYSTEM ====================
void enterMainMenu() {
  menuState = MENU_MAIN;
  menuTimeout = millis() + MENU_TIMEOUT_MS;
  lcd.clear();
  lcd.print("1:M1 2:M2 3:Test");
  lcd.setCursor(0, 1);
  lcd.print("4:Open 5:Reset");
}

void handleMenu() {
  char k = getDebouncedKey();
  if (!k) return;

  menuTimeout = millis() + MENU_TIMEOUT_MS;

  switch (menuState) {
    case MENU_MAIN:
      handleMainMenu(k);
      break;
    case MENU_MED_SELECT:
      handleMedSelect(k);
      break;
    case MENU_DOSE_SELECT:
      handleDoseSelect(k);
      break;
    case MENU_DOSE_ACTION:
      handleDoseAction(k);
      break;
    case MENU_TEST_SELECT:
      handleTestSelect(k);
      break;
    case MENU_OPEN_SELECT:
      handleOpenSelect(k);
      break;
    default:
      exitMenu();
  }
}

void handleMainMenu(char k) {
  if (k == '1') {
    menuSelection = 1;
    menuState = MENU_DOSE_SELECT;
    showDoseSelectMenu(1);
  }
  else if (k == '2') {
    menuSelection = 2;
    menuState = MENU_DOSE_SELECT;
    showDoseSelectMenu(2);
  }
  else if (k == '3') {
    menuState = MENU_TEST_SELECT;
    lcd.clear();
    lcd.print("Test 1 or 2?");
    lcd.setCursor(0, 1);
    lcd.print("*:cancel");
  }
  else if (k == '4') {
    menuState = MENU_OPEN_SELECT;
    lcd.clear();
    lcd.print("Open 1 or 2?");
    lcd.setCursor(0, 1);
    lcd.print("*:cancel");
    menuOpenedServo = 0;
  }
  else if (k == '5') {
    resetDaily();
    showMessage("Reset Complete!", "", 1000);
    beep(1);
    exitMenu();
  }
  else if (k == '*') {
    exitMenu();
  }
}

void showDoseSelectMenu(int med) {
  DoseTime* doses = (med == 1) ? med1Doses : med2Doses;
  int count = (med == 1) ? med1Count : med2Count;

  lcd.clear();
  lcd.print("Med");
  lcd.print(med);
  lcd.print(" Doses:");
  lcd.print(count);
  lcd.print("/");
  lcd.print(MAX_DOSES);
  lcd.setCursor(0, 1);
  lcd.print("1:");
  char timeBuf[6];
  formatTime(timeBuf, doses[0].hour, doses[0].minute);
  lcd.print(timeBuf);
  if (count > 1) lcd.print("+");
  lcd.print(" #:all");
}

void handleMedSelect(char k) {
  // Currently handled in main menu
}

void handleDoseSelect(char k) {
  DoseTime* doses = (menuSelection == 1) ? med1Doses : med2Doses;
  int count = (menuSelection == 1) ? med1Count : med2Count;

  if (k >= '1' && k <= '4') {
    int idx = k - '1';
    // Only allow selecting existing doses (0 to count-1)
    // Adding new doses is handled separately in the action menu
    if (idx >= 0 && idx < count) {
      timeEntry.doseIndex = idx;
      menuState = MENU_DOSE_ACTION;
      showDoseActionMenu(menuSelection, idx);
    }
  }
  else if (k == '#') {
    // Show all doses summary
    showDosesSummary(menuSelection);
  }
  else if (k == '*') {
    menuState = MENU_MAIN;
    enterMainMenu();
  }
}

void showDoseActionMenu(int med, int doseIdx) {
  DoseTime* doses = (med == 1) ? med1Doses : med2Doses;
  int* count = (menuSelection == 1) ? &med1Count : &med2Count;

  lcd.clear();
  lcd.print("M");
  lcd.print(med);
  lcd.print("-D");
  lcd.print(doseIdx + 1);
  lcd.print(" ");
  char timeBuf[6];
  formatTime(timeBuf, doses[doseIdx].hour, doses[doseIdx].minute);
  lcd.print(timeBuf);

  lcd.setCursor(0, 1);
  // Show status with symbol - use shorter version
  if (doses[doseIdx].taken) {
    lcd.print("v ");
  } else if (doses[doseIdx].missed) {
    lcd.print("X ");
  } else {
    lcd.print("  ");
  }

  // Show all options concisely - must fit in 16 chars
  // "v 1:Ed 2:Del *:back" = 17 chars - too long!
  // Need: "v 1:Ed 2:Del" = 12 chars ✓

  lcd.print("1:Ed");
  if (*count > 1) {
    lcd.print(" 2:Del");
  } else {
    lcd.print(" *:back");
  }
}

void handleDoseAction(char k) {
  DoseTime* doses = (menuSelection == 1) ? med1Doses : med2Doses;
  int* count = (menuSelection == 1) ? &med1Count : &med2Count;

  if (k == '1') {
    // Edit time
    menuState = MENU_TIME_ENTRY;
    startTimeEntry(4, menuSelection, timeEntry.doseIndex, false, true);
  }
  else if (k == '2') {
    // Delete dose
    if (*count > 1) {  // Must keep at least one dose
      doses[timeEntry.doseIndex].hour = 0;
      doses[timeEntry.doseIndex].minute = 0;
      doses[timeEntry.doseIndex].taken = false;
      doses[timeEntry.doseIndex].missed = false;

      // Shift remaining doses
      for (int i = timeEntry.doseIndex; i < MAX_DOSES - 1; i++) {
        doses[i] = doses[i + 1];
      }
      doses[MAX_DOSES - 1].hour = 0;
      doses[MAX_DOSES - 1].minute = 0;
      doses[MAX_DOSES - 1].taken = false;
      doses[MAX_DOSES - 1].missed = false;

      (*count)--;
      saveEEPROM();

      showMessage("Deleted!", "", 1000);
      menuState = MENU_DOSE_SELECT;
      showDoseSelectMenu(menuSelection);
    } else {
      // Can't delete - need at least one dose
      beep(2);
      showDoseActionMenu(menuSelection, timeEntry.doseIndex);
    }
  }
  else if (k == '#') {
    // View all doses summary
    showDosesSummary(menuSelection);
  }
  else if (k == '*') {
    menuState = MENU_DOSE_SELECT;
    showDoseSelectMenu(menuSelection);
  }
}

void showDosesSummary(int med) {
  DoseTime* doses = (med == 1) ? med1Doses : med2Doses;
  int count = (med == 1) ? med1Count : med2Count;

  // Use menu state for cycling through doses
  menuState = MENU_DOSE_ACTION;

  // Start from first dose
  timeEntry.doseIndex = 0;
  showDoseSummaryScreen(med, timeEntry.doseIndex);keyboard

  // Wait for user input or auto-cycle
  unsigned long cycleStart = millis();

  while (true) {
    // Check menu timeout
    if (millis() > menuTimeout) {
      timeEntry.doseIndex = 0;
      exitMenu();
      return;
    }

    char k = getDebouncedKey();

    if (k == '*') {
      // Exit
      timeEntry.doseIndex = 0;
      menuState = MENU_DOSE_SELECT;
      showDoseSelectMenu(med);
      return;
    }
    else if (k == '#') {
      // Next dose manually
      timeEntry.doseIndex = (timeEntry.doseIndex + 1) % count;
      showDoseSummaryScreen(med, timeEntry.doseIndex);
      cycleStart = millis();
    }

    // Auto-cycle every 3 seconds
    if (millis() - cycleStart >= 3000) {
      timeEntry.doseIndex = (timeEntry.doseIndex + 1) % count;
      showDoseSummaryScreen(med, timeEntry.doseIndex);
      cycleStart = millis();
    }

    updateDisplay();  // Keep display refreshed during blocking loop
    delay(50);
  }
}

void showDoseSummaryScreen(int med, int doseIdx) {
  DoseTime* doses = (med == 1) ? med1Doses : med2Doses;
  int count = (med == 1) ? med1Count : med2Count;

  lcd.clear();
  lcd.print("M");
  lcd.print(med);
  lcd.print("-D");
  lcd.print(doseIdx + 1);
  lcd.print("/");
  lcd.print(count);
  lcd.print(" ");
  char timeBuf[6];
  formatTime(timeBuf, doses[doseIdx].hour, doses[doseIdx].minute);
  lcd.print(timeBuf);

  lcd.setCursor(0, 1);

  // Show status
  if (doses[doseIdx].taken) {
    lcd.print("[v] Done");
  } else if (doses[doseIdx].missed) {
    lcd.print("[X] Missed");
  } else {
    lcd.print("[ ] Pending");
  }

  // Show navigation hint
  if (count > 1) {
    lcd.print(" #>next");
  }
  lcd.print(" *:exit");
}

void handleTestSelect(char k) {
  if (k == '1') {
    activeMed = 1;
    exitMenu();
    state = OPENING;
    stateTimer = millis();
  }
  else if (k == '2') {
    activeMed = 2;
    exitMenu();
    state = OPENING;
    stateTimer = millis();
  }
  else if (k == '*') {
    menuState = MENU_MAIN;
    enterMainMenu();
  }
}

void handleOpenSelect(char k) {
  if (k == '1' && menuOpenedServo == 0) {
    openServo(1);
    menuOpenedServo = 1;
    lcd.clear();
    lcd.print("Med1 OPEN");
    lcd.setCursor(0, 1);
    lcd.print("C:close *:exit");
  }
  else if (k == '2' && menuOpenedServo == 0) {
    openServo(2);
    menuOpenedServo = 2;
    lcd.clear();
    lcd.print("Med2 OPEN");
    lcd.setCursor(0, 1);
    lcd.print("C:close *:exit");
  }
  else if (k == 'C' && menuOpenedServo > 0) {
    closeServo(menuOpenedServo);
    exitMenu();
  }
  else if (k == '*') {
    if (menuOpenedServo > 0) {
      closeServo(menuOpenedServo);
    }
    exitMenu();
  }
}

void checkMenuTimeout() {
  if (millis() > menuTimeout) {
    if (menuOpenedServo > 0) {
      closeServo(menuOpenedServo);
    }
    showMessage("Menu timeout", "", 500);
    exitMenu();
  }
}

void exitMenu() {
  menuState = MENU_NONE;
  menuOpenedServo = 0;
  state = NORMAL;
}

// ==================== KEYPAD HANDLER ====================
// Get debounced key (prevents multiple reads of same key press)
char getDebouncedKey() {
  char k = keypad.getKey();
  if (!k) {
    lastKey = '\0';
    return '\0';
  }

  // Debounce: ignore same key within 500ms
  if (k == lastKey && millis() - lastKeyTime < 500) {
    return '\0';
  }

  lastKey = k;
  lastKeyTime = millis();
  return k;
}

void handleKeypad() {
  char k = getDebouncedKey();
  if (!k) return;

  if (state == ALERTING_MED1 || state == ALERTING_MED2 || state == ALERTING_BOTH) {
    if (k == 'D') {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerOn = false;

      if (state == ALERTING_MED1) {
        activeMed = 1;
      } else if (state == ALERTING_MED2) {
        activeMed = 2;
      } else if (state == ALERTING_BOTH) {
        lcd.clear();
        lcd.print("Press 1 or 2");
        lcd.setCursor(0, 1);
        lcd.print("for medicine");

        unsigned long selectTimeout = millis() + 10000;
        while (millis() < selectTimeout) {
          char t = getDebouncedKey();
          if (t == '1') { activeMed = 1; break; }
          if (t == '2') { activeMed = 2; break; }
          if (t == 'C') { state = NORMAL; return; }
          updateDisplay();  // Keep display refreshed during blocking wait
          delay(50);  // Longer delay for stability
        }

        if (activeMed == 0) {
          showMessage("Timeout!", "", 500);
          state = NORMAL;
          return;
        }
      }

      state = OPENING;
      stateTimer = millis();
    }

    if (k == 'C') {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerOn = false;

      if (state == ALERTING_MED1) {
        markAllDosesMissed(1);
      }
      if (state == ALERTING_MED2) {
        markAllDosesMissed(2);
      }
      if (state == ALERTING_BOTH) {
        markAllDosesMissed(1);
        markAllDosesMissed(2);
      }
      state = NORMAL;
    }
    return;
  }

  if (state == WAITING_TAKE || state == WAITING_RETURN) {
    if (k == 'C') {
      closeServo(activeMed);
      state = NORMAL;
    }
    return;
  }

  if (state == RETURNED_COUNTDOWN || state == TAKEN_COUNTDOWN) {
    if (k == 'D') {
      state = CLOSING;
      beep(1);
    }
    if (k == 'C') {
      closeServo(activeMed);
      state = NORMAL;
    }
    return;
  }

  if (state == NORMAL) {
    if (k == '*') {
      enterMainMenu();
    }
    if (k == 'C') {
      closeServo(1);
      closeServo(2);
    }
  }
}

// ==================== STATE MACHINE ====================
void handleState() {
  checkSchedules();

  switch (state) {
    case NORMAL:
      digitalWrite(BUZZER_PIN, LOW);
      break;

    case ALERTING_MED1:
    case ALERTING_MED2:
    case ALERTING_BOTH:
      if (millis() - buzzerTimer >= 300) {
        buzzerTimer = millis();
        buzzerOn = !buzzerOn;
        digitalWrite(BUZZER_PIN, buzzerOn ? HIGH : LOW);
      }
      checkMissedOverlap();
      break;

    case OPENING:
      openServo(activeMed);
      delay(500);  // Extra stabilization delay before state transition
      state = WAITING_TAKE;
      stateTimer = millis();
      break;

    case WAITING_TAKE:
      if (getIRTransition(activeMed)) {
        delay(200);  // Stabilization delay before state change
        state = TAKEN_COUNTDOWN;
        countdownVal = 10;
        stateTimer = millis();
        beep(1);
      }

      if (millis() - stateTimer >= 60000) {
        closeServo(activeMed);
        state = NORMAL;
      }

      delay(100);  // Longer delay for stability
      break;

    case TAKEN_COUNTDOWN:
      if (millis() - stateTimer >= 1000) {
        stateTimer = millis();
        countdownVal--;
      }

      if (countdownVal <= 0) {
        handRemoved = false;  // Reset when entering WAITING_RETURN
        state = WAITING_RETURN;
        stateTimer = millis();
      }

      delay(100);  // Longer delay for stability
      break;

    case WAITING_RETURN:
      if (!handRemoved && getIRReleased(activeMed)) {
        delay(200);  // Stabilization delay
        handRemoved = true;
      }

      if (handRemoved && getIRTransition(activeMed)) {
        delay(200);  // Stabilization delay before state change
        state = RETURNED_COUNTDOWN;
        countdownVal = 10;
        stateTimer = millis();
        beep(2);
        handRemoved = false;
      }

      if (millis() - stateTimer >= 60000) {
        closeServo(activeMed);
        state = NORMAL;
        handRemoved = false;
      }

      delay(100);  // Longer delay for stability
      break;

    case RETURNED_COUNTDOWN:
      if (millis() - stateTimer >= 1000) {
        stateTimer = millis();
        countdownVal--;
      }

      if (countdownVal <= 0) {
        delay(200);  // Stabilization before state change
        state = CLOSING;
        beep(1);
      }

      delay(100);  // Longer delay for stability
      break;

    case CLOSING:
      delay(200);  // Small delay before closing
      closeServo(activeMed);
      state = COMPLETE;
      stateTimer = millis();
      beep(3);
      break;

    case COMPLETE:
      if (millis() - stateTimer >= 2000) {
        DoseTime* doses = (activeMed == 1) ? med1Doses : med2Doses;
        int count = (activeMed == 1) ? med1Count : med2Count;
        if (activeDoseIndex >= 0 && activeDoseIndex < count) {
          doses[activeDoseIndex].taken = true;
        }
        state = NORMAL;
      }
      break;
  }
}

void checkSchedules() {
  if (state != NORMAL && state != ALERTING_MED1 && state != ALERTING_MED2 && state != ALERTING_BOTH) {
    return;
  }

  // Check med1 doses
  for (int i = 0; i < med1Count; i++) {
    if (!med1Doses[i].taken && !med1Doses[i].missed) {
      if (hour == med1Doses[i].hour && minute == med1Doses[i].minute) {
        if (state == ALERTING_MED2) {
          markAllDosesMissed(2);
          state = ALERTING_BOTH;
        } else {
          state = ALERTING_MED1;
          activeDoseIndex = i;
        }
        break;
      }
    }
  }

  // Check med2 doses
  for (int i = 0; i < med2Count; i++) {
    if (!med2Doses[i].taken && !med2Doses[i].missed) {
      if (hour == med2Doses[i].hour && minute == med2Doses[i].minute) {
        if (state == ALERTING_MED1) {
          markAllDosesMissed(1);
          state = ALERTING_BOTH;
        } else if (state == NORMAL) {
          state = ALERTING_MED2;
          activeDoseIndex = i;
        }
        break;
      }
    }
  }
}

void checkMissedOverlap() {
  if (state == ALERTING_MED1) {
    for (int i = 0; i < med2Count; i++) {
      if (!med2Doses[i].taken && hour == med2Doses[i].hour && minute == med2Doses[i].minute) {
        markAllDosesMissed(1);
        state = ALERTING_BOTH;
        break;
      }
    }
  }

  if (state == ALERTING_MED2) {
    for (int i = 0; i < med1Count; i++) {
      if (!med1Doses[i].taken && hour == med1Doses[i].hour && minute == med1Doses[i].minute) {
        markAllDosesMissed(2);
        state = ALERTING_BOTH;
        break;
      }
    }
  }
}

void markAllDosesMissed(int med) {
  DoseTime* doses = (med == 1) ? med1Doses : med2Doses;
  int count = (med == 1) ? med1Count : med2Count;

  for (int i = 0; i < count; i++) {
    if (!doses[i].taken) {
      doses[i].missed = true;
      doses[i].taken = true;
    }
  }
}

// ==================== DISPLAY ====================
void updateDisplay() {
  // Skip if message is active
  if (message.active) {
    return;
  }

  // Handle display cycling in NORMAL mode
  if (state == NORMAL) {
    if (millis() - displayCycleTimer >= 5000) {
      displayCycleTimer = millis();
      displayMedIndex = (displayMedIndex == 1) ? 2 : 1;
    }
  }

  bool needRefresh = false;

  // 1. State changed? Force refresh
  if (state != lastDisplayState) {
    lastDisplayState = state;
    needRefresh = true;
  }

  // 2. Home screen rotating? Force refresh
  if (state == NORMAL && displayMedIndex != lastDisplayMedIndex) {
    lastDisplayMedIndex = displayMedIndex;
    needRefresh = true;
  }

  // 3. Content changed? Need refresh
  char content[34];
  getDisplayContent(content, sizeof(content));
  if (strcmp(content, lastDisplay) != 0) {
    needRefresh = true;
  }

  // Nothing changed? Exit
  if (!needRefresh) return;

  // Do the refresh
  strcpy(lastDisplay, content);
  lcd.clear();
  lcd.home();

  // Print line 1
  char* newlinePos = strchr(content, '\n');
  if (newlinePos != nullptr) {
    *newlinePos = '\0';  // Temporarily terminate at newline
    lcd.print(content);
    lcd.setCursor(0, 1);
    lcd.print(newlinePos + 1);
  } else {
    lcd.print(content);
  }
}

void getDisplayContent(char* buffer, size_t bufferSize) {
  switch (state) {
    case NORMAL: {
      char line1[17];
      char line2[17];

      // Build line 1
      char timeStr[6];
      formatTime(timeStr, hour, minute);
      snprintf(line1, sizeof(line1), "%s Next: M%d", timeStr, displayMedIndex);

      // Build line 2
      DoseTime* doses = (displayMedIndex == 1) ? med1Doses : med2Doses;
      int count = (displayMedIndex == 1) ? med1Count : med2Count;

      bool foundNext = false;
      int currentMinutes = hour * 60 + minute;

      for (int i = 0; i < count; i++) {
        if (!doses[i].taken && !doses[i].missed) {
          int doseMinutes = doses[i].hour * 60 + doses[i].minute;
          if (doseMinutes > currentMinutes) {
            char doseTime[6];
            formatTime(doseTime, doses[i].hour, doses[i].minute);
            snprintf(line2, sizeof(line2), "%s (d%d)", doseTime, i + 1);
            foundNext = true;
            break;
          }
        }
      }

      if (!foundNext && count > 0) {
        char doseTime[6];
        formatTime(doseTime, doses[0].hour, doses[0].minute);
        snprintf(line2, sizeof(line2), "%s Tom", doseTime);
      }

      snprintf(buffer, bufferSize, "%s\n%s", line1, line2);
      break;
    }

    case ALERTING_MED1: {
      if (activeDoseIndex >= 0 && activeDoseIndex < med1Count) {
        char timeStr[6];
        formatTime(timeStr, med1Doses[activeDoseIndex].hour, med1Doses[activeDoseIndex].minute);
        snprintf(buffer, bufferSize, "MED1 D%d %s\nD:Open C:Skip", activeDoseIndex + 1, timeStr);
      } else {
        snprintf(buffer, bufferSize, "MED1 ALERT\nD:Open C:Skip");
      }
      break;
    }

    case ALERTING_MED2: {
      if (activeDoseIndex >= 0 && activeDoseIndex < med2Count) {
        char timeStr[6];
        formatTime(timeStr, med2Doses[activeDoseIndex].hour, med2Doses[activeDoseIndex].minute);
        snprintf(buffer, bufferSize, "MED2 D%d %s\nD:Open C:Skip", activeDoseIndex + 1, timeStr);
      } else {
        snprintf(buffer, bufferSize, "MED2 ALERT\nD:Open C:Skip");
      }
      break;
    }

    case ALERTING_BOTH:
      snprintf(buffer, bufferSize, "MED1+2 ALERT!\nD:Select C:Skip");
      break;

    case OPENING:
      snprintf(buffer, bufferSize, "Opening Box...\nWait...");
      break;

    case WAITING_TAKE:
      snprintf(buffer, bufferSize, "TAKE MEDICINE\nWaiting...");
      break;

    case TAKEN_COUNTDOWN:
      snprintf(buffer, bufferSize, "TAKEN\n%ds", countdownVal);
      break;

    case WAITING_RETURN:
      snprintf(buffer, bufferSize, "PUT BACK NOW\nWaiting...");
      break;

    case RETURNED_COUNTDOWN:
      snprintf(buffer, bufferSize, "RETURNED\n%ds", countdownVal);
      break;

    case CLOSING:
      snprintf(buffer, bufferSize, "CLOSING...\nWait...");
      break;

    case COMPLETE:
      snprintf(buffer, bufferSize, "COMPLETE!\nThank you!");
      break;

    default:
      snprintf(buffer, bufferSize, "UNKNOWN\nState %d", state);
      break;
  }
}

void showMessage(const char* line1, const char* line2, unsigned long duration) {
  // Use non-blocking message system
  showMessageNonBlocking(line1, line2, duration);

  // For backward compatibility, wait for message to complete
  unsigned long waitStart = millis();
  while (message.active && millis() - waitStart < duration + 100) {
    updateDisplay();  // Keep display updated
    delay(50);
  }
}

void showMessageNonBlocking(const char* line1, const char* line2, unsigned long duration) {
  message.active = true;
  strncpy(message.line1, line1, sizeof(message.line1) - 1);
  message.line1[sizeof(message.line1) - 1] = '\0';
  strncpy(message.line2, line2, sizeof(message.line2) - 1);
  message.line2[sizeof(message.line2) - 1] = '\0';
  message.endTime = millis() + duration;

  lcd.clear();
  lcd.print(message.line1);
  if (strlen(message.line2) > 0) {
    lcd.setCursor(0, 1);
    lcd.print(message.line2);
  }
}

// ==================== SERVO CONTROL ====================
void openServo(int med) {
  Servo* s = (med == 1) ? &servo1 : &servo2;

  // Gradual opening with power management
  for (int pos = SERVO_MAX_ANGLE; pos >= SERVO_MIN_ANGLE; pos--) {
    s->write(pos + 90);
    delay(SERVO_SPEED_MS);

    // Extra delay every 3 steps for power stability
    if ((pos - SERVO_MIN_ANGLE) % 3 == 0) {
      delay(50);
    }

    // Check for emergency stop
    if (getDebouncedKey() == 'C') {
      // Stay at current position
      break;
    }
  }

  // Long stabilization delay after movement
  delay(500);
}

void closeServo(int med) {
  Servo* s = (med == 1) ? &servo1 : &servo2;

  // Gradual closing with power management
  for (int pos = SERVO_MIN_ANGLE; pos <= SERVO_MAX_ANGLE; pos++) {
    s->write(pos + 90);
    delay(SERVO_SPEED_MS);

    // Extra delay every 3 steps for power stability
    if ((SERVO_MAX_ANGLE - pos) % 3 == 0) {
      delay(50);
    }

    // Check for emergency stop
    if (getDebouncedKey() == 'C') {
      // Stay at current position
      break;
    }
  }

  // Long stabilization delay after movement
  delay(500);
}

// ==================== UTILITIES ====================
void beep(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
  }
}

void formatTime(char* buffer, int h, int m) {
  // Buffer must be at least 6 bytes (HH:MM + null)
  // Validate inputs to prevent invalid output
  if (h < 0) h = 0;
  if (h > 23) h = 23;
  if (m < 0) m = 0;
  if (m > 59) m = 59;

  buffer[0] = '0' + (h / 10);
  buffer[1] = '0' + (h % 10);
  buffer[2] = ':';
  buffer[3] = '0' + (m / 10);
  buffer[4] = '0' + (m % 10);
  buffer[5] = '\0';
}

// For backward compatibility
void resetDaily() {
  for (int i = 0; i < MAX_DOSES; i++) {
    med1Doses[i].taken = false;
    med1Doses[i].missed = false;
    med2Doses[i].taken = false;
    med2Doses[i].missed = false;
  }
}

void loadEEPROM() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) == EEPROM_MAGIC) {
    // Load med1 doses
    for (int i = 0; i < MAX_DOSES; i++) {
      med1Doses[i].hour = EEPROM.read(i * 4);
      med1Doses[i].minute = EEPROM.read(i * 4 + 1);
      med1Doses[i].taken = EEPROM.read(i * 4 + 2);
      med1Doses[i].missed = EEPROM.read(i * 4 + 3);

      // Validate loaded values
      if (med1Doses[i].hour < 0 || med1Doses[i].hour > 23) med1Doses[i].hour = 0;
      if (med1Doses[i].minute < 0 || med1Doses[i].minute > 59) med1Doses[i].minute = 0;
    }

    // Load med2 doses
    for (int i = 0; i < MAX_DOSES; i++) {
      med2Doses[i].hour = EEPROM.read(16 + i * 4);
      med2Doses[i].minute = EEPROM.read(16 + i * 4 + 1);
      med2Doses[i].taken = EEPROM.read(16 + i * 4 + 2);
      med2Doses[i].missed = EEPROM.read(16 + i * 4 + 3);

      // Validate loaded values
      if (med2Doses[i].hour < 0 || med2Doses[i].hour > 23) med2Doses[i].hour = 0;
      if (med2Doses[i].minute < 0 || med2Doses[i].minute > 59) med2Doses[i].minute = 0;
    }

    // Count active doses
    med1Count = 0;
    for (int i = 0; i < MAX_DOSES; i++) {
      if (med1Doses[i].hour != 0 || med1Doses[i].minute != 0) {
        med1Count++;
      }
    }

    med2Count = 0;
    for (int i = 0; i < MAX_DOSES; i++) {
      if (med2Doses[i].hour != 0 || med2Doses[i].minute != 0) {
        med2Count++;
      }
    }

    // Validate and fix
    if (med1Count == 0) {
      med1Doses[0].hour = 8;
      med1Doses[0].minute = 0;
      med1Count = 1;
    }
    if (med2Count == 0) {
      med2Doses[0].hour = 20;
      med2Doses[0].minute = 0;
      med2Count = 1;
    }
  }
}

void saveEEPROM() {
  // Save med1 doses (bytes 0-15)
  for (int i = 0; i < MAX_DOSES; i++) {
    // Validate and clamp hour/minute values before saving
    int h = med1Doses[i].hour;
    int m = med1Doses[i].minute;
    if (h < 0 || h > 23) h = 0;
    if (m < 0 || m > 59) m = 0;

    EEPROM.write(i * 4, h);
    EEPROM.write(i * 4 + 1, m);
    EEPROM.write(i * 4 + 2, med1Doses[i].taken ? 1 : 0);
    EEPROM.write(i * 4 + 3, med1Doses[i].missed ? 1 : 0);
  }

  // Save med2 doses (bytes 16-31)
  for (int i = 0; i < MAX_DOSES; i++) {
    // Validate and clamp hour/minute values before saving
    int h = med2Doses[i].hour;
    int m = med2Doses[i].minute;
    if (h < 0 || h > 23) h = 0;
    if (m < 0 || m > 59) m = 0;

    EEPROM.write(16 + i * 4, h);
    EEPROM.write(16 + i * 4 + 1, m);
    EEPROM.write(16 + i * 4 + 2, med2Doses[i].taken ? 1 : 0);
    EEPROM.write(16 + i * 4 + 3, med2Doses[i].missed ? 1 : 0);
  }

  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);

#if defined(ESP32) || defined(ESP8266)
  EEPROM.commit();  // Required for ESP32/ESP8266
#endif
}
