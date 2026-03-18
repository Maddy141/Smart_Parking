# 🅿️ Smart Pay Parking — ESP32 IoT Parking System

> A fully automated smart parking system built on the **ESP32** microcontroller with a web-based dashboard, real-time IR slot detection, servo-controlled boom gates, UPI payment flow, and token-based exit authorization.



## 🧠 What I Built

This is an end-to-end smart parking system with **three user roles** (Admin, User, Walk-in Guest), **automated gate control**, and a **mobile-friendly web dashboard** served directly from the ESP32 over WiFi.

### Core Features

| Feature | Details |
|---|---|
| **Entry Gate** | Ultrasonic sensor detects approaching vehicle → Serial Monitor prompts operator: Booked or Walk-in? |
| **Slot Detection** | 4× IR sensors (B1–B4) monitor occupancy in real time |
| **Automated Billing** | Timer starts on entry, fare calculated at ₹10 per 30 minutes |
| **Web Dashboard** | Admin, User, and Guest pages served from the ESP32 |
| **UPI Payment Flow** | Guest scans QR on `/guest` page → confirms payment → receives exit token |
| **Token-Based Exit** | Operator enters token in Serial Monitor → exit gate servo opens |
| **mDNS** | Accessible at `http://smartparking.local` on local network |
| **LCD Display** | 16×2 I2C LCD shows slot status, welcome messages, and fare |
| **Receipt System** | Auto-generated receipt with duration, billing units, and total |

---

## 🏗️ System Architecture

```
ESP32
├── WiFi (Access Point + mDNS: smartparking.local)
├── WebServer (port 80)
│   ├── /              → Login page (Admin / User)
│   ├── /app           → Admin dashboard
│   ├── /userpage      → User portal
│   ├── /guest         → Walk-in guest payment dashboard
│   ├── /qr            → Printable QR sticker generator
│   └── /gate/open     → Token-validated exit gate endpoint
├── Sensors
│   ├── Ultrasonic (GPIO 13/14)  → Entry gate detection
│   ├── IR Slot B1 (GPIO 32)     → Occupancy
│   ├── IR Slot B2 (GPIO 33)     → Occupancy
│   ├── IR Slot B3 (GPIO 34)     → Occupancy
│   └── IR Slot B4 (GPIO 35)     → Occupancy
├── Actuators
│   ├── Entry Servo (GPIO 16)    → Boom gate (0° closed / 90° open)
│   └── Exit Servo  (GPIO 15)    → Boom gate (0° closed / 90° open)
└── Display
    └── LCD 16×2 I2C (SDA: GPIO 21, SCL: GPIO 22, Addr: 0x27)
```

---

## 🌊 End-to-End Flow

```
Car arrives at Entry Gate
        │
        ▼
Ultrasonic detects vehicle
        │
        ▼
Serial Monitor: "Booked? Y or N"
   ┌────┴────┐
   Y         N
   │         │
   ▼         ▼
Enter      Enter
vehicle    vehicle
number     number
   │         │
   ▼         ▼
Match in   Auto-create
booking DB  booking
   │         │
   └────┬────┘
        ▼
  Entry Gate Opens (Servo 90°)
  Billing Timer Starts
        │
        ▼
  Vehicle Parked → IR Sensor Active
        │
        ▼
  Car Leaves Slot → IR Clears
  Fare Calculated (₹10 / 30 min)
        │
        ▼
  Guest opens /guest → Enters vehicle no.
  Live timer + fare shown
  PAY NOW button activates
        │
        ▼
  Scan UPI QR → "I Have Paid"
  Exit Token Generated
        │
        ▼
  Operator types TOKEN in Serial Monitor
  Token validated → Exit Gate Opens
```

---

## 🖥️ Web Pages

### `/` — Login Page
- Role selector: **Admin** or **User**
- Animated parking background with cars driving across
- Demo credentials shown inline

### `/app` — Admin Dashboard
- Live slot grid (B1–B4) with colour-coded occupancy
- Entry/Exit gate status (open/closed pill indicators)
- Payment live feed panel (SSE stream — walk-ins, payments, tokens)
- Advance reservation with slot picker and fare preview
- Manual entry / exit confirmation + receipt modal
- Payment confirmation + QR gate pass generation
- Booking manager with filter by status (upcoming / active / completed)

### `/userpage` — User Portal
- Live slot availability view
- Self-service reservation with date/time/duration picker
- My Bookings view with pay and QR pass buttons
- Pay & get QR exit pass flow

### `/guest` — Walk-in Guest Payment Dashboard *(no login)*
- Enter vehicle number → personal dashboard opens
- Live parking timer (updates every second)
- Live fare display (polls every 30 seconds)
- PAY NOW button greyed out while car is in slot (IR = 1)
- PAY NOW activates when car leaves (IR = 0)
- Demo UPI QR shown → "I Have Paid" → exit token displayed with 10-minute countdown

### `/qr` — Print QR Sticker
- Generates a printable QR code pointing to `/guest`
- Supports mDNS (`smartparking.local`) or direct IP mode
- One-click print or download as PNG
- WiFi name shown on sticker for guests to connect

---

## 🔌 Hardware

| Component | Spec | GPIO |
|---|---|---|
| Microcontroller | ESP32 (30-pin) | — |
| Entry Detection | HC-SR04 Ultrasonic | TRIG: 13, ECHO: 14 |
| Slot B1 IR | IR Obstacle Sensor | 32 |
| Slot B2 IR | IR Obstacle Sensor | 33 |
| Slot B3 IR | IR Obstacle Sensor | 34 |
| Slot B4 IR | IR Obstacle Sensor | 35 |
| Entry Gate | SG90 Servo | 16 |
| Exit Gate | SG90 Servo | 15 |
| LCD | 16×2 I2C (0x27) | SDA: 21, SCL: 22 |

**Physical model** built inside a cardboard box with:
- Black foam sheet floor with yellow dashed lane markings
- White cardboard slot dividers
- Toy cars for demo (Hot Wheels scale)
- Entry and Exit labels with servo-mounted boom barriers

---

## 📦 Libraries Required

Install via Arduino Library Manager or PlatformIO:

```
WiFi.h            (built-in ESP32)
WebServer.h       (built-in ESP32)
ESPmDNS.h         (built-in ESP32)
ArduinoJson       → v6.x
Wire.h            (built-in)
LiquidCrystal_I2C → Frank de Brabander
ESP32Servo        → Kevin Harrington
```

---

## ⚙️ Configuration

Edit these constants at the top of `Smart_Pay_Parking.ino`:

```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

String adminUsername = "admin";
String adminPassword = "1234";
String userUsername  = "user";
String userPassword  = "user123";

#define ENTRY_TRIG_PIN  13
#define ENTRY_ECHO_PIN  14
#define ENTRY_SERVO_PIN 16
#define EXIT_SERVO_PIN  15
#define LCD_ADDR        0x27
```

---

## 🚀 Getting Started

1. **Clone this repo**
   ```bash
   git clone https://github.com/YOUR_USERNAME/smart-pay-parking.git
   ```

2. **Install Arduino IDE** (v2.x recommended) with the **ESP32 board package**
   ```
   Board Manager URL: https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```

3. **Install required libraries** (see Libraries section above)

4. **Update WiFi credentials** in the sketch

5. **Upload to ESP32** — select board: `ESP32 Dev Module`, Port: your COM port

6. **Open Serial Monitor** at `115200` baud

7. **Connect your phone/PC to the same WiFi network** and navigate to:
   - `http://smartparking.local` (mDNS) — or —
   - IP printed in Serial Monitor on boot

---

## 🖨️ Serial Monitor Commands

While the system is running, type these commands in Serial Monitor:

| Command | Action |
|---|---|
| `STATUS` | Print all slot states + active bookings |
| `TOKENS` | List all active exit tokens |
| `TOKEN` | Enter a token to manually open the exit gate |

**Entry Flow prompts appear automatically** when the ultrasonic detects a vehicle.

---

## 💸 Fare Structure

| Duration | Fare |
|---|---|
| 0–30 min | ₹10 |
| 31–60 min | ₹20 |
| 61–90 min | ₹30 |
| Each additional 30 min | +₹10 |

Formula: `ceil(minutes / 30) × ₹10` — minimum ₹10.

---

## 📁 File Structure

```
smart-pay-parking/
├── Smart_Pay_Parking.ino   ← Main Arduino sketch (all-in-one)
├── html/
│   ├── login.html          ← Login page (embedded in sketch)
│   ├── admin.html          ← Admin dashboard (embedded in sketch)
│   ├── user.html           ← User portal (embedded in sketch)
│   ├── guest.html          ← Guest payment dashboard (embedded)
│   └── qr.html             ← QR print page (embedded in sketch)
├── docs/
│   ├── Smart_Parking_Flowchart.pdf
│   └── Smart_Pay_Parking_pptx.pptx
├── imgproject.jpeg         ← Physical model photo
└── README.md
```

> All HTML is embedded directly in the `.ino` file as `const char*` strings and served by the ESP32 WebServer — no SD card or external server needed.

---

## 🔐 Security Notes

- This is a **demo/prototype** project intended for lab/exhibition use
- Credentials are stored in plaintext in the sketch — do not use default passwords in any real deployment
- Tokens are valid for 10 minutes and are single-use
- No HTTPS — operates over plain HTTP on a local WiFi network

---

## 🧪 Tested On

- **Board:** ESP32 Dev Module (30-pin, 4MB flash)
- **Arduino IDE:** 2.3.x
- **ArduinoJson:** 6.21.x
- **Network:** 2.4 GHz WiFi (5 GHz not supported by ESP32)

---

## 🏆 Project Context

Built as an IoT engineering project demonstrating:
- Embedded web server on a microcontroller
- Real-time sensor integration (IR + ultrasonic)
- Multi-user role-based web interface
- Automated billing and payment token system
- Physical hardware model with servo-controlled gates

---

## 📄 License

This project is open for educational and personal use. Attribution appreciated if you build on it.

---

*Built with ❤️ using ESP32, Arduino, and a lot of hot glue.*
