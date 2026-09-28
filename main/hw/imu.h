/*
 * imu.h -- QMI8658 6-axis IMU on I2C.
 * Provides raw accel + gyro, plus derived tilt gestures:
 *   TILT_LEFT / TILT_RIGHT / TILT_UP / TILT_DOWN / TAP
 * used by the App manager for HoloCubic-style page switching.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float ax, ay, az;    /* g (approx +/- 4g range) */
    float gx, gy, gz;    /* deg/s */
    float tempC;         /* die temperature */
    float pitch_deg;     /* -180..+180 */
    float roll_deg;
} imu_sample_t;

typedef enum {
    IMU_EV_NONE = 0,
    IMU_EV_LEFT,      /* tilt to the left  -> previous page */
    IMU_EV_RIGHT,     /* tilt to the right -> next page     */
    IMU_EV_UP,        /* tilt forward */
    IMU_EV_DOWN,      /* tilt backward */
    IMU_EV_TAP,       /* short shake / tap */
} imu_event_t;

/** Probe & initialize the QMI8658 on I2C. Returns false if the chip is missing.
 *  Continues booting the rest of the firmware even when false. */
bool imu_init(void);

/** True if imu_init() found the chip. */
bool imu_is_present(void);

/** Read the latest sample. Returns false on I/O error. */
bool imu_read(imu_sample_t *out);

/** Consume one pending tilt event (returns IMU_EV_NONE if none). */
imu_event_t imu_poll_event(void);

/** Blocking task-friendly: run one detection tick. Call every 30-50 ms.
 *  Tilt events are measured relative to a reference pose that TRACKS the
 *  resting pose at runtime (see imu.c), so the response does not depend on
 *  how the device happened to be held while it booted. */
void imu_tick(void);

#ifdef __cplusplus
}
#endif
