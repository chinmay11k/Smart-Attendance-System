/* Attendance System — STM32F439ZI Nucleo-144
 *
 * UART3  PD8 TX 115200  →  VEGA GP3/RX1  (LCD commands + ATT: events)
 * USART6 PC6 TX PC7 RX 57600  →  AS608/R307 fingerprint sensor
 * GPIOD  PD0-3 rows out, PD4-7 cols in pull-up  →  4×4 keypad
 *
 * ── LCD TIMING ──────────────────────────────────────────────────
 *   VEGA now runs I2C at 400kHz → each row write takes ~5ms.
 *   STM32 waits 10ms after each row send: 5ms I2C + 5ms margin.
 *   lcd_update() = 2 × (1.7ms UART tx + 10ms delay) = ~24ms total.
 *   No FIFO overflow, no dropped commands, instant-feeling response.
 * ────────────────────────────────────────────────────────────────
 */

#include "main.h"
#include "string.h"
#include "stdio.h"

#define MAX_STU  20
#define ROLL_LEN  8

typedef struct {
    char    roll[ROLL_LEN + 1];
    uint8_t fp_id;
    uint8_t present;
} Student;

typedef struct {
    char    name[8];
    char    code[8];
    Student stu[MAX_STU];
    int     cnt;
} Course;

static Course  courses[2];
static int     cur_course = 0;
static uint8_t next_fp_id = 1;

typedef enum {
    ST_COURSE_SELECT,
    ST_ATTEND_SCANNING,
    ST_ATTEND_SUCCESS,
    ST_ATTEND_FAIL,
    ST_ATTEND_ALREADY,
    ST_ADMIN_MENU,
    ST_ADMIN_NEW_ROLL,
    ST_ADMIN_NEW_FP1,
    ST_ADMIN_NEW_REMOVE,
    ST_ADMIN_NEW_FP2,
    ST_ADMIN_NEW_DONE,
    ST_ADMIN_NEW_FAIL,
    ST_ADMIN_DEL_ROLL,
    ST_ADMIN_DEL_CONFIRM,
    ST_ADMIN_DEL_DONE,
    ST_ADMIN_MAN_ROLL,
    ST_ADMIN_MAN_DONE,
    ST_ADMIN_RST_CONFIRM,
    ST_ADMIN_RST_DONE,
} AppState;

static AppState  app_state;
static uint32_t  state_timer;
static char      input_buf[ROLL_LEN + 2];
static int       input_len;
static int       admin_idx = 0;
static char      enroll_roll[ROLL_LEN + 2];
static Student  *found_stu = NULL;

static const char *admin_items[5] = {
    "1.New Student  ",
    "2.Delete Stu   ",
    "3.Manual Attend",
    "4.Reset Today  ",
    "5.Exit Admin   "
};

ETH_TxPacketConfig TxConfig;
ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT];
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT];
ETH_HandleTypeDef  heth;
UART_HandleTypeDef huart3;
UART_HandleTypeDef huart6;
PCD_HandleTypeDef  hpcd_USB_OTG_FS;

static uint8_t  fp_available = 0;
static uint32_t last_fp_poll = 0;
static uint32_t last_refresh = 0;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ETH_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_USB_OTG_FS_PCD_Init(void);
static void FP_UART_Init(void);

/* ── Fingerprint AS608/R307 ──────────────────────────────────── */
#define FP_OK       0x00
#define FP_NOFINGER 0x02

static uint8_t fp_read_resp(uint8_t *extra, uint8_t emax, uint32_t tms)
{
    uint8_t  b[32];
    uint16_t n = 0;
    uint32_t start = HAL_GetTick();
    uint8_t  byte;
    while (HAL_GetTick() - start < tms) {
        if (HAL_UART_Receive(&huart6, &byte, 1, 10) != HAL_OK) continue;
        b[n++] = byte;
        if (n >= 2 && (b[0] != 0xEF || b[1] != 0x01)) { n = 0; continue; }
        if (n >= 12) break;
    }
    if (n < 12) return 0xFF;
    if (extra)
        for (uint8_t i = 0; i < emax && (10 + i) < n; i++)
            extra[i] = b[10 + i];
    return b[9];
}

static uint8_t fp_tx(uint8_t *cmd, uint16_t len,
                     uint8_t *extra, uint8_t emax, uint32_t tms)
{
    uint8_t dummy;
    while (HAL_UART_Receive(&huart6, &dummy, 1, 5) == HAL_OK);
    HAL_UART_Transmit(&huart6, cmd, len, 200);
    return fp_read_resp(extra, emax, tms);
}

/* GetImage — 300ms timeout. Returns ~50ms on no-finger (live sensor). */
static uint8_t fp_get_image_fast(void)
{
    uint8_t c[12] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,
                     0x01,0x00,0x03,0x01,0x00,0x05};
    return fp_tx(c, 12, NULL, 0, 300);
}

static uint8_t fp_img2tz(uint8_t slot)
{
    uint8_t c[13] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,
                     0x01,0x00,0x04,0x02,slot,0,0};
    uint16_t cs = 0x01+0x00+0x04+0x02+slot;
    c[11] = cs>>8; c[12] = cs&0xFF;
    return fp_tx(c, 13, NULL, 0, 2000);
}

static uint8_t fp_create_model(void)
{
    uint8_t c[12] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,
                     0x01,0x00,0x03,0x05,0x00,0x09};
    return fp_tx(c, 12, NULL, 0, 2000);
}

static uint8_t fp_store(uint16_t id)
{
    uint8_t c[15] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,
                     0x01,0x00,0x06,0x06,0x01,
                     (uint8_t)(id>>8),(uint8_t)(id&0xFF),0,0};
    uint16_t cs = 0x01+0x00+0x06+0x06+0x01+(id>>8)+(id&0xFF);
    c[13] = cs>>8; c[14] = cs&0xFF;
    return fp_tx(c, 15, NULL, 0, 2000);
}

static uint8_t fp_delete_id(uint16_t id)
{
    uint8_t c[16] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,
                     0x01,0x00,0x07,0x0C,
                     (uint8_t)(id>>8),(uint8_t)(id&0xFF),
                     0x00,0x01,0,0};
    uint16_t cs = 0x01+0x00+0x07+0x0C+(id>>8)+(id&0xFF)+0x00+0x01;
    c[14] = cs>>8; c[15] = cs&0xFF;
    return fp_tx(c, 16, NULL, 0, 2000);
}

/*
 * fp_search checksum:
 *   PID(01)+LenH(00)+LenL(08)+Instr(04)+BufID(01)
 *   +StartH(00)+StartL(00)+CntH(00)+CntL(7F) = 0x8F
 */
static uint8_t fp_search(uint16_t *fid)
{
    uint8_t c[17] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,
                     0x01,0x00,0x08,0x04,
                     0x01,0x00,0x00,0x00,0x7F,
                     0x00,0x00};
    uint16_t cs = 0x01+0x00+0x08+0x04+0x01+0x00+0x00+0x00+0x7F;
    c[15] = cs>>8; c[16] = cs&0xFF;
    uint8_t ex[4] = {0};
    uint8_t r = fp_tx(c, 17, ex, 4, 2000);
    if (r == FP_OK && fid) *fid = ((uint16_t)ex[0]<<8)|ex[1];
    return r;
}

static uint8_t fp_verify_pwd(void)
{
    uint8_t c[16] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,
                     0x01,0x00,0x07,0x13,
                     0x00,0x00,0x00,0x00,0x00,0x1B};
    return fp_tx(c, 16, NULL, 0, 500);  /* 500ms: fast even when sensor absent */
}

static uint8_t fp_get_count(void)
{
    uint8_t c[12] = {0xEF,0x01,0xFF,0xFF,0xFF,0xFF,
                     0x01,0x00,0x03,0x1D,0x00,0x21};
    uint8_t ex[2] = {0};
    uint8_t r = fp_tx(c, 12, ex, 2, 500);
    return (r == FP_OK) ? ((ex[0]<<8)|ex[1]) : 0;
}

/* ── Student helpers ─────────────────────────────────────────── */
static Student *find_by_fp(uint16_t fid, int *ci_out)
{
    for (int c = 0; c < 2; c++)
        for (int s = 0; s < courses[c].cnt; s++)
            if (courses[c].stu[s].fp_id == (uint8_t)fid) {
                if (ci_out) *ci_out = c;
                return &courses[c].stu[s];
            }
    return NULL;
}

static Student *find_by_roll(int ci, const char *roll)
{
    for (int s = 0; s < courses[ci].cnt; s++)
        if (strcmp(courses[ci].stu[s].roll, roll) == 0)
            return &courses[ci].stu[s];
    return NULL;
}

static int present_count(int ci)
{
    int n = 0;
    for (int s = 0; s < courses[ci].cnt; s++)
        if (courses[ci].stu[s].present) n++;
    return n;
}

static void delete_student(int ci, Student *stu)
{
    Course *c = &courses[ci];
    int idx = (int)(stu - c->stu);
    for (int i = idx; i < c->cnt-1; i++) c->stu[i] = c->stu[i+1];
    memset(&c->stu[c->cnt-1], 0, sizeof(Student));
    c->cnt--;
}

/* ── LCD helpers ─────────────────────────────────────────────── */
static void uart_raw(const char *s)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)s, (uint16_t)strlen(s), 200);
}

/*
 * Send one LCD row command and wait 10ms.
 *
 * WHY 10ms:
 *   VEGA now uses Wire.setClock(400000) → I2C at 400kHz.
 *   Per-row I2C write time: ~5ms (4× faster than 100kHz's ~20ms).
 *   10ms delay = 5ms I2C + 5ms margin before next command.
 *   HAL_UART_Transmit is fully blocking (returns after all bytes sent),
 *   so the 1.74ms UART transmission is already over before delay starts.
 *   No FIFO overflow possible: VEGA finishes I2C + drains FIFO at ~5ms,
 *   new command arrives at ~11.74ms → 6ms of clear headroom.
 *
 *   lcd_update() total: 2 × (1.74ms tx + 10ms delay) = ~24ms.
 */
static void lcd_row(int row, const char *text)
{
    char buf[22];
    snprintf(buf, sizeof(buf), "R%d:%-16.16s\n", row, text ? text : "");
    uart_raw(buf);
    HAL_Delay(2);   /* was 5ms — safe at 400kHz I2C since VEGA drains fast */
}

static void lcd_update(const char *r0, const char *r1)
{
    lcd_row(0, r0);
    lcd_row(1, r1);
}

/* R1-only for digit echo — saves 10ms per keypress vs full lcd_update */
static void lcd_r1(const char *r1) { lcd_row(1, r1); }

static void buz(void) { uart_raw("BUZ\n"); }

static void send_att(const char *roll, const char *code)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "ATT:%s:%s\n", roll, code);
    uart_raw(buf);
}

/* ── Display helpers ─────────────────────────────────────────── */
static void make_roll_row(char *out)
{
    for (int i = 0; i < ROLL_LEN; i++)
        out[i] = (i < input_len) ? input_buf[i] : '_';
    out[ROLL_LEN] = '\0';
}

static void show_roll_input(const char *label)
{
    char l1[ROLL_LEN+2]; make_roll_row(l1);
    lcd_update(label, l1);
}

static void show_roll_digit_fast(void)
{
    char l1[ROLL_LEN+2]; make_roll_row(l1);
    lcd_r1(l1);  /* R1 only: 10ms vs 20ms for full update */
}

static void show_admin_menu(void)
{
    char r0[20];
    snprintf(r0, sizeof(r0), "ADMIN %-10s", courses[cur_course].name);
    lcd_update(r0, admin_items[admin_idx]);
}

static void show_scan_screen(void)
{
    char r0[20], r1[20];
    snprintf(r0, sizeof(r0), "%-6s P:%02d/%02d  ",
             courses[cur_course].name,
             present_count(cur_course),
             courses[cur_course].cnt);
    snprintf(r1, sizeof(r1), "%s",
             fp_available ? "Place Finger    " : "FP Offline A=Adm");
    lcd_update(r0, r1);
}

static void refresh_if_idle(void)
{
    if (HAL_GetTick() - last_refresh < 10000) return;
    last_refresh = HAL_GetTick();
    switch (app_state) {
        case ST_ATTEND_SCANNING: show_scan_screen(); break;
        case ST_COURSE_SELECT:
            lcd_update("Select Course:  ", "1=ES333  2=ES215"); break;
        case ST_ADMIN_MENU: show_admin_menu(); break;
        default: break;
    }
}

/* ── FSM enter_state ─────────────────────────────────────────── */
static void enter_state(AppState s)
{
    app_state    = s;
    state_timer  = HAL_GetTick();
    last_refresh = HAL_GetTick();

    if (s == ST_ADMIN_NEW_ROLL || s == ST_ADMIN_DEL_ROLL || s == ST_ADMIN_MAN_ROLL) {
        input_len = 0;
        memset(input_buf, 0, sizeof(input_buf));
    }

    switch (s) {
        case ST_COURSE_SELECT:
            lcd_update("Select Course:  ", "1=ES333  2=ES215");
            break;
        case ST_ATTEND_SCANNING:
            show_scan_screen();
            break;
        case ST_ATTEND_SUCCESS:
        case ST_ATTEND_FAIL:
        case ST_ATTEND_ALREADY:
            break;  /* LCD already set by caller before setting app_state */
        case ST_ADMIN_MENU:
            admin_idx = 0;
            show_admin_menu();
            break;
        case ST_ADMIN_NEW_ROLL:
            lcd_update("New Roll No:    ", "________        ");
            break;
        case ST_ADMIN_NEW_FP1:
            lcd_update("Place Finger 1  ", "*=Cancel        ");
            break;
        case ST_ADMIN_NEW_REMOVE:
            lcd_update("Remove Finger!  ", "Then place again");
            break;
        case ST_ADMIN_NEW_FP2:
            lcd_update("Place Finger 2  ", "Same finger...  ");
            break;
        case ST_ADMIN_NEW_DONE:
            lcd_update("Registered OK!  ", enroll_roll);
            break;
        case ST_ADMIN_NEW_FAIL:
            lcd_update("FP Mismatch!    ", "D=Retry *=Cancel");
            break;
        case ST_ADMIN_DEL_ROLL:
            lcd_update("Del Roll No:    ", "________        ");
            break;
        case ST_ADMIN_DEL_CONFIRM: {
            char r1[20];
            snprintf(r1, sizeof(r1), "%-8s D=Yes", found_stu ? found_stu->roll : "");
            lcd_update("Delete? *=No    ", r1);
            break;
        }
        case ST_ADMIN_DEL_DONE:
            lcd_update("Deleted!        ", "                ");
            break;
        case ST_ADMIN_MAN_ROLL:
            lcd_update("Manual Roll:    ", "________        ");
            break;
        case ST_ADMIN_MAN_DONE:
            lcd_update("Manual Present! ", found_stu ? found_stu->roll : "Done            ");
            break;
        case ST_ADMIN_RST_CONFIRM:
            lcd_update("Reset Today?    ", "D=Yes  *=Cancel ");
            break;
        case ST_ADMIN_RST_DONE:
            lcd_update("Reset Done!     ", "All Absent Now  ");
            break;
    }
}

/* ── handle_key ──────────────────────────────────────────────── */
static int handle_roll_digit(char key)
{
    if (key >= '0' && key <= '9' && input_len < ROLL_LEN) {
        input_buf[input_len++] = key;
        input_buf[input_len]   = '\0';
        return 1;
    }
    if (key == '#' && input_len > 0) {
        input_buf[--input_len] = '\0';
        return 1;
    }
    return 0;
}

static void handle_key(char key)
{
    switch (app_state) {

    case ST_COURSE_SELECT:
        if      (key == '1') { cur_course = 0; enter_state(ST_ATTEND_SCANNING); }
        else if (key == '2') { cur_course = 1; enter_state(ST_ATTEND_SCANNING); }
        else if (key == 'A')   enter_state(ST_ADMIN_MENU);
        break;

    case ST_ATTEND_SCANNING:
        if (key == 'A') enter_state(ST_ADMIN_MENU);
        if (key == '#') enter_state(ST_COURSE_SELECT);
        break;

    case ST_ATTEND_SUCCESS:
    case ST_ATTEND_FAIL:
    case ST_ATTEND_ALREADY:
        break;  /* timed auto-return */

    case ST_ADMIN_MENU:
        if (key == 'B') { admin_idx = (admin_idx+4)%5; show_admin_menu(); }
        if (key == 'C') { admin_idx = (admin_idx+1)%5; show_admin_menu(); }
        if (key == 'D') {
            switch (admin_idx) {
                case 0: enter_state(ST_ADMIN_NEW_ROLL);    break;
                case 1: enter_state(ST_ADMIN_DEL_ROLL);    break;
                case 2: enter_state(ST_ADMIN_MAN_ROLL);    break;
                case 3: enter_state(ST_ADMIN_RST_CONFIRM); break;
                case 4: enter_state(ST_ATTEND_SCANNING);   break;
            }
        }
        if (key == '*') enter_state(ST_ATTEND_SCANNING);
        break;

    case ST_ADMIN_NEW_ROLL:
        if (handle_roll_digit(key)) {
            show_roll_digit_fast();
        } else if (key == 'D') {
            if (input_len != ROLL_LEN) {
                lcd_update("Need 8 digits!  ", input_buf);
                HAL_Delay(1500);
                show_roll_input("New Roll No:    ");
                break;
            }
            if (courses[cur_course].cnt >= MAX_STU) {
                lcd_update("Course Full!    ", "Max 20 students ");
                HAL_Delay(2000); enter_state(ST_ADMIN_MENU);
            } else if (find_by_roll(cur_course, input_buf)) {
                lcd_update("Roll Exists!    ", input_buf);
                HAL_Delay(2000); enter_state(ST_ADMIN_NEW_ROLL);
            } else {
                memcpy(enroll_roll, input_buf, ROLL_LEN+1);
                if (fp_available) {
                    enter_state(ST_ADMIN_NEW_FP1);
                } else {
                    Course *c = &courses[cur_course];
                    memcpy(c->stu[c->cnt].roll, enroll_roll, ROLL_LEN+1);
                    c->stu[c->cnt].fp_id   = 0;
                    c->stu[c->cnt].present = 0;
                    c->cnt++;
                    enter_state(ST_ADMIN_NEW_DONE);
                }
            }
        } else if (key == '*') enter_state(ST_ADMIN_MENU);
        break;

    case ST_ADMIN_NEW_FP1:
    case ST_ADMIN_NEW_REMOVE:
    case ST_ADMIN_NEW_FP2:
        if (key == '*') enter_state(ST_ADMIN_MENU);
        break;

    case ST_ADMIN_NEW_FAIL:
        if (key == 'D') enter_state(ST_ADMIN_NEW_FP1);
        if (key == '*') enter_state(ST_ADMIN_MENU);
        break;

    case ST_ADMIN_NEW_DONE: break;

    case ST_ADMIN_DEL_ROLL:
        if (handle_roll_digit(key)) {
            show_roll_digit_fast();
        } else if (key == 'D') {
            if (input_len != ROLL_LEN) break;
            found_stu = find_by_roll(cur_course, input_buf);
            if (!found_stu) {
                lcd_update("Not Found!      ", input_buf);
                HAL_Delay(2000); enter_state(ST_ADMIN_DEL_ROLL);
            } else {
                enter_state(ST_ADMIN_DEL_CONFIRM);
            }
        } else if (key == '*') enter_state(ST_ADMIN_MENU);
        break;

    case ST_ADMIN_DEL_CONFIRM:
        if (key == 'D' && found_stu) {
            if (fp_available && found_stu->fp_id > 0)
                fp_delete_id(found_stu->fp_id);
            delete_student(cur_course, found_stu);
            found_stu = NULL;
            enter_state(ST_ADMIN_DEL_DONE);
        }
        if (key == '*') { found_stu = NULL; enter_state(ST_ADMIN_MENU); }
        break;

    case ST_ADMIN_DEL_DONE: break;

    case ST_ADMIN_MAN_ROLL:
        if (handle_roll_digit(key)) {
            show_roll_digit_fast();
        } else if (key == 'D') {
            if (input_len != ROLL_LEN) break;
            found_stu = find_by_roll(cur_course, input_buf);
            if (!found_stu) {
                lcd_update("Not Found!      ", input_buf);
                HAL_Delay(2000); enter_state(ST_ADMIN_MAN_ROLL);
            } else if (found_stu->present) {
                lcd_update("Already Present!", found_stu->roll);
                found_stu = NULL;
                HAL_Delay(2000); enter_state(ST_ADMIN_MENU);
            } else {
                found_stu->present = 1;
                send_att(found_stu->roll, courses[cur_course].code);
                enter_state(ST_ADMIN_MAN_DONE);
            }
        } else if (key == '*') enter_state(ST_ADMIN_MENU);
        break;

    case ST_ADMIN_MAN_DONE: break;

    case ST_ADMIN_RST_CONFIRM:
        if (key == 'D') {
            for (int s = 0; s < courses[cur_course].cnt; s++)
                courses[cur_course].stu[s].present = 0;
            enter_state(ST_ADMIN_RST_DONE);
        }
        if (key == '*') enter_state(ST_ADMIN_MENU);
        break;

    case ST_ADMIN_RST_DONE: break;
    }
}

/* ── FP polling ──────────────────────────────────────────────── */
static void fp_poll_attend(void)
{
    if (!fp_available) return;

    uint8_t r = fp_get_image_fast();
    if (r == FP_NOFINGER || r == 0xFF) return;
    if (r != FP_OK) return;

    r = fp_img2tz(1);
    if (r != FP_OK) {
        lcd_update("Bad Image!      ", "Try Again       ");
        app_state = ST_ATTEND_FAIL; state_timer = HAL_GetTick(); return;
    }

    uint16_t fid = 0;
    r = fp_search(&fid);
    if (r != FP_OK) {
        lcd_update("Not Matched!    ", "Try Again/A=Adm ");
        buz();
        app_state = ST_ATTEND_FAIL; state_timer = HAL_GetTick(); return;
    }

    int ci;
    Student *stu = find_by_fp(fid, &ci);
    if (!stu) {
        lcd_update("Unknown FP!     ", "Enroll via Admin");
        app_state = ST_ATTEND_FAIL; state_timer = HAL_GetTick(); return;
    }
    if (ci != cur_course) {
        char r1[20];
        snprintf(r1, sizeof(r1), "Use %s mode", courses[ci].name);
        lcd_update("Wrong Course!   ", r1);
        app_state = ST_ATTEND_FAIL; state_timer = HAL_GetTick(); return;
    }
    if (stu->present) {
        lcd_update("Already Present!", stu->roll);
        app_state = ST_ATTEND_ALREADY; state_timer = HAL_GetTick(); return;
    }

    stu->present = 1;
    send_att(stu->roll, courses[cur_course].code);

    char r0[20], r1[20];
    snprintf(r0, sizeof(r0), "%-16s", stu->roll);
    snprintf(r1, sizeof(r1), "Present!%-8s", courses[cur_course].name);
    lcd_update(r0, r1);
    app_state = ST_ATTEND_SUCCESS; state_timer = HAL_GetTick();
}

static void fp_poll_enroll1(void)
{
    uint8_t r = fp_get_image_fast();
    if (r == FP_NOFINGER || r == 0xFF) return;
    if (r != FP_OK) return;
    if (fp_img2tz(1) == FP_OK) enter_state(ST_ADMIN_NEW_REMOVE);
}

static void fp_poll_remove(void)
{
    uint8_t r = fp_get_image_fast();
    if (r == FP_NOFINGER) enter_state(ST_ADMIN_NEW_FP2);
}

static void fp_poll_enroll2(void)
{
    uint8_t r = fp_get_image_fast();
    if (r == FP_NOFINGER || r == 0xFF) return;
    if (r != FP_OK) return;
    if (fp_img2tz(2) != FP_OK) return;
    if (fp_create_model() != FP_OK) { enter_state(ST_ADMIN_NEW_FAIL); return; }
    if (next_fp_id > 127) {
        lcd_update("FP Memory Full! ", "                ");
        HAL_Delay(2000); enter_state(ST_ADMIN_MENU); return;
    }
    if (fp_store(next_fp_id) != FP_OK) {
        lcd_update("Store Failed!   ", "                ");
        HAL_Delay(2000); enter_state(ST_ADMIN_MENU); return;
    }
    Course *c = &courses[cur_course];
    memcpy(c->stu[c->cnt].roll, enroll_roll, ROLL_LEN+1);
    c->stu[c->cnt].fp_id   = next_fp_id;
    c->stu[c->cnt].present = 0;
    c->cnt++;
    next_fp_id++;
    enter_state(ST_ADMIN_NEW_DONE);
}

/* ── Timeouts ────────────────────────────────────────────────── */
static void check_timeouts(void)
{
    uint32_t el = HAL_GetTick() - state_timer;
    switch (app_state) {
        case ST_ATTEND_SUCCESS:
            if (el > 2500) enter_state(ST_ATTEND_SCANNING); break;
        case ST_ATTEND_FAIL:
            if (el > 2000) enter_state(ST_ATTEND_SCANNING); break;
        case ST_ATTEND_ALREADY:
            if (el > 2000) enter_state(ST_ATTEND_SCANNING); break;
        case ST_ADMIN_NEW_DONE:
            if (el > 2500) enter_state(ST_ADMIN_MENU); break;
        case ST_ADMIN_DEL_DONE:
            if (el > 2000) enter_state(ST_ADMIN_MENU); break;
        case ST_ADMIN_MAN_DONE:
            if (el > 2000) { found_stu = NULL; enter_state(ST_ADMIN_MENU); } break;
        case ST_ADMIN_RST_DONE:
            if (el > 2000) enter_state(ST_ADMIN_MENU); break;
        default: break;
    }
}

/* ── Keypad scan ─────────────────────────────────────────────── */
static char keypad_scan(void)
{
    static const char km[4][4] = {
        {'1','2','3','A'},
        {'4','5','6','B'},
        {'7','8','9','C'},
        {'*','0','#','D'}
    };
    static const uint16_t rp[4] = {GPIO_PIN_0,GPIO_PIN_1,GPIO_PIN_2,GPIO_PIN_3};
    static const uint16_t cp[4] = {GPIO_PIN_4,GPIO_PIN_5,GPIO_PIN_6,GPIO_PIN_7};

    for (int r = 0; r < 4; r++) {
        HAL_GPIO_WritePin(GPIOD,
            GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3, GPIO_PIN_SET);
        HAL_GPIO_WritePin(GPIOD, rp[r], GPIO_PIN_RESET);
        HAL_Delay(2);
        for (int c = 0; c < 4; c++) {
            if (HAL_GPIO_ReadPin(GPIOD, cp[c]) == GPIO_PIN_RESET) {
                HAL_Delay(25);
                if (HAL_GPIO_ReadPin(GPIOD, cp[c]) != GPIO_PIN_RESET) break;
                uint32_t t = HAL_GetTick() + 500;
                while (HAL_GPIO_ReadPin(GPIOD, cp[c]) == GPIO_PIN_RESET)
                    if (HAL_GetTick() > t) break;
                HAL_Delay(25);
                return km[r][c];
            }
        }
    }
    return 0;
}

/* ── Init ────────────────────────────────────────────────────── */
static void init_data(void)
{
    memset(courses, 0, sizeof(courses));
    strcpy(courses[0].name, "ES-333"); strcpy(courses[0].code, "ES333");
    strcpy(courses[1].name, "ES-215"); strcpy(courses[1].code, "ES215");
    next_fp_id = 1; cur_course = 0; admin_idx = 0; found_stu = NULL;
}

static void FP_UART_Init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_USART6_CLK_ENABLE();
    g.Pin = GPIO_PIN_6|GPIO_PIN_7; g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP; g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF8_USART6;
    HAL_GPIO_Init(GPIOC, &g);

    huart6.Instance = USART6; huart6.Init.BaudRate = 57600;
    huart6.Init.WordLength = UART_WORDLENGTH_8B;
    huart6.Init.StopBits = UART_STOPBITS_1; huart6.Init.Parity = UART_PARITY_NONE;
    huart6.Init.Mode = UART_MODE_TX_RX; huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart6.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart6);
}

/* ── main ────────────────────────────────────────────────────── */
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_ETH_Init();
    MX_USART3_UART_Init();
    MX_USB_OTG_FS_PCD_Init();
    FP_UART_Init();

    init_data();

    /* Wait 2s for VEGA to boot, run Wire.begin(), Wire.setClock(),
     * lcd.init(), and enter loop() before first command arrives.    */
    HAL_Delay(2000);
    lcd_update("Attendance Sys  ", "IITGN  Init...  ");

    fp_available = (fp_verify_pwd() == FP_OK) ? 1 : 0;
    if (fp_available) {
        uint8_t cnt = fp_get_count();
        if (cnt > 0) next_fp_id = cnt + 1;
        lcd_update("Attendance Sys  ", "FP Sensor: OK   ");
    } else {
        lcd_update("Attendance Sys  ", "FP: Not Found   ");
    }
    HAL_Delay(1200);

    last_refresh = HAL_GetTick();
    enter_state(ST_COURSE_SELECT);

    char last_key = 0;
    while (1) {
        check_timeouts();
        refresh_if_idle();

        char key = keypad_scan();
        if (key != 0) {
            if (key != last_key) {
                last_refresh = HAL_GetTick();
                handle_key(key);
            }
            last_key = key;
        } else {
            last_key = 0;
        }

        switch (app_state) {
            case ST_ATTEND_SCANNING:
                if (HAL_GetTick() - last_fp_poll > 200) {
                    fp_poll_attend();
                    last_fp_poll = HAL_GetTick();
                }
                break;
            case ST_ADMIN_NEW_FP1:    fp_poll_enroll1(); break;
            case ST_ADMIN_NEW_REMOVE: fp_poll_remove();  break;
            case ST_ADMIN_NEW_FP2:    fp_poll_enroll2(); break;
            default: HAL_Delay(10); break;
        }
    }
}

/* ── Peripheral inits ────────────────────────────────────────── */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef o = {0}; RCC_ClkInitTypeDef c = {0};
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    o.OscillatorType = RCC_OSCILLATORTYPE_HSE; o.HSEState = RCC_HSE_BYPASS;
    o.PLL.PLLState = RCC_PLL_ON; o.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    o.PLL.PLLM = 4; o.PLL.PLLN = 168;
    o.PLL.PLLP = RCC_PLLP_DIV2; o.PLL.PLLQ = 7;
    if (HAL_RCC_OscConfig(&o) != HAL_OK) Error_Handler();
    c.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    c.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    c.APB1CLKDivider = RCC_HCLK_DIV4;
    c.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&c, FLASH_LATENCY_5) != HAL_OK) Error_Handler();
}
static void MX_ETH_Init(void)
{
    static uint8_t mac[6] = {0x00,0x80,0xE1,0x00,0x00,0x00};
    heth.Instance = ETH; heth.Init.MACAddr = mac;
    heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
    heth.Init.TxDesc = DMATxDscrTab; heth.Init.RxDesc = DMARxDscrTab;
    heth.Init.RxBuffLen = 1524;
    if (HAL_ETH_Init(&heth) != HAL_OK) Error_Handler();
    memset(&TxConfig, 0, sizeof(ETH_TxPacketConfig));
    TxConfig.Attributes   = ETH_TX_PACKETS_FEATURES_CSUM|ETH_TX_PACKETS_FEATURES_CRCPAD;
    TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
    TxConfig.CRCPadCtrl   = ETH_CRC_PAD_INSERT;
}
static void MX_USART3_UART_Init(void)
{
    huart3.Instance = USART3; huart3.Init.BaudRate = 115200;
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1; huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX; huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart3) != HAL_OK) Error_Handler();
}
static void MX_USB_OTG_FS_PCD_Init(void)
{
    hpcd_USB_OTG_FS.Instance = USB_OTG_FS;
    hpcd_USB_OTG_FS.Init.dev_endpoints = 4; hpcd_USB_OTG_FS.Init.speed = PCD_SPEED_FULL;
    hpcd_USB_OTG_FS.Init.dma_enable = DISABLE; hpcd_USB_OTG_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
    hpcd_USB_OTG_FS.Init.Sof_enable = ENABLE; hpcd_USB_OTG_FS.Init.low_power_enable = DISABLE;
    hpcd_USB_OTG_FS.Init.lpm_enable = DISABLE; hpcd_USB_OTG_FS.Init.vbus_sensing_enable = ENABLE;
    hpcd_USB_OTG_FS.Init.use_dedicated_ep1 = DISABLE;
    if (HAL_PCD_Init(&hpcd_USB_OTG_FS) != HAL_OK) Error_Handler();
}
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE(); __HAL_RCC_GPIOG_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB, LD1_Pin|LD3_Pin|LD2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(USB_PowerSwitchOn_GPIO_Port, USB_PowerSwitchOn_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3, GPIO_PIN_SET);
    g.Pin = USER_Btn_Pin; g.Mode = GPIO_MODE_IT_RISING; g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(USER_Btn_GPIO_Port, &g);
    g.Pin = LD1_Pin|LD3_Pin|LD2_Pin; g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = USB_PowerSwitchOn_Pin;
    HAL_GPIO_Init(USB_PowerSwitchOn_GPIO_Port, &g);
    g.Pin = USB_OverCurrent_Pin; g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(USB_OverCurrent_GPIO_Port, &g);
    g.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3;
    g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &g);
    g.Pin = GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7;
    g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOD, &g);
}
void Error_Handler(void) { __disable_irq(); while (1) {} }
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif
