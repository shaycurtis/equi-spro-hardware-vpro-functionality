#pragma once
/*
 * VPro-compatible live services for the Equimetrics app (same UUIDs and byte layouts as the
 * VPro platform's BLE_Local), so the existing app can use this board unchanged:
 *
 *   Raw ECG      18a6e117-39f1-4bc2-975c-ff2015f7d6ef
 *     ECG data   c2dc2fc5-4100-4421-82bc-2751eb1ea34a  read/notify
 *                notifications of up to 10 samples (fewer if the ATT MTU is small), 256 sps,
 *                16 bytes each, little-endian (the platform's ECGData_t, unpacked):
 *                  0  int64  timestamp   Unix time, ms
 *                  8  int32  ecg_raw     18-bit signed AFE value (gain 80: 0.0954 uV/LSB)
 *                 12  uint8  status      bit0 breath (0: not detected yet), bit1 RR change (0),
 *                                        bits 2-3 leads: 3 = beats detected, 0 = none (no
 *                                        lead-off detector yet: "no beats" stands in for it)
 *                 13  uint8  (padding)
 *                 14  uint16 rr_interval ms, on the sample where a beat was reported, else 0
 *   Respiration  46741235-185f-442e-8c3d-b18480147692 / 950af83d-... read/notify (not sent yet)
 *   Heart Rate   0x180D / 0x2A37  read/notify, standard format (flags 0, bpm uint8), every 1 s
 *   ECG control  0df7cc6e-5089-11ee-be56-0242ac120002
 *     command        0df7cf16-...  write   "start" / "stop" / "discard"
 *     duration       0df7d394-...  write   seconds, big-endian, added to the total
 *     recording ID   0df7d4f2-...  read/write/notify  uint64 big-endian
 *     status         f2c136be-7802-11ee-b962-0242ac120002  read/notify "none" / "running" /
 *                                  "stopped" / "discarded" / "finished" / "error: ..."
 *     total duration f2c13984-...  read/notify  uint32 big-endian, seconds
 *
 * Every characteristic needs an encrypted link (as VPro): the first access makes the phone
 * pair; we request MITM, so a phone with a keyboard asks for TELEMETRY_BLE_PASSKEY. Bonds
 * are kept in NVS.
 *
 * Live ECG flows while a client is subscribed to ECG data. "start" runs a timed recording
 * session for the app (status, total duration, auto-finish); writing it to the SD card is
 * not done yet.
 */
#include <stdbool.h>
#include <stdint.h>

int live_service_register(void);
void live_service_on_disconnect(uint16_t conn);
void live_service_on_subscribe(uint16_t conn, uint16_t attr, bool notify_on);
