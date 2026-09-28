/*
 * imu.c -- Minimal QMI8658 driver + tilt-gesture detector.
 *
 * Register map summary (from QST QMI8658 datasheet, rev 0.9):
 *   0x00 WHO_AM_I         -> 0x05
 *   0x01 REVISION_ID
 *   0x02 CTRL1            (I2C addr auto-inc, big/little endian bits)
 *   0x03 CTRL2            (accel: ODR + FS)
 *   0x04 CTRL3            (gyro:  ODR + FS)
 *   0x08 CTRL7            (enable A/G: 0x03 -> both on)
 *   0x33 TEMP_L / 0x34 TEMP_H
 *   0x35..0x3A AX_L..AZ_H (accel  little-endian, int16)
 *   0x3B..0x40 GX_L..GZ_H (gyro   little-endian, int16)
 *
 * Only what we need for a HoloCubic-style motion input is implemented.
 */
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "imu.h"
#include "app_config.h"

static const char *TAG = "imu";

#define QMI_WHO_AM_I   0x00
#define QMI_REVISION   0x01
#define QMI_CTRL1      0x02
#define QMI_CTRL2      0x03
#define QMI_CTRL3      0x04
#define QMI_CTRL5      0x06
#define QMI_CTRL7      0x08
#define QMI_CTRL8      0x09
#define QMI_RESET      0x60
#define QMI_RST_RESULT 0x4D   /* reads 0x80 when reset completes */
#define QMI_TEMP_L     0x33
#define QMI_AX_L       0x35
#define QMI_GX_L       0x3B

#define WHO_AM_I_VAL   0x05

/* Register values mirror SensorLib (lewisxhe) exactly -- the same library the
 * working WS_QMI8658.cpp Arduino sketch uses. Data registers are read and
 * parsed LITTLE-ENDIAN (lib does buffer[1]<<8|buffer[0] with CTRL1=0x40).
 *
 * Accel: 4g, 250Hz (ACC_RANGE_4G=1, ACC_ODR_250Hz=2), LPF_MODE_0 -> CTRL2=0x12
 * Gyro : 1024dps, 224.2Hz (GYR_RANGE_1024DPS=6, GYR_ODR_224_2Hz=5), no LPF
 *        -> CTRL3=0x65
 * CTRL5: accel LPF on, mode 0 (2.66% ODR) = bit0 + (0<<1) = 0x01
 * CTRL7: aEN|gEN = 0x03, async sample mode (bit7 clear). */
#define ACC_LSB_PER_G   (32768.0f / 4.0f)   /* 8192 LSB/g */
#define GYR_LSB_PER_DPS (32768.0f / 1024.0f)/* 32 LSB/dps */

static bool     s_present   = false;
static uint8_t  s_addr      = 0;
static bool     s_big_endian = false;  /* auto-detected at init */
static bool     s_tilt_active = false; /* gesture edge-trigger state */
static imu_sample_t s_last  = { 0 };

/* Gesture detector state (edge-triggered with debounce) */
static int64_t  s_last_event_us = 0;
static imu_event_t s_pending     = IMU_EV_NONE;

/* ---- Reference pose ("what counts as level") ----------------------------
 *
 * All tilt decisions are RELATIVE to a reference pose, so the device works
 * whether it lies flat, stands upright (pitch/roll permanently > threshold!)
 * or leans. Without a relative reference, a vertically-standing device never
 * satisfies the "return to level" re-arm condition and the detector fires
 * once, then dies forever.
 *
 * The reference USED to be latched once, ~200ms after init. That made the
 * whole gesture response depend on how the board happened to be held during
 * that tiny window, which is why "USB boot works, battery boot does not":
 *  - booting over USB the board lies flat and still on the desk -> reference
 *    ~= level, so every later left/right tilt is measured from level;
 *  - booting on battery the board is being held in the hand while it starts
 *    (and often still moving from plugging the pack in) -> the reference is
 *    captured at some arbitrary tilted pose.
 * A wrong reference shifts the trigger point by the boot offset: with |off|
 * = 15 deg one direction needs 37 deg of tilt while the other fires at 7
 * (the "very insensitive / wrong direction" symptom), and if |off| >= the
 * threshold the pose is permanently inside the tilt zone, so s_tilt_active
 * latches true on the first tick and never re-arms (the "cannot switch at
 * all" symptom). Re-arming required returning within 13.2 deg of the BOOT
 * pose, which a hand-held device may never do again.
 *
 * So the reference now TRACKS the resting pose instead of being frozen:
 *  - while the device is at rest (gyro small, |a| ~ 1g) and outside the tilt
 *    zone, the reference slowly follows the current pose (fast for the first
 *    ~2.5s so a bad boot capture is corrected immediately, then slow);
 *  - while the device is moving (a real gesture) the reference is frozen, so
 *    a gesture can never be absorbed into the reference and a release never
 *    fires a spurious opposite event;
 *  - if the pose rests far outside the zone for several seconds, the reference
 *    is pulled over gradually: that means the user's neutral pose moved (or
 *    the boot capture was wrong) and staying latched would kill the detector
 *    permanently.
 * The net effect: gestures are always measured from however the device is
 * being held NOW, independent of the pose at power-on. */
static float s_base_roll = 0.0f, s_base_pitch = 0.0f;
static bool  s_ref_valid = false;      /* was a trustworthy boot pose captured? */

static int64_t s_still_since_us = 0;   /* 0 = moving right now */
static int64_t s_zone_since_us  = 0;   /* 0 = outside the tilt zone */
static bool    s_recovered      = false;
static int64_t s_last_dbg_us    = 0;

#define BASE_SAMPLES   10
#define BASE_PERIOD_MS 20

/* Boot-pose capture: retry until the samples are consistent (a device that is
 * still being handled cannot produce a usable reference). */
#define REF_TRIES        6       /* <=6 bursts x 200ms = 1.2s worst case */
#define REF_MIN_SAMPLES  6
#define REF_GOOD_SPREAD  4.0f    /* deg of roll/pitch spread within one burst */

/* Rest / tracking thresholds */
#define REF_STILL_GYRO_DPS  12.0f             /* "properly at rest" (snap gate) */
#define REF_STUCK_GYRO_DPS  30.0f             /* "not being waved" (recovery gate) */
#define REF_STILL_MAG_MIN   0.75f             /* reject free-fall / shake: the */
#define REF_STILL_MAG_MAX   1.25f             /* tilt math is garbage while |a|!=1g */
#define REF_STILL_HOLD_US   (600 * 1000LL)    /* must be still this long */
#define REF_SETTLE_US       (2500 * 1000LL)   /* fast-tracking window after rest */
#define REF_STUCK_US        (3000 * 1000LL)   /* pull the reference over after this */
#define REF_ALPHA_SNAP      0.20f             /* per tick, initial settle */
#define REF_ALPHA_TRACK     0.004f            /* per tick, normal tracking (~10s) */
#define REF_ALPHA_RECOVER   0.01f             /* per tick, stuck-pose recovery */

/* ---------------- I2C helpers ---------------- */

/* I2C bus recovery for a stuck slave (I2C-bus spec rev 7.0 §3.1.16):
 * after a cold power-on the QMI8658 may have latched a bogus START and
 * holds SDA low waiting for clocks. Toggle SCL up to 9 times until SDA
 * is released high, then generate a STOP. This runs BEFORE the I2C
 * peripheral claims the pins. Without it the first probe fails on every
 * cold boot (works after flashing only because power never cycles). */
static void i2c_bus_recover(void)
{
    gpio_set_direction(PIN_IMU_SCL, GPIO_MODE_OUTPUT_OD);
    gpio_set_direction(PIN_IMU_SDA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_IMU_SCL, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_IMU_SDA, GPIO_PULLUP_ONLY);

    for (int i = 0; i < 9; i++) {
        if (gpio_get_level(PIN_IMU_SDA) == 1) break;   /* released */
        gpio_set_level(PIN_IMU_SCL, 0);
        esp_rom_delay_us(5);
        gpio_set_level(PIN_IMU_SCL, 1);
        esp_rom_delay_us(5);
    }
    /* STOP condition: SDA low -> high while SCL high */
    gpio_set_direction(PIN_IMU_SDA, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(PIN_IMU_SDA, 0);
    esp_rom_delay_us(5);
    gpio_set_level(PIN_IMU_SCL, 1);
    esp_rom_delay_us(5);
    gpio_set_level(PIN_IMU_SDA, 1);
    esp_rom_delay_us(5);
    /* release both lines; the i2c driver reconfigures them next */
    gpio_set_direction(PIN_IMU_SCL, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_IMU_SDA, GPIO_MODE_INPUT);
}

static esp_err_t i2c_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    /* Retry: on a weak battery rail during boot the bus can NACK once. */
    esp_err_t e;
    for (int i = 0; i < 3; i++) {
        e = i2c_master_write_to_device(IMU_I2C_PORT, s_addr, buf, 2, pdMS_TO_TICKS(50));
        if (e == ESP_OK) return e;
        esp_rom_delay_us(200);
    }
    return e;
}

static esp_err_t i2c_read_regs(uint8_t reg, uint8_t *dst, size_t n)
{
    esp_err_t e;
    for (int i = 0; i < 3; i++) {
        e = i2c_master_write_read_device(IMU_I2C_PORT, s_addr, &reg, 1, dst, n, pdMS_TO_TICKS(50));
        if (e == ESP_OK) return e;
        esp_rom_delay_us(200);
    }
    return e;
}

static bool try_addr(uint8_t addr)
{
    s_addr = addr;
    uint8_t id = 0;
    if (i2c_read_regs(QMI_WHO_AM_I, &id, 1) != ESP_OK) return false;
    ESP_LOGI(TAG, "probe 0x%02X: WHO_AM_I=0x%02X", addr, id);
    return id == WHO_AM_I_VAL;
}

/* ---------------- Register configuration ----------------
 * Same masked-write sequence as SensorLib / the working Arduino sketch, but
 * factored out so it can be RE-APPLIED: on a cold battery boot a write can be
 * ACKed yet not stick while the rail is still settling, which leaves the
 * sensor at a wrong ODR/range and shows up as a sluggish tilt response. */
static esp_err_t qmi_apply_config(void)
{
    uint8_t v = 0;

    if (i2c_write_reg(QMI_CTRL2, 0x00) != ESP_OK) return ESP_FAIL;  /* aEN off while cfg */
    if (i2c_read_regs(QMI_CTRL2, &v, 1) != ESP_OK) return ESP_FAIL;
    v = (uint8_t)((v & 0x8F) | (1 << 4));       /* ACC_RANGE_4G  */
    v = (uint8_t)((v & 0xF0) | 2);              /* ACC_ODR_250Hz */
    if (i2c_write_reg(QMI_CTRL2, v) != ESP_OK) return ESP_FAIL;

    if (i2c_read_regs(QMI_CTRL5, &v, 1) != ESP_OK) return ESP_FAIL;
    v = (uint8_t)((v & 0xF9) | (0 << 1));       /* LPF_MODE_0 */
    v |= 0x01;                                  /* accel LPF enable */
    if (i2c_write_reg(QMI_CTRL5, v) != ESP_OK) return ESP_FAIL;

    if (i2c_read_regs(QMI_CTRL3, &v, 1) != ESP_OK) return ESP_FAIL;
    v = (uint8_t)((v & 0x8F) | (6 << 4));       /* GYR_RANGE_1024DPS */
    v = (uint8_t)((v & 0xF0) | 5);              /* GYR_ODR_224_2Hz */
    if (i2c_write_reg(QMI_CTRL3, v) != ESP_OK) return ESP_FAIL;

    if (i2c_write_reg(QMI_CTRL7, 0x03) != ESP_OK) return ESP_FAIL;  /* aEN|gEN */
    return ESP_OK;
}

/* Read the config back and check the nibbles we care about. Bit 7 of CTRL2/3
 * is not touched by the masked writes, so only the documented fields are
 * compared. */
static bool qmi_config_ok(void)
{
    uint8_t c2 = 0, c3 = 0, c5 = 0, c7 = 0;
    if (i2c_read_regs(QMI_CTRL2, &c2, 1) != ESP_OK) return false;
    if (i2c_read_regs(QMI_CTRL3, &c3, 1) != ESP_OK) return false;
    if (i2c_read_regs(QMI_CTRL5, &c5, 1) != ESP_OK) return false;
    if (i2c_read_regs(QMI_CTRL7, &c7, 1) != ESP_OK) return false;
    ESP_LOGI(TAG, "config check: CTRL2=0x%02X CTRL3=0x%02X CTRL5=0x%02X CTRL7=0x%02X",
             c2, c3, c5, c7);
    return ((c2 & 0x70) == 0x10) && (c2 & 0x0F) == 2 &&   /* aFS=4g, aODR=250Hz */
           ((c3 & 0x70) == 0x60) && (c3 & 0x0F) == 5 &&   /* gFS=1024dps, 224Hz */
           ((c5 & 0x07) == 0x01) &&                       /* accel LPF on, mode 0 */
           ((c7 & 0x03) == 0x03);                         /* aEN|gEN */
}

/* Does the accelerometer actually stream live data? (A still device reads
 * ~1g; all-zero output means the chip's clock/POR is not done yet, or the
 * enable bits did not stick.) */
static bool qmi_accel_live(int *waited_ms)
{
    for (int i = 0; i < 100; i++) {         /* 100 x 10ms = 1s max */
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t ab[6];
        if (i2c_read_regs(QMI_AX_L, ab, 6) != ESP_OK) continue;
        int16_t ax = (int16_t)((ab[1] << 8) | ab[0]);
        int16_t ay = (int16_t)((ab[3] << 8) | ab[2]);
        int16_t az = (int16_t)((ab[5] << 8) | ab[4]);
        float mag = sqrtf((ax / ACC_LSB_PER_G) * (ax / ACC_LSB_PER_G) +
                          (ay / ACC_LSB_PER_G) * (ay / ACC_LSB_PER_G) +
                          (az / ACC_LSB_PER_G) * (az / ACC_LSB_PER_G));
        if (mag > 0.4f && mag < 2.5f) {
            if (waited_ms) *waited_ms = i * 10;
            ESP_LOGI(TAG, "accel live after %dms: %.2f/%.2f/%.2f g (raw %02X %02X %02X %02X %02X %02X)",
                     i * 10, ax / ACC_LSB_PER_G, ay / ACC_LSB_PER_G, az / ACC_LSB_PER_G,
                     ab[0], ab[1], ab[2], ab[3], ab[4], ab[5]);
            return true;
        }
    }
    if (waited_ms) *waited_ms = -1;
    return false;
}

/* ---------------- Reference-pose capture ----------------
 * Average a burst of samples that are consistent with "at rest". A device
 * that is being handled during boot (the normal case on a battery cold boot)
 * yields a moving pose; re-capture instead of latching garbage. Even if this
 * fails, the reference now tracks the resting pose at runtime, so the failure
 * is not fatal -- it only costs the first second of gesture response. */
static bool capture_reference(void)
{
    float best_r = 0.0f, best_p = 0.0f, best_spread = 1e9f;
    int   best_ok = 0;

    for (int t = 0; t < REF_TRIES; t++) {
        float sr = 0.0f, sp = 0.0f;
        float rmin = 1e9f, rmax = -1e9f, pmin = 1e9f, pmax = -1e9f;
        int ok = 0;

        for (int i = 0; i < BASE_SAMPLES; i++) {
            vTaskDelay(pdMS_TO_TICKS(BASE_PERIOD_MS));
            imu_sample_t s0;
            if (!imu_read(&s0)) continue;
            float mag = sqrtf(s0.ax*s0.ax + s0.ay*s0.ay + s0.az*s0.az);
            float g   = sqrtf(s0.gx*s0.gx + s0.gy*s0.gy + s0.gz*s0.gz);
            if (mag < REF_STILL_MAG_MIN || mag > REF_STILL_MAG_MAX) continue;
            if (g > REF_STILL_GYRO_DPS * 3.0f) continue;
            sr += s0.roll_deg; sp += s0.pitch_deg; ok++;
            if (s0.roll_deg  < rmin) rmin = s0.roll_deg;
            if (s0.roll_deg  > rmax) rmax = s0.roll_deg;
            if (s0.pitch_deg < pmin) pmin = s0.pitch_deg;
            if (s0.pitch_deg > pmax) pmax = s0.pitch_deg;
        }

        if (ok >= REF_MIN_SAMPLES) {
            float spread = fmaxf(rmax - rmin, pmax - pmin);
            if (spread < best_spread) {
                best_spread = spread; best_ok = ok;
                best_r = sr / ok; best_p = sp / ok;
            }
            if (spread <= REF_GOOD_SPREAD) break;   /* consistent -> use it */
            ESP_LOGW(TAG, "boot pose still moving (spread %.1f deg), re-capturing...", spread);
        } else {
            ESP_LOGW(TAG, "only %d/%d usable samples while capturing the reference pose",
                     ok, BASE_SAMPLES);
        }
    }

    if (best_ok >= REF_MIN_SAMPLES) {
        s_base_roll = best_r; s_base_pitch = best_p; s_ref_valid = true;
        ESP_LOGI(TAG, "gesture reference pose: roll=%.1f pitch=%.1f deg "
                      "(spread %.1f deg over %d samples)",
                 s_base_roll, s_base_pitch, best_spread, best_ok);
        return true;
    }

    s_ref_valid = false;
    ESP_LOGW(TAG, "no stable boot pose found; starting from 0/0 and tracking the "
                  "resting pose instead (keep the device still to speed this up)");
    return false;
}

/* ---------------- Public API ---------------- */
bool imu_init(void)
{
    /* Give the QMI8658 time to power up. On a cold power-on the chip's VDD
     * ramp + internal POR takes ~100ms (datasheet: 2ms typ, but the board's
     * LDO and I2C pull-up domain may be slower). Flashing via USB resets the
     * ESP32 *without* cutting the sensor's power, so the chip is already
     * alive when init runs -- which is why IMU worked right after flashing
     * but died after a true power cycle. Retry the probe for up to 1s. */
    bool found = false;
    for (int attempt = 0; attempt < 20; attempt++) {
        if (attempt == 0) {
            i2c_bus_recover();     /* unlock a bus latched during cold boot */
        }
        i2c_config_t cfg = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = PIN_IMU_SDA,
            .scl_io_num = PIN_IMU_SCL,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = IMU_I2C_HZ,
        };
        if (i2c_param_config(IMU_I2C_PORT, &cfg) != ESP_OK) {
            ESP_LOGE(TAG, "i2c param config failed"); return false;
        }
        if (i2c_driver_install(IMU_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
            ESP_LOGE(TAG, "i2c driver install failed"); return false;
        }

        if (try_addr(IMU_ADDR_PRI) || try_addr(IMU_ADDR_SEC)) {
            found = true;
            break;
        }

        /* chip not answering yet: tear the driver down and retry */
        ESP_LOGW(TAG, "QMI8658 probe attempt %d failed, waiting 50ms...", attempt + 1);
        i2c_driver_delete(IMU_I2C_PORT);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!found) {
        ESP_LOGW(TAG, "QMI8658 not found on I2C%d SDA=%d SCL=%d", IMU_I2C_PORT, PIN_IMU_SDA, PIN_IMU_SCL);
        return false;
    }

    /* Mirror SensorLib initImpl() exactly (the working Arduino sketch's path):
     * 1. soft reset (0xB0 -> reg 0x60), wait RST_RESULT(0x4D)==0x80 (max 15ms
     *    per datasheet; poll up to 500ms like the lib)
     * 2. reset() leaves CTRL1 with ADDR_AI(bit6)=1 -> keep, do NOT set BE
     * 3. CTRL8 = 0x80 (STATUS_INT.bit7 as CTRL9 handshake)
     * 4. CTRL2/CTRL3 via masked writes (range<<4, odr), CTRL5 accel LPF,
     * 5. CTRL7 = 0x03 (aEN|gEN), async mode. */
    uint8_t ctrl1_after_reset = 0;
    if (i2c_write_reg(QMI_RESET, 0xB0) != ESP_OK) goto io_err;
    {
        bool rst_ok = false;
        for (int i = 0; i < 50; i++) {           /* 50 x 10ms = 500ms */
            vTaskDelay(pdMS_TO_TICKS(10));
            uint8_t rv = 0;
            if (i2c_read_regs(QMI_RST_RESULT, &rv, 1) == ESP_OK && rv == 0x80) {
                rst_ok = true;
                break;
            }
        }
        if (!rst_ok) {
            ESP_LOGE(TAG, "QMI8658 soft-reset did not complete (RST_RESULT!=0x80)");
            goto io_err;
        }
    }
    if (i2c_read_regs(QMI_CTRL1, &ctrl1_after_reset, 1) != ESP_OK) goto io_err;
    ESP_LOGI(TAG, "CTRL1 after reset = 0x%02X (expect 0x40: ADDR_AI set by reset)", ctrl1_after_reset);
    if (!(ctrl1_after_reset & 0x40)) {
        /* be safe: set ADDR_AI bit if reset did not (lib sets it explicitly) */
        if (i2c_write_reg(QMI_CTRL1, ctrl1_after_reset | 0x40) != ESP_OK) goto io_err;
    }
    if (i2c_write_reg(QMI_CTRL8, 0x80) != ESP_OK) goto io_err;

    /* configAccelerometer(4G, 250Hz, LPF_MODE_0) + configGyroscope(1024DPS,
     * 224.2Hz) + enable both (CTRL7 bit0/bit1). */
    if (qmi_apply_config() != ESP_OK) goto io_err;

    /* Verify that the config actually stuck, and re-apply it if not. On a cold
     * battery boot a write can be ACKed yet corrupted (rail still settling);
     * a lost ODR nibble leaves the sensor running at a low default rate, which
     * shows up as a sluggish/insensitive tilt response. Previously this was
     * only logged, so a bad boot stayed bad for the whole session. */
    {
        bool cfg_ok = false;
        for (int pass = 0; pass < 3 && !cfg_ok; pass++) {
            if (qmi_config_ok()) {
                cfg_ok = true;
                break;
            }
            ESP_LOGW(TAG, "QMI8658 config did not stick, re-applying (pass %d)", pass + 1);
            if (qmi_apply_config() != ESP_OK) ESP_LOGW(TAG, "config re-write failed (pass %d)", pass + 1);
            vTaskDelay(pdMS_TO_TICKS(60));
        }
        if (!cfg_ok) {
            ESP_LOGE(TAG, "QMI8658 register config could not be verified -- "
                          "gesture response may be sluggish");
        }
    }

    /* Poll until the accelerometer actually streams live data. The log on a
     * battery cold boot showed all-zero accel at +80ms (internal clock still
     * starting). All-zero output is NOT "ready": repair the config and wait
     * again instead of booting with a sensor that never reports anything --
     * that is exactly the "gestures do not work on a battery boot" symptom,
     * and it used to be only a warning. A still device must read ~1g. */
    {
        int waited = -1;
        bool live = qmi_accel_live(&waited);
        if (!live) {
            ESP_LOGW(TAG, "accel not streaming after 1s -- re-applying config and waiting again");
            qmi_apply_config();
            live = qmi_accel_live(&waited);
        }
        if (!live) {
            ESP_LOGE(TAG, "QMI8658 accelerometer never reported live data -- IMU unusable");
            s_present = false;
            return false;
        }
        s_big_endian = false;   /* little-endian parse, verified correct on HW */
    }

    s_present = true;
    ESP_LOGI(TAG, "QMI8658 ready at 0x%02X", s_addr);

    /* Capture the boot pose as the initial reference pose. See the comment
     * block at the top of this file: this is only the starting point, the
     * reference tracks the resting pose from now on. */
    capture_reference();
    return true;

io_err:
    ESP_LOGE(TAG, "QMI8658 register config I/O error");
    s_present = false;
    return false;
}

bool imu_is_present(void)
{
    return s_present;
}

bool imu_read(imu_sample_t *out)
{
    if (!s_present) return false;

    /* Temperature (int16 signed, little-endian, 1/256 degC/LSB per datasheet) */
    uint8_t tbuf[2];
    if (i2c_read_regs(QMI_TEMP_L, tbuf, 2) != ESP_OK) return false;
    int16_t traw = (int16_t)((tbuf[1] << 8) | tbuf[0]);  /* little-endian */

    /* Read 6 bytes accel + 6 bytes gyro contiguously */
    uint8_t ab[6];
    if (i2c_read_regs(QMI_AX_L, ab, 6) != ESP_OK) return false;
    uint8_t gb[6];
    if (i2c_read_regs(QMI_GX_L, gb, 6) != ESP_OK) return false;

    /* LITTLE-endian, same as SensorLib getAccelRaw/getGyroRaw */
    int16_t ax = (int16_t)((ab[1] << 8) | ab[0]);
    int16_t ay = (int16_t)((ab[3] << 8) | ab[2]);
    int16_t az = (int16_t)((ab[5] << 8) | ab[4]);
    int16_t gx = (int16_t)((gb[1] << 8) | gb[0]);
    int16_t gy = (int16_t)((gb[3] << 8) | gb[2]);
    int16_t gz = (int16_t)((gb[5] << 8) | gb[4]);

    if (s_big_endian) {
        /* kept as safety path; init now detects CTRL1 BE bit */
        ax = (int16_t)((ab[0] << 8) | ab[1]);
        ay = (int16_t)((ab[2] << 8) | ab[3]);
        az = (int16_t)((ab[4] << 8) | ab[5]);
        gx = (int16_t)((gb[0] << 8) | gb[1]);
        gy = (int16_t)((gb[2] << 8) | gb[3]);
        gz = (int16_t)((gb[4] << 8) | gb[5]);
    }

    out->ax = ax / ACC_LSB_PER_G;
    out->ay = ay / ACC_LSB_PER_G;
    out->az = az / ACC_LSB_PER_G;
    out->gx = gx / GYR_LSB_PER_DPS;
    out->gy = gy / GYR_LSB_PER_DPS;
    out->gz = gz / GYR_LSB_PER_DPS;
    out->tempC = traw / 256.0f;

    /* Pitch/roll from gravity vector (screen flat = pitch=roll=0) */
    out->roll_deg  = atan2f(out->ax, out->az) * (180.0f / (float)M_PI);
    out->pitch_deg = atan2f(-out->ay, sqrtf(out->ax*out->ax + out->az*out->az))
                     * (180.0f / (float)M_PI);
    s_last = *out;
    return true;
}

/* Simple magnitude-based tap detector using accel jerk */
static float s_prev_mag = 1.0f;

/* Telemetry: 1 line every 2s (set to 0 to silence). Keep it on while the
 * gesture response is being diagnosed -- on a misbehaving device it shows
 * immediately whether the accelerometer is streaming, what the reference pose
 * is, how far the current pose is from it and whether the detector is armed.
 * NOTE: the serial port needs USB power, but this state is latched at boot and
 * survives plugging USB in, so a bad battery boot can still be inspected by
 * plugging the cable in afterwards and reading this line. */
#define IMU_DEBUG_TELEMETRY 1

void imu_tick(void)
{
    if (!s_present) return;
    imu_sample_t s;
    if (!imu_read(&s)) return;

    int64_t now = esp_timer_get_time();
    float mag  = sqrtf(s.ax*s.ax + s.ay*s.ay + s.az*s.az);
    float gmag = sqrtf(s.gx*s.gx + s.gy*s.gy + s.gz*s.gz);

    /* ---- at rest? (drives the reference tracking below) ---- */
    bool still = (gmag < REF_STILL_GYRO_DPS) &&
                 (mag > REF_STILL_MAG_MIN) && (mag < REF_STILL_MAG_MAX);
    if (still) {
        if (s_still_since_us == 0) s_still_since_us = now;
    } else {
        s_still_since_us = 0;
    }
    int64_t still_for = s_still_since_us ? (now - s_still_since_us) : 0;
    bool still_ok = still_for > REF_STILL_HOLD_US;

    /* ---- tilt, RELATIVE to the (self-tracking) reference pose ---- */
    imu_event_t ev = IMU_EV_NONE;
    bool in_zone = false;
    imu_event_t zone_ev = IMU_EV_NONE;
    float droll  = s.roll_deg  - s_base_roll;
    float dpitch = s.pitch_deg - s_base_pitch;

    if (fabsf(droll) > APP_TILT_DEG && fabsf(droll) > fabsf(dpitch)) {
        in_zone = true;
        zone_ev = (droll > 0) ? IMU_EV_RIGHT : IMU_EV_LEFT;
    } else if (fabsf(dpitch) > APP_TILT_DEG) {
        in_zone = true;
        zone_ev = (dpitch > 0) ? IMU_EV_DOWN : IMU_EV_UP;
    }
    if (in_zone) {
        if (s_zone_since_us == 0) s_zone_since_us = now;
    } else {
        s_zone_since_us = 0;
        s_recovered = false;
    }

    if (in_zone && !s_tilt_active) {
        ev = zone_ev;                           /* rising edge */
        s_tilt_active = true;
        ESP_LOGI(TAG, "tilt ev=%d ref=%.1f/%.1f pose=%.1f/%.1f d=%.1f/%.1f gyro=%.0f/%.0f/%.0f",
                 (int)ev, s_base_roll, s_base_pitch, s.roll_deg, s.pitch_deg,
                 droll, dpitch, s.gx, s.gy, s.gz);
    } else if (!in_zone &&
               fabsf(droll)  < APP_TILT_DEG * 0.6f &&
               fabsf(dpitch) < APP_TILT_DEG * 0.6f) {
        s_tilt_active = false;                  /* re-armed */
    }

    /* ---- reference-pose tracking (rationale at the top of this file) ----
     * Frozen while inside the tilt zone, so a gesture is never absorbed into
     * the reference and a release never fires a spurious opposite event. */
    float alpha = 0.0f;
    bool  mag_ok = (mag > REF_STILL_MAG_MIN) && (mag < REF_STILL_MAG_MAX);
    if (!in_zone && mag_ok) {
        /* Always on: the reference follows however the device is being held,
         * so a wrong boot pose self-corrects within seconds instead of
         * lasting the whole session. Time constant ~10s, which means a
         * half-second gesture overshoot moves it by only a few percent and it
         * decays straight back afterwards. Right after coming to rest the
         * tracking is briefly fast, so the correction happens immediately. */
        alpha = (still_ok && still_for < REF_SETTLE_US) ? REF_ALPHA_SNAP
                                                        : REF_ALPHA_TRACK;
    } else if (in_zone && mag_ok && gmag < REF_STUCK_GYRO_DPS &&
               s_zone_since_us && (now - s_zone_since_us) > REF_STUCK_US) {
        /* Resting far outside the zone for seconds: the neutral pose moved
         * (or the boot pose was captured wrong). Pull the reference over
         * instead of staying latched and dead forever. */
        alpha = REF_ALPHA_RECOVER;
        if (!s_recovered) {
            s_recovered = true;
            ESP_LOGW(TAG, "rest pose %.1f/%.1f deg from reference -- re-centring reference",
                     droll, dpitch);
        }
    }
    if (alpha > 0.0f) {
        s_base_roll  += alpha * (s.roll_deg  - s_base_roll);
        s_base_pitch += alpha * (s.pitch_deg - s_base_pitch);
    }

    /* ---- tap: sudden magnitude jump beyond 1.6g ---- */
    if (fabsf(mag - s_prev_mag) > 0.6f && mag > 1.6f) ev = IMU_EV_TAP;
    s_prev_mag = mag;

    if (ev != IMU_EV_NONE &&
        (now - s_last_event_us) > (APP_TILT_DEBOUNCE * 1000LL)) {
        s_pending = ev;
        s_last_event_us = now;
    }

#if IMU_DEBUG_TELEMETRY
    if (now - s_last_dbg_us > 2000000LL) {
        s_last_dbg_us = now;
        ESP_LOGI(TAG, "imu ref=%.1f/%.1f(v%d) pose=%.1f/%.1f d=%.1f/%.1f "
                      "mag=%.2f gyro=%.0f zone=%d armed=%d still=%d",
                 s_base_roll, s_base_pitch, (int)s_ref_valid,
                 s.roll_deg, s.pitch_deg, droll, dpitch,
                 mag, gmag, (int)in_zone, (int)!s_tilt_active, (int)still);
    }
#endif
}

imu_event_t imu_poll_event(void)
{
    imu_event_t ev = s_pending;
    s_pending = IMU_EV_NONE;
    return ev;
}
