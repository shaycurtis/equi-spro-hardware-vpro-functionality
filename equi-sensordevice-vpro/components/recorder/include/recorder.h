#pragma once
/*
 * Session recording to the SD card, one folder per session:
 *
 *   /sessions/<session id>/<type>_<device id>_<chip number>_<session id>.edf
 *   e.g. /sessions/1794186950000/ecg_15B0B4_999012345678436_1794186950000.edf
 *
 * The session ID is Unix time in ms, and "Unknown" is the chip number when the session was
 * started without a tag: the same naming as the VPro platform, whose uploader reads the
 * session from the folder and the other fields from the file name.
 *
 *   ecg_...edf   EDF (not EDF+), 1 s records: ECG at 256 (default) or 512 Hz, int16 (0.381 uV/LSB), mV.
 *   imu_...edf   EDF, records of `rate` samples: AccX/Y/Z (g, 0.488 mg/LSB) and GyrX/Y/Z (dps,
 *                70 mdps/LSB) at 104 or 416 Hz, MagX/Y/Z (uT, 0.15 uT/LSB) at 104 Hz. The
 *                record duration is the IMU's real one (its oscillator trim), e.g. 0.998503 s.
 *
 * Header: patient = "<chip number>" (or "Unknown"), recording = "Device EQ-<id> Session <session>",
 * start date/time = session start (UTC; 01.01.85 00.00.00 if the RTC has no time). Sample 0 of
 * every file is the session start (each stream is padded back to it), so the files line up.
 * Samples are buffered in PSRAM and written every RECORDER_FLUSH_S seconds; each write also
 * updates the record count and syncs the file, so a battery cut loses at most that much.
 *
 * A new session (different horse) switches every file at the same moment, without stopping
 * the sensors.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RECORDER_FLUSH_S   30

typedef struct {
    const char *device_id;   /* backend ID, e.g. "50787DFFFE15B0B4" (file names, EDF header) */
    uint64_t session;        /* Unix time in ms */
    uint64_t file_id;        /* last field of the file names; 0 = the session ID. A session
                              * resumed after a reset gives its restart time (ms), so the files
                              * written before the reset are not overwritten */
    uint32_t epoch;          /* Unix time now, 0 = unknown */
    const char *horse;       /* 15-digit chip number, NULL = unknown */
    uint16_t ecg_rate_hz;    /* 256 / 512, 0 = no ECG file */
    uint16_t imu_rate_hz;    /* 104 / 416, 0 = no IMU file */
} recorder_cfg_t;

/* Start recording this session (needs a mounted card). */
esp_err_t recorder_start(const recorder_cfg_t *cfg);

/* Close the current session's file at this sample and continue in a new session's file. */
void recorder_new_session(uint64_t session, uint32_t epoch, const char *horse);

/*
 * App-started recording (the app's ECG control service): one ECG-only file,
 *   /sessions/<session>/ecg_<device id>_<chip number>_<id>.edf
 * where id is the recording's start time in ms (the app's recording ID). Used when the session
 * is not already recording ECG; one at a time. Stopping saves it (keep) or deletes it.
 */
typedef struct {
    const char *device_id;   /* backend ID */
    uint64_t session;        /* folder: the current session, or the recording ID if none */
    uint64_t id;             /* recording ID = start time, ms */
    uint32_t epoch;          /* EDF start date/time (Unix s), 0 = unknown */
    const char *horse;       /* chip number, NULL = Unknown */
    uint16_t rate_hz;        /* 256 / 512 */
} recorder_clip_cfg_t;

/* ESP_ERR_INVALID_STATE: one is running, or the session file is recording the ECG;
 * ESP_ERR_NOT_FOUND: no SD card. */
esp_err_t recorder_clip_start(const recorder_clip_cfg_t *cfg);
/* Non-blocking: the file is finished (or deleted) by its writer task. */
void recorder_clip_stop(bool keep);
bool recorder_clip_active(void);

/* Shutdown hook: write what is buffered and close the file (before the card is unmounted).
 * Does nothing if recording never started. */
void recorder_shutdown(void);
