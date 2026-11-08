#pragma once
/*
 * Equimetrics ECG GATT service, protocol v1.
 *
 *   Service  7a5e0001-3c2b-4a8e-9f61-6b1e4c0d5e01
 *   Control  7a5e0002-...  write
 *       01 <seconds u8> <flags u8>   start capture (flags bit0 = internal test signal)
 *       02                           stop capture
 *       10 <epoch u32 LE>            set RTC / system time (UTC)
 *   Data     7a5e0003-...  notify (subscribe before START; needs ATT MTU >= 233)
 *       E0 <rate u16> <uV/LSB f32> <seconds u8> <flags u8> <block u8>   capture started
 *       E1 <first_index u32> <n u8> <n x int16 LE>                       samples
 *       EF <status u8> <total u32> <overflows u16>                       capture finished
 *   Info     7a5e0004-...  read
 *       <version u8> <rate u16> <uV/LSB f32> <block u8> <max_seconds u8>
 *
 * Application ATT errors on Control: 0x80 not subscribed, 0x81 busy, 0x82 MTU too small.
 */
#include <stdbool.h>
#include <stdint.h>

#define ECG_PROTOCOL_VERSION   1

#define ECG_CMD_START          0x01
#define ECG_CMD_STOP           0x02
#define ECG_CMD_PAIR        0x20   /* no payload: mark this central as a trusted gateway */
#define ECG_CMD_SET_TIME       0x10
#define ECG_FLAG_TEST_SIGNAL   0x01

#define ECG_PKT_START          0xE0
#define ECG_PKT_DATA           0xE1
#define ECG_PKT_END            0xEF
#define ECG_PKT_HDR            6

#define ECG_ATT_ERR_NOT_SUBSCRIBED  0x80
#define ECG_ATT_ERR_BUSY            0x81
#define ECG_ATT_ERR_MTU_TOO_SMALL   0x82

int ecg_service_register(void);
void ecg_service_on_connect(uint16_t conn);
void ecg_service_on_disconnect(uint16_t conn);
void ecg_service_on_subscribe(uint16_t conn, uint16_t attr, bool notify_on);
