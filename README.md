# Smart Attendance System

Clean GitHub-ready code package for the STM32 + VEGA + Python attendance system.

## Folder contents

- `stm32/Core/` - STM32F439ZI Core folder from the Cube project. The active STM32 application is `stm32/Core/Src/main.c`.
- `stm32/Source_Code.c` - same STM32 application kept as the original standalone source copy.
- `stm32/micro_krypad_test2.ioc` - STM32CubeMX project configuration.
- `vega/I2C_LCD.ino` - VEGA ARIES v3 display node code.
- `python_app/app.py` - Flask attendance portal and serial attendance logger.
- `CONNECTION_REPORT.md` - local comparison and communication trace.

## Run notes

STM32 sends UART data at 115200 baud on USART3. The VEGA node receives LCD/buzzer commands from the STM32 UART stream. The Python app also reads the same 115200 baud stream and records lines formatted as:

```text
ATT:<roll>:<course>
```

Example Python run:

```powershell
cd python_app
python app.py COM5
```

