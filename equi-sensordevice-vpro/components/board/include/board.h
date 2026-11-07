#pragma once
/*
 * SPRO PCBA pin map (ESP32-S3-WROOM-1-N16R8). Source: SPRO.PcbDoc netlist.
 * See HARDWARE.md at the repository root for the full description.
 */
#include "driver/gpio.h"

/* ---- Power / user interface ---- */
#define BOARD_PIN_PWR_BTN     GPIO_NUM_4   /* power button sense, active high, needs pull-down (diode-isolated) */
#define BOARD_PIN_SCAN_BTN    GPIO_NUM_6   /* scan button, active low, 10k external pull-up */
#define BOARD_PIN_PMIC_EN     GPIO_NUM_18  /* high = latch TPS22810 load switch (3.3 V rail) on */
#define BOARD_PIN_PWR_GD      GPIO_NUM_14  /* high = USB/charger input present (REGN divider) */
#define BOARD_PIN_CHG_IND     GPIO_NUM_21  /* BQ25303J STAT: low = charging, high = done/idle, 1 Hz blink = fault */
#define BOARD_PIN_FG_ALRT     GPIO_NUM_47  /* MAX17048 ALRT, active low */
#define BOARD_PIN_LED         GPIO_NUM_1   /* WS2812B-2020 chain data (D1 -> D2), 4.7k pull-down */
#define BOARD_LED_COUNT       2
#define BOARD_PIN_MOTOR       GPIO_NUM_17  /* vibration motor driver (Q8), high = on */
#define BOARD_PIN_BUZZER      GPIO_NUM_48  /* magnetic buzzer driver (Q1), high = on */

/* ---- Power-switched peripherals ---- */
#define BOARD_PIN_VCC_CTRL    GPIO_NUM_13  /* high = 1.8 V LDOs on (MAX30001 AVDD/OVDD) */
#define BOARD_PIN_RFID_EN     GPIO_NUM_5   /* high = RFID module powered (via Q3/Q2 from V_BATT) */

/* ---- I2C: LSM6DSOX (0x6B), SHTC3 (0x70), PCF85063A (0x51), MAX17048 (0x36) ---- */
#define BOARD_PIN_I2C_SCL     GPIO_NUM_19
#define BOARD_PIN_I2C_SDA     GPIO_NUM_20
#define BOARD_PIN_IMU_INT1    GPIO_NUM_8
#define BOARD_PIN_RTC_INT     GPIO_NUM_16

/* ---- MAX30001 ECG/BioZ (SPI) ---- */
#define BOARD_PIN_ECG_MISO    GPIO_NUM_39
#define BOARD_PIN_ECG_MOSI    GPIO_NUM_40
#define BOARD_PIN_ECG_SCLK    GPIO_NUM_41
#define BOARD_PIN_ECG_CS      GPIO_NUM_42
#define BOARD_PIN_ECG_INT     GPIO_NUM_7
#define BOARD_PIN_ECG_INT2    GPIO_NUM_15

/* ---- microSD (SPI mode) ---- */
#define BOARD_PIN_SD_CS       GPIO_NUM_3
#define BOARD_PIN_SD_SCLK     GPIO_NUM_46
#define BOARD_PIN_SD_MISO     GPIO_NUM_9
#define BOARD_PIN_SD_MOSI     GPIO_NUM_10

/* ---- UARTs ---- */
#define BOARD_PIN_GPS_RX_NET  GPIO_NUM_11  /* net "GPS_RX" -> LC29H pin 20 (verify direction) */
#define BOARD_PIN_GPS_TX_NET  GPIO_NUM_2   /* net "GPS_TX" -> LC29H pin 21 (verify direction) */
#define BOARD_PIN_RFID_RX     GPIO_NUM_38  /* JY-LD6900 TX -> ESP RX */
#define BOARD_PIN_1WIRE       GPIO_NUM_12  /* magnetic connector pin 4 */
