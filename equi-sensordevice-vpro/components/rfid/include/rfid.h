#pragma once
/*
 * JY-LD6900 134.2 kHz / 125 kHz reader (ISO 11784/11785 FDX-B and EM4100 tags).
 *
 * Hardware: module TX -> GPIO38 only (receive-only, so no commands), 9600 8N1 ASCII
 * ("free port" protocol, AutoSend mode: each read is sent twice). Power from V_BATT
 * switched by RFID_EN (GPIO5) via Q3/Q2; up to 250 mA, so it is only on while scanning.
 * Note: the module is specified for 3.6-5.5 V; a low battery may make scans unreliable.
 *
 * Frame: '$' 'F'|'E' <15 digits> <BCC 2 hex> '#'. A read is accepted when its BCC
 * verifies (or, failing that, the same ID arrives twice).
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t country;     /* ISO 3166 numeric or 900-999 manufacturer code */
    uint64_t national;    /* 38 bits */
    bool fdx_b;           /* false = EM4100 (country = customer code) */
} rfid_tag_t;

/* tag == NULL: timed out or cancelled. Called from the RFID task. */
typedef void (*rfid_cb_t)(const rfid_tag_t *tag, void *ctx);

esp_err_t rfid_scan_start(int timeout_s, rfid_cb_t cb, void *ctx);
void rfid_scan_cancel(void);
bool rfid_scanning(void);
void rfid_shutdown(void);

/* ISO 11784 packing: country (10 bits) << 38 | national (38 bits). 0 = no tag. */
uint64_t rfid_tag_pack(const rfid_tag_t *t);

/* 15-digit form, e.g. "985141000123456". */
void rfid_tag_str(const rfid_tag_t *t, char out[16]);
