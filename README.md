# WiFi Remote Power-On + PIN Auto-Login

> Turn an ESP32-S3 into a **remote power button** and a **USB keyboard** — wake your locked/shut-down laptop from anywhere in the world and log in automatically.

[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)]()
[![Framework](https://img.shields.io/badge/framework-Arduino-teal)]()
[![Cost](https://img.shields.io/badge/BOM-%C2%A528-brightgreen)]()
[![License](https://img.shields.io/badge/license-MIT-lightgrey)]()

**Languages**: **English** ｜ [简体中文](README.zh-CN.md)

---

## The Problem

You're away from home. Your laptop is shut down or locked. You need in.

| Situation | Common solutions | Why they fail |
|---|---|---|
| Laptop powered off | Wake-on-LAN | NIC loses power when off; Wi-Fi WoWLAN often unreliable |
| Locked, at login screen | ToDesk / AnyDesk unattended, AutoHotkey, pynput | The Windows login screen runs on the **Secure Desktop** — no user session exists, so **all software keystroke injection is ignored** |
| Locked, at login screen | Commercial remote-desktop w/ system service | Works, but heavy, closed-source, privacy concerns |

**Key insight:** the Windows login screen accepts input from **real keyboards only**. So if you want to automate that step, you need **hardware HID** — and the ESP32-S3 is perfect for it (native USB-OTG, enumerates as a standard keyboard).

**And** if the machine is fully powered off, no amount of USB magic will help — so we also bolt on a tiny servo to **physically press the power button**.

```
        Your phone (anywhere with internet)
                    │  MQTT
                    ▼
    ┌──────────────────────────────┐
    │         ESP32-S3             │
    │  ┌────────────┐ ┌─────────┐  │
    │  │ USB HID    │ │ Servo   │  │
    │  │ keyboard   │ │ (GPIO4) │  │
    │  └─────┬──────┘ └────┬────┘  │
    └────────┼─────────────┼───────┘
             │ USB cable   │ presses
             ▼             ▼
    ┌──────────────────────────────┐
    │   Your laptop                │
    │   [power button]   [login]   │
    └──────────────────────────────┘
```

One board, two roles: **a keyboard** and **a finger**.

---

## Features

- 🔌 **Remote power-on** — servo physically presses the power button (works when the machine is fully off)
- ⌨️ **Auto PIN entry** — ESP32-S3 enumerates as a standard USB HID keyboard and types your PIN at the login screen
- ☁️ **Cloud control** — MQTT (Bemfa Cloud), controllable from anywhere; ships with a WeChat mini-program flow
- 🌐 **LAN fallback** — built-in web server with a browser UI (token-protected) when you're on the same network
- 🔁 **Self-healing** — Wi-Fi auto-reconnect + MQTT heartbeat, survives network hiccups
- 🧪 **Built-in self-test** — a `test` command types your PIN into Notepad to isolate device-side vs host-side issues

---

## Hardware

| Part | Approx. price | Notes |
|---|---|---|
| **ESP32-S3 dev board** (e.g. YD-ESP32-S3 N16R8) | ¥19.5 | **Must have dual USB-C ports, both wired directly to the S3** |
| **SG90 servo** | ¥4 | Standard 9g micro servo |
| 5V/2A USB charger | — | ESP must stay powered 24/7 |
| USB-C data cable | — | Must be a *data* cable, not charge-only |
| Double-sided tape / nano tape | ¥3 | For mounting the servo |
| **Total** | **≈ ¥28** | |

### ⚠️ Board selection warning

Ask the seller: **"Are both USB-C ports wired directly to the S3 chip?"**

If a port goes through a CH340 / CP2102 USB-to-serial chip, **that port can never act as a keyboard**. Also avoid ESP32-C3/C6 — they have no USB HID capability.

---

## Wiring

```
5V/2A charger ──USB-C──▶ ESP "COM" port        (power, always on)
                          ├ 5V pin ──▶ Servo red
                          ├ GPIO4  ──▶ Servo orange  (PWM signal)
                          └ GND    ──▶ Servo brown   (common ground!)

ESP "OTG" port ──USB-C data cable──▶ Laptop USB port   (keyboard signal)
```

- **Common ground is mandatory** between servo and ESP
- The servo can draw ~800 mA when pressing — a 5V/2A supply has headroom
- Two USB ports on the board are for power + data separately; no conflict if both are plugged in

### ⚠️ The `5Vin` header pin is NOT live by default

On boards like the **YD-ESP32-S3**, the `5Vin` header pin is **not connected to the board's 5V rail out of the box**. Two paths run in parallel from that pin to the 5V net:

```
5Vin ──┬──[ D3 diode    ]──▶ 5V    one-way (external → board), ~0.3V drop   ← fitted by default
       └──[ 0Ω resistor ]──▶ 5V    bidirectional, no drop  (marked "IN-OUT") ← NOT fitted by default
```

So by default `5Vin` can only be an **input** (feed the board from an external supply). To *source* 5V from it you must bridge the 0Ω `IN-OUT` pad.

**The vendor's own documentation states:**

> The 5V-IN pin does not supply power by default, because drawing power directly from the core board affects the stability of the ESP32. This design protects the board and keeps it running reliably. If you need 5V output, you can bridge the pad to output 5V.

**Bridging works — but do NOT power a servo from it.**

A servo is a bursty load: 10–50 mA idle, 200–300 mA moving, **700 mA–1 A stalled**. After bridging, that current has to travel

```
charger → USB cable → D1 diode → board 5V net → 5Vin → servo
```

Every element on that path adds resistance (cable, connector, **D1's 0.3–0.5 V drop**, thin PCB copper). By `ΔV = I × R`, a stalled servo drags the board's 5V rail down → the ESP32-S3's 3.3V LDO falls below dropout → **brownout reset**.

The nasty part is *when* it resets. If the ESP resets **inside** the 600 ms button-press window, the servo loses PWM mid-press and freezes on the power button — and the ESP needs 3–5 s to reboot and home it. That is exactly the window that triggers a **forced power-off** on most laptops. You asked it to turn the machine on; you turned it off instead.

**→ Give the servo its own 5V supply:**

| Servo wire | Connect to |
|---|---|
| Orange (signal) | `GPIO14` |
| Red (power) | **External 5V +** — straight from the charger, *not* the board |
| Brown (ground) | Board `GND`, **and** external 5V − must also reach board `GND` (a common ground is mandatory) |

Bridging `IN-OUT` is still perfectly fine for **small loads** (OLED, sensors, <100 mA).

Serve yourself from a spare USB cable (red = 5V, black = GND) or a ¥3 USB-A-to-screw-terminal adapter.

### Mounting the servo (cam method)

1. Mount the servo body **directly above** the power button, disc edge aligned with the key
2. Stick a small silicone pad on the disc edge — this is the "presser"
3. Idle at **10°** (clear of the key), press at **95°** (key fully down). Tune `SERVO_IDLE_DEG` / `SERVO_PRESS_DEG` to your machine.
4. The motion has **one vertical degree of freedom only** — it physically cannot swipe sideways

> The real risk isn't "servo vs solenoid" — it's **excessive force + no travel limit**. A laptop power button needs only ~60–120 g of force.

---

## Software Setup

### 1. Arduino IDE configuration

Install the ESP32 board package via Boards Manager, then select **ESP32S3 Dev Module** and set **all** of these:

| Setting | Value | Why |
|---|---|---|
| **USB Mode** | `USB-OTG (TinyUSB)` | Without this it will **not** enumerate as a keyboard |
| **USB CDC On Boot** | `Enabled` | With `Disabled`, HID doesn't enumerate (verified) |
| **Flash Size** | `16MB (128Mb)` | Match your board; default 4MB will fail to flash |
| **PSRAM** | `OPI PSRAM` | Match your board |
| **Partition Scheme** | `16M Flash (3MB APP/9.9MB FATFS)` | Match your flash layout |

### 2. Install libraries

- **ESP32Servo** by Kevin Harrington
- **PubSubClient** by Nick O'Leary

### 3. Configure

**Option A (recommended — keeps secrets out of git):** copy `secrets.h.example` to `secrets.h` and fill in your real values there. `secrets.h` is already listed in `.gitignore`, so it will never be committed.

```cpp
// secrets.h
#define SECRET_WIFI_SSID  "YourWiFi"
#define SECRET_WIFI_PASS  "YourPassword"
#define SECRET_PIN_CODE   "123456"
#define SECRET_PAGE_TOKEN "change_me"
#define SECRET_BEMFA_UID   "your_bemfa_private_key"
#define SECRET_BEMFA_TOPIC "your_topic_name"
#define SECRET_PC_MAC     "aabbccddeeff"
```

**Option B (simplest):** edit the config block directly at the top of `wifi_pin_login.ino`.

```cpp
const char* WIFI_SSID = "YourWiFi";
const char* WIFI_PASS = "YourPassword";     // leave "" for open networks
const char* PIN_CODE  = "123456";           // your Windows PIN
const char* PAGE_TOKEN = "change_me";       // LAN web UI password

const char* BEMFA_UID   = "your_bemfa_private_key";
const char* BEMFA_TOPIC = "your_topic_name";  // must be an MQTT-type topic

const char* PC_MAC = "aabbccddeeff";        // laptop NIC MAC, no colons
```

> The firmware uses `#if __has_include("secrets.h")` — with `secrets.h` present it takes values from there, otherwise it falls back to the inline defaults above. So either way compiles.

### 4. Flash

Plug the cable into the **`COM` port** to flash, then move it to the **`OTG` port** for operation.

On ESP32-S3, GPIO19/20 change identity: during flashing they act as USB-Serial-JTAG (a COM port); after boot they become the TinyUSB HID keyboard. Same cable, same port — it just transforms.

### 5. MQTT cloud setup (Bemfa)

1. Register at Bemfa Cloud → copy your **private key** → set as `BEMFA_UID`
2. Create a topic — see **"Three gotchas when creating the topic"** below, and get it right the first time
3. Point `BEMFA_TOPIC` at that topic name and re-flash

**Three gotchas when creating the topic — get it right the first time:**

**① Set the name at creation time — don't rename it afterwards**

Both the **topic name** and its **nickname** must be written as your final intended value (e.g. `power003`) **inside the "New Topic" dialog**.

- ❌ Wrong: create it as something throwaway (`test`, `power001`, `fan`), then rename it later
- ✅ Right: type `power003` into both the name and nickname fields when creating it

**Why renaming breaks things:** Bemfa requires **nickname == topic name**. A rename easily updates only one of the two, and once they diverge you get a *phantom-online* state — the ESP reports "subscribed successfully" while the console shows "subscriber offline" and messages are silently dropped. That failure mode is extremely hard to diagnose. **If you got it wrong, delete the topic and recreate it — far more reliable than renaming.**

**② Must be created in the "MQTT device cloud" section, type = MQTT**

- **TCP device cloud** and **MQTT device cloud** are **isolated namespaces**:
  - MQTT protocol (port **9501**) only reaches topics in the **MQTT device cloud**
  - TCP protocol (port **8344**) only reaches topics in the **TCP device cloud**
- A mismatch produces the same *phantom-online* symptom: subscription succeeds but no messages arrive.

**③ Choose the "message type" topic, not the "switch type"**

| Topic type | What it can relay |
|---|---|
| **Switch type** | **Only `on` / `off`** — any other text is silently ignored by the server |
| **Message type** | **Any text** (`on`, `login`, `login1`, `test` …) ✅ |

> This project has 5 commands, so it needs a **message-type** topic. With a switch-type topic only `on`/`off` will work and the rest will appear to do nothing at all.

---

## Commands

| MQTT message | Web UI button | Route | Action |
|---|---|---|---|
| `on` | Power on & auto-login | `/servologin` | Servo presses power → wait → full login sequence |
| `off` | Power on only | `/servopower` | Just press the power button |
| `login` | Type PIN (full flow) | `/login` | WOL → ESC → Enter → PIN → Enter |
| `login1` | Type PIN only | `/login1` | PIN + Enter (**most robust — daily driver**) |
| `test` | Typing self-test | `/test` | Types PIN into Notepad after 5s (debugging) |

### Which login command should I use?

| Scenario | Use | Why |
|---|---|---|
| **Locked** (`Win+L`, most common) | `login1` | PIN field is already focused — fastest and safest, touches nothing else |
| **Display is off** (system still running) | `login` | Needs ESC to wake the display, then Enter to focus the field |
| **Unlocked, focus in browser** | `login1` or set `LOGIN_SEND_PREAMBLE 0` | ⚠️ ESC/Enter would misbehave here |

> **Rule of thumb:** if unsure, send `login1`. It does exactly one thing — type the PIN and press Enter — and can't do any harm.

---

## Troubleshooting

### Extra characters appearing in the PIN

**First: check your test scenario.** This device is designed for the **locked** screen. If you test while unlocked, focus may be in a browser — autocomplete in a search box can append characters and the Enter key triggers a search instead of submitting the PIN. **Lock the screen with `Win+L` before testing.**

**Then use the `test` command to isolate the problem:**

1. Open **Notepad** on the laptop and click into it
2. Click "Typing self-test" in the web UI, or send `test` via MQTT
3. You have 5 seconds to switch to Notepad, then look at what appears

| Result in Notepad | Conclusion | Fix |
|---|---|---|
| Clean `123456` (6 chars) | **Device side is fine** — extra chars come from the host | Check autocomplete / IME / focus |
| Extra characters | Problem is on the send side | Increase `KEY_SETTLE_MS` to 80–100 |

**Why the firmware types slowly:** `Keyboard.write()` sends the key-down and key-up HID reports back-to-back with zero gap. If the host's USB polling misses the intermediate state, down+up can be merged into a long press → triggering **auto-repeat**. `typeString()` instead does `press → 30ms → release → 60ms` per character, so every keystroke is a clean down+up the host can't miss. Slower cadence also makes browser autocomplete less likely to fire.

### Flashing fails ("No serial data received")

- Make sure the cable is in the **`COM` port**, not `OTG`
- Try the other USB-C port (ESP32-S3 can flash from USB-Serial-JTAG too, no BOOT button needed with esptool 5.x)
- Some boards need: hold **BOOT**, tap **RST**, release **BOOT**

### MQTT won't connect

The firmware prints diagnostic hints based on the PubSubClient state code:

- `state = -4` → TCP timeout (network blocked)
- `state = -2` → TCP connect failed (server refused / DNS failure)
- `state = 5` → Unauthorized (check your `BEMFA_UID`)

### Wi-Fi keeps dropping on campus networks

Many campus portals kick idle clients. The firmware auto-reconnects every 10 seconds — you'll see `[WiFi] dropped, reconnecting...` in the serial log. This is expected.

> **Note on captive portals:** many campus portals only hijack **HTTP port 80** (to serve the login page) while other ports pass through. If MQTT works but HTTP doesn't, that's why — no portal authentication is needed. Test directly with a raw TCP connect to your MQTT broker's port rather than inferring from HTTP behavior.

---

## Why not Wake-on-LAN?

WoL magic packets are **Layer-2 broadcast frames** — no IP address, no authentication, no routing required. But that also makes the only requirement a hard one: **sender and receiver must sit in the same broadcast domain.**

| Method | Works when off? | Reliability |
|---|---|---|
| Wi-Fi WoWLAN | ❌ | Only in sleep states where the NIC stays powered — many laptops cut Wi-Fi power in S3 |
| Wired WoL | ⚠️ | Works, **but only when the ESP and the target share a broadcast domain** |
| **Physical button press (this project)** | ✅ | **Any machine, zero config, immune to network topology** |

### The failure mode we actually hit

```
ESP on the campus Wi-Fi    →  subnet A   ┐
                                         ├─ two different VLANs
Laptop on a dorm Ethernet  →  subnet B   ┘
```

The limited broadcast `255.255.255.255` is **never forwarded by Layer-3 devices**, so the magic packet never arrives. **No network-adapter setting can fix this** — it is a topology problem, not a driver problem.

> We verified every box on the PC side was ticked: `WakeOnMagicPacket = Enabled`, the adapter armed as a wake source (`powercfg /devicequery wake_armed`), fast startup disabled, both **wired and wireless** MACs sent, direct subnet broadcast *plus* unicast — all correct, all useless behind a VLAN boundary. Don't burn a weekend on it; check the topology first.

> **Want WoL to actually work?** Put the ESP and the target machine behind **the same router**, so they share a `192.168.x.x` subnet. Then set `ENABLE_WOL` to `1`.

### What we recommend instead: lock screen, not sleep

The machine keeps running, the ESP keyboard stays online, and a single `login1` command logs you in — **no WoL, no servo, no extra hardware**:

```powershell
powercfg /change standby-timeout-ac 0     # never sleep on AC
powercfg /change standby-timeout-dc 0     # never sleep on battery
```

Also set *Control Panel → Power Options → Choose what closing the lid does → Do nothing* (and mind the thermals if you close the lid).

| State | ESP keyboard works? | Command |
|---|---|---|
| Display off (system running) | ✅ | `login` |
| Locked (`Win+L`) | ✅ **sweet spot** | `login1` |
| Sleep / hibernate | ❌ **avoid — the ESP cannot wake it** | — |

---

## Project Structure

```
.
├── wifi_pin_login.ino      # Main firmware
├── README.md               # This file (English)
├── README.zh-CN.md         # 简体中文说明
├── TECH_SUMMARY.md         # Deep-dive: architecture, protocol notes, lessons learned
├── secrets.h.example       # Credentials template → copy to secrets.h (gitignored)
├── power_bridge.scad       # Parametric 3D-printable servo mount (OpenSCAD)
├── LICENSE                 # MIT
└── .gitignore              # Excludes secrets.h and build artifacts
```

---

## Roadmap

- [ ] Status feedback (confirm login succeeded, not just that keys were sent)
- [ ] ESP32-CAM variant: screenshot the screen after boot to verify
- [ ] Closed-loop servo control with position feedback
- [ ] Refactor into a generic `actuator + hid + cloud` framework for other remote physical tasks

---

## License

MIT — do whatever you want, no warranty.

## Acknowledgements

- [ESP32Servo](https://github.com/madhephaestus/ESP32Servo) — servo control
- [PubSubClient](https://github.com/knolleary/pubsubclient) — MQTT
- [Bemfa Cloud](https://cloud.bemfa.com/) — free MQTT broker with a WeChat mini-program

---

**⚠️ Security note:** This device types your PIN on command. Keep your MQTT private key and web token secret, and don't expose the web UI to the internet without a strong token.
