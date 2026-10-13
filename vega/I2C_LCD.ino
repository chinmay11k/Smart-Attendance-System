/*
 * VEGA ARIES v3 — display node
 * UART1 (0x10000200) ← STM32 PD8 via GP3/RX1
 *
 * SPEED FIX: Wire.setClock(400000) sets I2C to Fast Mode (400kHz).
 * This cuts lcdRow time from ~20ms to ~5ms, and full lcd_update
 * (two rows) from ~40ms to ~10ms. THEJAS32 I2C peripheral supports
 * Fast Mode — this is the single most impactful change possible.
 *
 * Architecture:
 *   Phase 1 every loop: drain ALL UART bytes into ring buffer (fast).
 *   Phase 2 every loop: process ALL queued commands (not just one).
 *
 * DO NOT add Serial.print() debug anywhere in loop() — it blocks
 * for 20-30ms per call and causes UART FIFO overflow and garbling.
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

TwoWire Wire(1);
LiquidCrystal_I2C lcd(0x27, 16, 2);  /* try 0x3F if screen stays blank */

#define BUZZER_PIN 8

/* ── UART1 registers (THEJAS32, UART1 = 0x10000200) ─────────── */
#define U1_BASE  0x10000200UL
#define U1_RBR   (*(volatile uint32_t*)(U1_BASE + 0x00))
#define U1_DLL   (*(volatile uint32_t*)(U1_BASE + 0x00))
#define U1_DLH   (*(volatile uint32_t*)(U1_BASE + 0x04))
#define U1_FCR   (*(volatile uint32_t*)(U1_BASE + 0x08))
#define U1_LCR   (*(volatile uint32_t*)(U1_BASE + 0x0C))
#define U1_LSR   (*(volatile uint32_t*)(U1_BASE + 0x14))
#define LSR_DR   0x01

static void uart1_init() {
    U1_LCR = 0x83;
    U1_DLL = 54; U1_DLH = 0;   /* 115200 @ 100MHz */
    U1_LCR = 0x03;
    U1_FCR = 0x07;              /* enable + clear FIFOs */
    U1_DLH = 0x00;              /* no interrupts */
    for (int i = 0; i < 32; i++) {
        if (U1_LSR & LSR_DR) { volatile uint32_t d = U1_RBR; (void)d; }
    }
}

static int  u1_avail() { return (U1_LSR & LSR_DR) ? 1 : 0; }
static char u1_getc()  { while (!(U1_LSR & LSR_DR)); return (char)(U1_RBR & 0xFF); }

/* ── Ring buffer ─────────────────────────────────────────────── */
#define CMD_SLOTS 8
#define CMD_WIDTH 24

static char cmdQ[CMD_SLOTS][CMD_WIDTH];
static int  qHead = 0, qTail = 0;

static void q_push(const char *s, int len) {
    int next = (qTail + 1) % CMD_SLOTS;
    if (next == qHead) return;
    int i;
    for (i = 0; i < CMD_WIDTH - 1 && i < len; i++) cmdQ[qTail][i] = s[i];
    cmdQ[qTail][i] = '\0';
    qTail = next;
}

static int q_pop(char *out) {
    if (qHead == qTail) return 0;
    int i;
    for (i = 0; i < CMD_WIDTH - 1 && cmdQ[qHead][i]; i++) out[i] = cmdQ[qHead][i];
    out[i] = '\0';
    qHead = (qHead + 1) % CMD_SLOTS;
    return 1;
}

/* ── LCD helpers ─────────────────────────────────────────────── */
static void lcdRow(int row, const char *text) {
    char padded[17];
    int i = 0;
    while (i < 16 && text[i]) { padded[i] = text[i]; i++; }
    while (i < 16)             { padded[i] = ' ';     i++; }
    padded[16] = '\0';
    lcd.setCursor(0, row);
    lcd.print(padded);
}

/* ── Command processor ───────────────────────────────────────── */
static void processCmd(const char *cmd, int len) {
    if (len >= 4 && cmd[0] == 'R' && cmd[2] == ':') {
        int row = cmd[1] - '0';
        if (row == 0 || row == 1) lcdRow(row, cmd + 3);
        return;
    }
    if (len >= 3 && cmd[0]=='B' && cmd[1]=='U' && cmd[2]=='Z') {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(200);
        digitalWrite(BUZZER_PIN, LOW);
        return;
    }
    /* ATT: and anything else — silently ignore */
}

/* ── Arduino setup ───────────────────────────────────────────── */
void setup() {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    uart1_init();

    Wire.begin();

    /*
     * KEY FIX: Fast Mode I2C at 400kHz.
     *
     * At 100kHz: lcdRow() ≈ 20ms  → lcd_update() ≈ 40ms lag
     * At 400kHz: lcdRow() ≈  5ms  → lcd_update() ≈ 10ms lag
     *
     * THEJAS32 I2C peripheral (same as I2C0/I2C1 in the SoC datasheet)
     * supports Standard (100kHz) and Fast (400kHz) modes.
     * PCF8574 I2C expander on the LCD module is also rated to 400kHz.
     * Both sides support it — use it.
     *
     * Wire.setClock() MUST be called after Wire.begin().
     */
    Wire.setClock(400000);

    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcdRow(0, "Attendance Sys");
    lcdRow(1, "IITGN Ready...");
}

/* ── Arduino loop ────────────────────────────────────────────── */
void loop() {
    /* Phase 1: drain UART FIFO into ring buffer — never blocks */
    static char rxbuf[CMD_WIDTH] = "";
    static int  rxlen = 0;

    while (u1_avail()) {
        char c = u1_getc();
        if (c == '\n' || c == '\r') {
            if (rxlen > 0) {
                q_push(rxbuf, rxlen);
                rxlen = 0;
            }
        } else if (rxlen < CMD_WIDTH - 1) {
            rxbuf[rxlen++] = c;
        } else {
            rxlen = 0; /* overlong command — discard */
        }
    }

    /*
     * Phase 2: process ALL queued commands per loop iteration.
     *
     * WHY "ALL" not "ONE":
     * At 400kHz, lcdRow takes ~5ms. An lcd_update sends R0 + R1.
     * If we process only ONE per loop, there is an extra loop()
     * overhead (~1µs) between R0 and R1. With "ALL", both rows
     * update back-to-back in ~10ms total — no visible gap between rows.
     *
     * Safety: STM32 sends at most 2 commands per lcd_update with
     * 5ms spacing. At 400kHz we process both in ~10ms and return
     * to Phase 1 to drain any new bytes. The FIFO never fills.
     */
    char cmd[CMD_WIDTH];
    while (q_pop(cmd)) {
        processCmd(cmd, (int)strlen(cmd));
    }
}
