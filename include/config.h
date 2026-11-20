#pragma once

#ifndef FW_VERSION
#define FW_VERSION 1
#endif

#define UPSTREAM_SSID "Network - 2.4G"
#define UPSTREAM_PASSWORD "qwert4321"
#define AP_SSID "smarttrap-gw"
#define AP_PASSWORD "smarttrap-gw"

#define API_BASE "http://192.168.0.4:8000"
#define API_KEY "CHANGE_ME"

#define LORA_MOSI 27
#define LORA_MISO 19
#define LORA_SCK 5
#define LORA_NSS 18
#define LORA_RST 14
#define LORA_DIO0 26
#define LORA_FREQUENCY 915E6
#define LORA_SF 7
#define LORA_BANDWIDTH 125E3
#define LORA_CODING_RATE 5
#define LORA_SYNC_WORD 0x12
#define LORA_TX_POWER 17

#define POLL_INTERVAL_MS 60000
