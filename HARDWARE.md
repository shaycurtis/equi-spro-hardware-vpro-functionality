# SPRO PCBA – hardware reference (from Altium SPRO.PcbDoc netlist + schematics)

The Altium files themselves (`SPRO_PCBA/`) are not in the repository; see README, "Not in this repository".

MCU: **ESP32-S3-WROOM-1-N16R8** (U1): 16 MB quad flash, 8 MB octal PSRAM (GPIO35-37 used internally, not routed).

## ESP32-S3 GPIO map

| GPIO | Module pin | Net | Connects to | Notes |
|---|---|---|---|---|
| EN | 3 | EN | SW4 (reset), Q7 auto-reset, 10k pull-up | |
| 0 | 27 | IO0 | SW3 (boot), Q6 auto-reset, 4.7k pull-up | strapping |
| 1 | 39 | LED | D1 DIN (WS2812B-2020) → D2 | 4.7k pull-down R5 |
| 2 | 38 | GPS_TX | LC29H pin 21 (RXD), TP13 | ESP TX -> GPS (verified) |
| 3 | 15 | SD_CS | microSD | strapping (JTAG source) |
| 4 | 4 | PWR_BTN | SW2 via D7 + divider | reads power button, active high |
| 5 | 5 | RFID_EN | Q3 → Q2 P-FET → RFID VIN | high = RFID powered |
| 6 | 6 | SCAN_BTN | SW1, 10k pull-up | active low |
| 7 | 7 | ECG_INT | MAX30001 INTB | |
| 8 | 12 | IMU_INT1 | LSM6DSOX INT1 | |
| 9 | 17 | SD_MISO | microSD | |
| 10 | 18 | SD_MOSI | microSD | |
| 11 | 19 | GPS_RX | LC29H pin 20 (TXD), TP14 | ESP RX <- GPS NMEA, 115200 baud (verified) |
| 12 | 20 | ESP_1WIRE | magnetic connector pin 4 (ESD U15), 4.7k pull-up | |
| 13 | 21 | VCC_CTRL | enables both 1.8 V LDOs (MAX30001 AVDD/OVDD) | |
| 14 | 22 | PWR_GD | charger REGN divider (100k/150k) | high = USB power present |
| 15 | 8 | ECG_INT2 | MAX30001 INT2B | |
| 16 | 9 | RTC_INT | PCF85063A INT | |
| 17 | 10 | MOTOR | Q8 → vibration motor J3 | |
| 18 | 11 | PMIC_EN | TPS22810 load-switch EN (diode-OR) | **drive high at boot to stay on battery** |
| 19 | 13 | SCL | I2C bus, 4.7k pull-up | also USB D- (native USB unusable) |
| 20 | 14 | SDA | I2C bus, 4.7k pull-up | also USB D+ |
| 21 | 23 | CHG_IND | BQ25303J STAT (100k pull-up) | |
| 38 | 31 | RFID_RX | JY-LD6900 TX, TP10 | UART RX only |
| 39 | 32 | SPI_MISO | MAX30001 | |
| 40 | 33 | SPI_MOSI | MAX30001 | |
| 41 | 34 | SPI_SCLK | MAX30001 | |
| 42 | 35 | SPI_ECG_SS | MAX30001 CSB | |
| 43 | 37 | TX0 | CP2102N RXD | console |
| 44 | 36 | RX0 | CP2102N TXD | console |
| 45 | 26 | – | NC | strapping |
| 46 | 16 | SD_SCLK | microSD | strapping |
| 47 | 24 | FG_ALRT | MAX17048 ALRT | |
| 48 | 25 | BUZZER | Q1 → CMT-8530S magnetic buzzer | |

## I2C bus (GPIO19 SCL / GPIO20 SDA)
- LSM6DSOX 6-axis IMU (U4). LIS2MDL magnetometer (U3) is on the IMU's **sensor hub** (SDX/SCX), not the main bus.
- SHTC3 humidity/temperature (U5)
- PCF85063A RTC (U6), 32.768 kHz crystal, coin-cell backup (J1)
- MAX17048 fuel gauge (U8)

## Other peripherals
- **MAX30001** ECG + bioimpedance/GSR AFE (U14), SPI, 32.768 kHz ECS-327MV clock, powered from 2x XC6221 1.8 V LDOs gated by VCC_CTRL. Electrodes on magnetic connector pins 1-3.
- **Quectel LC29H** GNSS (U7), UART, U.FL antenna, always powered from VDD33.
- **JY-LD6900** RFID reader module, UART TX → GPIO38, power switched by RFID_EN (from V_BATT).
- microSD (SPI mode), vibration motor, magnetic buzzer, 2x WS2812B-2020.
- No IR thermometer fitted (the MLX90632 sheet is in the schematics); an external 1-Wire sensor on the magnetic connector takes its place.
- Buttons: SW1 scan (active low, GPIO6), SW2 power (from V_BATT, read on GPIO4), SW3 boot, SW4 reset.

## 9-pin magnetic connector (MC1, from SPRO.PcbDoc)
| Pin | Net | Notes |
|---|---|---|
| 1 / 2 / 3 | C_ECG_N / C_ECG_VCM / C_ECG_P | ECG electrodes (MAX30001) |
| 4 | CON_1WIRE | USBLC6-2SC6 (U15) -> ESP_1WIRE (GPIO12), 4.7k pull-up R38 to VDD33 |
| 5 | CON_3V3 | VDD33 through F1 (PTC, 50 mA hold), C45 100 nF; off when the device is off |
| 6 / 7 | D1_P / D1_N | CP2102N programming / console |
| 8 | GND | |
| 9 | V_USB | 5 V charging input |

## Power
- USB 5 V on magnetic connector pin 9 → BQ25303J charger (500 mA set) → Li-ion via J2 (with NTC).
- TPS22810 load switch → AP2112K-3.3 → VDD33. Load switch EN = OR(PMIC_EN GPIO18, V_USB divider, power button).
- WS2812B-2020 run from 3.3 V (below their 3.7 V spec); keep brightness modest.
- Programming/console: CP2102N on magnetic connector D1_P/D1_N, with classic DTR/RTS auto-reset (Q6/Q7).

## Firmware config notes (helloworld sdkconfig, ESP-IDF 5.2.1)
- Secure Boot V2 (RSA), signing key used for these boards: `private/spro/fw_signing_key.pem` (helloworld sdkconfig defaulted to `vpro`).
- Flash encryption enabled, **development mode**, AES-128. Key is generated on-chip in eFuse; not in this folder.
- Partition table at 0x10000: nvs, phy_init, factory 3M, otadata, ota_0 3M, ota_1 3M.
- Console UART0 @115200, secondary console on USB-Serial-JTAG (GPIO19/20 are I2C on this board, so this should be disabled for the real firmware).
