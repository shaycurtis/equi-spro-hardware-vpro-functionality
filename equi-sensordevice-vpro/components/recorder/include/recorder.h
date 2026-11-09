#pragma once
/*
 * Session recording to the SD card, one folder per session:
 *
 *   /sessions/<session id>/<type>_<device id>_<chip number>_<session id>.edf
 *   e.g. /sessions/1794186950/ecg_15B0B4_999012345678436_1794186950.edf
 *
 * with "unknown" as the chip number when the session was started without a tag.
 *
 *   ecg_...edf   ECG, EDF (not EDF+), 1 s data records at 512 or 256 Hz, int16 samples
 *                (0.381 uV/LSB), physical unit uV.
 *   (imu_...edf comes later: accel + gyro at 104/416 Hz, magnetometer at 104 Hz.)
 *
 * Header: patient = "HorseID <chip number>", recording = "Device EQ-<id> Session <session>",
 * start date/time = session start (UTC; 01.01.85 00.00.00 if the RTC has no time). Samples are
 * buffered in PSRAM and written every RECORDER_FLUSH_S seconds; each write also updates the
 * record count and syncs the file, so a battery cut loses at most that much.
 *
 * A new session (different horse) switches to a new file at the exact sample, without
 * stopping the ECG stream.
 */
#include <stdint.h>
#include "esp_err.h"

#define RECORDER_FLUSH_S   30

/*
 * Start recording ECG at rate_hz (512 or 256) for this session. epoch = Unix time now
 * (0 = unknown), horse = 15-digit chip number or NULL (unknown). Needs a mounted card.
 */
esp_err_t recorder_start(uint32_t device_id, uint32_t session, uint32_t epoch, uint16_t rate_hz,
                         const char *horse);

/* Close the current session's file at this sample and continue in a new session's file. */
void recorder_new_session(uint32_t session, uint32_t epoch, const char *horse);

/* Shutdown hook: write what is buffered and close the file (before the card is unmounted).
 * Does nothing if recording never started. */
void recorder_shutdown(void);
