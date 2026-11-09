#pragma once
/*
 * Session recording to the SD card, one folder per session:
 *
 *   /sessions/<session id>/<type>_<device id>_<chip number>_<session id>.edf
 *   e.g. /sessions/1794186950/ecg_15B0B4_999012345678436_1794186950.edf
 *
 * with "unknown" as the chip number when the session was started without a tag.
 *
 *   ecg_...edf   EDF (not EDF+), 1 s records: ECG at 512 or 256 Hz, int16 (0.381 uV/LSB), uV.
 *   imu_...edf   EDF, records of `rate` samples: AccX/Y/Z (g, 0.488 mg/LSB) and GyrX/Y/Z (dps,
 *                70 mdps/LSB) at 104 or 416 Hz, MagX/Y/Z (uT, 0.15 uT/LSB) at 104 Hz. The
 *                record duration is the IMU's real one (its oscillator trim), e.g. 0.998503 s.
 *
 * Header: patient = "HorseID <chip number>", recording = "Device EQ-<id> Session <session>",
 * start date/time = session start (UTC; 01.01.85 00.00.00 if the RTC has no time). Sample 0 of
 * every file is the session start (each stream is padded back to it), so the files line up.
 * Samples are buffered in PSRAM and written every RECORDER_FLUSH_S seconds; each write also
 * updates the record count and syncs the file, so a battery cut loses at most that much.
 *
 * A new session (different horse) switches every file at the same moment, without stopping
 * the sensors.
 */
#include <stdint.h>
#include "esp_err.h"

#define RECORDER_FLUSH_S   30

typedef struct {
    uint32_t device_id;
    uint32_t session;
    uint32_t epoch;          /* Unix time now, 0 = unknown */
    const char *horse;       /* 15-digit chip number, NULL = unknown */
    uint16_t ecg_rate_hz;    /* 512 / 256, 0 = no ECG file */
    uint16_t imu_rate_hz;    /* 104 / 416, 0 = no IMU file */
} recorder_cfg_t;

/* Start recording this session (needs a mounted card). */
esp_err_t recorder_start(const recorder_cfg_t *cfg);

/* Close the current session's file at this sample and continue in a new session's file. */
void recorder_new_session(uint32_t session, uint32_t epoch, const char *horse);

/* Shutdown hook: write what is buffered and close the file (before the card is unmounted).
 * Does nothing if recording never started. */
void recorder_shutdown(void);
