/*
 * ============================================================
 *  SMART PARKING PRO v3 — Full Reservation + Gate Control + Payment Dashboard
 * ============================================================
 *
 *  NEW IN v3:
 *   • Entry gate now asks "Booked? Y or N" first
 *   • Walk-in vehicle number saved and linked to payment dashboard
 *   • Payment dashboard page at /guest (no login needed)
 *     - Customer types vehicle number → personal dashboard opens
 *     - Live parking timer + live fare (updates every 30s)
 *     - PAY NOW button greyed out while car in slot (IR=1)
 *     - PAY NOW activates when car leaves slot (IR=0)
 *   • Demo payment: static UPI QR image shown → "I HAVE PAID" button
 *   • Exit gate: operator types TOKEN command → enter token → gate opens
 *   • mDNS: accessible at http://smartparking.local
 *
 *  GATE FLOW:
 *   Entry: Car detected → "Booked? Y/N" →
 *          Y: Enter vehicle number → match found → gate opens
 *          N: Enter vehicle number → slot assigned → gate opens
 *             → LCD shows "Scan QR sticker"
 *   Exit:  Operator types TOKEN in Serial Monitor → validated → gate opens
 *
 *  SERIAL MONITOR COMMANDS:
 *   STATUS  → print all slot states
 *   TOKENS  → list active tokens
 *
 *  SLOT STATES:
 *   0 = available (green)
 *   1 = occupied  (red)
 *   2 = reserved  (yellow)
 *
 *  HARDWARE PINS (ESP32):
 *   Entry Gate IR       → GPIO 13
 *   Entry Gate Servo    → GPIO 12
 *   Exit  Gate IR       → GPIO 27
 *   Exit  Gate Servo    → GPIO 26
 *   LCD SDA             → GPIO 21
 *   LCD SCL             → GPIO 22
 *   LCD: 16x2 I2C (address 0x27)
 *   RC522 SDA (SS)      → GPIO 5  (SPI CS)
 *   RC522 RST           → GPIO 0
 *   RC522 SCK           → GPIO 18 (SPI CLK)
 *   RC522 MOSI          → GPIO 23 (SPI MOSI)
 *   RC522 MISO          → GPIO 19 (SPI MISO)
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

const char* ssid     = "Redmi 12 5G";
const char* password = "99999999";

WebServer server(80);

String adminUsername = "admin";
String adminPassword = "1234";
String userUsername  = "user";
String userPassword  = "user123";

// ── LCD 16x2 I2C ────────────────────────────────────────────
#define LCD_ADDR    0x27
#define LCD_COLS    16
#define LCD_ROWS    2
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// ── Servos ────────────────────────────────────────────────
Servo entryServo;
Servo exitServo;
#define ENTRY_SERVO_PIN  16
#define EXIT_SERVO_PIN   15
#define SERVO_OPEN       90
#define SERVO_CLOSED     0


#define GATE_OPEN_MS     4000   // gate stays open 4 seconds

unsigned long entryGateOpenTime = 0;
unsigned long exitGateOpenTime  = 0;
bool entryGateOpen = false;
bool exitGateOpen  = false;

// ── Entry gate ultrasonic ─────────────────────────────────
#define ENTRY_TRIG_PIN  13
#define ENTRY_ECHO_PIN  14
#define ENTRY_DETECT_CM 30
#define ENTRY_DEBOUNCE_MS 300


// ── Exit gate IR ────────────────────────────────────────────
#define IR_EXIT_GATE      27
#define EXIT_DEBOUNCE_MS  300

// ── IR Sensors (slot occupancy only) ─────────────────────
#define IR_SLOT_B1      32   // slot B1 occupancy sensor
#define IR_SLOT_B2      33   // slot B2 occupancy sensor
#define IR_SLOT_B3      34   // slot B3 occupancy sensor
#define IR_SLOT_B4      35   // slot B4 occupancy sensor
#define IR_ACTIVE_LOW   true
#define IR_DEBOUNCE_MS  300


// Entry gate IR debounce
bool          irEntryLastStable   = false;
bool          irEntryPending      = false;
bool          irEntryPendingVal   = false;
unsigned long irEntryPendingStart = 0;

// Exit gate IR debounce

// Exit gate IR debounce
bool          irExitLastStable   = false;
bool          irExitPending      = false;
bool          irExitPendingVal   = false;
unsigned long irExitPendingStart = 0;

// Slot B1 IR debounce
bool          irB1LastStable    = false;
bool          irB1Pending       = false;
bool          irB1PendingVal    = false;
unsigned long irB1PendingStart  = 0;

// Slot B2 IR debounce
bool          irB2LastStable    = false;
bool          irB2Pending       = false;
bool          irB2PendingVal    = false;
unsigned long irB2PendingStart  = 0;

// Slot B3 IR debounce
bool          irB3LastStable    = false;
bool          irB3Pending       = false;
bool          irB3PendingVal    = false;
unsigned long irB3PendingStart  = 0;

// Slot B4 IR debounce
bool          irB4LastStable    = false;
bool          irB4Pending       = false;
bool          irB4PendingVal    = false;
unsigned long irB4PendingStart  = 0;

// ── Serial Monitor Entry State ────────────────────────────
enum SerialState { SER_IDLE, SER_WAIT_BOOKED, SER_WAIT_VEHICLE, SER_WAIT_WALKIN_VEH, SER_WAIT_WALKIN, SER_WAIT_TOKEN };
SerialState serState          = SER_IDLE;
String      serPendingVehicle = "";
String      serPendingVehicleConfirm = "";
bool        serIsBooked       = false;
unsigned long serStartMs      = 0;
#define SER_TIMEOUT_MS  30000   // 30 sec timeout

// ── RC522 RFID ────────────────────────────────────────────

// ── Gate Token System ─────────────────────────────────────
#define MAX_TOKENS 10
struct GateToken {
  String token;
  String bookingId;
  String vehicleNumber;
  unsigned long createdAt;
  unsigned long expiresAt;    // 5 minutes
  bool   used;
  bool   valid;
};
GateToken gateTokens[MAX_TOKENS];
int tokenCount = 0;
int tokenSeq   = 1;

// ── Slot Arrays  0=free  1=occupied  2=reserved ───────────
int fourSlots[4] = {0, 0, 0, 0};  // B1, B2, B3, B4

// ── Reservation struct ────────────────────────────────────
struct Reservation {
  String bookingId;
  String vehicleNumber;
  String vehicleType;
  int    slotIndex;
  String slotName;
  String ownerName;
  String ownerPhone;
  unsigned long reservedAt;
  unsigned long scheduledStart;
  unsigned long scheduledEnd;
  unsigned long actualEntry;
  unsigned long actualExit;
  float  amount;
  bool   isPaid;
  String status;
};

#define MAX_RESERVATIONS 30
Reservation reservations[MAX_RESERVATIONS];
int resCount   = 0;
int bookingSeq = 1;

// ── Notification queue ─────────────────────────────────────
#define NOTIF_MAX 12
String notifQueue[NOTIF_MAX];
int notifHead  = 0;
int notifCount = 0;

// ── Guest activity feed (for admin live panel) ────────────
#define GFEED_MAX 20
struct GuestEvent {
  String type;       // "paid" | "token" | "walkin" | "view"
  String vehicle;
  String slot;
  String detail;     // amount, token, etc.
  unsigned long ts;  // millis() at event
};
GuestEvent guestFeed[GFEED_MAX];
int gfeedHead  = 0;
int gfeedCount = 0;

void pushGuestEvent(String type, String vehicle, String slot, String detail) {
  GuestEvent& e = guestFeed[gfeedHead % GFEED_MAX];
  e.type    = type;
  e.vehicle = vehicle;
  e.slot    = slot;
  e.detail  = detail;
  e.ts      = millis();
  gfeedHead++;
  if (gfeedCount < GFEED_MAX) gfeedCount++;
}

void pushNotif(String msg) {
  notifQueue[notifHead % NOTIF_MAX] = msg;
  notifHead++;
  if (notifCount < NOTIF_MAX) notifCount++;
}

// ── Last receipt ───────────────────────────────────────────
struct Receipt {
  String bookingId, vehicle, slotName, ownerName;
  unsigned long durationMs;
  int    durationMin, billSlots;
  float  amount;
  bool   valid;
};
Receipt lastReceipt = {"","","","",0,0,0,0,false};

// ── Stats ──────────────────────────────────────────────────
struct Stats {
  int fourOcc, fourFree, fourRes;
  int totalRev;
};
Stats stats;

// ─────────────────────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────────────────────
String makeBid() {
  String id = "BK";
  if (bookingSeq < 10)       id += "00";
  else if (bookingSeq < 100) id += "0";
  id += String(bookingSeq++);
  return id;
}

float calcFare(unsigned long durationMs) {
  unsigned long mins  = durationMs / 60000UL;
  if (mins == 0) mins = 1;
  unsigned long slots = (mins + 29) / 30;
  return slots * 10.0f;
}

void updateStats() {
  stats.fourOcc=0; stats.fourFree=0; stats.fourRes=0; stats.totalRev=0;
  for (int i=0;i<2;i++) {
  }
  for (int i=0;i<4;i++) {
    if (fourSlots[i]==1) stats.fourOcc++;
    if (fourSlots[i]==2) stats.fourRes++;
  }
  stats.fourFree = 4 - stats.fourOcc - stats.fourRes;
  for (int i=0;i<resCount;i++)
    if (reservations[i].isPaid) stats.totalRev += (int)reservations[i].amount;
}

void checkExpiredReservations() {
  unsigned long now = millis();
  for (int i=0;i<resCount;i++) {
    Reservation& r = reservations[i];
    if (r.status == "upcoming" || r.status == "active") {
      if (r.scheduledEnd > 0 && now > r.scheduledEnd + 900000UL) {
        if (r.status == "upcoming") {
          r.status = "expired";
          if (r.vehicleType=="four" && r.slotIndex<4) fourSlots[r.slotIndex] = 0;
          pushNotif("EXP:" + r.bookingId + ":" + r.vehicleNumber);
        }
      }
    }
  }
}

// ── Find free slot ─────────────────────────────────────────
int findFreeSlot(String type) {
  for (int i=0;i<4;i++) if (fourSlots[i]==0) return i;
  return -1;
}

bool isParkingFull() {
  int freeFour = 0;
  for (int i=0;i<4;i++) if (fourSlots[i]==0) freeFour++;
  return (freeFour == 0);
}

// ─────────────────────────────────────────────────────────
//  GATE TOKEN HELPERS
// ─────────────────────────────────────────────────────────
String generateToken(String bookingId, String vehicleNum) {
  // Clean expired/used tokens first
  for (int i=0;i<tokenCount;i++) {
    if (gateTokens[i].used || millis() > gateTokens[i].expiresAt) {
      gateTokens[i].valid = false;
    }
  }

  // Generate a unique token string
  String tok = "TK" + String(tokenSeq++, HEX);
  tok.toUpperCase();
  tok += String(millis() % 9999, HEX);
  tok.toUpperCase();

  // Store token
  int idx = tokenCount % MAX_TOKENS;
  gateTokens[idx].token       = tok;
  gateTokens[idx].bookingId   = bookingId;
  gateTokens[idx].vehicleNumber = vehicleNum;
  gateTokens[idx].createdAt   = millis();
  gateTokens[idx].expiresAt   = millis() + 600000UL; // 10 minutes
  gateTokens[idx].used        = false;
  gateTokens[idx].valid       = true;
  if (tokenCount < MAX_TOKENS) tokenCount++;

  Serial.println("[TOKEN] Generated: " + tok + " for " + bookingId);
  return tok;
}

bool validateToken(String tok) {
  for (int i=0;i<tokenCount;i++) {
    if (gateTokens[i].token == tok &&
        gateTokens[i].valid &&
        !gateTokens[i].used &&
        millis() < gateTokens[i].expiresAt) {
      gateTokens[i].used = true;
      Serial.println("[TOKEN] Valid: " + tok + " -> " + gateTokens[i].bookingId);
      return true;
    }
  }
  Serial.println("[TOKEN] Invalid or expired: " + tok);
  return false;
}

// ─────────────────────────────────────────────────────────
//  GATE CONTROL
// ─────────────────────────────────────────────────────────

// ── Exit gate feedback helpers ────────────────────────────
void openEntryGate() {
  entryServo.write(SERVO_OPEN);
  entryGateOpen     = true;
  entryGateOpenTime = millis();
  Serial.println("[GATE] Entry OPEN");
}

void closeEntryGate() {
  entryServo.write(SERVO_CLOSED);
  entryGateOpen = false;
  Serial.println("[GATE] Entry CLOSED");
}

void openExitGate() {
  exitServo.write(SERVO_OPEN);
  exitGateOpen     = true;
  exitGateOpenTime = millis();
  Serial.println("[GATE] Exit OPEN");
}

void closeExitGate() {
  exitServo.write(SERVO_CLOSED);
  exitGateOpen = false;
  Serial.println("[GATE] Exit CLOSED");
}

void checkGateTimers() {
  if (entryGateOpen && millis() - entryGateOpenTime >= GATE_OPEN_MS) {
    closeEntryGate();
  }
  if (exitGateOpen && millis() - exitGateOpenTime >= GATE_OPEN_MS) {
    closeExitGate();
  }
}

// ─────────────────────────────────────────────────────────
//  LCD HELPERS
// ─────────────────────────────────────────────────────────
// lcdPrint(line1, line2, line3)
// 16x2 LCD — line1 on row 0, line2 on row 1
// line3 is shown briefly by scrolling line2 off (best effort)
void lcdPrint(String line1, String line2 = "", String line3 = "") {
  lcd.clear();
  // Row 0 — line1 truncated to 16 chars
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  // Row 1 — prefer line2, fall back to line3 if line2 empty
  String row1 = line2.length() ? line2 : line3;
  if (row1.length()) {
    lcd.setCursor(0, 1);
    lcd.print(row1.substring(0, 16));
  }
  // If all 3 lines given, show line3 after 2s on row 1
  if (line2.length() && line3.length()) {
    delay(2000);
    lcd.setCursor(0, 1);
    lcd.print("                "); // clear row 1
    lcd.setCursor(0, 1);
    lcd.print(line3.substring(0, 16));
  }
}

void lcdWelcome() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  SMART PARKING ");
  lcd.setCursor(0, 1);
  lcd.print("  -- WELCOME! -- ");
}



// ─────────────────────────────────────────────────────────
//  SERIAL MONITOR ENTRY FLOW
// ─────────────────────────────────────────────────────────
void startSerialEntry() {
  serState     = SER_WAIT_BOOKED;
  serStartMs   = millis();
  serPendingVehicle = "";
  serIsBooked = false;
  Serial.println("--------------------------------------------");
  Serial.println("[ENTRY] Car detected at entry gate!");
  Serial.println("[ENTRY] Is this a PRE-BOOKED customer?");
  Serial.println("[ENTRY] Type  Y = Yes (booked)   N = No (walk-in):");
  Serial.print("> ");
  lcdPrint("Pre-Booked?", "Y=Yes  N=No");
}

void processSerialInput() {
  // Timeout check
  if (serState != SER_IDLE && millis() - serStartMs > SER_TIMEOUT_MS) {
    Serial.println("\n[ENTRY] Timeout — no input received.");
    serState = SER_IDLE;
    serPendingVehicle = "";
    serPendingVehicleConfirm = "";
    lcdWelcome();
    return;
  }

  while (Serial.available()) {
    char c = (char)Serial.read();

    // Special commands (only when IDLE)
    if (serState == SER_IDLE) {
      static String idleCmd = "";
      if (c == '\r') { continue; }
      if (c == '\n') {
        idleCmd.trim();
        idleCmd.toUpperCase();
        if (idleCmd == "STATUS") printStatus();
        else if (idleCmd == "TOKENS") printTokens();
        else if (idleCmd == "TOKEN") {
          serState   = SER_WAIT_TOKEN;
          serStartMs = millis();
          Serial.println("[EXIT] Enter token to open exit gate:");
          Serial.print("> ");
        }
        idleCmd = "";
      } else {
        idleCmd += c;
      }
      continue;
    }

    if (c == '\r') { continue; }
    if (c == '\n') {
      String line = serPendingVehicle;
      line.trim();
      line.toUpperCase();
      serPendingVehicle = "";

      // ── State: waiting for Y or N (booked?) ──
      if (serState == SER_WAIT_BOOKED) {
        Serial.println(line);
        if (line == "Y" || line == "YES") {
          serIsBooked = true;
          serState = SER_WAIT_VEHICLE;
          serStartMs = millis();
          Serial.println("[ENTRY] Pre-booked. Enter vehicle number:");
          Serial.print("> ");
          lcdPrint("Enter Veh No:", "(Serial Mon.)");
        } else if (line == "N" || line == "NO") {
          serIsBooked = false;
          serState = SER_WAIT_WALKIN_VEH;
          serStartMs = millis();
          Serial.println("[ENTRY] Walk-in. Enter vehicle number:");
          Serial.print("> ");
          lcdPrint("Enter Veh No:", "(Walk-in)");
        } else {
          Serial.println("[ENTRY] Invalid. Type Y=Booked  N=Walk-in:");
          Serial.print("> ");
        }

      // ── State: waiting for vehicle number (booked path) ──
      } else if (serState == SER_WAIT_VEHICLE) {
        if (line.length() == 0) {
          Serial.println("[ENTRY] Empty input — cancelled.");
          serState = SER_IDLE;
          lcdWelcome();
        } else {
          Serial.println(line);
          processVehicleEntry(line);
        }

      // ── State: waiting for vehicle number (walk-in path) ──
      } else if (serState == SER_WAIT_WALKIN_VEH) {
        if (line.length() == 0) {
          Serial.println("[ENTRY] Empty input — cancelled.");
          serState = SER_IDLE;
          lcdWelcome();
        } else {
          Serial.println(line);
          // Check parking full first
          if (isParkingFull()) {
            lcdPrint("Sorry! Parking", "FULL right now");
            Serial.println("[ENTRY] Parking FULL. Gate denied.");
            Serial.println("--------------------------------------------");
            serState = SER_IDLE;
            delay(3000);
            lcdWelcome();
          } else {
            serPendingVehicleConfirm = line;
            serState = SER_WAIT_WALKIN;
            serStartMs = millis();
            lcdPrint("Walk-in OK?", "W=Yes  N=No");
            Serial.println("[ENTRY] Walk-in for: " + line);
            Serial.println("[ENTRY] Confirm? W=Yes  N=No:");
            Serial.print("> ");
          }
        }

      // ── State: confirm walk-in ──
      } else if (serState == SER_WAIT_WALKIN) {
        Serial.println(line);
        if (line == "W" || line == "Y" || line == "YES") {
          processWalkIn(serPendingVehicleConfirm);
          serState = SER_IDLE;
          serPendingVehicleConfirm = "";
        } else if (line == "N" || line == "NO") {
          Serial.println("[ENTRY] Walk-in declined.");
          serState = SER_IDLE;
          serPendingVehicleConfirm = "";
          lcdWelcome();
        } else {
          Serial.println("[ENTRY] Invalid. Type W=Yes  N=No:");
          Serial.print("> ");
        }
      } else if (serState == SER_WAIT_TOKEN) {
        String tok = line;
        tok.trim();
        tok.toUpperCase();
        if (tok.length() == 0) {
          Serial.println("[EXIT] Empty input — cancelled.");
          serState = SER_IDLE;
        } else if (validateToken(tok)) {
          Serial.println("[EXIT] Token valid — opening exit gate!");
          lcdPrint("Exit Gate", "Opening...");
          openExitGate();
          serState = SER_IDLE;
        } else {
          Serial.println("[EXIT] Invalid or expired token — gate denied.");
          lcdPrint("Invalid Token", "Gate Denied");
          serState = SER_IDLE;
        }
      }

    } else if (c == 8 || c == 127) {
      if (serPendingVehicle.length() > 0)
        serPendingVehicle.remove(serPendingVehicle.length() - 1);
    } else if (isPrintable(c)) {
      serPendingVehicle += c;
    }
  }
}
// end of processSerialInput

// ─────────────────────────────────────────────────────────
//  VEHICLE ENTRY LOGIC
// ─────────────────────────────────────────────────────────
void processVehicleEntry(String vehicleNum) {
  vehicleNum.toUpperCase();
  vehicleNum.trim();

  // Search reservations for this vehicle
  for (int i=0;i<resCount;i++) {
    Reservation& r = reservations[i];
    if ((r.vehicleNumber == vehicleNum) &&
        (r.status == "upcoming" || r.status == "active") &&
        r.actualEntry == 0) {
      // BOOKED customer found
      r.actualEntry = millis();
      r.status      = "active";
      if (r.vehicleType=="four" && r.slotIndex<4) fourSlots[r.slotIndex] = 1;
      openEntryGate();
      lcdPrint("Welcome!", "Slot: " + r.slotName);
      pushNotif("ENTRY:" + r.bookingId + ":" + vehicleNum);
      Serial.println("[ENTRY] Booked customer: " + vehicleNum + " → Slot " + r.slotName);
      Serial.println("[ENTRY] Gate OPENING. Billing started.");
      Serial.println("--------------------------------------------");
      serState = SER_IDLE;
      delay(3000);
      lcdWelcome();
      return;
    }
  }

  // No booking found for this vehicle number
  lcdPrint("No Booking Found", "Try Walk-in: N");
  Serial.println("[ENTRY] No booking found for: " + vehicleNum);
  Serial.println("[ENTRY] Ask customer to re-enter or use walk-in (N).");
  Serial.println("--------------------------------------------");
  serState = SER_IDLE;
  delay(3000);
  lcdWelcome();
}

void processWalkIn(String vehicleNum) {
  // Find first free slot (prefer 2-wheeler)
  int slotIdx  = -1;
  String stype = "";
  String sname = "";

  slotIdx = findFreeSlot("four");
  if (slotIdx >= 0) {
    stype = "four";
    sname = "B" + String(slotIdx + 1);
    fourSlots[slotIdx] = 1;
  }

  if (slotIdx < 0) {
    lcdPrint("Parking Full!", "Sorry!");
    delay(3000);
    lcdWelcome();
    return;
  }

  // Auto-create booking
  String bid = makeBid();
  unsigned long now = millis();
  Reservation& r = reservations[resCount];
  r.bookingId      = bid;
  r.vehicleNumber  = vehicleNum;
  r.vehicleType    = stype;
  r.slotIndex      = slotIdx;
  r.slotName       = sname;
  r.ownerName      = "Walk-in";
  r.ownerPhone     = "";
  r.reservedAt     = now;
  r.scheduledStart = now;
  r.scheduledEnd   = now + 3600000UL; // 1 hour placeholder
  r.actualEntry    = now;
  r.actualExit     = 0;
  r.amount         = 0;
  r.isPaid         = false;
  r.status         = "active";
  resCount++;

  openEntryGate();
  lcdPrint("Walk-In OK!", "Slot: " + sname);
  pushNotif("WALKIN:" + bid + ":" + vehicleNum);
  pushGuestEvent("walkin", vehicleNum, sname, bid);
  Serial.println("[ENTRY] Walk-in created: " + bid + " -> " + sname);
  Serial.println("[ENTRY] Guest dashboard: http://smartparking.local/guest");
  delay(3000);
  lcdPrint("Scan QR sticker", "for your", "dashboard!");
  delay(3000);
  lcdWelcome();
}

// ─────────────────────────────────────────────────────────
//  SERIAL MONITOR DEBUG COMMANDS
// ─────────────────────────────────────────────────────────
void printStatus() {
  Serial.println("============ SLOT STATUS ============");
  const char* stateStr[] = {"FREE","OCCUPIED","RESERVED"};
  Serial.println("4-Wheeler: B1=" + String(stateStr[fourSlots[0]]) +
                 "  B2=" + String(stateStr[fourSlots[1]]) +
                 "  B3=" + String(stateStr[fourSlots[2]]) +
                 "  B4=" + String(stateStr[fourSlots[3]]));
  Serial.println("Active bookings: " + String(resCount));
  for (int i=0;i<resCount;i++) {
    Reservation& r = reservations[i];
    Serial.println("  " + r.bookingId + " | " + r.vehicleNumber + " | " +
                   r.slotName + " | " + r.status + " | paid=" + String(r.isPaid));
  }
  Serial.println("Entry gate: " + String(entryGateOpen?"OPEN":"CLOSED"));
  Serial.println("Exit  gate: " + String(exitGateOpen?"OPEN":"CLOSED"));
  Serial.println("=====================================");
}

void printTokens() {
  Serial.println("============ ACTIVE TOKENS ============");
  int found = 0;
  for (int i=0;i<tokenCount;i++) {
    GateToken& t = gateTokens[i];
    if (t.valid && !t.used && millis() < t.expiresAt) {
      long remaining = (t.expiresAt - millis()) / 1000;
      Serial.println("  " + t.token + " | " + t.bookingId + " | " +
                     t.vehicleNumber + " | expires in " + String(remaining) + "s");
      found++;
    }
  }
  if (found == 0) Serial.println("  No active tokens.");
  Serial.println("=======================================");
}



// ─────────────────────────────────────────────────────────
//  IR SENSOR HANDLERS
// ─────────────────────────────────────────────────────────
bool readIR(int pin) {
  int raw = digitalRead(pin);
  return IR_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
}


// Entry gate IR — triggers Serial Monitor input


// ─────────────────────────────────────────────────────────
//  ULTRASONIC HELPER
// ─────────────────────────────────────────────────────────
float readUltrasonicCm(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 30000); // 30ms timeout
  if (duration == 0) return 999.0;               // no echo = no object
  return duration * 0.0343 / 2.0;
}



// ─────────────────────────────────────────────────────────
//  SLOT OCCUPANCY IR HANDLERS — B1 & B2
// ─────────────────────────────────────────────────────────
void handleSlotB1IR() {
  bool detected = readIR(IR_SLOT_B1);
  if (detected != irB1LastStable) {
    if (!irB1Pending || irB1PendingVal != detected) {
      irB1Pending = true; irB1PendingVal = detected; irB1PendingStart = millis();
    }
    if (millis() - irB1PendingStart >= IR_DEBOUNCE_MS) {
      irB1LastStable = detected; irB1Pending = false;
      if (detected) {
        if (fourSlots[0] == 0 || fourSlots[0] == 2) {
          bool wasRes = (fourSlots[0] == 2);
          fourSlots[0] = 1;
          for (int i=0;i<resCount;i++) {
            if (reservations[i].slotName=="B1" &&
                (reservations[i].status=="upcoming"||reservations[i].status=="active")) {
              reservations[i].status = "active";
              if (reservations[i].actualEntry == 0) reservations[i].actualEntry = millis();
            }
          }
          pushNotif(wasRes ? "RES_OCC:B1" : "OCC:B1");
          Serial.println("[IR] Slot B1 → OCCUPIED" + String(wasRes?" (was reserved)":""));
        }
      } else {
        if (fourSlots[0] == 1) {
          fourSlots[0] = 0;
          for (int i=0;i<resCount;i++) {
            if (reservations[i].slotName=="B1" && reservations[i].status=="active") {
              unsigned long entry = reservations[i].actualEntry > 0 ? reservations[i].actualEntry : reservations[i].reservedAt;
              unsigned long elapsed = millis() - entry;
              unsigned long mins = elapsed / 60000UL; if (mins == 0) mins = 1;
              unsigned long slots = (mins + 29) / 30;
              reservations[i].amount = slots * 10.0f;
              reservations[i].status = "completed";
              reservations[i].actualExit = millis();
              lcdPrint("Pay: Rs" + String((int)reservations[i].amount), "Scan QR to pay", "& exit");
              Serial.println("[IR] Slot B1 → CLEAR  Fare: Rs" + String((int)reservations[i].amount));
            }
          }
          pushNotif("CLEAR:B1");
        }
      }
      Serial.print("[IR] Slot B1 → "); Serial.println(detected ? "OCC" : "CLEAR");
    }
  } else { irB1Pending = false; }
}

void handleSlotB2IR() {
  bool detected = readIR(IR_SLOT_B2);
  if (detected != irB2LastStable) {
    if (!irB2Pending || irB2PendingVal != detected) {
      irB2Pending = true; irB2PendingVal = detected; irB2PendingStart = millis();
    }
    if (millis() - irB2PendingStart >= IR_DEBOUNCE_MS) {
      irB2LastStable = detected; irB2Pending = false;
      if (detected) {
        if (fourSlots[1] == 0 || fourSlots[1] == 2) {
          bool wasRes = (fourSlots[1] == 2);
          fourSlots[1] = 1;
          for (int i=0;i<resCount;i++) {
            if (reservations[i].slotName=="B2" &&
                (reservations[i].status=="upcoming"||reservations[i].status=="active")) {
              reservations[i].status = "active";
              if (reservations[i].actualEntry == 0) reservations[i].actualEntry = millis();
            }
          }
          pushNotif(wasRes ? "RES_OCC:B2" : "OCC:B2");
          Serial.println("[IR] Slot B2 → OCCUPIED" + String(wasRes?" (was reserved)":""));
        }
      } else {
        if (fourSlots[1] == 1) {
          fourSlots[1] = 0;
          for (int i=0;i<resCount;i++) {
            if (reservations[i].slotName=="B2" && reservations[i].status=="active") {
              unsigned long entry = reservations[i].actualEntry > 0 ? reservations[i].actualEntry : reservations[i].reservedAt;
              unsigned long elapsed = millis() - entry;
              unsigned long mins = elapsed / 60000UL; if (mins == 0) mins = 1;
              unsigned long slots = (mins + 29) / 30;
              reservations[i].amount = slots * 10.0f;
              reservations[i].status = "completed";
              reservations[i].actualExit = millis();
              lcdPrint("Pay: Rs" + String((int)reservations[i].amount), "Scan QR to pay", "& exit");
              Serial.println("[IR] Slot B2 → CLEAR  Fare: Rs" + String((int)reservations[i].amount));
            }
          }
          pushNotif("CLEAR:B2");
        }
      }
      Serial.print("[IR] Slot B2 → "); Serial.println(detected ? "OCC" : "CLEAR");
    }
  } else { irB2Pending = false; }
}

void handleSlotB3IR() {
  bool detected = readIR(IR_SLOT_B3);
  if (detected != irB3LastStable) {
    if (!irB3Pending || irB3PendingVal != detected) {
      irB3Pending = true; irB3PendingVal = detected; irB3PendingStart = millis();
    }
    if (millis() - irB3PendingStart >= IR_DEBOUNCE_MS) {
      irB3LastStable = detected; irB3Pending = false;
      if (detected) {
        if (fourSlots[2] == 0 || fourSlots[2] == 2) {
          bool wasRes = (fourSlots[2] == 2);
          fourSlots[2] = 1;
          for (int i=0;i<resCount;i++) {
            if (reservations[i].slotName=="B3" &&
                (reservations[i].status=="upcoming"||reservations[i].status=="active")) {
              reservations[i].status = "active";
              if (reservations[i].actualEntry == 0) reservations[i].actualEntry = millis();
            }
          }
          pushNotif(wasRes ? "RES_OCC:B3" : "OCC:B3");
          Serial.println("[IR] Slot B3 → OCCUPIED" + String(wasRes?" (was reserved)":""));
        }
      } else {
        if (fourSlots[2] == 1) {
          fourSlots[2] = 0;
          for (int i=0;i<resCount;i++) {
            if (reservations[i].slotName=="B3" && reservations[i].status=="active") {
              unsigned long entry = reservations[i].actualEntry > 0 ? reservations[i].actualEntry : reservations[i].reservedAt;
              unsigned long elapsed = millis() - entry;
              unsigned long mins = elapsed / 60000UL; if (mins == 0) mins = 1;
              unsigned long slots = (mins + 29) / 30;
              reservations[i].amount = slots * 10.0f;
              reservations[i].status = "completed";
              reservations[i].actualExit = millis();
              lcdPrint("Pay: Rs" + String((int)reservations[i].amount), "Scan QR to pay", "& exit");
              Serial.println("[IR] Slot B3 → CLEAR  Fare: Rs" + String((int)reservations[i].amount));
            }
          }
          pushNotif("CLEAR:B3");
        }
      }
      Serial.print("[IR] Slot B3 → "); Serial.println(detected ? "OCC" : "CLEAR");
    }
  } else { irB3Pending = false; }
}

void handleSlotB4IR() {
  bool detected = readIR(IR_SLOT_B4);
  if (detected != irB4LastStable) {
    if (!irB4Pending || irB4PendingVal != detected) {
      irB4Pending = true; irB4PendingVal = detected; irB4PendingStart = millis();
    }
    if (millis() - irB4PendingStart >= IR_DEBOUNCE_MS) {
      irB4LastStable = detected; irB4Pending = false;
      if (detected) {
        if (fourSlots[3] == 0 || fourSlots[3] == 2) {
          bool wasRes = (fourSlots[3] == 2);
          fourSlots[3] = 1;
          for (int i=0;i<resCount;i++) {
            if (reservations[i].slotName=="B4" &&
                (reservations[i].status=="upcoming"||reservations[i].status=="active")) {
              reservations[i].status = "active";
              if (reservations[i].actualEntry == 0) reservations[i].actualEntry = millis();
            }
          }
          pushNotif(wasRes ? "RES_OCC:B4" : "OCC:B4");
          Serial.println("[IR] Slot B4 → OCCUPIED" + String(wasRes?" (was reserved)":""));
        }
      } else {
        if (fourSlots[3] == 1) {
          fourSlots[3] = 0;
          for (int i=0;i<resCount;i++) {
            if (reservations[i].slotName=="B4" && reservations[i].status=="active") {
              unsigned long entry = reservations[i].actualEntry > 0 ? reservations[i].actualEntry : reservations[i].reservedAt;
              unsigned long elapsed = millis() - entry;
              unsigned long mins = elapsed / 60000UL; if (mins == 0) mins = 1;
              unsigned long slots = (mins + 29) / 30;
              reservations[i].amount = slots * 10.0f;
              reservations[i].status = "completed";
              reservations[i].actualExit = millis();
              lcdPrint("Pay: Rs" + String((int)reservations[i].amount), "Scan QR to pay", "& exit");
              Serial.println("[IR] Slot B4 → CLEAR  Fare: Rs" + String((int)reservations[i].amount));
            }
          }
          pushNotif("CLEAR:B4");
        }
      }
      Serial.print("[IR] Slot B4 → "); Serial.println(detected ? "OCC" : "CLEAR");
    }
  } else { irB4Pending = false; }
}

void handleEntryGateIR() {
  float dist    = readUltrasonicCm(ENTRY_TRIG_PIN, ENTRY_ECHO_PIN);
  bool detected = (dist < ENTRY_DETECT_CM);
  if (detected != irEntryLastStable) {
    if (!irEntryPending || irEntryPendingVal != detected) {
      irEntryPending = true; irEntryPendingVal = detected; irEntryPendingStart = millis();
    }
    if (millis() - irEntryPendingStart >= ENTRY_DEBOUNCE_MS) {
      irEntryLastStable = detected; irEntryPending = false;
      if (detected && (serState == SER_IDLE) && !entryGateOpen) {
        Serial.println("[ENTRY] Car at " + String(dist,1) + " cm — starting Serial input");
        startSerialEntry();
      } else if (!detected) {
        Serial.println("[ENTRY] Entry clear");
      }
    }
  } else { irEntryPending = false; }
}



// ─────────────────────────────────────────────────────────
//  GUEST PAGE (no login — walk-in self service)
//  Styled to match booking dashboard exactly
// ─────────────────────────────────────────────────────────
const char* GUEST_PAGE = R"html(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Smart Parking — Payment Dashboard</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Rajdhani:wght@400;500;600;700&family=Exo+2:wght@300;400;600;700;800&display=swap');
*{margin:0;padding:0;box-sizing:border-box;}
:root{--g:#10b981;--bg:#050a14;--card:rgba(10,18,36,0.88);--border:rgba(16,185,129,0.18);--tx:#e2e8f0;--tm:#64748b;}
body{font-family:'Exo 2',sans-serif;background:var(--bg);color:var(--tx);height:100vh;overflow:hidden;display:flex;flex-direction:column;}
.bg{position:fixed;inset:0;z-index:0;pointer-events:none;}
.bg-grid{position:absolute;inset:0;background-image:linear-gradient(rgba(16,185,129,.03)1px,transparent 1px),linear-gradient(90deg,rgba(16,185,129,.03)1px,transparent 1px);background-size:50px 50px;}
.bg-g{position:absolute;border-radius:50%;filter:blur(110px);}
.bg-g1{width:500px;height:500px;background:rgba(16,185,129,.06);top:-160px;right:-70px;}
.bg-g2{width:350px;height:350px;background:rgba(5,150,105,.04);bottom:0;left:-40px;}
.app{display:flex;flex-direction:column;height:100vh;position:relative;z-index:1;}
.hdr{background:linear-gradient(180deg,rgba(5,10,20,.98),rgba(8,14,28,.95));border-bottom:1px solid var(--border);padding:12px 18px;position:relative;overflow:hidden;}
.hdr::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;background:linear-gradient(90deg,#059669,#10b981,#34d399,#10b981,#059669);background-size:300% 100%;animation:hdrShimmer 4s linear infinite;}
@keyframes hdrShimmer{0%{background-position:100% 0}100%{background-position:-200% 0}}
.hdr-top{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;}
.hdr-brand{font-family:'Rajdhani',sans-serif;font-size:19px;font-weight:700;letter-spacing:2px;background:linear-gradient(135deg,#34d399,#6ee7b7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}
.guest-pill{display:flex;align-items:center;gap:6px;background:rgba(245,158,11,.1);border:1px solid rgba(245,158,11,.25);padding:4px 11px;border-radius:16px;font-size:10px;color:#fbbf24;font-weight:700;}
.gdot{width:6px;height:6px;background:#f59e0b;border-radius:50%;animation:blink 1.5s ease-in-out infinite;}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.15}}
.fare-strip{display:flex;align-items:center;gap:10px;background:rgba(245,158,11,.07);border:1px solid rgba(245,158,11,.18);border-radius:10px;padding:10px 14px;margin-bottom:14px;font-size:11px;color:#fde68a;}
.fare-strip strong{font-family:'Rajdhani',sans-serif;font-size:13px;letter-spacing:.5px;}
#tc{position:fixed;top:14px;right:14px;z-index:9999;display:flex;flex-direction:column;gap:8px;pointer-events:none;}
.toast{background:rgba(6,12,24,.96);backdrop-filter:blur(20px);border-radius:12px;padding:12px 16px;border-left:4px solid var(--g);box-shadow:0 8px 32px rgba(0,0,0,.6);font-size:12px;color:var(--tx);max-width:280px;pointer-events:all;animation:tIn .35s cubic-bezier(.34,1.56,.64,1);display:flex;align-items:flex-start;gap:10px;}
.toast.tw{border-left-color:#ef4444;}.toast.ty{border-left-color:#f59e0b;}
.ti{font-size:18px;flex-shrink:0;}.tt{font-weight:700;font-family:'Rajdhani',sans-serif;font-size:13px;letter-spacing:.4px;margin-bottom:2px;}.tm2{font-size:11px;color:#94a3b8;}
@keyframes tIn{from{opacity:0;transform:translateX(55px)}to{opacity:1;transform:none}}
.content{flex:1;overflow-y:auto;padding:14px 14px 6px;}
.content::-webkit-scrollbar{width:3px;}
.content::-webkit-scrollbar-thumb{background:rgba(16,185,129,.2);border-radius:2px;}
.screen{display:none;max-width:520px;margin:0 auto;animation:fadeUp .35s ease;}
.screen.active{display:block;}
@keyframes fadeUp{from{opacity:0;transform:translateY(16px)}to{opacity:1;transform:none}}
.fw{padding:6px 0 14px;}
.fc{background:var(--card);backdrop-filter:blur(18px);border:1px solid var(--border);border-radius:16px;padding:22px;box-shadow:0 18px 48px rgba(0,0,0,.4);}
.fc h2{font-family:'Rajdhani',sans-serif;font-size:19px;font-weight:700;letter-spacing:1.8px;margin-bottom:18px;color:#34d399;}
.fg{margin-bottom:13px;}
.fg label{display:block;font-size:10px;font-weight:700;color:var(--tm);text-transform:uppercase;letter-spacing:1.5px;margin-bottom:6px;}
.fg input{width:100%;padding:11px 13px;background:rgba(5,10,20,.65);border:1.5px solid rgba(16,185,129,.16);border-radius:9px;color:var(--tx);font-size:13px;font-family:'Exo 2',sans-serif;transition:all .3s;text-transform:uppercase;}
.fg input:focus{outline:none;border-color:#10b981;box-shadow:0 0 0 3px rgba(16,185,129,.07);}
.msg{padding:10px 13px;border-radius:9px;margin-bottom:11px;font-size:11px;border-left:4px solid;display:none;}
.msg.show{display:block;}
.msg.err{background:rgba(239,68,68,.09);color:#fca5a5;border-color:#ef4444;}
.msg.suc{background:rgba(16,185,129,.09);color:#86efac;border-color:#10b981;}
.fb{padding:12px;border:none;border-radius:9px;cursor:pointer;font-family:'Exo 2',sans-serif;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.8px;transition:all .25s;width:100%;}
.fb.cg{background:linear-gradient(135deg,#059669,#10b981);color:#fff;}
.fb.cam{background:linear-gradient(135deg,#d97706,#f59e0b);color:#fff;}
.fb.bk{background:rgba(15,25,50,.55);color:#64748b;border:1.5px solid rgba(16,185,129,.16);}
.fb.dis{background:rgba(15,25,50,.35);color:#334155;border:1.5px solid rgba(59,130,246,.1);cursor:not-allowed;}
.fb:hover:not(.dis){transform:translateY(-2px);filter:brightness(1.1);}
.br{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:16px;}
/* Booking card style */
.bk-card{background:var(--card);border:1px solid var(--border);border-radius:13px;padding:15px;position:relative;overflow:hidden;margin-bottom:12px;}
.bk-card::before{content:'';position:absolute;left:0;top:0;bottom:0;width:3px;}
.bk-card.ac::before{background:#10b981;}
.bk-card.co::before{background:#f59e0b;}
.bk-card.pd::before{background:#34d399;}
.bk-top{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:9px;}
.bk-id{font-family:'Rajdhani',sans-serif;font-size:16px;font-weight:700;}
.bk-badge{font-size:9px;font-weight:700;padding:3px 9px;border-radius:10px;text-transform:uppercase;letter-spacing:.8px;}
.bk-badge.ac{background:rgba(16,185,129,.14);color:#34d399;border:1px solid rgba(16,185,129,.18);}
.bk-badge.co{background:rgba(245,158,11,.14);color:#fbbf24;border:1px solid rgba(245,158,11,.18);}
.bk-badge.pd{background:rgba(16,185,129,.2);color:#6ee7b7;border:1px solid rgba(16,185,129,.3);}
.bk-rows{display:grid;grid-template-columns:1fr 1fr;gap:5px;margin-bottom:10px;}
.bk-row{font-size:11px;}
.bk-rl{color:var(--tm);font-size:9px;text-transform:uppercase;letter-spacing:.5px;margin-bottom:1px;}
.bk-rv{color:var(--tx);font-weight:600;}
/* Timer and fare inside bk-card */
.timer-box{background:rgba(5,10,20,.7);border:1px solid rgba(59,130,246,.2);border-radius:9px;padding:12px;text-align:center;margin-bottom:10px;}
.tl{font-size:9px;color:var(--tm);text-transform:uppercase;letter-spacing:.9px;margin-bottom:5px;}
.tv{font-family:'Rajdhani',sans-serif;font-size:32px;font-weight:700;color:#f59e0b;letter-spacing:2px;}
.fbox{background:rgba(16,185,129,.06);border:1px solid rgba(16,185,129,.2);border-radius:9px;padding:12px;text-align:center;margin-bottom:10px;}
.fl{font-size:9px;color:var(--tm);text-transform:uppercase;letter-spacing:.9px;margin-bottom:5px;}
.fv{font-family:'Rajdhani',sans-serif;font-size:30px;font-weight:700;color:#34d399;}
.fbd{font-size:10px;color:#64748b;margin-top:3px;}
.ibox{border-radius:9px;padding:11px 13px;margin-bottom:13px;font-size:11px;border-left:3px solid;}
.ibox.b{background:rgba(59,130,246,.07);border-color:#3b82f6;color:#93c5fd;}
.ibox.a{background:rgba(245,158,11,.07);border-color:#f59e0b;color:#fde68a;}
.ibox.g{background:rgba(16,185,129,.07);border-color:#10b981;color:#86efac;}
/* UPI payment card */
.upi-card{background:white;border-radius:13px;padding:18px;text-align:center;margin-bottom:13px;}
.upi-title{font-family:'Rajdhani',sans-serif;font-size:15px;font-weight:700;color:#1e293b;letter-spacing:1px;margin-bottom:3px;}
.upi-id{font-size:11px;color:#64748b;margin-bottom:12px;}
.upi-qr{width:160px;height:160px;margin:0 auto 10px;background:linear-gradient(135deg,#f0fdf4,#dcfce7);border:2px solid #10b981;border-radius:10px;display:flex;align-items:center;justify-content:center;font-size:55px;}
.upi-amt{font-family:'Rajdhani',sans-serif;font-size:26px;font-weight:700;color:#059669;}
/* Token display */
.token-box{background:rgba(245,158,11,.07);border:1.5px solid rgba(245,158,11,.25);border-radius:9px;padding:16px;text-align:center;margin-bottom:10px;}
.token-lbl{font-size:9px;color:var(--tm);text-transform:uppercase;letter-spacing:.9px;margin-bottom:8px;}
.token-val{font-family:'Rajdhani',sans-serif;font-size:36px;font-weight:700;color:#f59e0b;letter-spacing:4px;}
.token-exp{font-size:10px;color:#f59e0b;font-weight:600;margin-top:6px;}
.bnav{display:grid;grid-template-columns:1fr;background:rgba(5,9,18,.98);border-top:1px solid var(--border);}
</style></head><body>
<div id="tc"></div>
<div class="bg"><div class="bg-grid"></div><div class="bg-g bg-g1"></div><div class="bg-g bg-g2"></div></div>
<div class="app">

<!-- Header -->
<div class="hdr">
  <div class="hdr-top">
    <div class="hdr-brand">🅿️ SMART PARKING</div>
    <div class="guest-pill"><div class="gdot"></div>PAYMENT</div>
  </div>
  <div class="fare-strip"><span style="font-size:16px">💡</span><div><strong>FARE: ₹10 / 30 MIN</strong> · Min ₹10 · Pay when slot IR goes clear</div></div>
</div>

<div class="content">

<!-- LOOKUP SCREEN -->
<div class="screen active" id="lookupScreen">
<div class="fw"><div class="fc">
  <h2>🚗 FIND YOUR BOOKING</h2>
  <div class="msg err" id="lookupErr"></div>
  <div class="ibox b">📱 Scan QR sticker at entry gate · Enter your vehicle number below</div>
  <div class="fg">
    <label>Your Vehicle Number</label>
    <input type="text" id="vehInput" placeholder="MH01AB1234" autocomplete="off">
  </div>
  <button class="fb cg" onclick="findBooking()">🔍 FIND MY BOOKING</button>
</div></div>
</div>

<!-- DASHBOARD SCREEN -->
<div class="screen" id="dashScreen">
  <div class="bk-card ac" id="bkCard">
    <div class="bk-top">
      <div class="bk-id" id="dBid">BK001</div>
      <div class="bk-badge ac" id="dBadge">ACTIVE ●</div>
    </div>
    <div class="bk-rows">
      <div class="bk-row"><div class="bk-rl">Vehicle</div><div class="bk-rv" id="dVeh">--</div></div>
      <div class="bk-row"><div class="bk-rl">Slot</div><div class="bk-rv" id="dSlot">--</div></div>
      <div class="bk-row"><div class="bk-rl">Type</div><div class="bk-rv" id="dType">--</div></div>
      <div class="bk-row"><div class="bk-rl">Status</div><div class="bk-rv" id="dStatus">Parked</div></div>
    </div>
    <div class="timer-box">
      <div class="tl">⏱️ PARKING DURATION</div>
      <div class="tv" id="timerDisp">00:00:00</div>
    </div>
    <div class="fbox">
      <div class="fl">💰 CURRENT FARE</div>
      <div class="fv" id="fareDisp">₹0</div>
      <div class="fbd" id="fareNote">₹10 per 30 min · updates every 30s</div>
    </div>
    <div class="ibox b" id="statusMsg">🚗 Vehicle is parked — fare updates every 30 seconds</div>
    <button class="fb dis" id="payNowBtn" onclick="goToPay()">💳 PAY NOW (available after slot clears)</button>
  </div>
</div>

<!-- PAYMENT SCREEN -->
<div class="screen" id="payScreen">
<div class="fw"><div class="fc">
  <h2>💳 PAY &amp; GET EXIT TOKEN</h2>
  <div class="msg err" id="payErr"></div>
  <div class="fbox">
    <div class="fl">💰 AMOUNT DUE</div>
    <div class="fv" id="payAmtDisp">₹0</div>
  </div>
  <div class="upi-card">
    <div class="upi-title">📱 Scan &amp; Pay via UPI</div>
    <div class="upi-id">UPI ID: parking@upi</div>
    <div class="upi-qr">📱</div>
    <div class="upi-amt" id="upiAmt">₹0</div>
  </div>
  <div class="ibox a">⚠️ After scanning and paying, tap the confirm button below.</div>
  <div class="br">
    <button class="fb cg" onclick="confirmPayment()">✅ I HAVE PAID</button>
    <button class="fb bk" onclick="sw('dashScreen')">← BACK</button>
  </div>
</div></div>
</div>

<!-- TOKEN SCREEN -->
<div class="screen" id="tokenScreen">
<div class="fw"><div class="fc">
  <h2>✅ PAYMENT CONFIRMED</h2>
  <div class="ibox g">✅ Payment received! Use the token below to open the exit gate.</div>
  <div class="bk-rows" style="margin-bottom:12px;">
    <div class="bk-row"><div class="bk-rl">Vehicle</div><div class="bk-rv" id="tVeh">--</div></div>
    <div class="bk-row"><div class="bk-rl">Amount Paid</div><div class="bk-rv" id="tAmt" style="color:#34d399">₹0</div></div>
  </div>
  <div class="token-box">
    <div class="token-lbl">🔑 YOUR EXIT TOKEN</div>
    <div class="token-val" id="tokenDisp">------</div>
    <div class="token-exp">⏳ Expires in: <span id="tokenCountdown">600</span>s</div>
  </div>
  <div class="ibox b">
    🚗 Drive to exit gate<br>
    💳 Tap your RFID card on the reader<br>
    🔑 Gate opens automatically ✅
  </div>
  <button class="fb cam" id="regenBtn" onclick="regenToken()" style="display:none;margin-bottom:10px;">🔄 GET NEW TOKEN (FREE — already paid)</button>
  <button class="fb bk" onclick="sw('dashScreen')">← BACK TO DASHBOARD</button>
</div></div>
</div>

</div><!-- /content -->
<div class="bnav">
  <div style="padding:10px;text-align:center;font-size:10px;color:#1e3a5f;">
    Lost token? Just tap your RFID card at the exit gate
  </div>
</div>
</div><!-- /app -->

<script>
let currentBookingId='',currentVehicle='',currentEntryMs=0,currentNowMs=0,timerInterval=null,pollInterval=null,tokenTimer=null,tokenSecs=600;

function toast(icon,title,msg,type='o'){const c=document.getElementById('tc');const t=document.createElement('div');t.className='toast t'+type[0];t.innerHTML=`<div class="ti">${icon}</div><div><div class="tt">${title}</div><div class="tm2">${msg}</div></div>`;c.appendChild(t);setTimeout(()=>t.remove(),4500);}
function showMsg(id,msg,type){const el=document.getElementById(id);el.textContent=(type==='err'?'❌ ':'✅ ')+msg;el.className='msg show '+type;setTimeout(()=>el.classList.remove('show'),6000);}
function sw(id){document.querySelectorAll('.screen').forEach(s=>s.classList.remove('active'));document.getElementById(id).classList.add('active');}
function pad(n){return String(n).padStart(2,'0');}
function calcFare(ms){const mins=Math.max(1,Math.ceil(ms/60000));return Math.ceil(mins/30)*10;}

function findBooking(){
  const veh=document.getElementById('vehInput').value.trim().toUpperCase();
  if(!veh){showMsg('lookupErr','Please enter your vehicle number','err');return;}
  fetch('/api/guest-lookup?vehicle='+encodeURIComponent(veh))
    .then(r=>r.json()).then(d=>{
      if(d.found){
        currentBookingId=d.bookingId; currentVehicle=d.vehicleNumber;
        currentEntryMs=d.entryMs; currentNowMs=d.nowMs;
        document.getElementById('dBid').textContent=d.bookingId;
        document.getElementById('dVeh').textContent=d.vehicleNumber;
        document.getElementById('tVeh').textContent=d.vehicleNumber;
        document.getElementById('dSlot').textContent=d.slotName;
        document.getElementById('dType').textContent='🚗 4-Wheeler';
        updateCard(d);
        startTimers(d);
        sw('dashScreen');
        toast('🚗','BOOKING FOUND',d.bookingId+' · Slot '+d.slotName,'o');
      } else {
        showMsg('lookupErr',d.message||'No booking found for this vehicle number','err');
      }
    }).catch(()=>showMsg('lookupErr','Connection error. Try again.','err'));
}

function updateCard(d){
  const card=document.getElementById('bkCard');
  const badge=document.getElementById('dBadge');
  const statusMsg=document.getElementById('statusMsg');
  const btn=document.getElementById('payNowBtn');
  const fareNote=document.getElementById('fareNote');
  document.getElementById('dStatus').textContent=d.status==='active'?'Parked':d.status==='completed'?'Left Slot':'Paid';
  if(d.isPaid){
    card.className='bk-card pd'; badge.className='bk-badge pd'; badge.textContent='PAID ✓';
    statusMsg.className='ibox g'; statusMsg.textContent='✅ Payment confirmed — show exit token at gate';
    btn.className='fb cg'; btn.textContent='📱 SHOW EXIT TOKEN'; btn.onclick=showExistingToken;
    document.getElementById('fareDisp').textContent='₹'+d.amount;
    fareNote.textContent='Paid · '+d.durationMin+' min parked';
  } else if(d.status==='completed'){
    card.className='bk-card co'; badge.className='bk-badge co'; badge.textContent='PAY NOW ⚠️';
    statusMsg.className='ibox a'; statusMsg.textContent='⚠️ Slot cleared — fare locked, please pay to exit';
    btn.className='fb cam'; btn.textContent='💳 PAY NOW — ₹'+d.amount; btn.onclick=goToPay;
    document.getElementById('fareDisp').textContent='₹'+d.amount;
    fareNote.textContent='Fare locked · '+d.durationMin+' min parked';
  } else {
    card.className='bk-card ac'; badge.className='bk-badge ac'; badge.textContent='ACTIVE ●';
    statusMsg.className='ibox b'; statusMsg.textContent='🚗 Vehicle is parked — fare updates every 30 seconds';
    btn.className='fb dis'; btn.textContent='💳 PAY NOW (available after slot clears)'; btn.onclick=null;
  }
}

function startTimers(d){
  clearInterval(timerInterval); clearInterval(pollInterval);
  const offset=Date.now()-currentNowMs;
  if(d.status==='active' && d.entryMs>0){
    timerInterval=setInterval(()=>{
      const elapsed=(Date.now()-offset)-currentEntryMs;
      if(elapsed<0)return;
      const h=Math.floor(elapsed/3600000),m=Math.floor((elapsed%3600000)/60000),s=Math.floor((elapsed%60000)/1000);
      document.getElementById('timerDisp').textContent=pad(h)+':'+pad(m)+':'+pad(s);
      document.getElementById('fareDisp').textContent='₹'+calcFare(elapsed);
    },1000);
  }
  // Poll every 30s to check if slot cleared
  pollInterval=setInterval(()=>{
    fetch('/api/guest-lookup?vehicle='+encodeURIComponent(currentVehicle))
      .then(r=>r.json()).then(d=>{
        if(!d.found)return;
        updateCard(d);
        if(d.status!=='active'){
          clearInterval(timerInterval);
          if(d.status==='completed'){
            document.getElementById('timerDisp').textContent=pad(Math.floor(d.durationMin/60))+':'+pad(d.durationMin%60)+':00';
            document.getElementById('fareDisp').textContent='₹'+d.amount;
            toast('⚠️','SLOT CLEARED','Fare locked: ₹'+d.amount+' · Please pay now','y');
          }
        }
      });
  },30000);
}

function goToPay(){
  fetch('/api/guest-lookup?vehicle='+encodeURIComponent(currentVehicle))
    .then(r=>r.json()).then(d=>{
      if(d.status!=='completed'){toast('⚠️','Not Yet','Please wait until your car leaves the slot.','y');return;}
      document.getElementById('payAmtDisp').textContent='₹'+d.amount;
      document.getElementById('upiAmt').textContent='₹'+d.amount;
      sw('payScreen');
    });
}

function confirmPayment(){
  fetch('/api/guest-pay?vehicle='+encodeURIComponent(currentVehicle))
    .then(r=>r.json()).then(d=>{
      if(d.success){
        document.getElementById('tAmt').textContent='₹'+d.amount;
        showToken(d.token,d.amount);
        toast('✅','PAYMENT CONFIRMED','₹'+d.amount+' · Exit token generated','o');
      } else {
        showMsg('payErr',d.message||'Payment failed. Try again.','err');
      }
    });
}

function showToken(tok,amt){
  document.getElementById('tokenDisp').textContent=tok;
  if(amt)document.getElementById('tAmt').textContent='₹'+amt;
  tokenSecs=600;
  document.getElementById('tokenCountdown').textContent=tokenSecs;
  document.getElementById('regenBtn').style.display='none';
  sw('tokenScreen');
  clearInterval(tokenTimer);
  tokenTimer=setInterval(()=>{
    tokenSecs--;
    document.getElementById('tokenCountdown').textContent=tokenSecs;
    if(tokenSecs<=0){
      clearInterval(tokenTimer);
      document.getElementById('tokenDisp').textContent='EXPIRED';
      document.getElementById('regenBtn').style.display='block';
    }
  },1000);
}

function showExistingToken(){
  fetch('/api/guest-regen-token?vehicle='+encodeURIComponent(currentVehicle))
    .then(r=>r.json()).then(d=>{if(d.success)showToken(d.token,null);});
}

function regenToken(){
  fetch('/api/guest-regen-token?vehicle='+encodeURIComponent(currentVehicle))
    .then(r=>r.json()).then(d=>{
      if(d.success){showToken(d.token,null);toast('🔄','NEW TOKEN','Fresh exit token generated — free!','o');}
    });
}
</script>
</body></html>
)html";

// ─────────────────────────────────────────────────────────
//  EXIT HELP PAGE (no login — token or vehicle number)
// ─────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────
//  QR PRINT PAGE  —  /qr  (no login, printable)
// ─────────────────────────────────────────────────────────
const char* QR_PAGE = R"html(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Smart Parking — Print QR</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Rajdhani:wght@600;700&family=Exo+2:wght@400;600;700&display=swap');
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:'Exo 2',sans-serif;background:#050a14;color:#e2e8f0;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:24px 16px;}
.hdr{font-family:'Rajdhani',sans-serif;font-size:22px;font-weight:700;letter-spacing:2px;background:linear-gradient(135deg,#34d399,#6ee7b7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;margin-bottom:4px;text-align:center;}
.sub{font-size:11px;color:#475569;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:24px;text-align:center;}
/* Print card */
.print-card{background:white;border-radius:18px;padding:28px 24px;width:100%;max-width:320px;text-align:center;box-shadow:0 20px 60px rgba(0,0,0,.5);}
.pc-brand{font-family:'Rajdhani',sans-serif;font-size:20px;font-weight:700;letter-spacing:2px;color:#059669;margin-bottom:2px;}
.pc-sub{font-size:10px;color:#94a3b8;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:16px;}
#qrCanvas{margin:0 auto 14px;display:block;}
.pc-url{font-family:'Exo 2',sans-serif;font-size:12px;font-weight:700;color:#1e293b;background:#f0fdf4;border:1.5px solid #10b981;border-radius:8px;padding:8px 14px;margin-bottom:10px;word-break:break-all;}
.pc-inst{font-size:11px;color:#64748b;line-height:1.6;margin-bottom:14px;}
.pc-foot{font-size:10px;color:#94a3b8;border-top:1px solid #e2e8f0;padding-top:10px;margin-top:4px;}
.wifi-box{background:#fefce8;border:1.5px solid #fbbf24;border-radius:8px;padding:8px 12px;margin-bottom:14px;}
.wifi-lbl{font-size:9px;font-weight:700;color:#92400e;text-transform:uppercase;letter-spacing:.8px;margin-bottom:3px;}
.wifi-val{font-family:'Rajdhani',sans-serif;font-size:15px;font-weight:700;color:#b45309;}
/* Buttons */
.btn-row{display:flex;gap:10px;margin-top:20px;width:100%;max-width:320px;}
.btn{flex:1;padding:13px;border:none;border-radius:10px;font-family:'Exo 2',sans-serif;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.8px;cursor:pointer;transition:all .2s;}
.btn.print{background:linear-gradient(135deg,#059669,#10b981);color:#fff;}
.btn.dl{background:rgba(16,185,129,.1);color:#34d399;border:1.5px solid rgba(16,185,129,.25);}
.btn:hover{transform:translateY(-2px);filter:brightness(1.1);}
/* IP selector */
.sel-row{display:flex;gap:8px;align-items:center;margin-bottom:16px;width:100%;max-width:320px;flex-wrap:wrap;}
.sel-lbl{font-size:10px;color:#64748b;text-transform:uppercase;letter-spacing:.8px;font-weight:700;}
.sel{flex:1;padding:8px 10px;background:rgba(10,18,36,.9);border:1.5px solid rgba(16,185,129,.2);border-radius:8px;color:#e2e8f0;font-size:12px;font-family:'Exo 2',sans-serif;}
.sel:focus{outline:none;border-color:#10b981;}
/* Print media */
@media print{
  body{background:white;padding:0;display:block;}
  .hdr,.sub,.btn-row,.sel-row{display:none!important;}
  .print-card{box-shadow:none;border-radius:0;max-width:100%;margin:0 auto;}
}
</style>
</head><body>

<div class="hdr">🅿️ SMART PARKING</div>
<p class="sub">Payment Dashboard QR — Print &amp; Stick at Entry Gate</p>

<div class="sel-row">
  <span class="sel-lbl">URL Mode:</span>
  <select class="sel" id="urlMode" onchange="buildQR()">
    <option value="mdns">smartparking.local (mDNS)</option>
    <option value="ip">IP Address (direct)</option>
  </select>
</div>

<!-- Print card -->
<div class="print-card" id="printCard">
  <div class="pc-brand">🅿️ SMART PARKING</div>
  <div class="pc-sub">Payment Dashboard</div>
  <canvas id="qrCanvas" width="200" height="200"></canvas>
  <div class="pc-url" id="urlDisp">smartparking.local/guest</div>
  <div class="wifi-box">
    <div class="wifi-lbl">📶 Connect to WiFi First</div>
    <div class="wifi-val" id="wifiName">Redmi 12 5G</div>
  </div>
  <div class="pc-inst">
    1. Connect phone to parking WiFi<br>
    2. Scan QR code above<br>
    3. Enter your vehicle number<br>
    4. Track time · Pay · Get exit token
  </div>
  <div class="pc-foot">₹10 per 30 min · Pay online · Token exit</div>
</div>

<div class="btn-row">
  <button class="btn print" onclick="window.print()">🖨️ PRINT</button>
  <button class="btn dl" onclick="downloadQR()">⬇️ SAVE PNG</button>
</div>

<!-- QR generator (pure JS, no internet needed) -->
<script>
// Compact QR Code generator — Reed-Solomon, pure JS
// Based on qrcodejs (MIT license) — self-contained minimal build
var QRCode=(function(){var a={};a.stringToBytes=function(s){var b=[];for(var i=0;i<s.length;i++){var c=s.charCodeAt(i);if(c<128)b.push(c);else if(c<2048)b.push(192|(c>>6),128|(c&63));else b.push(224|(c>>12),128|((c>>6)&63),128|(c&63));}return b;};
var QR8bitByte=function(data){this.mode=4;this.data=data;this.parsedData=a.stringToBytes(data);};
QR8bitByte.prototype={getLength:function(){return this.parsedData.length;},write:function(buf){for(var i=0;i<this.parsedData.length;i++)buf.put(this.parsedData[i],8);}};
var QRCodeModel=function(typeNumber,errorCorrectLevel){this.typeNumber=typeNumber;this.errorCorrectLevel=errorCorrectLevel;this.modules=null;this.moduleCount=0;this.dataCache=null;this.dataList=[];};
QRCodeModel.prototype={addData:function(data){this.dataList.push(new QR8bitByte(data));this.dataCache=null;},isDark:function(row,col){if(row<0||this.moduleCount<=row||col<0||this.moduleCount<=col)throw new Error(row+","+col);return this.modules[row][col];},getModuleCount:function(){return this.moduleCount;},make:function(){if(this.typeNumber<1){var typeNumber=1;for(;typeNumber<40;typeNumber++){var rs=QRRSBlock.getRSBlocks(typeNumber,this.errorCorrectLevel);var buf=new QRBitBuffer();var totalDataCount=0;for(var i=0;i<rs.length;i++)totalDataCount+=rs[i].dataCount;buf.put(4,4);buf.put(this.dataList[0].getLength(),QRUtil.getLengthInBits(4,typeNumber));this.dataList[0].write(buf);if(buf.getLengthInBits()<=totalDataCount*8)break;}this.typeNumber=typeNumber;}this.makeImpl(false,this.getBestMaskPattern());},makeImpl:function(test,maskPattern){this.moduleCount=this.typeNumber*4+17;this.modules=[];for(var i=0;i<this.moduleCount;i++){this.modules.push([]);for(var j=0;j<this.moduleCount;j++)this.modules[i].push(null);}this.setupPositionProbePattern(0,0);this.setupPositionProbePattern(this.moduleCount-7,0);this.setupPositionProbePattern(0,this.moduleCount-7);this.setupPositionAdjustPattern();this.setupTimingPattern();this.setupTypeInfo(test,maskPattern);if(this.typeNumber>=7)this.setupTypeNumber(test);if(this.dataCache==null)this.dataCache=QRCodeModel.createData(this.typeNumber,this.errorCorrectLevel,this.dataList);this.mapData(this.dataCache,maskPattern);},setupPositionProbePattern:function(row,col){for(var r=-1;r<=7;r++){if(row+r<=-1||this.moduleCount<=row+r)continue;for(var c=-1;c<=7;c++){if(col+c<=-1||this.moduleCount<=col+c)continue;this.modules[row+r][col+c]=((0<=r&&r<=6&&(c==0||c==6))||(0<=c&&c<=6&&(r==0||r==6))||(2<=r&&r<=4&&2<=c&&c<=4));}}},getBestMaskPattern:function(){var minLostPoint=0;var pattern=0;for(var i=0;i<8;i++){this.makeImpl(true,i);var lostPoint=QRUtil.getLostPoint(this);if(i==0||minLostPoint>lostPoint){minLostPoint=lostPoint;pattern=i;}}return pattern;},setupTimingPattern:function(){for(var r=8;r<this.moduleCount-8;r++)if(this.modules[r][6]==null)this.modules[r][6]=(r%2==0);for(var c=8;c<this.moduleCount-8;c++)if(this.modules[6][c]==null)this.modules[6][c]=(c%2==0);},setupPositionAdjustPattern:function(){var pos=QRUtil.getPatternPosition(this.typeNumber);for(var i=0;i<pos.length;i++)for(var j=0;j<pos.length;j++){var row=pos[i];var col=pos[j];if(this.modules[row][col]!=null)continue;for(var r=-2;r<=2;r++)for(var c=-2;c<=2;c++)this.modules[row+r][col+c]=(r==-2||r==2||c==-2||c==2||(r==0&&c==0));}},setupTypeNumber:function(test){var bits=QRUtil.getBCHTypeNumber(this.typeNumber);for(var i=0;i<18;i++){this.modules[Math.floor(i/3)][i%3+this.moduleCount-8-3]=(!test&&((bits>>i)&1)==1);}for(var i=0;i<18;i++){this.modules[i%3+this.moduleCount-8-3][Math.floor(i/3)]=(!test&&((bits>>i)&1)==1);}},setupTypeInfo:function(test,maskPattern){var data=(this.errorCorrectLevel<<3)|maskPattern;var bits=QRUtil.getBCHTypeInfo(data);for(var i=0;i<15;i++){var mod=(!test&&((bits>>i)&1)==1);if(i<6)this.modules[i][8]=mod;else if(i<8)this.modules[i+1][8]=mod;else this.modules[this.moduleCount-15+i][8]=mod;}for(var i=0;i<15;i++){var mod=(!test&&((bits>>i)&1)==1);if(i<8)this.modules[8][this.moduleCount-i-1]=mod;else if(i<9)this.modules[8][15-i-1+1]=mod;else this.modules[8][15-i-1]=mod;}this.modules[this.moduleCount-8][8]=!test;},mapData:function(data,maskPattern){var inc=-1;var row=this.moduleCount-1;var bitIndex=7;var byteIndex=0;for(var col=this.moduleCount-1;col>0;col-=2){if(col==6)col--;while(true){for(var c=0;c<2;c++){if(this.modules[row][col-c]==null){var dark=false;if(byteIndex<data.length)dark=((data[byteIndex]>>>bitIndex)&1)==1;var mask=QRUtil.getMask(maskPattern,row,col-c);if(mask)dark=!dark;this.modules[row][col-c]=dark;bitIndex--;if(bitIndex==-1){byteIndex++;bitIndex=7;}}}row+=inc;if(row<0||this.moduleCount<=row){row-=inc;inc=-inc;break;}}}}};
QRCodeModel.PAD0=0xEC;QRCodeModel.PAD1=0x11;
QRCodeModel.createData=function(typeNumber,errorCorrectLevel,dataList){var rs=QRRSBlock.getRSBlocks(typeNumber,errorCorrectLevel);var buf=new QRBitBuffer();for(var i=0;i<dataList.length;i++){var data=dataList[i];buf.put(data.mode,4);buf.put(data.getLength(),QRUtil.getLengthInBits(data.mode,typeNumber));data.write(buf);}var totalDataCount=0;for(var i=0;i<rs.length;i++)totalDataCount+=rs[i].dataCount;if(buf.getLengthInBits()>totalDataCount*8)throw new Error("code length overflow");if(buf.getLengthInBits()+4<=totalDataCount*8)buf.put(0,4);while(buf.getLengthInBits()%8!=0)buf.putBit(false);while(true){if(buf.getLengthInBits()>=totalDataCount*8)break;buf.put(QRCodeModel.PAD0,8);if(buf.getLengthInBits()>=totalDataCount*8)break;buf.put(QRCodeModel.PAD1,8);}return QRCodeModel.createBytes(buf,rs);};
QRCodeModel.createBytes=function(buf,rs){var offset=0;var maxDcCount=0;var maxEcCount=0;var dcdata=[];var ecdata=[];for(var r=0;r<rs.length;r++){var dcCount=rs[r].dataCount;var ecCount=rs[r].totalCount-dcCount;maxDcCount=Math.max(maxDcCount,dcCount);maxEcCount=Math.max(maxEcCount,ecCount);dcdata.push([]);for(var i=0;i<dcCount;i++)dcdata[r].push(0xff&buf.buffer[i+offset]);offset+=dcCount;var rsPoly=QRUtil.getErrorCorrectPolynomial(ecCount);var rawPoly=new QRPolynomial(dcdata[r],rsPoly.getLength()-1);var modPoly=rawPoly.mod(rsPoly);ecdata.push([]);for(var i=0;i<rsPoly.getLength()-1;i++){var modIndex=i+modPoly.getLength()-rsPoly.getLength()+1;ecdata[r].push((modIndex>=0)?modPoly.get(modIndex):0);}}var totalCodeCount=0;for(var i=0;i<rs.length;i++)totalCodeCount+=rs[i].totalCount;var data=[];var index=0;for(var i=0;i<maxDcCount;i++)for(var r=0;r<rs.length;r++)if(i<dcdata[r].length)data.push(dcdata[r][i]);for(var i=0;i<maxEcCount;i++)for(var r=0;r<rs.length;r++)if(i<ecdata[r].length)data.push(ecdata[r][i]);return data;};
var QRUtil={PATTERN_POSITION_TABLE:[[],[6,18],[6,22],[6,26],[6,30],[6,34],[6,22,38],[6,24,42],[6,26,46],[6,28,50],[6,30,54],[6,32,58],[6,34,62],[6,26,46,66],[6,26,48,70],[6,26,50,74],[6,30,54,78],[6,30,56,82],[6,30,58,86],[6,34,62,90],[6,28,50,72,94],[6,26,50,74,98],[6,30,54,78,102],[6,28,54,80,106],[6,32,58,84,110],[6,30,58,86,114],[6,34,62,90,118],[6,26,50,74,98,122],[6,30,54,78,102,126],[6,26,52,78,104,130],[6,30,56,82,108,134],[6,34,60,86,112,138],[6,30,58,86,114,142],[6,34,62,90,118,146],[6,30,54,78,102,126,150],[6,24,50,76,102,128,154],[6,28,54,80,106,132,158],[6,32,58,84,110,136,162],[6,26,54,82,110,138,166],[6,30,58,86,114,142,170]],G15:(1<<10)|(1<<8)|(1<<5)|(1<<4)|(1<<2)|(1<<1)|(1<<0),G18:(1<<12)|(1<<11)|(1<<10)|(1<<9)|(1<<8)|(1<<5)|(1<<2)|(1<<0),G15_MASK:(1<<14)|(1<<12)|(1<<10)|(1<<4)|(1<<1),getBCHTypeInfo:function(data){var d=data<<10;while(QRUtil.getBCHDigit(d)-QRUtil.getBCHDigit(QRUtil.G15)>=0)d^=(QRUtil.G15<<(QRUtil.getBCHDigit(d)-QRUtil.getBCHDigit(QRUtil.G15)));return((data<<10)|d)^QRUtil.G15_MASK;},getBCHTypeNumber:function(data){var d=data<<12;while(QRUtil.getBCHDigit(d)-QRUtil.getBCHDigit(QRUtil.G18)>=0)d^=(QRUtil.G18<<(QRUtil.getBCHDigit(d)-QRUtil.getBCHDigit(QRUtil.G18)));return(data<<12)|d;},getBCHDigit:function(data){var digit=0;while(data!=0){digit++;data>>>=1;}return digit;},getPatternPosition:function(typeNumber){return QRUtil.PATTERN_POSITION_TABLE[typeNumber-1];},getMask:function(maskPattern,i,j){switch(maskPattern){case 0:return(i+j)%2==0;case 1:return i%2==0;case 2:return j%3==0;case 3:return(i+j)%3==0;case 4:return(Math.floor(i/2)+Math.floor(j/3))%2==0;case 5:return(i*j)%2+(i*j)%3==0;case 6:return((i*j)%2+(i*j)%3)%2==0;case 7:return((i*j)%3+(i+j)%2)%2==0;}throw new Error("bad maskPattern:"+maskPattern);},getErrorCorrectPolynomial:function(errorCorrectLength){var a=new QRPolynomial([1],0);for(var i=0;i<errorCorrectLength;i++)a=a.multiply(new QRPolynomial([1,QRMath.gexp(i)],0));return a;},getLengthInBits:function(mode,type){if(1<=type&&type<10){if(mode==1)return 10;if(mode==2)return 9;if(mode==4)return 8;if(mode==8)return 8;}else if(type<27){if(mode==1)return 12;if(mode==2)return 11;if(mode==4)return 16;if(mode==8)return 10;}else if(type<41){if(mode==1)return 14;if(mode==2)return 13;if(mode==4)return 16;if(mode==8)return 12;}throw new Error("mode:"+mode+"/type:"+type);},getLostPoint:function(qrCode){var moduleCount=qrCode.getModuleCount();var lostPoint=0;for(var row=0;row<moduleCount;row++){for(var col=0;col<moduleCount;col++){var sameCount=0;var dark=qrCode.isDark(row,col);for(var r=-1;r<=1;r++){if(row+r<0||moduleCount<=row+r)continue;for(var c=-1;c<=1;c++){if(col+c<0||moduleCount<=col+c)continue;if(r==0&&c==0)continue;if(dark==qrCode.isDark(row+r,col+c))sameCount++;}}if(sameCount>5)lostPoint+=(3+sameCount-5);}}for(var row=0;row<moduleCount-1;row++){for(var col=0;col<moduleCount-1;col++){var count=0;if(qrCode.isDark(row,col))count++;if(qrCode.isDark(row+1,col))count++;if(qrCode.isDark(row,col+1))count++;if(qrCode.isDark(row+1,col+1))count++;if(count==0||count==4)lostPoint+=3;}}for(var row=0;row<moduleCount;row++){for(var col=0;col<moduleCount-6;col++){if(qrCode.isDark(row,col)&&!qrCode.isDark(row,col+1)&&qrCode.isDark(row,col+2)&&qrCode.isDark(row,col+3)&&qrCode.isDark(row,col+4)&&!qrCode.isDark(row,col+5)&&qrCode.isDark(row,col+6))lostPoint+=40;}}for(var col=0;col<moduleCount;col++){for(var row=0;row<moduleCount-6;row++){if(qrCode.isDark(row,col)&&!qrCode.isDark(row+1,col)&&qrCode.isDark(row+2,col)&&qrCode.isDark(row+3,col)&&qrCode.isDark(row+4,col)&&!qrCode.isDark(row+5,col)&&qrCode.isDark(row+6,col))lostPoint+=40;}}var darkCount=0;for(var col=0;col<moduleCount;col++)for(var row=0;row<moduleCount;row++)if(qrCode.isDark(row,col))darkCount++;var ratio=Math.abs(100*darkCount/moduleCount/moduleCount-50)/5;lostPoint+=ratio*10;return lostPoint;}};
var QRMath={glog:function(n){if(n<1)throw new Error("glog("+n+")");return QRMath.LOG_TABLE[n];},gexp:function(n){while(n<0)n+=255;while(n>=256)n-=255;return QRMath.EXP_TABLE[n];},EXP_TABLE:new Array(256),LOG_TABLE:new Array(256)};
for(var i=0;i<8;i++)QRMath.EXP_TABLE[i]=1<<i;
for(var i=8;i<256;i++)QRMath.EXP_TABLE[i]=QRMath.EXP_TABLE[i-4]^QRMath.EXP_TABLE[i-5]^QRMath.EXP_TABLE[i-6]^QRMath.EXP_TABLE[i-8];
for(var i=0;i<255;i++)QRMath.LOG_TABLE[QRMath.EXP_TABLE[i]]=i;
var QRPolynomial=function(num,shift){if(num==undefined)throw new Error("num is undefined");var offset=0;while(offset<num.length&&num[offset]==0)offset++;this.num=[];for(var i=0;i<num.length-offset+shift;i++)this.num.push(i<num.length-offset?num[i+offset]:0);};
QRPolynomial.prototype={get:function(index){return this.num[index];},getLength:function(){return this.num.length;},multiply:function(e){var num=[];for(var i=0;i<this.getLength()+e.getLength()-1;i++)num.push(0);for(var i=0;i<this.getLength();i++)for(var j=0;j<e.getLength();j++)num[i+j]^=QRMath.gexp(QRMath.glog(this.get(i))+QRMath.glog(e.get(j)));return new QRPolynomial(num,0);},mod:function(e){if(this.getLength()-e.getLength()<0)return this;var ratio=QRMath.glog(this.get(0))-QRMath.glog(e.get(0));var num=[];for(var i=0;i<this.getLength();i++)num.push(this.get(i));for(var i=0;i<e.getLength();i++)num[i]^=QRMath.gexp(QRMath.glog(e.get(i))+ratio);return new QRPolynomial(num,0).mod(e);}};
var QRRSBlock=function(totalCount,dataCount){this.totalCount=totalCount;this.dataCount=dataCount;};
QRRSBlock.RS_BLOCK_TABLE=[[1,26,19],[1,26,16],[1,26,13],[1,26,9],[1,44,34],[1,44,28],[1,44,22],[1,44,16],[1,70,55],[1,70,44],[2,35,17],[2,35,13],[1,100,80],[2,50,32],[2,50,24],[4,25,9],[1,134,108],[2,67,43],[2,33,15,2,34,16],[2,33,11,2,34,12],[2,86,68],[4,43,27],[4,43,19],[4,43,15],[2,98,78],[4,49,31],[2,32,14,4,33,15],[4,39,13,1,40,14],[2,121,97],[2,60,38,2,61,39],[4,40,18,2,41,19],[4,40,14,2,41,15],[2,146,116],[3,58,36,2,59,37],[4,36,16,4,37,17],[4,36,12,4,37,13],[2,86,68,2,87,69],[4,69,43,1,70,44],[6,43,19,2,44,20],[6,43,15,2,44,16],[4,101,81],[1,80,50,4,81,51],[4,50,22,4,51,23],[3,36,12,8,37,13],[2,116,92,2,117,93],[6,58,36,2,59,37],[4,46,20,6,47,21],[7,42,14,4,43,15],[4,133,107],[8,59,37,1,60,38],[8,44,20,4,45,21],[12,33,11,4,34,12],[3,145,115,1,146,116],[4,64,40,5,65,41],[11,36,16,5,37,17],[11,36,12,5,37,13],[5,109,87,1,110,88],[5,65,41,5,66,42],[5,54,24,7,55,25],[11,36,12,7,37,13],[5,122,98,1,123,99],[7,73,45,3,74,46],[15,43,19,2,44,20],[3,45,15,13,46,16],[1,135,107,5,136,108],[10,74,46,1,75,47],[1,50,22,15,51,23],[2,42,14,17,43,15],[5,150,120,1,151,121],[9,69,43,4,70,44],[17,50,22,1,51,23],[2,42,14,19,43,15],[3,141,113,4,142,114],[3,70,44,11,71,45],[17,47,21,4,48,22],[9,39,13,16,40,14],[3,135,107,5,136,108],[3,67,41,13,68,42],[15,54,24,5,55,25],[15,43,15,10,44,16],[4,144,116,4,145,117],[17,68,42],[17,50,22,6,51,23],[19,46,16,6,47,17],[2,139,111,7,140,112],[17,74,46],[7,54,24,16,55,25],[34,37,13],[4,151,121,5,152,122],[4,75,47,14,76,48],[11,54,24,14,55,25],[16,45,15,14,46,16],[6,147,117,4,148,118],[6,73,45,14,74,46],[11,54,24,16,55,25],[30,46,16,2,47,17],[8,132,106,4,133,107],[8,75,47,13,76,48],[7,54,24,22,55,25],[22,45,15,13,46,16],[10,142,114,2,143,115],[19,74,46,4,75,47],[28,50,22,6,51,23],[33,46,16,4,47,17],[8,152,122,4,153,123],[22,73,45,3,74,46],[8,53,23,26,54,24],[12,45,15,28,46,16],[3,147,117,10,148,118],[3,73,45,23,74,46],[4,54,24,31,55,25],[11,45,15,31,46,16],[7,146,116,7,147,117],[21,73,45,7,74,46],[1,53,23,37,54,24],[19,45,15,26,46,16],[5,145,115,10,146,116],[19,75,47,10,76,48],[15,54,24,25,55,25],[23,45,15,25,46,16],[13,145,115,3,146,116],[2,74,46,29,75,47],[42,54,24,1,55,25],[23,45,15,28,46,16],[17,145,115],[10,74,46,23,75,47],[10,54,24,35,55,25],[19,45,15,35,46,16],[17,145,115,1,146,116],[14,74,46,21,75,47],[29,54,24,19,55,25],[11,45,15,46,46,16],[13,145,115,6,146,116],[14,74,46,23,75,47],[44,54,24,7,55,25],[59,46,16,1,47,17],[12,151,121,7,152,122],[12,75,47,26,76,48],[39,54,24,14,55,25],[22,45,15,41,46,16],[6,151,121,14,152,122],[6,75,47,34,76,48],[46,54,24,10,55,25],[2,45,15,64,46,16],[17,152,122,4,153,123],[29,74,46,14,75,47],[49,54,24,10,55,25],[24,45,15,46,46,16],[4,152,122,18,153,123],[13,74,46,32,75,47],[48,54,24,14,55,25],[42,45,15,32,46,16],[20,147,117,4,148,118],[40,75,47,7,76,48],[43,54,24,22,55,25],[10,45,15,67,46,16],[19,148,118,6,149,119],[18,75,47,31,76,48],[34,54,24,34,55,25],[20,45,15,61,46,16]];
QRRSBlock.getRSBlocks=function(typeNumber,errorCorrectLevel){var rsBlock=QRRSBlock.RS_BLOCK_TABLE[(typeNumber-1)*4+errorCorrectLevel];var list=[];for(var i=0;i<rsBlock.length;i+=3){var count=rsBlock[i];var totalCount=rsBlock[i+1];var dataCount=rsBlock[i+2];for(var j=0;j<count;j++)list.push(new QRRSBlock(totalCount,dataCount));}return list;};
var QRBitBuffer=function(){this.buffer=[];this.length=0;};
QRBitBuffer.prototype={get:function(index){var bufIndex=Math.floor(index/8);return((this.buffer[bufIndex]>>>(7-index%8))&1)==1;},put:function(num,length){for(var i=0;i<length;i++)this.putBit(((num>>>(length-i-1))&1)==1);},getLengthInBits:function(){return this.length;},putBit:function(bit){var bufIndex=Math.floor(this.length/8);if(this.buffer.length<=bufIndex)this.buffer.push(0);if(bit)this.buffer[bufIndex]|=(0x80>>>(this.length%8));this.length++;}};
return{draw:function(text,canvas,size){size=size||200;canvas.width=size;canvas.height=size;var qr=new QRCodeModel(-1,1);qr.addData(text);qr.make();var ctx=canvas.getContext('2d');var count=qr.getModuleCount();var cell=size/count;ctx.fillStyle='#ffffff';ctx.fillRect(0,0,size,size);ctx.fillStyle='#000000';for(var r=0;r<count;r++)for(var c=0;c<count;c++)if(qr.isDark(r,c))ctx.fillRect(Math.round(c*cell),Math.round(r*cell),Math.ceil(cell),Math.ceil(cell));}};
})();

// Get ESP32 IP from current page URL
var espIP = window.location.hostname;
var wifiSSID = 'Redmi 12 5G';

function buildQR() {
  var mode = document.getElementById('urlMode').value;
  var url  = (mode === 'ip')
    ? 'http://' + espIP + '/guest'
    : 'http://smartparking.local/guest';
  document.getElementById('urlDisp').textContent = url;
  QRCode.draw(url, document.getElementById('qrCanvas'), 200);
}

function downloadQR() {
  var canvas = document.getElementById('qrCanvas');
  // Create a white-bg version with padding for saving
  var out = document.createElement('canvas');
  out.width = 280; out.height = 280;
  var ctx = out.getContext('2d');
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0,0,280,280);
  ctx.drawImage(canvas, 40, 40, 200, 200);
  var a = document.createElement('a');
  a.download = 'smartparking-payment-qr.png';
  a.href = out.toDataURL('image/png');
  a.click();
}

// Build on load
window.onload = buildQR;
</script>
</body></html>
)html";


// ─────────────────────────────────────────────────────────
//  HTML PAGES
// ─────────────────────────────────────────────────────────
const char* LOGIN_PAGE = R"html(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Smart Parking Pro</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Rajdhani:wght@400;500;600;700&family=Exo+2:wght@300;400;600;700;800&display=swap');
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:'Exo 2',sans-serif;min-height:100vh;display:flex;justify-content:center;align-items:center;overflow:hidden;background:#050a14;position:relative;}
.bg{position:fixed;inset:0;z-index:0;overflow:hidden;}
.bg-grid{position:absolute;inset:0;background-image:linear-gradient(rgba(59,130,246,.05)1px,transparent 1px),linear-gradient(90deg,rgba(59,130,246,.05)1px,transparent 1px);background-size:55px 55px;}
.bg-glow{position:absolute;border-radius:50%;filter:blur(90px);animation:gpulse 4s ease-in-out infinite;}
.g1{width:500px;height:500px;background:rgba(37,99,235,.12);top:-150px;right:-80px;}
.g2{width:380px;height:380px;background:rgba(16,185,129,.08);bottom:-60px;left:-40px;animation-delay:2s;}
.g3{width:250px;height:250px;background:rgba(245,158,11,.06);top:40%;left:40%;animation-delay:1s;}
@keyframes gpulse{0%,100%{opacity:.5;transform:scale(1)}50%{opacity:1;transform:scale(1.08)}}
.road{position:absolute;bottom:0;left:0;right:0;height:30%;background:linear-gradient(180deg,transparent,#0d1117);}
.road::before{content:'';position:absolute;top:45%;left:0;right:0;height:3px;background:repeating-linear-gradient(90deg,#f59e0b 0,#f59e0b 36px,transparent 36px,transparent 72px);}
.bays{position:absolute;top:8%;left:2%;right:2%;height:55%;display:grid;grid-template-columns:repeat(9,1fr);gap:6px;opacity:.1;}
.bay{border:1.5px solid #3b82f6;border-radius:3px;background:linear-gradient(180deg,rgba(59,130,246,.15),transparent);}
.bay.t{border-color:#ef4444;background:rgba(239,68,68,.1);}.bay.r{border-color:#f59e0b;background:rgba(245,158,11,.1);}
.car{position:absolute;animation:drive linear infinite;opacity:.65;}
.c1{font-size:26px;bottom:9%;animation-duration:9s;}
.c2{font-size:20px;bottom:20%;animation-duration:13s;animation-delay:-5s;}
.c3{font-size:18px;bottom:4%;animation-duration:11s;animation-delay:-8s;}
@keyframes drive{from{left:-50px}to{left:105%}}
.wrap{position:relative;z-index:10;width:100%;max-width:460px;padding:16px;}
.card{background:rgba(7,13,28,.92);backdrop-filter:blur(28px);border:1px solid rgba(59,130,246,.22);border-radius:22px;padding:32px 32px 28px;box-shadow:0 40px 80px rgba(0,0,0,.65);animation:slideUp .65s cubic-bezier(.34,1.56,.64,1);}
@keyframes slideUp{from{opacity:0;transform:translateY(56px)scale(.96)}to{opacity:1;transform:none}}
.brand{text-align:center;margin-bottom:26px;}
.logo{width:68px;height:68px;border-radius:18px;margin:0 auto 11px;background:linear-gradient(135deg,#1d4ed8,#2563eb);display:flex;align-items:center;justify-content:center;font-size:32px;box-shadow:0 14px 32px rgba(37,99,235,.45);animation:float 3s ease-in-out infinite;}
@keyframes float{0%,100%{transform:translateY(0)}50%{transform:translateY(-7px)}}
.brand h1{font-family:'Rajdhani',sans-serif;font-size:24px;font-weight:700;letter-spacing:3px;background:linear-gradient(135deg,#60a5fa,#34d399);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}
.brand p{font-size:10px;color:#475569;text-transform:uppercase;letter-spacing:2px;margin-top:4px;}
.role-lbl{font-size:10px;font-weight:700;color:#475569;text-transform:uppercase;letter-spacing:1.8px;text-align:center;margin-bottom:12px;}
.role-cards{display:grid;grid-template-columns:1fr 1fr;gap:11px;margin-bottom:22px;}
.role-card{border-radius:14px;padding:18px 10px;text-align:center;cursor:pointer;transition:all .3s;border:2px solid transparent;position:relative;overflow:hidden;user-select:none;}
.role-admin{background:rgba(37,99,235,.09);border-color:rgba(37,99,235,.22);}
.role-admin:hover,.role-admin.sel{border-color:#3b82f6;transform:translateY(-3px);box-shadow:0 12px 32px rgba(37,99,235,.22);}
.role-admin.sel{background:rgba(37,99,235,.16);}
.role-user{background:rgba(16,185,129,.07);border-color:rgba(16,185,129,.18);}
.role-user:hover,.role-user.sel{border-color:#10b981;transform:translateY(-3px);box-shadow:0 12px 32px rgba(16,185,129,.18);}
.role-user.sel{background:rgba(16,185,129,.14);}
.rchk{position:absolute;top:7px;right:7px;width:18px;height:18px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:9px;opacity:0;transform:scale(0);transition:all .25s;}
.role-admin .rchk{background:rgba(37,99,235,.5);color:#93c5fd;}
.role-user .rchk{background:rgba(16,185,129,.45);color:#6ee7b7;}
.role-card.sel .rchk{opacity:1;transform:scale(1);}
.ri{font-size:30px;margin-bottom:7px;display:block;}
.rname{font-family:'Rajdhani',sans-serif;font-size:15px;font-weight:700;letter-spacing:1.5px;margin-bottom:3px;}
.role-admin .rname{color:#60a5fa;}.role-user .rname{color:#34d399;}
.rdesc{font-size:9px;color:#64748b;letter-spacing:.4px;line-height:1.4;}
.rbadge{display:inline-block;padding:2px 7px;border-radius:8px;font-size:8px;font-weight:700;letter-spacing:.8px;margin-top:5px;}
.role-admin .rbadge{background:rgba(37,99,235,.18);color:#93c5fd;border:1px solid rgba(59,130,246,.22);}
.role-user .rbadge{background:rgba(16,185,129,.13);color:#6ee7b7;border:1px solid rgba(16,185,129,.18);}
.div{height:1px;background:linear-gradient(90deg,transparent,#1a3050,transparent);margin:0 0 20px;}
.panel{display:none;animation:pIn .38s cubic-bezier(.34,1.2,.64,1);}
.panel.act{display:block;}
@keyframes pIn{from{opacity:0;transform:translateY(12px)}to{opacity:1;transform:none}}
.no-role{text-align:center;padding:18px 0 6px;color:#2d4060;font-size:12px;}
.no-role .nri{font-size:30px;margin-bottom:8px;opacity:.35;}
.phdr{display:flex;align-items:center;gap:10px;padding:11px 13px;border-radius:10px;margin-bottom:16px;}
.phdr.adm{background:rgba(37,99,235,.08);border:1px solid rgba(37,99,235,.17);}
.phdr.usr{background:rgba(16,185,129,.07);border:1px solid rgba(16,185,129,.14);}
.phicon{font-size:18px;}
.phtitle{font-family:'Rajdhani',sans-serif;font-size:14px;font-weight:700;letter-spacing:1px;}
.adm .phtitle{color:#60a5fa;}.usr .phtitle{color:#34d399;}
.phsub{font-size:9px;color:#475569;text-transform:uppercase;letter-spacing:.7px;margin-top:1px;}
.fg{margin-bottom:13px;}
.fg label{display:block;font-size:10px;font-weight:700;color:#64748b;text-transform:uppercase;letter-spacing:1.8px;margin-bottom:6px;}
.fg input{width:100%;padding:11px 14px;background:rgba(12,20,40,.6);border:1.5px solid #1a3050;border-radius:9px;color:#e2e8f0;font-size:14px;font-family:'Exo 2',sans-serif;transition:all .3s;}
.fg input::placeholder{color:#2d4060;}
#adminPanel .fg input:focus{outline:none;border-color:#3b82f6;background:rgba(12,20,40,.9);}
#userPanel  .fg input:focus{outline:none;border-color:#10b981;background:rgba(12,20,40,.9);}
.btn{width:100%;padding:13px;border:none;border-radius:10px;color:#fff;font-size:11px;font-weight:700;font-family:'Exo 2',sans-serif;letter-spacing:1.8px;text-transform:uppercase;cursor:pointer;transition:all .3s;margin-top:4px;}
.btn-adm{background:linear-gradient(135deg,#2563eb,#1d4ed8);box-shadow:0 8px 22px rgba(37,99,235,.28);}
.btn-adm:hover{transform:translateY(-2px);box-shadow:0 14px 34px rgba(37,99,235,.42);}
.btn-usr{background:linear-gradient(135deg,#059669,#10b981);box-shadow:0 8px 22px rgba(16,185,129,.22);}
.btn-usr:hover{transform:translateY(-2px);box-shadow:0 14px 34px rgba(16,185,129,.36);}
.hint{text-align:center;font-size:11px;color:#2d4060;margin-top:13px;}
.hint code{background:rgba(59,130,246,.1);border:1px solid rgba(59,130,246,.18);padding:2px 7px;border-radius:5px;color:#60a5fa;}
.hint code.g{background:rgba(16,185,129,.09);border-color:rgba(16,185,129,.18);color:#34d399;}
.badge{position:fixed;bottom:16px;right:16px;z-index:20;background:rgba(7,13,28,.9);border:1px solid rgba(16,185,129,.32);border-radius:10px;padding:8px 13px;font-size:10px;color:#34d399;display:flex;align-items:center;gap:6px;}
.dot{width:7px;height:7px;border-radius:50%;background:#10b981;animation:blink 1.5s ease-in-out infinite;}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.15}}
.errmsg{color:#f87171;font-size:11px;margin:6px 0 10px;min-height:16px;text-align:center;display:none;}
.errmsg.show{display:block;}
.btn:disabled{opacity:.6;cursor:not-allowed;transform:none!important;}
</style></head><body>
<div class="bg">
  <div class="bg-grid"></div>
  <div class="bg-glow g1"></div><div class="bg-glow g2"></div><div class="bg-glow g3"></div>
  <div class="bays"><div class="bay"></div><div class="bay t"></div><div class="bay r"></div><div class="bay"></div><div class="bay t"></div><div class="bay"></div><div class="bay r"></div><div class="bay t"></div><div class="bay"></div></div>
  <div class="road"></div>
  <div class="car c1">🚗</div><div class="car c2">🏍️</div><div class="car c3">🚙</div>
</div>
<div class="wrap"><div class="card">
  <div class="brand">
    <div class="logo">🅿️</div>
    <h1>SMART PARKING PRO</h1>
    <p>Advanced Reservation &amp; Billing System</p>
  </div>
  <div class="role-lbl">SELECT YOUR ROLE TO CONTINUE</div>
  <div class="role-cards">
    <div class="role-card role-admin" id="adminCard" onclick="selRole('admin')">
      <div class="rchk">✓</div><span class="ri">🛡️</span>
      <div class="rname">ADMIN</div><div class="rdesc">Full system control &amp; management</div>
      <span class="rbadge">FULL ACCESS</span>
    </div>
    <div class="role-card role-user" id="userCard" onclick="selRole('user')">
      <div class="rchk">✓</div><span class="ri">🚘</span>
      <div class="rname">USER</div><div class="rdesc">Reserve &amp; track your parking</div>
      <span class="rbadge">SELF-SERVICE</span>
    </div>
  </div>
  <div class="div"></div>
  <div class="panel act" id="noPanel"><div class="no-role"><div class="nri">👆</div><p>Choose a role above to sign in</p></div></div>
  <div class="panel" id="adminPanel">
    <div class="phdr adm"><span class="phicon">🛡️</span>
      <div><div class="phtitle">ADMINISTRATOR LOGIN</div><div class="phsub">Full dashboard · Gate control · QR payments</div></div>
    </div>
    <div class="fg"><label>Username</label><input type="text" id="aUser" placeholder="admin" autocomplete="off"></div>
    <div class="fg"><label>Password</label><input type="password" id="aPass" placeholder="Enter admin password" onkeydown="if(event.key==='Enter')doLogin('admin')"></div>
    <div class="errmsg" id="aErr"></div>
    <button type="button" class="btn btn-adm" onclick="doLogin('admin')">🛡️ &nbsp; ACCESS ADMIN PANEL</button>
    <div class="hint">Demo: <code>admin</code> &nbsp;/&nbsp; <code>1234</code></div>
  </div>
  <div class="panel" id="userPanel">
    <div class="phdr usr"><span class="phicon">🚘</span>
      <div><div class="phtitle">USER LOGIN</div><div class="phsub">Reserve slots · Pay online · Get QR pass</div></div>
    </div>
    <div class="fg"><label>Username</label><input type="text" id="uUser" placeholder="yourname" autocomplete="off"></div>
    <div class="fg"><label>Password</label><input type="password" id="uPass" placeholder="Enter your password" onkeydown="if(event.key==='Enter')doLogin('user')"></div>
    <div class="errmsg" id="uErr"></div>
    <button type="button" class="btn btn-usr" onclick="doLogin('user')">🚘 &nbsp; ACCESS USER PORTAL</button>
    <div class="hint">Demo: <code class="g">user</code> &nbsp;/&nbsp; <code class="g">user123</code></div>
  </div>
</div></div>
<div class="badge"><div class="dot"></div>IR Sensors · Gates · QR Exit</div>
<script>
function selRole(r){
  document.getElementById('adminCard').classList.remove('sel');
  document.getElementById('userCard').classList.remove('sel');
  ['noPanel','adminPanel','userPanel'].forEach(id=>document.getElementById(id).classList.remove('act'));
  if(r==='admin'){document.getElementById('adminCard').classList.add('sel');document.getElementById('adminPanel').classList.add('act');document.getElementById('aUser').focus();}
  else{document.getElementById('userCard').classList.add('sel');document.getElementById('userPanel').classList.add('act');document.getElementById('uUser').focus();}
}
function doLogin(role){
  const isAdmin=(role==='admin');
  const u=document.getElementById(isAdmin?'aUser':'uUser').value.trim();
  const p=document.getElementById(isAdmin?'aPass':'uPass').value;
  const errEl=document.getElementById(isAdmin?'aErr':'uErr');
  const btn=document.querySelector(isAdmin?'.btn-adm':'.btn-usr');
  errEl.className='errmsg';
  if(!u||!p){errEl.textContent='❌ Please enter username and password.';errEl.className='errmsg show';return;}
  btn.disabled=true;btn.textContent='CHECKING…';
  fetch('/api/login?role='+role+'&user='+encodeURIComponent(u)+'&pass='+encodeURIComponent(p))
    .then(r=>r.json()).then(d=>{
      if(d.success){btn.textContent='✓ REDIRECTING…';window.location.href=(role==='admin')?'/app':'/userpage';}
      else{errEl.textContent='❌ '+(d.message||'Invalid credentials');errEl.className='errmsg show';btn.disabled=false;btn.textContent=isAdmin?'🛡️  ACCESS ADMIN PANEL':'🚘  ACCESS USER PORTAL';}
    }).catch(()=>{errEl.textContent='❌ Connection error.';errEl.className='errmsg show';btn.disabled=false;btn.textContent=isAdmin?'🛡️  ACCESS ADMIN PANEL':'🚘  ACCESS USER PORTAL';});
}
</script>
</body></html>
)html";

// ─────────────────────────────────────────────────────────
//  USER PAGE
// ─────────────────────────────────────────────────────────
const char* USER_PAGE = R"html(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Smart Parking — User Portal</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Rajdhani:wght@400;500;600;700&family=Exo+2:wght@300;400;600;700;800&display=swap');
*{margin:0;padding:0;box-sizing:border-box;}
:root{--g:#10b981;--bg:#050a14;--card:rgba(10,18,36,0.88);--border:rgba(16,185,129,0.18);--tx:#e2e8f0;--tm:#64748b;}
body{font-family:'Exo 2',sans-serif;background:var(--bg);color:var(--tx);height:100vh;overflow:hidden;display:flex;flex-direction:column;}
.bg{position:fixed;inset:0;z-index:0;pointer-events:none;}
.bg-grid{position:absolute;inset:0;background-image:linear-gradient(rgba(16,185,129,.03)1px,transparent 1px),linear-gradient(90deg,rgba(16,185,129,.03)1px,transparent 1px);background-size:50px 50px;}
.bg-g{position:absolute;border-radius:50%;filter:blur(110px);}
.bg-g1{width:500px;height:500px;background:rgba(16,185,129,.06);top:-160px;right:-70px;}
.bg-g2{width:350px;height:350px;background:rgba(5,150,105,.04);bottom:0;left:-40px;}
.app{display:flex;flex-direction:column;height:100vh;position:relative;z-index:1;}
.hdr{background:linear-gradient(180deg,rgba(5,10,20,.98),rgba(8,14,28,.95));border-bottom:1px solid var(--border);padding:12px 18px;position:relative;overflow:hidden;}
.hdr::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;background:linear-gradient(90deg,#059669,#10b981,#34d399,#10b981,#059669);background-size:300% 100%;animation:hdrShimmer 4s linear infinite;}
@keyframes hdrShimmer{0%{background-position:100% 0}100%{background-position:-200% 0}}
.hdr-top{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;}
.hdr-brand{font-family:'Rajdhani',sans-serif;font-size:19px;font-weight:700;letter-spacing:2px;background:linear-gradient(135deg,#34d399,#6ee7b7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}
.user-pill{display:flex;align-items:center;gap:6px;background:rgba(16,185,129,.1);border:1px solid rgba(16,185,129,.25);padding:4px 11px;border-radius:16px;font-size:10px;color:#34d399;font-weight:700;}
.udot{width:6px;height:6px;background:#10b981;border-radius:50%;animation:blink 1.5s ease-in-out infinite;}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.15}}
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;}
.sp{background:rgba(12,20,40,.65);border:1px solid rgba(16,185,129,.1);border-radius:9px;padding:8px 6px;text-align:center;}
.sv{font-family:'Rajdhani',sans-serif;font-size:18px;font-weight:700;color:#34d399;line-height:1;}
.sl{font-size:9px;color:var(--tm);margin-top:3px;text-transform:uppercase;letter-spacing:.4px;}
#tc{position:fixed;top:14px;right:14px;z-index:9999;display:flex;flex-direction:column;gap:8px;pointer-events:none;}
.toast{background:rgba(6,12,24,.96);backdrop-filter:blur(20px);border-radius:12px;padding:12px 16px;border-left:4px solid var(--g);box-shadow:0 8px 32px rgba(0,0,0,.6);font-size:12px;color:var(--tx);max-width:280px;pointer-events:all;animation:tIn .35s cubic-bezier(.34,1.56,.64,1);display:flex;align-items:flex-start;gap:10px;}
.toast.tw{border-left-color:#ef4444;}.toast.ty{border-left-color:#f59e0b;}
.ti{font-size:18px;flex-shrink:0;}.tt{font-weight:700;font-family:'Rajdhani',sans-serif;font-size:13px;letter-spacing:.4px;margin-bottom:2px;}.tm2{font-size:11px;color:#94a3b8;}
@keyframes tIn{from{opacity:0;transform:translateX(55px)}to{opacity:1;transform:none}}
.content{flex:1;overflow-y:auto;padding:14px 14px 6px;}
.content::-webkit-scrollbar{width:3px;}
.content::-webkit-scrollbar-thumb{background:rgba(16,185,129,.2);border-radius:2px;}
.screen{display:none;max-width:520px;margin:0 auto;animation:fadeUp .35s ease;}
.screen.active{display:block;}
@keyframes fadeUp{from{opacity:0;transform:translateY(16px)}to{opacity:1;transform:none}}
.banner{background:linear-gradient(135deg,rgba(16,185,129,.18),rgba(5,150,105,.08));border:1px solid rgba(16,185,129,.28);border-radius:14px;padding:18px;margin-bottom:14px;position:relative;overflow:hidden;}
.banner::after{content:'🚘';position:absolute;right:16px;top:50%;transform:translateY(-50%);font-size:52px;opacity:.1;}
.banner h2{font-family:'Rajdhani',sans-serif;font-size:17px;font-weight:700;letter-spacing:1px;margin-bottom:4px;color:#34d399;}
.banner p{font-size:11px;color:#64748b;}
.fare-strip{display:flex;align-items:center;gap:10px;background:rgba(245,158,11,.07);border:1px solid rgba(245,158,11,.18);border-radius:10px;padding:10px 14px;margin-bottom:14px;font-size:11px;color:#fde68a;}
.fare-strip strong{font-family:'Rajdhani',sans-serif;font-size:13px;letter-spacing:.5px;}
.sec{margin-bottom:14px;}
.sec-hdr{font-family:'Rajdhani',sans-serif;font-size:12px;font-weight:600;letter-spacing:2px;text-transform:uppercase;color:#64748b;margin-bottom:8px;padding-bottom:5px;border-bottom:1px solid rgba(16,185,129,.1);display:flex;align-items:center;gap:7px;}
.sgrid{display:grid;grid-template-columns:1fr 1fr;gap:8px;}
.sc{border-radius:11px;padding:13px 9px;text-align:center;position:relative;overflow:hidden;}
.sc::before{content:'';position:absolute;top:0;left:0;right:0;height:2.5px;}
.sc.av{background:rgba(16,185,129,.07);border:1.5px solid rgba(16,185,129,.32);}
.sc.av::before{background:linear-gradient(90deg,#10b981,#34d399);}
.sc.oc{background:rgba(239,68,68,.07);border:1.5px solid rgba(239,68,68,.3);}
.sc.oc::before{background:linear-gradient(90deg,#ef4444,#f87171);}
.sc.rs{background:rgba(245,158,11,.07);border:1.5px solid rgba(245,158,11,.38);}
.sc.rs::before{background:linear-gradient(90deg,#f59e0b,#fbbf24);}
.sid{font-family:'Rajdhani',sans-serif;font-size:15px;font-weight:700;margin-bottom:3px;}
.sc.av .sid{color:#34d399;}.sc.oc .sid{color:#f87171;}.sc.rs .sid{color:#fbbf24;}
.ss{font-size:10px;font-weight:600;text-transform:uppercase;letter-spacing:.7px;}
.sc.av .ss{color:#6ee7b7;}.sc.oc .ss{color:#fca5a5;}.sc.rs .ss{color:#fde68a;}
.arow{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:4px;}
.ab{padding:13px;border:none;border-radius:11px;cursor:pointer;font-family:'Exo 2',sans-serif;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.8px;transition:all .25s;display:flex;flex-direction:column;align-items:center;gap:4px;}
.ab .ai{font-size:18px;}
.ab.gr{background:linear-gradient(135deg,#059669,#10b981);color:#fff;box-shadow:0 6px 18px rgba(16,185,129,.25);}
.ab.bl{background:linear-gradient(135deg,#2563eb,#1d4ed8);color:#fff;box-shadow:0 6px 18px rgba(37,99,235,.25);}
.ab.am{background:linear-gradient(135deg,#d97706,#f59e0b);color:#fff;box-shadow:0 6px 18px rgba(245,158,11,.25);}
.ab:hover{transform:translateY(-3px);filter:brightness(1.1);}
.fw{padding:6px 0 14px;}
.fc{background:var(--card);backdrop-filter:blur(18px);border:1px solid var(--border);border-radius:16px;padding:22px;box-shadow:0 18px 48px rgba(0,0,0,.4);}
.fc h2{font-family:'Rajdhani',sans-serif;font-size:19px;font-weight:700;letter-spacing:1.8px;margin-bottom:18px;color:#34d399;}
.fg{margin-bottom:13px;}
.fg label{display:block;font-size:10px;font-weight:700;color:var(--tm);text-transform:uppercase;letter-spacing:1.5px;margin-bottom:6px;}
.fg input,.fg select{width:100%;padding:11px 13px;background:rgba(5,10,20,.65);border:1.5px solid rgba(16,185,129,.16);border-radius:9px;color:var(--tx);font-size:13px;font-family:'Exo 2',sans-serif;transition:all .3s;}
.fg input:focus,.fg select:focus{outline:none;border-color:#10b981;box-shadow:0 0 0 3px rgba(16,185,129,.07);}
.fg select option{background:#0a1220;}
.fg input[type="datetime-local"]{color-scheme:dark;}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:12px;}
.sp-picker{display:grid;grid-template-columns:repeat(3,1fr);gap:7px;margin-top:8px;}
.spb{padding:10px 6px;border-radius:7px;font-size:11px;font-weight:700;font-family:'Exo 2',sans-serif;cursor:pointer;transition:all .25s;text-align:center;background:rgba(10,20,40,.5);border:1.5px solid rgba(16,185,129,.16);color:#64748b;}
.spb:hover{border-color:#10b981;color:#34d399;}
.spb.sel{background:rgba(16,185,129,.16);border-color:#10b981;color:#34d399;}
.avbox{background:rgba(16,185,129,.05);border:1.5px solid rgba(16,185,129,.2);border-radius:10px;padding:13px;margin:11px 0;}
.avbox h3{font-size:10px;color:#34d399;font-weight:700;text-transform:uppercase;letter-spacing:.9px;margin-bottom:8px;}
.fullmsg{text-align:center;font-size:11px;color:#f87171;background:rgba(239,68,68,.07);padding:8px;border-radius:7px;}
.ibox{border-radius:9px;padding:11px 13px;margin-bottom:13px;font-size:11px;border-left:3px solid;}
.ibox.g{background:rgba(16,185,129,.07);border-color:#10b981;color:#86efac;}
.ibox.b{background:rgba(59,130,246,.07);border-color:#3b82f6;color:#93c5fd;}
.fbox{background:rgba(16,185,129,.06);border:1.5px solid rgba(16,185,129,.26);border-radius:11px;padding:14px;text-align:center;margin-bottom:13px;}
.fl{font-size:10px;color:var(--tm);text-transform:uppercase;letter-spacing:.9px;margin-bottom:5px;}
.fv{font-family:'Rajdhani',sans-serif;font-size:32px;font-weight:700;color:#34d399;}
.br{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:16px;}
.fb{padding:12px;border:none;border-radius:9px;cursor:pointer;font-family:'Exo 2',sans-serif;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.8px;transition:all .25s;}
.fb.cg{background:linear-gradient(135deg,#059669,#10b981);color:#fff;}
.fb.bk{background:rgba(15,25,50,.55);color:#64748b;border:1.5px solid rgba(16,185,129,.16);}
.fb:hover{transform:translateY(-2px);filter:brightness(1.1);}
.msg{padding:10px 13px;border-radius:9px;margin-bottom:11px;font-size:11px;border-left:4px solid;display:none;}
.msg.show{display:block;}
.msg.err{background:rgba(239,68,68,.09);color:#fca5a5;border-color:#ef4444;}
.msg.suc{background:rgba(16,185,129,.09);color:#86efac;border-color:#10b981;}
/* QR MODAL */
.qr-overlay{position:fixed;inset:0;background:rgba(0,0,0,.85);backdrop-filter:blur(14px);z-index:8000;display:flex;align-items:center;justify-content:center;}
.qr-modal{background:rgba(6,12,24,.98);border:1px solid rgba(16,185,129,.4);border-radius:22px;padding:28px;max-width:320px;width:92%;text-align:center;box-shadow:0 40px 80px rgba(0,0,0,.8);animation:rIn .38s cubic-bezier(.34,1.56,.64,1);}
@keyframes rIn{from{opacity:0;transform:scale(.82)}to{opacity:1;transform:scale(1)}}
.qr-title{font-family:'Rajdhani',sans-serif;font-size:20px;font-weight:700;letter-spacing:2px;background:linear-gradient(135deg,#34d399,#6ee7b7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;margin-bottom:4px;}
.qr-sub{font-size:10px;color:#475569;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:16px;}
.qr-box{background:white;border-radius:14px;padding:14px;display:inline-block;margin-bottom:14px;box-shadow:0 0 0 3px rgba(16,185,129,.3);}
.qr-amt{font-family:'Rajdhani',sans-serif;font-size:36px;font-weight:700;color:#34d399;margin-bottom:4px;}
.qr-bid{font-size:11px;color:#64748b;margin-bottom:12px;}
.qr-inst{font-size:10px;color:#94a3b8;background:rgba(16,185,129,.07);border:1px solid rgba(16,185,129,.15);border-radius:8px;padding:9px 12px;margin-bottom:12px;line-height:1.6;}
.qr-gate-info{font-size:10px;color:#fde68a;background:rgba(245,158,11,.07);border:1px solid rgba(245,158,11,.2);border-radius:8px;padding:8px 12px;margin-bottom:12px;line-height:1.6;}
.qr-timer{font-size:11px;color:#f59e0b;margin-bottom:14px;font-weight:600;}
.qr-close{width:100%;padding:12px;border:none;border-radius:10px;background:rgba(15,25,50,.55);color:#94a3b8;font-size:10px;font-weight:700;font-family:'Exo 2',sans-serif;letter-spacing:1.2px;text-transform:uppercase;cursor:pointer;border:1px solid rgba(16,185,129,.18);}
.qr-close:hover{border-color:#10b981;color:#34d399;}
/* Bookings */
.bk-list{display:flex;flex-direction:column;gap:10px;margin-bottom:12px;}
.bk-card{background:var(--card);border:1px solid var(--border);border-radius:13px;padding:15px;position:relative;overflow:hidden;}
.bk-card::before{content:'';position:absolute;left:0;top:0;bottom:0;width:3px;}
.bk-card.up::before{background:#3b82f6;}.bk-card.ac::before{background:#10b981;}.bk-card.co::before{background:#475569;}.bk-card.ca::before{background:#ef4444;}
.bk-top{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:9px;}
.bk-id{font-family:'Rajdhani',sans-serif;font-size:16px;font-weight:700;}
.bk-badge{font-size:9px;font-weight:700;padding:3px 9px;border-radius:10px;text-transform:uppercase;letter-spacing:.8px;}
.bk-badge.up{background:rgba(59,130,246,.14);color:#60a5fa;border:1px solid rgba(59,130,246,.18);}
.bk-badge.ac{background:rgba(16,185,129,.14);color:#34d399;border:1px solid rgba(16,185,129,.18);}
.bk-badge.co{background:rgba(100,116,139,.14);color:#94a3b8;border:1px solid rgba(100,116,139,.18);}
.bk-badge.ca{background:rgba(239,68,68,.14);color:#f87171;border:1px solid rgba(239,68,68,.18);}
.bk-rows{display:grid;grid-template-columns:1fr 1fr;gap:5px;}
.bk-row{font-size:11px;}.bk-rl{color:var(--tm);font-size:9px;text-transform:uppercase;letter-spacing:.5px;margin-bottom:1px;}.bk-rv{color:var(--tx);font-weight:600;}
.bk-actions{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap;}
.bk-btn{padding:7px 14px;border:none;border-radius:7px;font-size:10px;font-weight:700;font-family:'Exo 2',sans-serif;cursor:pointer;text-transform:uppercase;letter-spacing:.5px;transition:all .2s;}
.cancel-btn{background:rgba(239,68,68,.12);color:#f87171;border:1px solid rgba(239,68,68,.2);}
.pay-btn{background:rgba(245,158,11,.12);color:#fbbf24;border:1px solid rgba(245,158,11,.2);}
.pay-btn:hover{background:rgba(245,158,11,.22);}
.qr-btn{background:rgba(16,185,129,.12);color:#34d399;border:1px solid rgba(16,185,129,.2);}
.qr-btn:hover{background:rgba(16,185,129,.22);}
.bk-empty{text-align:center;padding:36px 18px;color:var(--tm);font-size:12px;}
.bk-empty .be-icon{font-size:38px;margin-bottom:11px;opacity:.35;}
.filter-row{display:flex;gap:7px;margin-bottom:13px;flex-wrap:wrap;}
.filter-btn{padding:5px 11px;border:1px solid var(--border);border-radius:16px;font-size:10px;font-weight:700;font-family:'Exo 2',sans-serif;cursor:pointer;transition:all .2s;background:transparent;color:var(--tm);letter-spacing:.5px;}
.filter-btn.active{background:rgba(16,185,129,.13);border-color:#10b981;color:#34d399;}
.bnav{display:grid;grid-template-columns:repeat(3,1fr);background:rgba(5,9,18,.98);border-top:1px solid var(--border);}
.nb{padding:12px 3px;text-align:center;border:none;background:none;color:var(--tm);cursor:pointer;transition:all .25s;font-family:'Exo 2',sans-serif;font-size:8px;font-weight:700;text-transform:uppercase;letter-spacing:.4px;border-top:2px solid transparent;display:flex;flex-direction:column;align-items:center;gap:3px;}
.nb .ni{font-size:17px;}
.nb.active{color:#34d399;border-top-color:#10b981;background:rgba(16,185,129,.05);}
.logout-btn{position:absolute;top:14px;right:60px;background:rgba(239,68,68,.1);border:1px solid rgba(239,68,68,.2);border-radius:8px;padding:5px 11px;font-size:9px;color:#f87171;cursor:pointer;font-family:'Exo 2',sans-serif;font-weight:700;text-transform:uppercase;letter-spacing:.5px;}
</style></head><body>
<div id="tc"></div>
<div class="bg"><div class="bg-grid"></div><div class="bg-g bg-g1"></div><div class="bg-g bg-g2"></div></div>
<div class="app">
<div class="hdr">
  <div class="hdr-top">
    <div class="hdr-brand">🚘 USER PORTAL</div>
    <button class="logout-btn" onclick="location.href='/'">⬅ LOGOUT</button>
    <div class="user-pill"><div class="udot"></div>USER</div>
  </div>
  <div class="stats">
    <div class="sp"><div class="sv" id="s4w">0/4</div><div class="sl">Used</div></div>
    <div class="sp"><div class="sv" id="s4a">2</div><div class="sl">Free</div></div>
  </div>
</div>
<div class="content">

<!-- HOME -->
<div class="screen active" id="homeScreen">
  <div class="banner"><h2>WELCOME, PARKER! 👋</h2><p>Check availability · Reserve · Pay online · Get QR pass to exit</p></div>
  <div class="fare-strip"><span style="font-size:18px">💡</span><div><strong>FARE: ₹10 / 30 MIN</strong> · Min ₹10 · Pay on dashboard, scan QR to exit</div></div>
  <div class="sec"><div class="sec-hdr"><span>🚗</span>PARKING BAYS</div><div class="sgrid" id="g4w"></div></div>
  <div class="arow">
    <button class="ab gr" onclick="sw('reserveScreen')"><span class="ai">📅</span>RESERVE</button>
    <button class="ab bl" onclick="sw('bookingsScreen')"><span class="ai">📋</span>MY BOOKINGS</button>
  </div>
</div>

<!-- RESERVE -->
<div class="screen" id="reserveScreen">
<div class="fw"><div class="fc">
  <h2>📅 RESERVE A SLOT</h2>
  <div class="msg err" id="rErr"></div><div class="msg suc" id="rSuc"></div>
  <div class="row2">
    <div class="fg"><label>Your Name</label><input type="text" id="rName" placeholder="Full name"></div>
    <div class="fg"><label>Phone</label><input type="tel" id="rPhone" placeholder="9876543210"></div>
  </div>
  <div class="fg"><label>Vehicle Number</label><input type="text" id="rVeh" placeholder="MH01AB1234"></div>
  <div class="fg"><label>Vehicle Type</label>
    <select id="rType" onchange="loadSlots()">
      <option value="four" selected>🚗 4-Wheeler</option>
    </select>
  </div>
  <div class="row2">
    <div class="fg"><label>Start Time</label><input type="datetime-local" id="rStart"></div>
    <div class="fg"><label>Duration</label>
      <select id="rDur" onchange="updateFarePreview()">
        <option value="">--</option>
        <option value="0.5">30 min</option><option value="1">1 hour</option>
        <option value="1.5">1.5 hrs</option><option value="2" selected>2 hours</option>
        <option value="3">3 hours</option><option value="4">4 hours</option>
        <option value="6">6 hours</option><option value="8">8 hours</option>
      </select>
    </div>
  </div>
  <div class="avbox" id="slotBox" style="display:none">
    <h3>✨ SELECT YOUR SLOT</h3>
    <div class="sp-picker" id="slotPicker"></div>
    <div class="fullmsg" id="slotFull" style="display:none"></div>
  </div>
  <div id="farePreview" style="display:none;background:rgba(16,185,129,.06);border:1px solid rgba(16,185,129,.18);border-radius:10px;padding:13px;margin-bottom:13px;text-align:center;">
    <div style="font-size:10px;color:#64748b;text-transform:uppercase;letter-spacing:.8px;margin-bottom:3px;">ESTIMATED FARE</div>
    <div id="farePreviewVal" style="font-family:'Rajdhani',sans-serif;font-size:26px;font-weight:700;color:#34d399;">₹0</div>
    <div id="farePreviewBreak" style="font-size:10px;color:#64748b;margin-top:3px;"></div>
  </div>
  <div class="ibox b">📋 Booking ID generated. After exit, pay here → get QR → scan at gate!</div>
  <div class="br">
    <button class="fb cg" onclick="doReserve()">✓ CONFIRM BOOKING</button>
    <button class="fb bk" onclick="sw('homeScreen')">← BACK</button>
  </div>
</div></div>
</div>

<!-- MY BOOKINGS -->
<div class="screen" id="bookingsScreen">
<div class="fw"><div class="fc">
  <h2>📋 MY BOOKINGS</h2>
  <div class="filter-row">
    <button class="filter-btn active" onclick="filterBooks('all',this)">ALL</button>
    <button class="filter-btn" onclick="filterBooks('upcoming',this)">UPCOMING</button>
    <button class="filter-btn" onclick="filterBooks('active',this)">ACTIVE</button>
    <button class="filter-btn" onclick="filterBooks('completed',this)">DONE</button>
    <button class="filter-btn" onclick="filterBooks('cancelled',this)">CANCELLED</button>
  </div>
  <div class="bk-list" id="bkList"></div>
  <div class="br" style="margin-top:4px;">
    <button class="fb cg" onclick="sw('reserveScreen')">+ NEW BOOKING</button>
    <button class="fb bk" onclick="sw('homeScreen')">← HOME</button>
  </div>
</div></div>
</div>

<!-- PAY & QR Screen -->
<div class="screen" id="payScreen">
<div class="fw"><div class="fc">
  <h2>💳 PAY &amp; GET QR PASS</h2>
  <div class="msg err" id="pErr"></div><div class="msg suc" id="pSuc"></div>
  <div class="fg"><label>Booking ID or Vehicle Number</label>
    <input type="text" id="pRef" placeholder="BK001 or MH01AB1234" oninput="fetchFare()">
  </div>
  <div class="fbox"><div class="fl">💰 AMOUNT DUE</div><div class="fv" id="payAmt">₹0</div></div>
  <div class="ibox g">✅ After payment, you'll receive a QR code. Scan it at the exit gate to open the boom barrier.</div>
  <div class="br">
    <button class="fb cg" onclick="doPay()">💳 PAY NOW &amp; GET QR</button>
    <button class="fb bk" onclick="sw('homeScreen')">← BACK</button>
  </div>
</div></div>
</div>

</div><!-- /content -->
<div class="bnav">
  <button class="nb active" onclick="sw('homeScreen')"><span class="ni">🏠</span>HOME</button>
  <button class="nb" onclick="sw('reserveScreen')"><span class="ni">📅</span>RESERVE</button>
  <button class="nb" onclick="sw('bookingsScreen')"><span class="ni">📋</span>BOOKINGS</button>
</div>
</div>

<!-- QR Modal -->
<div id="qrOverlay" class="qr-overlay" style="display:none;">
  <div class="qr-modal">
    <div class="qr-title">📱 YOUR EXIT PASS</div>
    <div class="qr-sub">Scan at exit gate to open boom barrier</div>
    <div class="qr-box"><canvas id="qrCanvas" width="160" height="160"></canvas></div>
    <div class="qr-amt" id="qrAmtDisp">₹0</div>
    <div class="qr-bid" id="qrBidDisp">Booking ID</div>
    <div class="qr-gate-info">🚧 Walk to exit gate → Car IR detected → LCD shows "Scan QR" → Scan this code → Gate opens automatically!</div>
    <div class="qr-inst">Open camera or any QR scanner app and point at the QR code above.</div>
    <div class="qr-timer">⏳ QR expires in: <span id="qrCountdown">300</span>s</div>
    <button class="qr-close" onclick="closeQr()">✕ CLOSE</button>
  </div>
</div>

<script>
let selSlot=-1,selSlotName='',curFilter='all',qrTimer=null;

function calcFareHours(hrs){const mins=Math.max(1,Math.round(hrs*60));const slots=Math.max(1,Math.ceil(mins/30));return{mins,slots,amount:slots*10};}
function toast(icon,title,msg,type='o'){const c=document.getElementById('tc');const t=document.createElement('div');t.className='toast t'+type[0];t.innerHTML=`<div class="ti">${icon}</div><div><div class="tt">${title}</div><div class="tm2">${msg}</div></div>`;c.appendChild(t);setTimeout(()=>{t.remove();},4500);}
function refreshStats(){fetch('/api/stats').then(r=>r.json()).then(d=>{document.getElementById('s4w').textContent=d.fourOccupied+'/4';document.getElementById('s4a').textContent=d.fourAvailable;buildGrid('4w',d.fourSlots,['B1','B2','B3','B4'],['🚗','🚗','🚗','🚗']);});}
function buildGrid(id,slots,names,icons){const g=document.getElementById('g'+id);g.innerHTML='';slots.forEach((st,i)=>{const cls=st===0?'av':st===2?'rs':'oc';const lbl=st===0?'✓ AVAILABLE':st===2?'⏳ RESERVED':'✗ OCCUPIED';const el=document.createElement('div');el.className='sc '+cls;el.innerHTML=`<div style="font-size:20px;margin-bottom:3px;">${icons[i]}</div><div class="sid">SLOT ${names[i]}</div><div class="ss">${lbl}</div>`;g.appendChild(el);});}
function initDatetime(){const inp=document.getElementById('rStart');if(!inp.value){const now=new Date();now.setMinutes(now.getMinutes()+5);inp.value=now.toISOString().slice(0,16);}}
function loadSlots(){const type=document.getElementById('rType').value;if(!type){document.getElementById('slotBox').style.display='none';return;}fetch('/api/available-slots?type='+type).then(r=>r.json()).then(d=>{const box=document.getElementById('slotBox');const picker=document.getElementById('slotPicker');const full=document.getElementById('slotFull');const names=['B1','B2','B3','B4'];selSlot=-1;selSlotName='';if(d.slots.length===0){full.innerHTML='❌ All slots full';full.style.display='block';picker.innerHTML='';}else{full.style.display='none';picker.innerHTML='';d.slots.forEach(idx=>{const btn=document.createElement('button');btn.type='button';btn.className='spb';btn.textContent='SLOT '+names[idx];btn.onclick=()=>{selSlot=idx;selSlotName=names[idx];document.querySelectorAll('.spb').forEach(b=>b.classList.remove('sel'));btn.classList.add('sel');updateFarePreview();};picker.appendChild(btn);});}box.style.display='block';updateFarePreview();});}
function updateFarePreview(){const dur=parseFloat(document.getElementById('rDur').value)||0;const fp=document.getElementById('farePreview');if(dur>0){const f=calcFareHours(dur);document.getElementById('farePreviewVal').textContent='₹'+f.amount;document.getElementById('farePreviewBreak').textContent=f.slots+' slot(s) × ₹10 ('+f.mins+' min est.)';fp.style.display='block';}else{fp.style.display='none';}}
function doReserve(){if(selSlot===-1){showMsg('rErr','Please select a slot','err');return;}const name=document.getElementById('rName').value;const phone=document.getElementById('rPhone').value;const veh=document.getElementById('rVeh').value;const type=document.getElementById('rType').value;const start=document.getElementById('rStart').value;const dur=document.getElementById('rDur').value;if(!name||!phone||!veh||!type||!start||!dur){showMsg('rErr','Fill all fields','err');return;}const url=`/api/reserve?name=${encodeURIComponent(name)}&phone=${encodeURIComponent(phone)}&vehicle=${encodeURIComponent(veh)}&type=${type}&slot=${selSlot}&start=${encodeURIComponent(start)}&duration=${dur}`;fetch(url).then(r=>r.json()).then(d=>{if(d.success){const f=calcFareHours(parseFloat(dur));showMsg('rSuc',`Booking confirmed! ID: ${d.bookingId} · Est. ₹${f.amount}`,'suc');toast('✅','BOOKING CONFIRMED',`${d.bookingId} · Slot ${selSlotName}`,'o');selSlot=-1;selSlotName='';refreshStats();refreshBookings();setTimeout(()=>sw('bookingsScreen'),1800);}else{showMsg('rErr',d.message||'Booking failed','err');}});}
function filterBooks(f,btn){curFilter=f;document.querySelectorAll('.filter-btn').forEach(b=>b.classList.remove('active'));btn.classList.add('active');refreshBookings();}
function refreshBookings(){fetch('/api/bookings').then(r=>r.json()).then(d=>{const list=document.getElementById('bkList');let rows=d.bookings;if(curFilter!=='all')rows=rows.filter(b=>b.status===curFilter);if(rows.length===0){list.innerHTML=`<div class="bk-empty"><div class="be-icon">📭</div>No bookings found.</div>`;return;}list.innerHTML='';rows.slice().reverse().forEach(b=>{const stCls={upcoming:'up',active:'ac',completed:'co',cancelled:'ca',expired:'up'}[b.status]||'co';const stLbl={upcoming:'UPCOMING',active:'ACTIVE ●',completed:'COMPLETED',cancelled:'CANCELLED',expired:'EXPIRED'}[b.status]||b.status.toUpperCase();const el=document.createElement('div');el.className='bk-card '+stCls;
const cancelBtn=(b.status==='upcoming')?`<button class="bk-btn cancel-btn" onclick="cancelBooking('${b.bookingId}')">✕ CANCEL</button>`:'';
const payBtn=(b.status==='completed'&&!b.isPaid)?`<button class="bk-btn pay-btn" onclick="openPayScreen('${b.bookingId}',${b.amount})">💳 PAY ₹${b.amount}</button>`:'';
const qrBtn=(b.status==='completed'&&b.isPaid)?`<button class="bk-btn qr-btn" onclick="showQrForPaid('${b.bookingId}',${b.amount})">📱 SHOW QR PASS</button>`:'';
el.innerHTML=`<div class="bk-top"><div class="bk-id">${b.bookingId}</div><div class="bk-badge ${stCls}">${stLbl}</div></div><div class="bk-rows"><div class="bk-row"><div class="bk-rl">Vehicle</div><div class="bk-rv">${b.vehicleNumber}</div></div><div class="bk-row"><div class="bk-rl">Slot</div><div class="bk-rv">${b.slotName}</div></div><div class="bk-row"><div class="bk-rl">Duration</div><div class="bk-rv">${b.duration}h</div></div><div class="bk-row"><div class="bk-rl">Amount</div><div class="bk-rv" style="color:#34d399">${b.amount>0?'₹'+b.amount:'--'}</div></div>${b.isPaid?'<div class="bk-row" style="grid-column:span 2"><div class="bk-rl">Payment</div><div class="bk-rv" style="color:#34d399">✓ PAID</div></div>':''}</div>${(cancelBtn||payBtn||qrBtn)?`<div class="bk-actions">${cancelBtn}${payBtn}${qrBtn}</div>`:''}`;list.appendChild(el);});});}
function cancelBooking(bid){if(!confirm('Cancel booking '+bid+'?'))return;fetch('/api/cancel?booking='+bid).then(r=>r.json()).then(d=>{if(d.success){toast('🗑️','CANCELLED',bid+' cancelled.','y');refreshBookings();refreshStats();}});}
function openPayScreen(bid,amount){document.getElementById('pRef').value=bid;document.getElementById('payAmt').textContent='₹'+amount;sw('payScreen');}
function fetchFare(){const ref=document.getElementById('pRef').value;if(!ref)return;fetch('/api/get-fare?booking='+encodeURIComponent(ref)).then(r=>r.json()).then(d=>{if(d.amount>0)document.getElementById('payAmt').textContent='₹'+d.amount;});}
function doPay(){const ref=document.getElementById('pRef').value.trim();const amt=parseInt(document.getElementById('payAmt').textContent.replace('₹',''))||0;if(!ref){showMsg('pErr','Enter booking ID or vehicle number','err');return;}if(amt===0){showMsg('pErr','Amount is ₹0 — check booking ID','err');return;}fetch('/api/payment?booking='+encodeURIComponent(ref)).then(r=>r.json()).then(d=>{if(d.success){toast('💳','PAYMENT DONE','₹'+d.amount+' received!','o');showMsg('pSuc','Payment successful! Showing QR exit pass...','suc');fetch('/api/generate-token?booking='+encodeURIComponent(ref)).then(r=>r.json()).then(td=>{if(td.success){showQrModal(td.token,d.amount,ref);}});}else{showMsg('pErr',d.message||'Payment failed','err');}});}
function showQrForPaid(bid,amount){fetch('/api/generate-token?booking='+encodeURIComponent(bid)).then(r=>r.json()).then(td=>{if(td.success){showQrModal(td.token,amount,bid);}else{toast('⚠️','Already exited','No active token for this booking.','y');}});}
function showQrModal(token,amount,bid){const espIp=window.location.hostname;const url='http://'+espIp+'/gate/open?token='+token;document.getElementById('qrAmtDisp').textContent='₹'+amount;document.getElementById('qrBidDisp').textContent='Booking: '+bid;drawQr(url);document.getElementById('qrOverlay').style.display='flex';let secs=300;document.getElementById('qrCountdown').textContent=secs;clearInterval(qrTimer);qrTimer=setInterval(()=>{secs--;document.getElementById('qrCountdown').textContent=secs;if(secs<=0){clearInterval(qrTimer);closeQr();}},1000);}
function closeQr(){clearInterval(qrTimer);document.getElementById('qrOverlay').style.display='none';}

/* Simple QR code generator using qrcode-generator library from CDN */
function drawQr(text){
  const canvas=document.getElementById('qrCanvas');
  const ctx=canvas.getContext('2d');
  ctx.fillStyle='white';ctx.fillRect(0,0,160,160);
  // Use Google Charts API as QR source (embedded as image)
  const img=new Image();
  img.onload=()=>ctx.drawImage(img,0,0,160,160);
  img.src='https://api.qrserver.com/v1/create-qr-code/?size=160x160&data='+encodeURIComponent(text);
}

function sw(id){document.querySelectorAll('.screen').forEach(s=>s.classList.remove('active'));document.getElementById(id).classList.add('active');const m={homeScreen:0,reserveScreen:1,bookingsScreen:2};document.querySelectorAll('.nb').forEach(b=>b.classList.remove('active'));if(m[id]!==undefined)document.querySelectorAll('.nb')[m[id]].classList.add('active');if(id==='reserveScreen'){initDatetime();loadSlots();updateFarePreview();}if(id==='bookingsScreen')refreshBookings();}
function showMsg(id,msg,type){const el=document.getElementById(id);el.textContent=(type==='err'?'❌ ':'✅ ')+msg;el.className='msg show '+type;setTimeout(()=>el.classList.remove('show'),6000);}

setInterval(refreshStats,3000);
setInterval(refreshBookings,8000);
refreshStats();refreshBookings();initDatetime();
</script>
</body></html>
)html";

// ─────────────────────────────────────────────────────────
//  ADMIN APP PAGE
// ─────────────────────────────────────────────────────────
const char* APP_PAGE = R"html(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Smart Parking Pro — Admin</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Rajdhani:wght@400;500;600;700&family=Exo+2:wght@300;400;600;700;800&display=swap');
*{margin:0;padding:0;box-sizing:border-box;}
:root{--p:#2563eb;--g:#10b981;--y:#f59e0b;--r:#ef4444;--bg:#050a14;--card:rgba(10,18,36,0.88);--border:rgba(59,130,246,0.18);--tx:#e2e8f0;--tm:#64748b;}
body{font-family:'Exo 2',sans-serif;background:var(--bg);color:var(--tx);height:100vh;overflow:hidden;display:flex;flex-direction:column;}
.bg{position:fixed;inset:0;z-index:0;pointer-events:none;}
.bg-grid{position:absolute;inset:0;background-image:linear-gradient(rgba(59,130,246,.035)1px,transparent 1px),linear-gradient(90deg,rgba(59,130,246,.035)1px,transparent 1px);background-size:50px 50px;}
.bg-g{position:absolute;border-radius:50%;filter:blur(110px);}
.bg-g1{width:550px;height:550px;background:rgba(37,99,235,.07);top:-180px;right:-80px;}
.bg-g2{width:380px;height:380px;background:rgba(16,185,129,.05);bottom:0;left:-40px;}
.app{display:flex;flex-direction:column;height:100vh;position:relative;z-index:1;}
.hdr{background:linear-gradient(180deg,rgba(5,10,20,.98),rgba(8,14,28,.95));border-bottom:1px solid var(--border);padding:12px 18px;position:relative;overflow:hidden;}
.hdr::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;background:linear-gradient(90deg,#1d4ed8,#2563eb,#10b981,#f59e0b,#2563eb,#1d4ed8);background-size:300% 100%;animation:hdrShimmer 4s linear infinite;}
@keyframes hdrShimmer{0%{background-position:100% 0}100%{background-position:-200% 0}}
.hdr-top{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;}
.hdr-brand{font-family:'Rajdhani',sans-serif;font-size:20px;font-weight:700;letter-spacing:2.5px;background:linear-gradient(135deg,#60a5fa,#34d399);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;}
.live{display:flex;align-items:center;gap:5px;background:rgba(16,185,129,.1);border:1px solid rgba(16,185,129,.25);padding:4px 10px;border-radius:16px;font-size:10px;color:#34d399;font-weight:700;}
.live-dot{width:6px;height:6px;background:#10b981;border-radius:50%;animation:blink 1.5s ease-in-out infinite;}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.15}}
.admin-badge{background:rgba(37,99,235,.12);border:1px solid rgba(59,130,246,.25);border-radius:6px;padding:3px 9px;font-size:9px;color:#60a5fa;font-weight:700;letter-spacing:.8px;margin-right:6px;}
.logout-link{font-size:9px;color:#475569;text-decoration:none;padding:4px 8px;border:1px solid #1a3050;border-radius:6px;font-family:'Exo 2',sans-serif;cursor:pointer;background:none;}
.logout-link:hover{color:#f87171;border-color:rgba(239,68,68,.3);}
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;}
.sp{background:rgba(12,20,40,.65);border:1px solid rgba(59,130,246,.12);border-radius:9px;padding:8px 6px;text-align:center;}
.sv{font-family:'Rajdhani',sans-serif;font-size:18px;font-weight:700;color:#fbbf24;line-height:1;}
.sl{font-size:9px;color:var(--tm);margin-top:3px;text-transform:uppercase;letter-spacing:.4px;}
.gate-bar{display:flex;align-items:center;justify-content:space-between;background:rgba(5,9,18,.8);border-bottom:1px solid var(--border);padding:6px 18px;font-size:10px;gap:12px;}
.gate-pill{display:flex;align-items:center;gap:5px;font-size:10px;font-weight:700;}
.gpd{width:7px;height:7px;border-radius:50%;display:inline-block;}
.gate-open .gpd{background:#10b981;box-shadow:0 0 5px #10b981;animation:gpulse .9s ease-in-out infinite;}
.gate-closed .gpd{background:#ef4444;}
.gate-car .gpd{background:#f59e0b;animation:gpulse .9s ease-in-out infinite;}
@keyframes gpulse{0%,100%{opacity:1}50%{opacity:.3}}
.gate-open{color:#34d399;}.gate-closed{color:#f87171;}.gate-car{color:#fbbf24;}
#tc{position:fixed;top:14px;right:14px;z-index:9999;display:flex;flex-direction:column;gap:8px;pointer-events:none;}
.toast{background:rgba(6,12,24,.96);backdrop-filter:blur(20px);border-radius:12px;padding:12px 16px;border-left:4px solid var(--p);box-shadow:0 8px 32px rgba(0,0,0,.6);font-size:12px;color:var(--tx);max-width:300px;pointer-events:all;animation:tIn .35s cubic-bezier(.34,1.56,.64,1);display:flex;align-items:flex-start;gap:10px;}
.toast.tw{border-left-color:#ef4444;}.toast.to{border-left-color:#10b981;}.toast.ty{border-left-color:#f59e0b;}.toast.tb{border-left-color:#3b82f6;}
.ti{font-size:18px;flex-shrink:0;}.tt{font-weight:700;font-family:'Rajdhani',sans-serif;font-size:13px;letter-spacing:.4px;margin-bottom:2px;}.tm2{font-size:11px;color:#94a3b8;}
@keyframes tIn{from{opacity:0;transform:translateX(55px)}to{opacity:1;transform:none}}
.content{flex:1;overflow-y:auto;padding:14px 14px 6px;}
.content::-webkit-scrollbar{width:3px;}
.content::-webkit-scrollbar-thumb{background:rgba(59,130,246,.25);border-radius:2px;}
.screen{display:none;max-width:580px;margin:0 auto;animation:fadeUp .35s ease;}
.screen.active{display:block;}
@keyframes fadeUp{from{opacity:0;transform:translateY(18px)}to{opacity:1;transform:none}}
.banner{background:linear-gradient(135deg,rgba(29,78,216,.28),rgba(16,185,129,.12));border:1px solid rgba(59,130,246,.25);border-radius:14px;padding:18px;margin-bottom:16px;position:relative;overflow:hidden;}
.banner::after{content:'🛡️';position:absolute;right:16px;top:50%;transform:translateY(-50%);font-size:55px;opacity:.1;}
.banner h2{font-family:'Rajdhani',sans-serif;font-size:18px;font-weight:700;letter-spacing:1px;margin-bottom:4px;}
.banner p{font-size:11px;color:#94a3b8;}
.fare-strip{display:flex;align-items:center;gap:10px;background:rgba(245,158,11,.07);border:1px solid rgba(245,158,11,.2);border-radius:10px;padding:10px 14px;margin-bottom:16px;font-size:11px;color:#fde68a;}
.fare-strip strong{font-family:'Rajdhani',sans-serif;font-size:13px;letter-spacing:.5px;}
.charts{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:16px;}
.cc{background:var(--card);backdrop-filter:blur(12px);border:1px solid var(--border);border-radius:12px;padding:16px;text-align:center;}
.ct{font-size:9px;text-transform:uppercase;letter-spacing:1.5px;color:var(--tm);margin-bottom:12px;}
.donut{width:80px;height:80px;border-radius:50%;margin:0 auto 10px;display:flex;align-items:center;justify-content:center;font-family:'Rajdhani',sans-serif;font-size:17px;font-weight:700;}
.ci{font-size:13px;font-weight:700;}.cs{font-size:10px;color:var(--tm);margin-top:1px;}
.sec{margin-bottom:16px;}
.sec-hdr{font-family:'Rajdhani',sans-serif;font-size:13px;font-weight:600;letter-spacing:2px;text-transform:uppercase;color:#94a3b8;margin-bottom:9px;padding-bottom:6px;border-bottom:1px solid rgba(59,130,246,.12);display:flex;align-items:center;gap:7px;}
.sgrid{display:grid;grid-template-columns:1fr 1fr;gap:9px;}
.sc{border-radius:11px;padding:14px 10px;text-align:center;position:relative;overflow:hidden;transition:transform .2s;}
.sc::before{content:'';position:absolute;top:0;left:0;right:0;height:2.5px;}
.sc.av{background:rgba(16,185,129,.07);border:1.5px solid rgba(16,185,129,.35);}
.sc.av::before{background:linear-gradient(90deg,#10b981,#34d399);}
.sc.oc{background:rgba(239,68,68,.07);border:1.5px solid rgba(239,68,68,.35);}
.sc.oc::before{background:linear-gradient(90deg,#ef4444,#f87171);}
.sc.rs{background:rgba(245,158,11,.08);border:1.5px solid rgba(245,158,11,.45);}
.sc.rs::before{background:linear-gradient(90deg,#f59e0b,#fbbf24);}
.irc{position:absolute;top:5px;left:5px;background:rgba(5,10,20,.8);border:1px solid rgba(16,185,129,.25);border-radius:5px;padding:1px 6px;font-size:8px;color:#34d399;font-weight:700;}
.sid{font-family:'Rajdhani',sans-serif;font-size:16px;font-weight:700;margin-bottom:4px;}
.sc.av .sid{color:#34d399;}.sc.oc .sid{color:#f87171;}.sc.rs .sid{color:#fbbf24;}
.ss{font-size:10px;font-weight:600;text-transform:uppercase;letter-spacing:.8px;}
.sc.av .ss{color:#6ee7b7;}.sc.oc .ss{color:#fca5a5;}.sc.rs .ss{color:#fde68a;}
.stime{font-size:9px;color:#64748b;margin-top:3px;}
.arow{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:6px;padding-bottom:6px;}
.ab{padding:13px;border:none;border-radius:11px;cursor:pointer;font-family:'Exo 2',sans-serif;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.8px;transition:all .25s;display:flex;flex-direction:column;align-items:center;gap:4px;}
.ab .ai{font-size:18px;}
.ab.bl{background:linear-gradient(135deg,#2563eb,#1d4ed8);color:#fff;box-shadow:0 6px 18px rgba(37,99,235,.28);}
.ab.gr{background:linear-gradient(135deg,#059669,#10b981);color:#fff;box-shadow:0 6px 18px rgba(16,185,129,.28);}
.ab.am{background:linear-gradient(135deg,#d97706,#f59e0b);color:#fff;box-shadow:0 6px 18px rgba(245,158,11,.28);}
.ab.rd{background:linear-gradient(135deg,#dc2626,#ef4444);color:#fff;box-shadow:0 6px 18px rgba(239,68,68,.28);}
.ab.pu{background:linear-gradient(135deg,#7c3aed,#8b5cf6);color:#fff;box-shadow:0 6px 18px rgba(124,58,237,.28);}
.ab:hover{transform:translateY(-3px);filter:brightness(1.1);}
.fw{padding:6px 0 14px;}
.fc{background:var(--card);backdrop-filter:blur(18px);border:1px solid var(--border);border-radius:16px;padding:22px;box-shadow:0 18px 48px rgba(0,0,0,.4);}
.fc h2{font-family:'Rajdhani',sans-serif;font-size:20px;font-weight:700;letter-spacing:1.8px;margin-bottom:18px;}
.fg{margin-bottom:15px;}
.fg label{display:block;font-size:10px;font-weight:700;color:var(--tm);text-transform:uppercase;letter-spacing:1.5px;margin-bottom:7px;}
.fg input,.fg select{width:100%;padding:11px 14px;background:rgba(5,10,20,.65);border:1.5px solid rgba(59,130,246,.18);border-radius:9px;color:var(--tx);font-size:13px;font-family:'Exo 2',sans-serif;transition:all .3s;}
.fg input:focus,.fg select:focus{outline:none;border-color:#3b82f6;box-shadow:0 0 0 3px rgba(59,130,246,.08);}
.fg select option{background:#0a1220;}
.fg input[type="datetime-local"]{color-scheme:dark;}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:12px;}
.sp-picker{display:grid;grid-template-columns:repeat(3,1fr);gap:7px;margin-top:9px;}
.spb{padding:10px 6px;border-radius:7px;font-size:11px;font-weight:700;font-family:'Exo 2',sans-serif;cursor:pointer;transition:all .25s;text-align:center;background:rgba(25,45,75,.5);border:1.5px solid rgba(59,130,246,.2);color:#94a3b8;}
.spb:hover{border-color:#3b82f6;color:#60a5fa;}
.spb.sel{background:rgba(16,185,129,.18);border-color:#10b981;color:#34d399;}
.avbox{background:rgba(16,185,129,.06);border:1.5px solid rgba(16,185,129,.22);border-radius:10px;padding:14px;margin:12px 0;}
.avbox h3{font-size:10px;color:#34d399;font-weight:700;text-transform:uppercase;letter-spacing:.9px;margin-bottom:9px;}
.fullmsg{text-align:center;font-size:11px;color:#f87171;background:rgba(239,68,68,.07);padding:8px;border-radius:7px;}
.ibox{border-radius:9px;padding:12px 14px;margin-bottom:15px;font-size:11px;border-left:3px solid;}
.ibox.g{background:rgba(16,185,129,.07);border-color:#10b981;color:#86efac;}
.ibox.a{background:rgba(245,158,11,.07);border-color:#f59e0b;color:#fde68a;}
.ibox.b{background:rgba(59,130,246,.07);border-color:#3b82f6;color:#93c5fd;}
.ibox.r{background:rgba(239,68,68,.07);border-color:#ef4444;color:#fca5a5;}
.timer-box{background:rgba(5,10,20,.85);border:1.5px solid rgba(59,130,246,.22);border-radius:11px;padding:16px;text-align:center;margin-bottom:15px;}
.tl{font-size:10px;color:var(--tm);text-transform:uppercase;letter-spacing:.9px;margin-bottom:8px;}
.tv{font-family:'Rajdhani',sans-serif;font-size:40px;font-weight:700;color:#f59e0b;}
.fbox{background:rgba(16,185,129,.06);border:1.5px solid rgba(16,185,129,.28);border-radius:11px;padding:16px;text-align:center;margin-bottom:15px;}
.fl{font-size:10px;color:var(--tm);text-transform:uppercase;letter-spacing:.9px;margin-bottom:6px;}
.fv{font-family:'Rajdhani',sans-serif;font-size:34px;font-weight:700;color:#34d399;}
.fbd{font-size:10px;color:#64748b;margin-top:4px;}
.tdb{background:rgba(16,185,129,.07);border-left:3px solid #10b981;border-radius:9px;padding:12px 14px;margin-bottom:15px;}
.tdb .tl{margin-bottom:6px;}.tdb .tv2{font-family:'Rajdhani',sans-serif;font-size:24px;font-weight:700;color:#34d399;}
.br{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:18px;}
.fb{padding:12px;border:none;border-radius:9px;cursor:pointer;font-family:'Exo 2',sans-serif;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.8px;transition:all .25s;}
.fb.cg{background:linear-gradient(135deg,#059669,#10b981);color:#fff;}
.fb.cr{background:linear-gradient(135deg,#dc2626,#ef4444);color:#fff;}
.fb.cb{background:linear-gradient(135deg,#2563eb,#1d4ed8);color:#fff;}
.fb.bk{background:rgba(25,45,75,.55);color:#94a3b8;border:1.5px solid rgba(59,130,246,.18);}
.fb:hover{transform:translateY(-2px);filter:brightness(1.1);}
/* QR Modal Admin */
.qr-overlay{position:fixed;inset:0;background:rgba(0,0,0,.82);backdrop-filter:blur(12px);z-index:6000;display:flex;align-items:center;justify-content:center;}
.qr-modal{background:rgba(6,12,24,.98);border:1px solid rgba(59,130,246,.4);border-radius:22px;padding:30px 28px 24px;max-width:320px;width:92%;box-shadow:0 40px 80px rgba(0,0,0,.8);animation:rIn .38s cubic-bezier(.34,1.56,.64,1);text-align:center;}
@keyframes rIn{from{opacity:0;transform:scale(.82)}to{opacity:1;transform:scale(1)}}
.qr-title{font-family:'Rajdhani',sans-serif;font-size:20px;font-weight:700;letter-spacing:2px;background:linear-gradient(135deg,#60a5fa,#34d399);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;margin-bottom:4px;}
.qr-sub{font-size:10px;color:#475569;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:18px;}
.qr-box{background:white;border-radius:14px;padding:14px;display:inline-block;margin-bottom:16px;box-shadow:0 0 0 3px rgba(59,130,246,.25);}
.qr-amt{font-family:'Rajdhani',sans-serif;font-size:36px;font-weight:700;color:#34d399;margin-bottom:4px;}
.qr-bid{font-size:11px;color:#64748b;margin-bottom:14px;}
.qr-inst{font-size:10px;color:#94a3b8;background:rgba(59,130,246,.07);border:1px solid rgba(59,130,246,.15);border-radius:8px;padding:9px 12px;margin-bottom:12px;line-height:1.6;}
.qr-steps{font-size:10px;color:#fde68a;background:rgba(245,158,11,.07);border:1px solid rgba(245,158,11,.18);border-radius:8px;padding:9px 12px;margin-bottom:14px;line-height:1.8;text-align:left;}
.qr-timer{font-size:11px;color:#f59e0b;margin-bottom:14px;font-weight:600;}
.qr-close{width:100%;margin-top:8px;padding:9px;border:1.5px solid rgba(59,130,246,.18);border-radius:10px;background:transparent;color:#64748b;font-size:10px;font-weight:700;font-family:'Exo 2',sans-serif;letter-spacing:1.2px;text-transform:uppercase;cursor:pointer;transition:all .3s;}
.qr-close:hover{border-color:#3b82f6;color:#60a5fa;}
.msg{padding:10px 14px;border-radius:9px;margin-bottom:12px;font-size:11px;border-left:4px solid;display:none;}
.msg.show{display:block;}
.msg.err{background:rgba(239,68,68,.09);color:#fca5a5;border-color:#ef4444;}
.msg.suc{background:rgba(16,185,129,.09);color:#86efac;border-color:#10b981;}
/* Receipt */
.overlay{position:fixed;inset:0;background:rgba(0,0,0,.75);backdrop-filter:blur(10px);z-index:5000;display:flex;align-items:center;justify-content:center;}
.receipt{background:rgba(6,12,24,.98);border:1px solid rgba(59,130,246,.35);border-radius:18px;padding:28px;max-width:330px;width:92%;box-shadow:0 40px 80px rgba(0,0,0,.75);animation:rIn .38s cubic-bezier(.34,1.56,.64,1);}
.rh{text-align:center;margin-bottom:20px;}
.ricon{font-size:44px;margin-bottom:10px;}
.rtitle{font-family:'Rajdhani',sans-serif;font-size:20px;font-weight:700;letter-spacing:2px;color:#34d399;}
.rsub{font-size:10px;color:#64748b;text-transform:uppercase;letter-spacing:.8px;margin-top:3px;}
.rdiv{height:1px;background:linear-gradient(90deg,transparent,rgba(59,130,246,.25),transparent);margin:14px 0;}
.rrow{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;font-size:12px;}
.rrl{color:#64748b;text-transform:uppercase;font-size:10px;letter-spacing:.4px;}
.rrv{font-weight:700;color:var(--tx);font-family:'Rajdhani',sans-serif;font-size:14px;}
.ramt{background:rgba(16,185,129,.09);border:1px solid rgba(16,185,129,.25);border-radius:10px;padding:12px;text-align:center;margin:14px 0;}
.ral{font-size:10px;color:#64748b;text-transform:uppercase;letter-spacing:.8px;margin-bottom:4px;}
.rav{font-family:'Rajdhani',sans-serif;font-size:38px;font-weight:700;color:#34d399;}
.rnote{background:rgba(59,130,246,.07);border:1px solid rgba(59,130,246,.18);border-radius:7px;padding:8px;text-align:center;font-size:10px;color:#60a5fa;margin-bottom:14px;}
.rcbtn{width:100%;padding:12px;background:linear-gradient(135deg,#059669,#10b981);border:none;border-radius:9px;color:#fff;font-family:'Exo 2',sans-serif;font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:.8px;cursor:pointer;transition:all .25s;margin-bottom:8px;}
.rcbtn2{width:100%;padding:10px;background:transparent;border:1.5px solid rgba(59,130,246,.25);border-radius:9px;color:#60a5fa;font-family:'Exo 2',sans-serif;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.8px;cursor:pointer;transition:all .25s;}
/* Bookings */
.bk-list{display:flex;flex-direction:column;gap:10px;margin-bottom:14px;}
.bk-card{background:var(--card);border:1px solid var(--border);border-radius:13px;padding:16px;position:relative;overflow:hidden;}
.bk-card::before{content:'';position:absolute;left:0;top:0;bottom:0;width:3px;}
.bk-card.up::before{background:#3b82f6;}.bk-card.ac::before{background:#10b981;}.bk-card.co::before{background:#64748b;}.bk-card.ca::before{background:#ef4444;}.bk-card.ex::before{background:#f59e0b;}
.bk-top{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:10px;}
.bk-id{font-family:'Rajdhani',sans-serif;font-size:16px;font-weight:700;color:var(--tx);}
.bk-badge{font-size:9px;font-weight:700;padding:3px 9px;border-radius:10px;text-transform:uppercase;letter-spacing:.8px;}
.bk-badge.up{background:rgba(59,130,246,.15);color:#60a5fa;border:1px solid rgba(59,130,246,.2);}
.bk-badge.ac{background:rgba(16,185,129,.15);color:#34d399;border:1px solid rgba(16,185,129,.2);}
.bk-badge.co{background:rgba(100,116,139,.15);color:#94a3b8;border:1px solid rgba(100,116,139,.2);}
.bk-badge.ca{background:rgba(239,68,68,.15);color:#f87171;border:1px solid rgba(239,68,68,.2);}
.bk-badge.ex{background:rgba(245,158,11,.15);color:#fbbf24;border:1px solid rgba(245,158,11,.2);}
.bk-rows{display:grid;grid-template-columns:1fr 1fr;gap:6px;}
.bk-row{font-size:11px;}.bk-rl{color:var(--tm);font-size:9px;text-transform:uppercase;letter-spacing:.5px;margin-bottom:1px;}.bk-rv{color:var(--tx);font-weight:600;}
.bk-actions{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap;}
.bk-btn{padding:7px 14px;border:none;border-radius:7px;font-size:10px;font-weight:700;font-family:'Exo 2',sans-serif;cursor:pointer;text-transform:uppercase;letter-spacing:.5px;transition:all .2s;}
.bk-btn.cancel-btn{background:rgba(239,68,68,.12);color:#f87171;border:1px solid rgba(239,68,68,.2);}
.bk-btn.entry-btn{background:rgba(16,185,129,.12);color:#34d399;border:1px solid rgba(16,185,129,.2);}
.bk-btn.pay-btn{background:rgba(245,158,11,.12);color:#fbbf24;border:1px solid rgba(245,158,11,.2);}
.bk-btn.qr-btn{background:rgba(59,130,246,.12);color:#60a5fa;border:1px solid rgba(59,130,246,.2);}
.bk-empty{text-align:center;padding:40px 20px;color:var(--tm);font-size:12px;}
.bk-empty .be-icon{font-size:40px;margin-bottom:12px;opacity:.4;}
.filter-row{display:flex;gap:7px;margin-bottom:14px;flex-wrap:wrap;}
.filter-btn{padding:5px 12px;border:1px solid var(--border);border-radius:16px;font-size:10px;font-weight:700;font-family:'Exo 2',sans-serif;cursor:pointer;transition:all .2s;background:transparent;color:var(--tm);letter-spacing:.5px;}
.filter-btn.active{background:rgba(37,99,235,.15);border-color:#3b82f6;color:#60a5fa;}
.bnav{display:grid;grid-template-columns:repeat(5,1fr);background:rgba(5,9,18,.98);border-top:1px solid var(--border);box-shadow:0 -8px 28px rgba(0,0,0,.55);}
.nb{padding:10px 3px;text-align:center;border:none;background:none;color:var(--tm);cursor:pointer;transition:all .25s;font-family:'Exo 2',sans-serif;font-size:8px;font-weight:700;text-transform:uppercase;letter-spacing:.4px;border-top:2px solid transparent;display:flex;flex-direction:column;align-items:center;gap:3px;}
.nb .ni{font-size:17px;}
.nb.active{color:#60a5fa;border-top-color:#3b82f6;background:rgba(37,99,235,.05);}
#bookingLookup{display:none;background:rgba(59,130,246,.07);border:1px solid rgba(59,130,246,.2);border-radius:10px;padding:14px;margin-bottom:14px;}
</style></head><body>
<div id="tc"></div>
<div class="bg"><div class="bg-grid"></div><div class="bg-g bg-g1"></div><div class="bg-g bg-g2"></div></div>
<div class="app">
<div class="hdr">
  <div class="hdr-top">
    <div class="hdr-brand">🅿️ SMART PARKING PRO</div>
    <div style="display:flex;align-items:center;gap:6px;">
      <span class="admin-badge">🛡️ ADMIN</span>
      <button class="logout-link" onclick="location.href='/'">LOGOUT</button>
      <div class="live"><div class="live-dot"></div>LIVE</div>
    </div>
  </div>
  <div class="stats">
    <div class="sp"><div class="sv" id="s4w">0/4</div><div class="sl">Used</div></div>
    <div class="sp"><div class="sv" id="s4a">2</div><div class="sl">Free</div></div>
  </div>
</div>
<div class="gate-bar">
  <div class="gate-pill" id="gateEntry"><div class="gpd"></div>ENTRY GATE: <span id="entryGateSt">CLOSED</span></div>
  <div class="gate-pill" id="gateExit"><div class="gpd"></div>EXIT GATE: <span id="exitGateSt">CLOSED</span></div>
  <div id="irStatus" style="font-size:10px;color:#64748b;">IR: checking...</div>
</div>
<div class="content">
<!-- DASHBOARD -->
<div class="screen active" id="dashboardScreen">
  <div class="banner"><h2>ADMIN COMMAND CENTER</h2><p>Full control · Entry/exit gates · QR payments · Live IR sensors</p></div>
  <div class="fare-strip"><span style="font-size:18px">💡</span><div><strong>FARE: ₹10 / 30 MIN</strong> &nbsp;·&nbsp; Min ₹10 &nbsp;·&nbsp; QR opens exit gate</div></div>
  <div class="charts">
    
    <div class="cc"><div class="ct">🚗 4-Wheeler</div><div class="donut" id="c4w" style="background:conic-gradient(#3b82f6 0deg,#1e3a5f 0deg);">0%</div><div class="ci" id="c4wv">0/4</div><div class="cs">Occupied</div></div>
  </div>
  <div class="sec"><div class="sec-hdr"><span>🚗</span>PARKING BAYS</div><div class="sgrid" id="g4w"></div></div>

  <!-- PAYMENT LIVE FEED PANEL -->
  <div class="sec">
    <div class="sec-hdr"><span>📡</span>PAYMENT LIVE FEED<span id="feedDot" style="display:inline-block;width:7px;height:7px;background:#10b981;border-radius:50%;margin-left:8px;animation:blink 1.5s infinite;vertical-align:middle;"></span></div>
    <div id="guestFeedList" style="display:flex;flex-direction:column;gap:7px;max-height:280px;overflow-y:auto;">
      <div style="text-align:center;padding:18px;color:#334155;font-size:11px;">Waiting for payment activity…</div>
    </div>
  </div>

  <div class="arow">
    <button class="ab bl" onclick="sw('reserveScreen')"><span class="ai">📅</span>RESERVE</button>
    <button class="ab gr" onclick="sw('entryScreen')"><span class="ai">➡️</span>ENTRY</button>
    <button class="ab am" onclick="sw('exitScreen')"><span class="ai">⬅️</span>EXIT</button>
    <button class="ab rd" onclick="sw('paymentScreen')"><span class="ai">💳</span>PAYMENT</button>
    <button class="ab pu" onclick="sw('bookingsScreen')" style="grid-column:span 2"><span class="ai">📋</span>ALL BOOKINGS</button>
  </div>
</div>
<!-- RESERVE -->
<div class="screen" id="reserveScreen">
<div class="fw"><div class="fc">
  <h2>📅 ADVANCE RESERVATION</h2>
  <div class="msg err" id="rErr"></div><div class="msg suc" id="rSuc"></div>
  <div class="row2">
    <div class="fg"><label>Owner Name</label><input type="text" id="rName" placeholder="Full name"></div>
    <div class="fg"><label>Phone</label><input type="tel" id="rPhone" placeholder="9876543210"></div>
  </div>
  <div class="fg"><label>Vehicle Number</label><input type="text" id="rVeh" placeholder="MH01AB1234"></div>
  <div class="fg"><label>Vehicle Type</label>
    <select id="rType" onchange="loadSlots()">
      <option value="four" selected>🚗 4-Wheeler</option>
    </select>
  </div>
  <div class="row2">
    <div class="fg"><label>Date &amp; Start Time</label><input type="datetime-local" id="rStart"></div>
    <div class="fg"><label>Duration (hours)</label>
      <select id="rDur">
        <option value="">--</option>
        <option value="0.5">30 min</option><option value="1">1 hour</option><option value="1.5">1.5 hrs</option>
        <option value="2" selected>2 hours</option><option value="3">3 hours</option><option value="4">4 hours</option>
        <option value="6">6 hours</option><option value="8">8 hours</option><option value="12">12 hours</option><option value="24">24 hours</option>
      </select>
    </div>
  </div>
  <div class="avbox" id="slotBox" style="display:none">
    <h3>✨ SELECT SLOT</h3>
    <div class="sp-picker" id="slotPicker"></div>
    <div class="fullmsg" id="slotFull" style="display:none"></div>
  </div>
  <div id="farePreview" style="display:none;background:rgba(16,185,129,.06);border:1px solid rgba(16,185,129,.2);border-radius:10px;padding:14px;margin-bottom:14px;text-align:center;">
    <div style="font-size:10px;color:#64748b;text-transform:uppercase;letter-spacing:.8px;margin-bottom:4px;">ESTIMATED FARE</div>
    <div id="farePreviewVal" style="font-family:'Rajdhani',sans-serif;font-size:28px;font-weight:700;color:#34d399;">₹0</div>
    <div id="farePreviewBreak" style="font-size:10px;color:#64748b;margin-top:3px;"></div>
  </div>
  <div class="ibox b">📅 Show Booking ID at entry gate OR car detected via IR sensor.</div>
  <div class="br">
    <button class="fb cg" onclick="doReserve()">✓ CONFIRM BOOKING</button>
    <button class="fb bk" onclick="sw('dashboardScreen')">← BACK</button>
  </div>
</div></div>
</div>
<!-- BOOKINGS -->
<div class="screen" id="bookingsScreen">
<div class="fw"><div class="fc">
  <h2>📋 BOOKINGS MANAGER</h2>
  <div class="filter-row">
    <button class="filter-btn active" onclick="filterBooks('all',this)">ALL</button>
    <button class="filter-btn" onclick="filterBooks('upcoming',this)">UPCOMING</button>
    <button class="filter-btn" onclick="filterBooks('active',this)">ACTIVE</button>
    <button class="filter-btn" onclick="filterBooks('completed',this)">DONE</button>
    <button class="filter-btn" onclick="filterBooks('cancelled',this)">CANCELLED</button>
  </div>
  <div class="bk-list" id="bkList"></div>
  <div class="br" style="margin-top:4px;">
    <button class="fb cb" onclick="sw('reserveScreen')">+ NEW BOOKING</button>
    <button class="fb bk" onclick="sw('dashboardScreen')">← BACK</button>
  </div>
</div></div>
</div>
<!-- ENTRY -->
<div class="screen" id="entryScreen">
<div class="fw"><div class="fc">
  <h2>➡️ VEHICLE ENTRY</h2>
  <div class="msg err" id="eErr"></div><div class="msg suc" id="eSuc"></div>
  <div class="fg"><label>Booking ID (or Vehicle Number)</label>
    <input type="text" id="eRef" placeholder="BK001 or MH01AB1234" oninput="lookupBooking()"></div>
  <div id="bookingLookup">
    <div style="font-size:10px;color:#60a5fa;text-transform:uppercase;letter-spacing:.8px;margin-bottom:8px;">📋 BOOKING FOUND</div>
    <div id="lookupContent"></div>
  </div>
  <div class="fg"><label>Vehicle Type</label>
    <select id="eType"><option value="">-- Auto from booking --</option><option value="four">🚗 4-Wheeler</option></select>
  </div>
  <div class="fg"><label>Slot (Auto-assigned if booked)</label><input type="text" id="eSlot" placeholder="A1 or B1 (optional)"></div>
  <div class="tdb"><div class="tl">⏰ ENTRY TIME</div><div class="tv2" id="entryTime">--:--:--</div></div>
  <div class="ibox b">📡 Entry gate servo opens automatically when booking is confirmed.</div>
  <div class="br">
    <button class="fb cg" onclick="doEntry()">✓ CONFIRM ENTRY + OPEN GATE</button>
    <button class="fb bk" onclick="sw('dashboardScreen')">← BACK</button>
  </div>
</div></div>
</div>
<!-- EXIT -->
<div class="screen" id="exitScreen">
<div class="fw"><div class="fc">
  <h2>⬅️ VEHICLE EXIT</h2>
  <div class="msg err" id="xErr"></div><div class="msg suc" id="xSuc"></div>
  <div class="fg"><label>Booking ID or Vehicle Number</label>
    <input type="text" id="xRef" placeholder="BK001 or MH01AB1234" oninput="calcExit()"></div>
  <div class="timer-box"><div class="tl">⏱️ PARKING DURATION</div><div class="tv" id="durDisp">00:00:00</div></div>
  <div class="fbox"><div class="fl">AUTO-CALCULATED FARE</div><div class="fv" id="fareDisp">₹0</div><div class="fbd" id="fareBrk">₹10 per 30 min</div></div>
  <div class="ibox a">⚠️ After exit, customer pays → Gets QR → Scans at exit gate → Gate opens.</div>
  <div class="br">
    <button class="fb cr" onclick="doExit()">✓ CONFIRM EXIT</button>
    <button class="fb bk" onclick="sw('dashboardScreen')">← BACK</button>
  </div>
</div></div>
</div>
<!-- PAYMENT + QR -->
<div class="screen" id="paymentScreen">
<div class="fw"><div class="fc">
  <h2>💳 PAYMENT + QR GATE PASS</h2>
  <div class="msg err" id="pErr"></div><div class="msg suc" id="pSuc"></div>
  <div class="fg"><label>Booking ID or Vehicle Number</label>
    <input type="text" id="pRef" placeholder="BK001 or MH01AB1234" oninput="fetchFare()"></div>
  <div class="fbox"><div class="fl">💰 AMOUNT DUE</div><div class="fv" id="payAmt">₹0</div></div>
  <div class="fg"><label>Payment Method</label>
    <select id="pMethod">
      <option value="">-- Select --</option>
      <option value="qr">📱 UPI / QR Scan</option>
      <option value="card">💳 Card</option>
      <option value="cash">💵 Cash</option>
    </select>
  </div>
  <div class="ibox g">✅ After payment confirmation, a QR code is generated. Customer scans it at the exit boom to open the gate.</div>
  <div class="br">
    <button class="fb cg" onclick="doPayment()">✓ CONFIRM PAYMENT + GENERATE QR</button>
    <button class="fb bk" onclick="sw('dashboardScreen')">← BACK</button>
  </div>
</div></div>
</div>
</div>
<div class="bnav">
  <button class="nb active" onclick="sw('dashboardScreen')"><span class="ni">🏠</span>HOME</button>
  <button class="nb" onclick="sw('reserveScreen')"><span class="ni">📅</span>RESERVE</button>
  <button class="nb" onclick="sw('bookingsScreen')"><span class="ni">📋</span>BOOKINGS</button>
  <button class="nb" onclick="sw('exitScreen')"><span class="ni">⬅️</span>EXIT</button>
  <button class="nb" onclick="sw('paymentScreen')"><span class="ni">💳</span>PAY+QR</button>
</div>
</div>

<!-- QR Modal Admin -->
<div id="qrOverlay" class="qr-overlay" style="display:none;">
  <div class="qr-modal">
    <div class="qr-title">📱 GATE QR PASS</div>
    <div class="qr-sub">Customer scans this to open exit gate</div>
    <div class="qr-box"><canvas id="qrCanvas" width="160" height="160"></canvas></div>
    <div class="qr-amt" id="qrAmtDisp">₹0</div>
    <div class="qr-bid" id="qrBidDisp">Booking ID</div>
    <div class="qr-steps">📋 <strong>HOW IT WORKS:</strong><br>
      1. Customer gets this QR (on phone or printed)<br>
      2. Car detected at exit gate (IR sensor)<br>
      3. LCD shows "Scan QR to Exit"<br>
      4. Customer scans → ESP32 validates token<br>
      5. Exit servo opens automatically ✅</div>
    <div class="qr-inst">Token expires in 5 minutes. One-time use only.</div>
    <div class="qr-timer">⏳ Expires in: <span id="qrCountdown">300</span>s</div>
    <button class="qr-close" onclick="closeQr()">✕ CLOSE</button>
  </div>
</div>

<script>
let selSlot=-1,selSlotName='',lastIrState=-1,curFilter='all',qrTimer=null;
function calcFareMs(ms){const mins=Math.max(1,Math.ceil(ms/60000));const slots=Math.max(1,Math.ceil(mins/30));return{mins,slots,amount:slots*10};}
function calcFareHours(hrs){const mins=Math.max(1,Math.round(hrs*60));const slots=Math.max(1,Math.ceil(mins/30));return{mins,slots,amount:slots*10};}
function toast(icon,title,msg,type='info'){const c=document.getElementById('tc');const t=document.createElement('div');t.className='toast t'+(type[0]||'b');t.innerHTML=`<div class="ti">${icon}</div><div><div class="tt">${title}</div><div class="tm2">${msg}</div></div>`;c.appendChild(t);setTimeout(()=>t.remove(),5000);}
function showReceipt(bid,vehicle,slot,owner,dMin,bSlots,amount){const h=Math.floor(dMin/60),m=dMin%60;const dur=(h>0?h+'h ':'')+m+'m';const ov=document.createElement('div');ov.className='overlay';ov.innerHTML=`<div class="receipt"><div class="rh"><div class="ricon">🧾</div><div class="rtitle">EXIT RECEIPT</div><div class="rsub">Smart Parking Pro</div></div><div class="rdiv"></div><div class="rrow"><span class="rrl">Booking ID</span><span class="rrv">${bid||'WALK-IN'}</span></div><div class="rrow"><span class="rrl">Vehicle</span><span class="rrv">${vehicle}</span></div><div class="rrow"><span class="rrl">Slot</span><span class="rrv">${slot}</span></div>${owner?`<div class="rrow"><span class="rrl">Owner</span><span class="rrv">${owner}</span></div>`:''}<div class="rrow"><span class="rrl">Duration</span><span class="rrv">${dur} (${dMin} min)</span></div><div class="rrow"><span class="rrl">Billing Units</span><span class="rrv">${bSlots} × ₹10</span></div><div class="rdiv"></div><div class="ramt"><div class="ral">TOTAL AMOUNT DUE</div><div class="rav">₹${amount}</div></div><div class="rnote">👇 Confirm payment to generate QR exit pass</div><button class="rcbtn" onclick="this.closest('.overlay').remove();document.getElementById('pRef').value='${bid||vehicle}';document.getElementById('payAmt').textContent='₹${amount}';sw('paymentScreen')">💳 PROCEED TO PAYMENT + QR</button><button class="rcbtn2" onclick="this.closest('.overlay').remove()">✕ CLOSE</button></div>`;document.body.appendChild(ov);}
function connectSSE(){const es=new EventSource('/api/events');es.addEventListener('ir',e=>{const d=JSON.parse(e.data);if(d.state!==lastIrState){lastIrState=d.state;if(d.state===1&&d.wasReserved)toast('🔔','RESERVED SLOT OCCUPIED','Vehicle entered reserved slot!','w');else if(d.state===1)toast('🚨','SLOT OCCUPIED','Walk-in vehicle detected.','w');else if(d.state===0)toast('✅','SLOT VACANT','Slot is now clear.','o');}refreshStats();refreshBookings();});es.onerror=()=>{es.close();setTimeout(connectSSE,3000);};}

// ── Guest Live Feed SSE ──
const FEED_CFG={
  paid:  {icon:'💳',label:'PAYMENT CONFIRMED',    bg:'rgba(16,185,129,.08)', border:'rgba(16,185,129,.3)', col:'#34d399'},
  token: {icon:'🔑',label:'TOKEN ISSUED',  bg:'rgba(245,158,11,.08)', border:'rgba(245,158,11,.3)', col:'#fbbf24'},
  walkin:{icon:'🚶',label:'WALK-IN ENTRY', bg:'rgba(59,130,246,.08)', border:'rgba(59,130,246,.3)', col:'#60a5fa'},
  view:  {icon:'👁️',label:'DASHBOARD VIEW',bg:'rgba(100,116,139,.08)',border:'rgba(100,116,139,.2)',col:'#94a3b8'},
};
let feedItems=[];
function addFeedItem(d){
  feedItems.unshift(d);
  if(feedItems.length>20)feedItems.pop();
  renderFeed();
  const cfg=FEED_CFG[d.type]||FEED_CFG.view;
  if(d.type!=='view')toast(cfg.icon,cfg.label,d.vehicle+' · '+d.detail,'o');
}
function fmtAgo(sec){if(sec<60)return sec+'s ago';if(sec<3600)return Math.floor(sec/60)+'m ago';return Math.floor(sec/3600)+'h ago';}
function renderFeed(){
  const list=document.getElementById('guestFeedList');
  if(!feedItems.length){list.innerHTML='<div style="text-align:center;padding:18px;color:#334155;font-size:11px;">Waiting for payment activity…</div>';return;}
  list.innerHTML=feedItems.map(d=>{
    const cfg=FEED_CFG[d.type]||FEED_CFG.view;
    return `<div style="background:${cfg.bg};border:1px solid ${cfg.border};border-radius:10px;padding:10px 13px;display:flex;align-items:center;gap:10px;">
      <div style="font-size:20px;flex-shrink:0;">${cfg.icon}</div>
      <div style="flex:1;min-width:0;">
        <div style="font-family:'Rajdhani',sans-serif;font-size:12px;font-weight:700;letter-spacing:.8px;color:${cfg.col};">${cfg.label}</div>
        <div style="font-size:11px;color:#e2e8f0;font-weight:600;margin-top:1px;">${d.vehicle} · Slot ${d.slot||'--'}</div>
        <div style="font-size:10px;color:#64748b;margin-top:1px;">${d.detail}</div>
      </div>
      <div style="font-size:9px;color:#334155;text-align:right;flex-shrink:0;">${fmtAgo(d.ago||0)}</div>
    </div>`;
  }).join('');
}
function connectGuestFeed(){
  const es=new EventSource('/api/guest-feed');
  es.addEventListener('guest',e=>{
    const d=JSON.parse(e.data);
    addFeedItem(d);
  });
  es.onerror=()=>{es.close();setTimeout(connectGuestFeed,4000);};
}
// Update ago timers every 10s
setInterval(()=>{feedItems.forEach(d=>{if(d.ago!==undefined)d.ago+=10;});renderFeed();},10000);
function refreshStats(){fetch('/api/stats').then(r=>r.json()).then(d=>{document.getElementById('s2w').textContent=d.twoOccupied+'/2';document.getElementById('s2a').textContent=d.twoAvailable;document.getElementById('s4w').textContent=d.fourOccupied+'/4';document.getElementById('s4a').textContent=d.fourAvailable;const p4=Math.round(d.fourOccupied/4*100);document.getElementById('c4w').textContent=p4+'%';document.getElementById('c4wv').textContent=d.fourOccupied+'/4';document.getElementById('c4w').style.background=`conic-gradient(#3b82f6 0deg,#3b82f6 ${3.6*p4}deg,#1e3a5f ${3.6*p4}deg)`;buildGrid('4w',d.fourSlots,['B1','B2','B3','B4'],['🚗','🚗','🚗','🚗'],d.slotInfo||{});if(d.entryGateOpen){document.getElementById('gateEntry').className='gate-pill gate-open';document.getElementById('entryGateSt').textContent='OPEN';}else{document.getElementById('gateEntry').className='gate-pill gate-closed';document.getElementById('entryGateSt').textContent='CLOSED';}if(d.exitGateOpen){document.getElementById('gateExit').className='gate-pill gate-open';document.getElementById('exitGateSt').textContent='OPEN';}else{document.getElementById('gateExit').className='gate-pill gate-closed';document.getElementById('exitGateSt').textContent='CLOSED';}document.getElementById('irStatus').textContent='Exit IR: '+(d.exitCarDetected?'CAR':'CLEAR');});}
function buildGrid(id,slots,names,icons,info){const g=document.getElementById('g'+id);g.innerHTML='';slots.forEach((st,i)=>{const cls=st===0?'av':st===2?'rs':'oc';const lbl=st===0?'✓ AVAILABLE':st===2?'⏳ RESERVED':'✗ OCCUPIED';const el=document.createElement('div');el.className='sc '+cls;const isA1=(id==='2w'&&i===0);const si=(info&&info[names[i]])||{};const sub=si.vehicle?`<div class="stime">${si.vehicle}${si.bid?' · '+si.bid:''}</div>`:'';el.innerHTML=(isA1?'<div class="irc">📡 IR</div>':'')+`<div style="font-size:20px;margin-bottom:4px;">${icons[i]}</div><div class="sid">SLOT ${names[i]}</div><div class="ss">${lbl}</div>${sub}`;g.appendChild(el);});}
function initDatetime(){const inp=document.getElementById('rStart');if(inp&&!inp.value){const now=new Date();now.setMinutes(now.getMinutes()+5);inp.value=now.toISOString().slice(0,16);}}
function loadSlots(){const type=document.getElementById('rType').value;if(!type){document.getElementById('slotBox').style.display='none';return;}fetch('/api/available-slots?type='+type).then(r=>r.json()).then(d=>{const box=document.getElementById('slotBox');const picker=document.getElementById('slotPicker');const full=document.getElementById('slotFull');const names=['B1','B2','B3','B4'];selSlot=-1;selSlotName='';if(d.slots.length===0){full.innerHTML='❌ All slots occupied';full.style.display='block';picker.innerHTML='';}else{full.style.display='none';picker.innerHTML='';d.slots.forEach(idx=>{const btn=document.createElement('button');btn.type='button';btn.className='spb';btn.textContent='SLOT '+names[idx];btn.onclick=()=>{selSlot=idx;selSlotName=names[idx];document.querySelectorAll('.spb').forEach(b=>b.classList.remove('sel'));btn.classList.add('sel');updateFarePreview();};picker.appendChild(btn);});}box.style.display='block';updateFarePreview();});}
function updateFarePreview(){const dur=parseFloat(document.getElementById('rDur').value)||0;const fp=document.getElementById('farePreview');if(dur>0){const f=calcFareHours(dur);document.getElementById('farePreviewVal').textContent='₹'+f.amount;document.getElementById('farePreviewBreak').textContent=f.slots+' slot(s) × ₹10 ('+f.mins+' min est.)';fp.style.display='block';}else{fp.style.display='none';}}
function doReserve(){if(selSlot===-1){showMsg('rErr','Please select a slot','err');return;}const name=document.getElementById('rName').value,phone=document.getElementById('rPhone').value,veh=document.getElementById('rVeh').value,type=document.getElementById('rType').value,start=document.getElementById('rStart').value,dur=document.getElementById('rDur').value;const url=`/api/reserve?name=${encodeURIComponent(name)}&phone=${encodeURIComponent(phone)}&vehicle=${encodeURIComponent(veh)}&type=${type}&slot=${selSlot}&start=${encodeURIComponent(start)}&duration=${dur}`;fetch(url).then(r=>r.json()).then(d=>{if(d.success){const f=calcFareHours(parseFloat(dur));showMsg('rSuc',`Booking confirmed! ID: ${d.bookingId} · Est. ₹${f.amount}`,'suc');toast('✅','BOOKING CONFIRMED',`${d.bookingId} · Slot ${selSlotName} · Est. ₹${f.amount}`,'o');selSlot=-1;selSlotName='';refreshStats();refreshBookings();setTimeout(()=>sw('bookingsScreen'),1800);}else{showMsg('rErr',d.message||'Booking failed','err');}});}
function filterBooks(f,btn){curFilter=f;document.querySelectorAll('.filter-btn').forEach(b=>b.classList.remove('active'));btn.classList.add('active');refreshBookings();}
function refreshBookings(){fetch('/api/bookings').then(r=>r.json()).then(d=>{const list=document.getElementById('bkList');let rows=d.bookings;if(curFilter!=='all')rows=rows.filter(b=>b.status===curFilter);if(rows.length===0){list.innerHTML=`<div class="bk-empty"><div class="be-icon">📭</div>No ${curFilter==='all'?'':curFilter+' '}bookings found.</div>`;return;}list.innerHTML='';rows.slice().reverse().forEach(b=>{const stCls={upcoming:'up',active:'ac',completed:'co',cancelled:'ca',expired:'ex'}[b.status]||'co';const stLbl={upcoming:'UPCOMING',active:'ACTIVE ●',completed:'COMPLETED',cancelled:'CANCELLED',expired:'EXPIRED'}[b.status]||b.status.toUpperCase();const el=document.createElement('div');el.className='bk-card '+stCls;
const entryBtn=(b.status==='upcoming')?`<button class="bk-btn entry-btn" onclick="quickEntry('${b.bookingId}')">➡ CHECK IN</button>`:'';
const cancelBtn=(b.status==='upcoming')?`<button class="bk-btn cancel-btn" onclick="cancelBooking('${b.bookingId}')">✕ CANCEL</button>`:'';
const exitBtn=(b.status==='active')?`<button class="bk-btn entry-btn" onclick="quickExit('${b.bookingId}')">⬅ CHECK OUT</button>`:'';
const payBtn=(b.status==='completed'&&!b.isPaid)?`<button class="bk-btn pay-btn" onclick="openPayAndQr('${b.bookingId}',${b.amount})">💳 PAY + QR</button>`:'';
const qrBtn=(b.status==='completed'&&b.isPaid)?`<button class="bk-btn qr-btn" onclick="generateQrForPaid('${b.bookingId}',${b.amount})">📱 QR PASS</button>`:'';
el.innerHTML=`<div class="bk-top"><div class="bk-id">${b.bookingId}</div><div class="bk-badge ${stCls}">${stLbl}</div></div><div class="bk-rows"><div class="bk-row"><div class="bk-rl">Vehicle</div><div class="bk-rv">${b.vehicleNumber}</div></div><div class="bk-row"><div class="bk-rl">Slot</div><div class="bk-rv">${b.slotName}</div></div><div class="bk-row"><div class="bk-rl">Owner</div><div class="bk-rv">${b.ownerName}</div></div><div class="bk-row"><div class="bk-rl">Phone</div><div class="bk-rv">${b.ownerPhone||'--'}</div></div><div class="bk-row"><div class="bk-rl">Scheduled</div><div class="bk-rv">${b.scheduledStart}</div></div><div class="bk-row"><div class="bk-rl">Duration</div><div class="bk-rv">${b.duration}h</div></div>${b.amount>0?`<div class="bk-row"><div class="bk-rl">Amount</div><div class="bk-rv" style="color:#34d399">₹${b.amount}</div></div>`:''}${b.isPaid?`<div class="bk-row"><div class="bk-rl">Payment</div><div class="bk-rv" style="color:#34d399">PAID ✓</div></div>`:''}</div>${(entryBtn||cancelBtn||exitBtn||payBtn||qrBtn)?`<div class="bk-actions">${entryBtn}${cancelBtn}${exitBtn}${payBtn}${qrBtn}</div>`:''}`;list.appendChild(el);});});}
function cancelBooking(bid){if(!confirm('Cancel booking '+bid+'?'))return;fetch('/api/cancel?booking='+bid).then(r=>r.json()).then(d=>{if(d.success){toast('🗑️','CANCELLED',bid+' cancelled.','y');refreshBookings();refreshStats();}});}
function quickEntry(bid){fetch('/api/entry?booking='+encodeURIComponent(bid)).then(r=>r.json()).then(d=>{if(d.success){toast('➡️','CHECK-IN + GATE OPEN','Vehicle checked in for '+bid,'o');refreshBookings();refreshStats();}});}
function quickExit(bid){fetch('/api/exit?booking='+encodeURIComponent(bid)).then(r=>r.json()).then(d=>{if(d.success){toast('⬅️','CHECK-OUT','Vehicle checked out for '+bid,'y');showReceipt(bid,d.vehicle,d.slot,d.owner,d.durationMin,d.billSlots,d.amount);refreshBookings();refreshStats();}});}
function openPayAndQr(bid,amount){document.getElementById('pRef').value=bid;document.getElementById('payAmt').textContent='₹'+amount;sw('paymentScreen');}
function generateQrForPaid(bid,amount){fetch('/api/generate-token?booking='+encodeURIComponent(bid)).then(r=>r.json()).then(td=>{if(td.success)showQrModal(td.token,amount,bid);else toast('⚠️','Error','Could not generate token.','y');});}
let lookupTimer=null;
function lookupBooking(){clearTimeout(lookupTimer);lookupTimer=setTimeout(()=>{const ref=document.getElementById('eRef').value.trim();if(!ref){document.getElementById('bookingLookup').style.display='none';return;}fetch('/api/lookup?ref='+encodeURIComponent(ref)).then(r=>r.json()).then(d=>{const box=document.getElementById('bookingLookup');const con=document.getElementById('lookupContent');if(d.found){box.style.display='block';con.innerHTML=`<div style="display:grid;grid-template-columns:1fr 1fr;gap:6px;font-size:11px;"><div><div style="color:#64748b;font-size:9px;text-transform:uppercase">Owner</div><div style="font-weight:600">${d.ownerName}</div></div><div><div style="color:#64748b;font-size:9px;text-transform:uppercase">Vehicle</div><div style="font-weight:600">${d.vehicleNumber}</div></div><div><div style="color:#64748b;font-size:9px;text-transform:uppercase">Slot</div><div style="font-weight:600">${d.slotName}</div></div><div><div style="color:#64748b;font-size:9px;text-transform:uppercase">Status</div><div style="font-weight:600;color:#f59e0b">${d.status.toUpperCase()}</div></div></div>`;if(d.slotName)document.getElementById('eSlot').value=d.slotName;if(d.type)document.getElementById('eType').value=d.type;}else{box.style.display='none';}});},400);}
function doEntry(){const ref=document.getElementById('eRef').value,type=document.getElementById('eType').value,slot=document.getElementById('eSlot').value;fetch(`/api/entry?booking=${encodeURIComponent(ref)}&type=${type}&slot=${encodeURIComponent(slot)}`).then(r=>r.json()).then(d=>{if(d.success){showMsg('eSuc','Entry confirmed! Gate opened. Billing started.','suc');toast('➡️','GATE OPENED',`${ref} · Slot ${d.slot||slot}`,'o');refreshStats();refreshBookings();setTimeout(()=>sw('dashboardScreen'),2000);}else showMsg('eErr',d.message||'Entry failed','err');});}
let exitCalcTimer=null;
function calcExit(){clearTimeout(exitCalcTimer);exitCalcTimer=setTimeout(()=>{const ref=document.getElementById('xRef').value.trim();if(!ref)return;fetch('/api/lookup?ref='+encodeURIComponent(ref)).then(r=>r.json()).then(d=>{if(!d.found||!d.elapsedMs)return;const f=calcFareMs(d.elapsedMs);const h=Math.floor(f.mins/60),m=f.mins%60,s=Math.floor((d.elapsedMs%60000)/1000);document.getElementById('durDisp').textContent=String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(s).padStart(2,'0');document.getElementById('fareDisp').textContent='₹'+f.amount;document.getElementById('fareBrk').textContent=`${f.slots} slot(s) × ₹10 = ₹${f.amount}`;});},400);}
function doExit(){const ref=document.getElementById('xRef').value;fetch('/api/exit?booking='+encodeURIComponent(ref)).then(r=>r.json()).then(d=>{if(d.success){showMsg('xSuc','Exit recorded! Proceeding to payment & QR…','suc');showReceipt(d.bookingId||ref,d.vehicle,d.slot,d.owner,d.durationMin,d.billSlots,d.amount);refreshStats();refreshBookings();}else showMsg('xErr',d.message||'Not found','err');});}
function fetchFare(){const ref=document.getElementById('pRef').value;if(!ref)return;fetch('/api/get-fare?booking='+encodeURIComponent(ref)).then(r=>r.json()).then(d=>{if(d.amount>0)document.getElementById('payAmt').textContent='₹'+d.amount;});}
function doPayment(){const ref=document.getElementById('pRef').value.trim();const amt=parseInt(document.getElementById('payAmt').textContent.replace('₹',''))||0;if(!ref){showMsg('pErr','Enter booking ID','err');return;}fetch('/api/payment?booking='+encodeURIComponent(ref)).then(r=>r.json()).then(d=>{if(d.success){showMsg('pSuc','Payment confirmed! Generating QR gate pass...','suc');toast('💳','PAYMENT DONE','₹'+d.amount+' received. Generating QR...','o');fetch('/api/generate-token?booking='+encodeURIComponent(ref)).then(r=>r.json()).then(td=>{if(td.success)showQrModal(td.token,d.amount,ref);});}else{showMsg('pErr',d.message||'Payment failed','err');}});}
function showQrModal(token,amount,bid){const espIp=window.location.hostname;const url='http://'+espIp+'/gate/open?token='+token;document.getElementById('qrAmtDisp').textContent='₹'+amount;document.getElementById('qrBidDisp').textContent='Booking: '+bid;drawQr(url);document.getElementById('qrOverlay').style.display='flex';let secs=300;document.getElementById('qrCountdown').textContent=secs;clearInterval(qrTimer);qrTimer=setInterval(()=>{secs--;document.getElementById('qrCountdown').textContent=secs;if(secs<=0){clearInterval(qrTimer);closeQr();}},1000);}
function closeQr(){clearInterval(qrTimer);document.getElementById('qrOverlay').style.display='none';}
function drawQr(text){const canvas=document.getElementById('qrCanvas');const ctx=canvas.getContext('2d');ctx.fillStyle='white';ctx.fillRect(0,0,160,160);const img=new Image();img.crossOrigin='anonymous';img.onload=()=>ctx.drawImage(img,0,0,160,160);img.onerror=()=>{ctx.fillStyle='#333';ctx.font='10px monospace';ctx.fillStyle='black';ctx.fillText('QR: Scan URL',10,80);};img.src='https://api.qrserver.com/v1/create-qr-code/?size=160x160&data='+encodeURIComponent(text);}
function sw(id){document.querySelectorAll('.screen').forEach(s=>s.classList.remove('active'));document.getElementById(id).classList.add('active');const m={dashboardScreen:0,reserveScreen:1,bookingsScreen:2,exitScreen:3,paymentScreen:4};document.querySelectorAll('.nb').forEach(b=>b.classList.remove('active'));if(m[id]!==undefined)document.querySelectorAll('.nb')[m[id]].classList.add('active');if(id==='entryScreen'){const n=new Date();document.getElementById('entryTime').textContent=String(n.getHours()).padStart(2,'0')+':'+String(n.getMinutes()).padStart(2,'0')+':'+String(n.getSeconds()).padStart(2,'0');}if(id==='reserveScreen'){initDatetime();loadSlots();updateFarePreview();}if(id==='bookingsScreen')refreshBookings();}
function showMsg(id,msg,type){const el=document.getElementById(id);el.textContent=(type==='err'?'❌ ':'✅ ')+msg;el.className='msg show '+type;setTimeout(()=>el.classList.remove('show'),6000);}
document.getElementById('rDur').addEventListener('change',updateFarePreview);
connectSSE();
connectGuestFeed();
setInterval(refreshStats,3000);
setInterval(refreshBookings,8000);
refreshStats();refreshBookings();initDatetime();
</script>
</body></html>
)html";

// ─────────────────────────────────────────────────────────
//  HTTP HANDLERS
// ─────────────────────────────────────────────────────────
void loginPageHandler()  { server.send(200,"text/html",LOGIN_PAGE); }
void serveAdminPage()    { server.send(200,"text/html",APP_PAGE); }
void serveUserPage()     { server.send(200,"text/html",USER_PAGE); }

void handleLogin() {
  String role = server.arg("role");
  String user = server.arg("user");
  String pass = server.arg("pass");
  StaticJsonDocument<64> doc;
  if (role=="admin") {
    if (user==adminUsername && pass==adminPassword) { doc["success"]=true; doc["role"]="admin"; }
    else { doc["success"]=false; doc["message"]="Invalid admin credentials"; }
  } else if (role=="user") {
    if (user==userUsername && pass==userPassword) { doc["success"]=true; doc["role"]="user"; }
    else { doc["success"]=false; doc["message"]="Invalid user credentials"; }
  } else { doc["success"]=false; doc["message"]="Unknown role"; }
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

void handleStats() {
  updateStats();
  StaticJsonDocument<1024> doc;
  doc["fourOccupied"] = stats.fourOcc;
  doc["fourAvailable"]= stats.fourFree;
  doc["revenue"]      = stats.totalRev;
  doc["entryGateOpen"]= entryGateOpen;
  doc["exitGateOpen"] = exitGateOpen;
  doc["exitCarDetected"] = irExitLastStable;
  JsonArray fa=doc.createNestedArray("fourSlots");
  for(int i=0;i<4;i++) fa.add(fourSlots[i]);
  JsonObject si=doc.createNestedObject("slotInfo");
  for(int i=0;i<resCount;i++){
    Reservation& r=reservations[i];
    if(r.status=="upcoming"||r.status=="active"){
      JsonObject s=si.createNestedObject(r.slotName);
      s["vehicle"]=r.vehicleNumber; s["bid"]=r.bookingId; s["owner"]=r.ownerName;
    }
  }
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

void handleAvailableSlots() {
  String type=server.arg("type");
  StaticJsonDocument<128> doc;
  JsonArray arr=doc.createNestedArray("slots");
  for(int i=0;i<4;i++) if(fourSlots[i]==0) arr.add(i);
  String r; serializeJson(doc,r);
  server.send(200,"application/json",r);
}

void handleReserve() {
  if(resCount>=MAX_RESERVATIONS){server.send(200,"application/json","{\"success\":false,\"message\":\"Reservation limit reached\"}");return;}
  String name=server.arg("name"),phone=server.arg("phone"),vehicle=server.arg("vehicle"),type=server.arg("type"),startStr=server.arg("start");
  int slot=server.arg("slot").toInt();
  float dur=server.arg("duration").toFloat();
  StaticJsonDocument<256> doc;
  bool ok=false;
  if(slot>=0 && slot<4 && fourSlots[slot]==0){fourSlots[slot]=2; ok=true;}
  if(ok){
    String bid=makeBid();
    String sname="B"+String(slot+1);
    unsigned long now=millis();
    unsigned long durMs=(unsigned long)(dur*3600000.0);
    Reservation& r=reservations[resCount];
    r.bookingId=bid; r.vehicleNumber=vehicle; r.vehicleType="four";
    r.slotIndex=slot; r.slotName=sname; r.ownerName=name; r.ownerPhone=phone;
    r.reservedAt=now; r.scheduledStart=now; r.scheduledEnd=now+durMs;
    r.actualEntry=0; r.actualExit=0; r.amount=0; r.isPaid=false; r.status="upcoming";
    resCount++;
    pushNotif("{\"type\":\"booked\",\"bid\":\""+bid+"\",\"slot\":\""+sname+"\"}");
    doc["success"]=true; doc["bookingId"]=bid; doc["slotName"]=sname;
  } else {
    doc["success"]=false; doc["message"]="Slot unavailable";
  }
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

void handleCancel() {
  String bid=server.arg("booking");
  for(int i=0;i<resCount;i++){
    Reservation& r=reservations[i];
    if(r.bookingId==bid && (r.status=="upcoming"||r.status=="active")){
      if(r.slotIndex<4) fourSlots[r.slotIndex]=0;
      r.status="cancelled";
      server.send(200,"application/json","{\"success\":true}");
      return;
    }
  }
  server.send(200,"application/json","{\"success\":false,\"message\":\"Not found\"}");
}

void handleLookup() {
  String ref=server.arg("ref");
  StaticJsonDocument<256> doc;
  for(int i=0;i<resCount;i++){
    Reservation& r=reservations[i];
    if(r.bookingId==ref || r.vehicleNumber==ref){
      doc["found"]=true; doc["bookingId"]=r.bookingId; doc["ownerName"]=r.ownerName;
      doc["vehicleNumber"]=r.vehicleNumber; doc["slotName"]=r.slotName;
      doc["type"]=r.vehicleType; doc["status"]=r.status;
      doc["duration"]=String((r.scheduledEnd-r.scheduledStart)/3600000.0);
      if(r.actualEntry>0){ unsigned long elapsed=millis()-r.actualEntry; doc["elapsedMs"]=elapsed; }
      String resp; serializeJson(doc,resp);
      server.send(200,"application/json",resp); return;
    }
  }
  doc["found"]=false;
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

void handleEntry() {
  String bid=server.arg("booking"),type=server.arg("type"),slot=server.arg("slot");
  StaticJsonDocument<128> doc;
  for(int i=0;i<resCount;i++){
    Reservation& r=reservations[i];
    if((r.bookingId==bid||r.vehicleNumber==bid) && r.status=="upcoming"){
      r.actualEntry=millis(); r.status="active";
      if(r.slotIndex<4) fourSlots[r.slotIndex]=1;
      // Open entry gate
      openEntryGate();
      lcdPrint("Welcome!", "Slot: "+r.slotName);
      delay(100); // non-blocking in prod; brief for demo
      doc["success"]=true; doc["bookingId"]=r.bookingId; doc["slot"]=r.slotName; doc["vehicle"]=r.vehicleNumber;
      String resp; serializeJson(doc,resp);
      server.send(200,"application/json",resp); return;
    }
  }
  // Walk-in via API
  if(resCount<MAX_RESERVATIONS){
    int slotIdx=-1; String sname=slot;
    for(int i=0;i<4;i++) if(fourSlots[i]==0){slotIdx=i;break;}
    if(slotIdx>=0){
      String bid2=makeBid();
      if(sname.length()==0) sname="B"+String(slotIdx+1);
      fourSlots[slotIdx]=1;
      Reservation& r=reservations[resCount];
      r={bid2,bid,"four",slotIdx,sname,"Walk-in","",millis(),millis(),millis()+3600000,millis(),0,0,false,"active"};
      resCount++;
      openEntryGate();
      lcdPrint("Walk-In OK!", "Slot: "+sname);
      doc["success"]=true; doc["bookingId"]=bid2; doc["slot"]=sname; doc["vehicle"]=bid;
      String resp; serializeJson(doc,resp);
      server.send(200,"application/json",resp); return;
    }
  }
  doc["success"]=false; doc["message"]="Booking not found or no free slot";
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

void handleExit() {
  String ref=server.arg("booking");
  StaticJsonDocument<256> doc;
  for(int i=0;i<resCount;i++){
    Reservation& r=reservations[i];
    if((r.bookingId==ref||r.vehicleNumber==ref) && r.status=="active"){
      unsigned long now=millis();
      unsigned long entry=r.actualEntry>0?r.actualEntry:r.reservedAt;
      unsigned long elapsed=now-entry;
      unsigned long mins=elapsed/60000UL; if(mins==0)mins=1;
      unsigned long slots=(mins+29)/30;
      r.actualExit=now; r.amount=slots*10.0f; r.status="completed";
      if(r.slotIndex<4) fourSlots[r.slotIndex]=0;
      lastReceipt={r.bookingId,r.vehicleNumber,r.slotName,r.ownerName,elapsed,(int)mins,(int)slots,r.amount,true};
      // Show fare on LCD at exit gate
      lcdPrint("Pay: Rs" + String((int)r.amount), "Scan QR to pay", "& exit");
      doc["success"]=true; doc["bookingId"]=r.bookingId; doc["vehicle"]=r.vehicleNumber;
      doc["slot"]=r.slotName; doc["owner"]=r.ownerName;
      doc["durationMin"]=(int)mins; doc["billSlots"]=(int)slots; doc["amount"]=(int)r.amount;
      String resp; serializeJson(doc,resp);
      server.send(200,"application/json",resp); return;
    }
  }
  doc["success"]=false; doc["message"]="Active booking not found";
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

void handleGetFare() {
  String ref=server.arg("booking");
  StaticJsonDocument<64> doc;
  for(int i=0;i<resCount;i++){
    Reservation& r=reservations[i];
    if((r.bookingId==ref||r.vehicleNumber==ref) && r.status=="completed" && !r.isPaid){
      doc["amount"]=(int)r.amount;
      String resp; serializeJson(doc,resp);
      server.send(200,"application/json",resp); return;
    }
  }
  doc["amount"]=0;
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

void handlePayment() {
  String ref=server.arg("booking");
  StaticJsonDocument<64> doc;
  for(int i=0;i<resCount;i++){
    Reservation& r=reservations[i];
    if((r.bookingId==ref||r.vehicleNumber==ref) && !r.isPaid && r.status=="completed"){
      r.isPaid=true;
      lcdPrint("Paid! Scan QR", "at exit gate");
      doc["success"]=true; doc["amount"]=(int)r.amount;
      String resp; serializeJson(doc,resp);
      server.send(200,"application/json",resp); return;
    }
  }
  doc["success"]=false; doc["message"]="Not found or already paid";
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

// NEW: /api/generate-token — generates QR gate pass token after payment
void handleGenerateToken() {
  String ref=server.arg("booking");
  StaticJsonDocument<128> doc;
  for(int i=0;i<resCount;i++){
    Reservation& r=reservations[i];
    if((r.bookingId==ref||r.vehicleNumber==ref) && r.isPaid){
      String tok=generateToken(r.bookingId, r.vehicleNumber);
      doc["success"]=true;
      doc["token"]=tok;
      doc["bookingId"]=r.bookingId;
      doc["expiresInSec"]=300;
      String resp; serializeJson(doc,resp);
      server.send(200,"application/json",resp); return;
    }
  }
  doc["success"]=false; doc["message"]="Booking not found or not paid";
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

// NEW: /gate/open?token=TKXXXX — QR scanned, validate token → open exit gate
void handleGateOpen() {
  String tok=server.arg("token");
  if(tok.length()==0){
    server.send(200,"text/html","<h2>❌ Invalid QR — no token provided</h2>");
    return;
  }
  if(validateToken(tok)){
    openExitGate();
    lcdPrint("Gate Opening!", "Safe Journey!");
    Serial.println("[GATE] QR token valid — exit gate opened: " + tok);
    server.send(200,"text/html",R"html(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Gate Opening</title>
<style>
body{font-family:sans-serif;background:#050a14;color:#fff;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;flex-direction:column;gap:16px;}
.icon{font-size:80px;animation:bounce .6s ease infinite alternate;}
@keyframes bounce{from{transform:scale(1)}to{transform:scale(1.15)}}
h1{color:#10b981;font-size:28px;letter-spacing:2px;}
p{color:#64748b;font-size:14px;}
.tick{color:#10b981;font-size:16px;}
</style></head><body>
<div class="icon">🚧</div>
<h1>✅ GATE OPENING!</h1>
<p class="tick">Token verified · Boom barrier rising...</p>
<p style="color:#475569;font-size:12px;">You may drive through now. Safe journey! 🚗</p>
</body></html>
)html");
  } else {
    lcdPrint("Invalid QR!", "Pay dashboard");
    Serial.println("[GATE] QR token invalid/expired: " + tok);
    server.send(200,"text/html",R"html(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Invalid QR</title>
<style>
body{font-family:sans-serif;background:#050a14;color:#fff;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;flex-direction:column;gap:16px;}
.icon{font-size:80px;}
h1{color:#ef4444;font-size:24px;}
p{color:#64748b;font-size:14px;text-align:center;max-width:300px;}
</style></head><body>
<div class="icon">❌</div>
<h1>INVALID QR CODE</h1>
<p>This QR has expired or already been used.<br>Please pay on the dashboard to get a new QR pass.</p>
</body></html>
)html");
  }
}

void handleBookings() {
  DynamicJsonDocument doc(4096);
  JsonArray arr=doc.createNestedArray("bookings");
  for(int i=0;i<resCount;i++){
    Reservation& r=reservations[i];
    JsonObject o=arr.createNestedObject();
    o["bookingId"]=r.bookingId; o["vehicleNumber"]=r.vehicleNumber;
    o["vehicleType"]=r.vehicleType; o["slotName"]=r.slotName;
    o["ownerName"]=r.ownerName; o["ownerPhone"]=r.ownerPhone;
    o["status"]=r.status;
    o["duration"]=String((r.scheduledEnd-r.scheduledStart)/3600000.0,1);
    o["amount"]=(int)r.amount; o["isPaid"]=r.isPaid;
    unsigned long secs=(r.scheduledStart/1000UL)%86400UL;
    unsigned long h=secs/3600, m=(secs%3600)/60;
    char buf[10]; sprintf(buf,"%02lu:%02lu",h,m);
    o["scheduledStart"]=String(buf)+" (uptime)";
  }
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

void handleEvents() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"text/event-stream","");
  server.sendContent("retry: 3000\n\n");
  int cs=fourSlots[0];
  String p="{\"state\":"+String(cs)+",\"wasReserved\":false,\"irExit\":false,\"amount\":0,\"durationMin\":0}";
  server.sendContent("event: ir\ndata: "+p+"\n\n");
  while(notifCount>0){
    int idx=(notifHead-notifCount+NOTIF_MAX)%NOTIF_MAX;
    String raw=notifQueue[idx];
    bool wr=(raw.indexOf("RES_OCC")!=-1);
    bool isExit=(raw.indexOf("EXIT_FARE")!=-1);
    int amt=0,dur=0;
    if(isExit && lastReceipt.valid){amt=(int)lastReceipt.amount;dur=lastReceipt.durationMin;}
    String p2="{\"state\":"+String(fourSlots[0])+",\"wasReserved\":"+(wr?"true":"false")+
              ",\"irExit\":"+(isExit?"true":"false")+
              ",\"amount\":"+String(amt)+",\"durationMin\":"+String(dur)+"}";
    server.sendContent("event: ir\ndata: "+p2+"\n\n");
    notifCount--;
  }
}

// ─────────────────────────────────────────────────────────
//  GUEST API HANDLERS
// ─────────────────────────────────────────────────────────

// /api/guest-lookup?vehicle=MH01AB1234
void handleGuestLookup() {
  String veh = server.arg("vehicle");
  veh.toUpperCase(); veh.trim();
  StaticJsonDocument<320> doc;
  for (int i=0;i<resCount;i++) {
    Reservation& r = reservations[i];
    if (r.vehicleNumber == veh) {
      pushGuestEvent("view", r.vehicleNumber, r.slotName, r.status);
      doc["found"]       = true;
      doc["bookingId"]   = r.bookingId;
      doc["vehicleNumber"] = r.vehicleNumber;
      doc["vehicleType"] = r.vehicleType;
      doc["slotName"]    = r.slotName;
      doc["status"]      = r.status;
      doc["isPaid"]      = r.isPaid;
      doc["amount"]      = (int)r.amount;
      doc["entryMs"]     = (unsigned long)r.actualEntry;
      doc["nowMs"]       = (unsigned long)millis();
      if (r.actualEntry > 0) {
        unsigned long elapsed = millis() - r.actualEntry;
        doc["elapsedMs"]  = elapsed;
        unsigned long mins = elapsed/60000UL; if(mins==0) mins=1;
        doc["durationMin"] = (int)mins;
      } else {
        doc["elapsedMs"]  = 0;
        doc["durationMin"] = 0;
      }
      String resp; serializeJson(doc,resp);
      server.send(200,"application/json",resp); return;
    }
  }
  doc["found"]   = false;
  doc["message"] = "No booking found for this vehicle number";
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

// /api/guest-pay?vehicle=MH01AB1234 — demo payment, auto-confirms, generates token
void handleGuestPay() {
  String veh = server.arg("vehicle");
  veh.toUpperCase(); veh.trim();
  StaticJsonDocument<128> doc;
  for (int i=0;i<resCount;i++) {
    Reservation& r = reservations[i];
    if (r.vehicleNumber == veh && r.status == "completed" && !r.isPaid) {
      r.isPaid = true;
      String tok = generateToken(r.bookingId, r.vehicleNumber);
      lcdPrint("Payment Done!", "Go to exit gate");
      Serial.println("[PAY] Payment confirmed for " + veh + " — token: " + tok);
      // Interrupt any entry flow and prompt for exit token immediately
      serState   = SER_WAIT_TOKEN;
      serStartMs = millis();
      serPendingVehicle = "";
      serPendingVehicleConfirm = "";
      Serial.println("--------------------------------------------");
      Serial.println("[EXIT] Enter your token to exit:");
      Serial.print("> ");
      pushGuestEvent("paid",  r.vehicleNumber, r.slotName, "Rs"+String((int)r.amount));
      pushGuestEvent("token", r.vehicleNumber, r.slotName, tok);
      doc["success"] = true;
      doc["token"]   = tok;
      doc["amount"]  = (int)r.amount;
      String resp; serializeJson(doc,resp);
      server.send(200,"application/json",resp); return;
    }
  }
  doc["success"] = false;
  doc["message"] = "Booking not found or already paid";
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

// /api/guest-regen-token?vehicle=MH01AB1234 — regen token if already paid (free)
void handleGuestRegenToken() {
  String veh = server.arg("vehicle");
  veh.toUpperCase(); veh.trim();
  StaticJsonDocument<128> doc;
  for (int i=0;i<resCount;i++) {
    Reservation& r = reservations[i];
    if (r.vehicleNumber == veh && r.isPaid) {
      String tok = generateToken(r.bookingId, r.vehicleNumber);
      Serial.println("[PAY] Token regenerated for " + veh + ": " + tok);
      // Interrupt any entry flow and prompt for exit token immediately
      serState   = SER_WAIT_TOKEN;
      serStartMs = millis();
      serPendingVehicle = "";
      serPendingVehicleConfirm = "";
      Serial.println("--------------------------------------------");
      Serial.println("[EXIT] Enter your token to exit:");
      Serial.print("> ");
      pushGuestEvent("token", r.vehicleNumber, r.slotName, tok+"(regen)");
      doc["success"] = true;
      doc["token"]   = tok;
      String resp; serializeJson(doc,resp);
      server.send(200,"application/json",resp); return;
    }
  }
  doc["success"] = false;
  doc["message"] = "No paid booking found";
  String resp; serializeJson(doc,resp);
  server.send(200,"application/json",resp);
}

// /api/guest-feed — SSE stream of guest activity events for admin live panel
void handleGuestFeed() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/event-stream", "");
  server.sendContent("retry: 3000\n\n");
  // Send all buffered events oldest first
  int count = gfeedCount;
  for (int i = 0; i < count; i++) {
    int idx = (gfeedHead - count + i + GFEED_MAX) % GFEED_MAX;
    GuestEvent& e = guestFeed[idx];
    unsigned long agoSec = (millis() - e.ts) / 1000UL;
    String payload = "{\"type\":\"" + e.type + "\","
                   + "\"vehicle\":\"" + e.vehicle + "\","
                   + "\"slot\":\"" + e.slot + "\","
                   + "\"detail\":\"" + e.detail + "\","
                   + "\"ago\":" + String(agoSec) + "}";
    server.sendContent("event: guest\ndata: " + payload + "\n\n");
  }
  // Clear sent events
  gfeedCount = 0;
}

// Serve pages
void serveGuestPage() { server.send(200,"text/html",GUEST_PAGE); }
void serveQRPage()    { server.send(200,"text/html",QR_PAGE); }

// ─────────────────────────────────────────────────────────
//  SETUP & LOOP
// ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // IR pins
  pinMode(ENTRY_TRIG_PIN, OUTPUT);
  pinMode(ENTRY_ECHO_PIN, INPUT);

  pinMode(IR_SLOT_B1,     INPUT);
  pinMode(IR_SLOT_B2,     INPUT);
  pinMode(IR_SLOT_B3,     INPUT);
  pinMode(IR_SLOT_B4,     INPUT);


  // OLED
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  Serial.println("[LCD] 16x2 I2C initialized");
  lcdPrint("SMART PARKING", "Booting...");

  // Servos
  entryServo.attach(ENTRY_SERVO_PIN);
  exitServo.attach(EXIT_SERVO_PIN);
  entryServo.write(SERVO_CLOSED);
  exitServo.write(SERVO_CLOSED);


  // WiFi
  lcdPrint("Connecting", "to WiFi", "Please wait...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  String ip = WiFi.localIP().toString();
  Serial.println("\nIP: " + ip);
  lcdPrint("WiFi OK!", ip, "smartparking.local");
  delay(2000);

  // mDNS — accessible at http://smartparking.local
  if (MDNS.begin("smartparking")) {
    Serial.println("mDNS: http://smartparking.local");
  }
  lcdWelcome();

  // Routes
  server.on("/",                    loginPageHandler);
  server.on("/api/login",           handleLogin);
  server.on("/app",                 serveAdminPage);
  server.on("/userpage",            serveUserPage);
  server.on("/guest",               serveGuestPage);
  server.on("/qr",                  serveQRPage);
  server.on("/api/stats",           handleStats);
  server.on("/api/available-slots", handleAvailableSlots);
  server.on("/api/reserve",         handleReserve);
  server.on("/api/cancel",          handleCancel);
  server.on("/api/lookup",          handleLookup);
  server.on("/api/entry",           handleEntry);
  server.on("/api/exit",            handleExit);
  server.on("/api/get-fare",        handleGetFare);
  server.on("/api/payment",         handlePayment);
  server.on("/api/generate-token",  handleGenerateToken);
  server.on("/gate/open",           handleGateOpen);
  server.on("/api/bookings",        handleBookings);
  server.on("/api/events",          handleEvents);
  server.on("/api/guest-feed",        handleGuestFeed);
  server.on("/api/guest-lookup",      handleGuestLookup);
  server.on("/api/guest-pay",         handleGuestPay);
  server.on("/api/guest-regen-token", handleGuestRegenToken);

  server.begin();
  Serial.println("Server ready.");
  Serial.println("Admin:      http://smartparking.local/app");
  Serial.println("User:       http://smartparking.local/userpage");
  Serial.println("Guest:      http://smartparking.local/guest  ← QR STICKER");
  Serial.println("QR page:    http://smartparking.local/qr");
  Serial.println("Entry ultrasonic: TRIG=GPIO" + String(ENTRY_TRIG_PIN) + " ECHO=GPIO" + String(ENTRY_ECHO_PIN));
  Serial.println("Exit  gate IR: GPIO " + String(IR_EXIT_GATE));
  Serial.println("Slot B1 IR: GPIO " + String(IR_SLOT_B1));
  Serial.println("Slot B2 IR: GPIO " + String(IR_SLOT_B2));
  Serial.println("Slot B3 IR: GPIO " + String(IR_SLOT_B3));
  Serial.println("Slot B4 IR: GPIO " + String(IR_SLOT_B4));
  Serial.println("--------------------------------------------");
  Serial.println("ENTRY FLOW:");
  Serial.println("  Car detected → Booked? Y/N");
  Serial.println("  Y → Enter vehicle number → gate opens");
  Serial.println("  N → Enter vehicle number → walk-in created → gate opens");
  Serial.println("      Customer scans QR sticker → guest dashboard");
  Serial.println("SERIAL COMMANDS:");
  Serial.println("  STATUS  — print all slot states");
  Serial.println("  TOKENS  — list active tokens");
  Serial.println("  TOKEN   — enter token to open exit gate");
  Serial.println("  TOKEN   — enter token to open exit gate");
  Serial.println("--------------------------------------------");
}

void loop() {
  server.handleClient();
  

  // IR sensors
  handleEntryGateIR();
  handleSlotB1IR();
  handleSlotB2IR();
  handleSlotB3IR();
  handleSlotB4IR();

  // Serial Monitor input (replaces keypad)
  processSerialInput();

  // Auto-close gates after timeout
  checkGateTimers();

  // Expire stale bookings
  checkExpiredReservations();

  // LCD: update slot count every 5 seconds
  static unsigned long lastLcdUpdate = 0;
  if (millis() - lastLcdUpdate >= 5000) {
    lastLcdUpdate = millis();
    int occ = 0;
    for (int i=0;i<4;i++) if (fourSlots[i]==1) occ++;
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Smart Parking");
    lcd.setCursor(0,1);
    lcd.print("Slots: " + String(occ) + "/4 occupied");
  }
  delay(10);
}
