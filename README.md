# Smart Attendance System

A small embedded attendance system developed as part of the ES333 – Microprocessors & Embedded Systems course at IIT Gandhinagar.

The system uses an STM32 board for attendance processing and a VEGA board for user interaction and display.

## Features

- Fingerprint-based student identification
- Student enrollment and deletion
- Automatic attendance marking
- Prevention of duplicate attendance
- Manual attendance option
- Course-wise attendance management
- LCD and buzzer feedback
- UART communication between STM32 and VEGA
- Basic web interface for attendance-related information

## Hardware

- STM32F439ZI Nucleo Board
- AS608/R307 Fingerprint Sensor
- 4×4 Keypad
- VEGA Board
- LCD Display
- Buzzer

## Working

```text
Fingerprint / Keypad
        |
        v
   STM32F439ZI
 Attendance Logic
        |
       UART
        |
        v
    VEGA Board
        |
   LCD + Buzzer
```
## Links

- Drive link of presentation and implementation https://drive.google.com/drive/folders/15KcZjWl9rCqLaDINMjhR5M9ou2LYambG?usp=sharing

## Course

**ES333 – Microprocessors & Embedded Systems**  
**IIT Gandhinagar**
