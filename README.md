# Smart Attendance System

An embedded attendance system using fingerprint authentication, keypad input, and a web-based attendance portal.

## Hardware Components
- **STM32 Nucleo-F439ZI** — Main controller for fingerprint processing and attendance management
- **Vega board** — LCD and buzzer interface
- **Fingerprint Sensor** — Biometric identification
- **4×4 Keypad** — Course selection, student input, and admin controls
- **I2C LCD** — User feedback and system status
- **Buzzer** — Audio feedback

## Communication Interfaces

| Interface | Connection | Purpose |
|---|---|---|
| UART | STM32 USART3 ↔ Vega UART1 | LCD commands and attendance events |
| UART | STM32 USART6 ↔ Fingerprint sensor | Fingerprint enrollment and matching |
| I2C | Vega ↔ LCD | Display communication |
| GPIO | STM32 ↔ 4×4 keypad | Row-column scanning for key input |
| UART | STM32 ↔ PC | Transfer attendance events to the web application |

## System Architecture

```text
Fingerprint Sensor
       │ UART
       ▼
 STM32 Nucleo-F439ZI ◄── GPIO ──► 4×4 Keypad
       │
       |
       |
       ├── UART ──► Vega ── I2C ──► LCD
       │              │
       │           GPIO ──► Buzzer
       │
       |
       |
       └── UART ──► PC Web Application
                         │
                         ▼
                    Attendance Records
```

## Working
1. The fingerprint sensor captures and matches the student's fingerprint.
2. The STM32 validates the student and manages attendance states.
3. The keypad provides course selection and administrative functions.
4. The STM32 sends attendance events to the PC over UART.
5. The web application stores attendance records for viewing through a browser.

## Repository Structure
- `stm32/` — STM32 firmware and peripheral configuration
- `vega/` — UART command handling, LCD control, and buzzer interface
- `python_app/` — Web-based attendance portal

## Demo
[Watch the project demonstration](https://drive.google.com/file/d/17RwWF3fFF3z1ESdOC-puxPEZ-02fGZ0c/view)

**Course:** ES333 – Microprocessors & Embedded Systems
