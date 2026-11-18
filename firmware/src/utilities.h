#pragma once

// LilyGO T3-S3 V1.2 with SX1262
// T3_S3_V1_2_SX1262 is set via platformio build flags
#ifndef USING_SX1262
#define USING_SX1262
#endif

#define UNUSED_PIN   (0)

// I2C (OLED)
#define I2C_SDA      18
#define I2C_SCL      17
#define OLED_RST     UNUSED_PIN

// LoRa SPI (SX1262)
#define RADIO_SCLK_PIN  5
#define RADIO_MISO_PIN  3
#define RADIO_MOSI_PIN  6
#define RADIO_CS_PIN    7
#define RADIO_RST_PIN   8
#define RADIO_DIO1_PIN  33
#define RADIO_BUSY_PIN  34

// SD Card
#define SDCARD_MOSI  11
#define SDCARD_MISO  2
#define SDCARD_SCLK  14
#define SDCARD_CS    13

// Onboard
#define BOARD_LED    37
#define LED_ON       HIGH
#define BUTTON_PIN   0
#define ADC_PIN      1

// Battery ADC
#define BAT_ADC_PULLUP_RES   (100000.0)
#define BAT_ADC_PULLDOWN_RES (100000.0)
#define BAT_MAX_VOLTAGE      (4.2)
#define BAT_VOL_COMPENSATION (0.0)

#define HAS_SDCARD
#define HAS_DISPLAY
#define BOARD_VARIANT_NAME   "T3-S3-V1.X"
#define DISPLAY_MODEL        U8G2_SSD1306_128X64_NONAME_F_HW_I2C
#define DISPLAY_MODEL_SSD_LIB SSD1306Wire
#define RADIO_TYPE_STR       "SX1262"
