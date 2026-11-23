#include <HTTPClient.h>
#include <LoRa.h>
#include <SPI.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include <cstring>

#include "config.h"
#include "protocol.h"

static bool loraReady = false;
static bool spiReady = false;
static uint32_t lastLoraTry = 0;
static bool apReady = false;
static int publishedTrapVersion = -1;
static uint32_t lastPoll = 0;
static uint32_t lastWifiTry = 0;
static char syncCodes[16][65];
static int syncCount = 0;

struct Upstream {
  bool https;
  String host;
  uint16_t port;
};

static Upstream upstream;
static WiFiServer proxyServer(80);

static bool beginHttp(HTTPClient &http, WiFiClient &plain, WiFiClientSecure &secure, const String &url) {
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (url.startsWith("https://")) {
    secure.setInsecure();
    if (!http.begin(secure, url)) {
      return false;
    }
  } else if (!http.begin(plain, url)) {
    return false;
  }
  http.addHeader("X-API-Key", API_KEY);
  return true;
}

static int jsonInt(const String &body, const char *key) {
  String token = String("\"") + key + "\"";
  int index = body.indexOf(token);
  if (index < 0) {
    return -1;
  }
  index = body.indexOf(':', index);
  if (index < 0) {
    return -1;
  }
  return body.substring(index + 1).toInt();
}

static int fetchVersion(const char *target) {
  String url = String(API_BASE) + "/api/v1/ingest/firmware/" + target + "?current=0";
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttp(http, plain, secure, url)) {
    return -1;
  }
  int code = http.GET();
  int version = -1;
  if (code == 200) {
    version = jsonInt(http.getString(), "version");
  }
  http.end();
  return version;
}

static bool applyOta(const char *target) {
  String url = String(API_BASE) + "/api/v1/ingest/firmware/" + target + "/bin?current=" + String(FW_VERSION);
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttp(http, plain, secure, url)) {
    return false;
  }
  http.setTimeout(65000);
  const char *headerKeys[] = {"x-MD5"};
  http.collectHeaders(headerKeys, 1);
  int code = http.GET();
  if (code == 304 || code == 204) {
    http.end();
    return false;
  }
  if (code != 200) {
    Serial.printf("OTA %s: HTTP %d\n", target, code);
    http.end();
    return false;
  }

  int length = http.getSize();
  String md5 = http.header("x-MD5");
  if (!Update.begin(length > 0 ? length : UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("OTA: %s\n", Update.errorString());
    http.end();
    return false;
  }
  if (md5.length() == 32) {
    Update.setMD5(md5.c_str());
  }
  WiFiClient *stream = http.getStreamPtr();
  uint32_t readyAt = millis();
  while (stream->available() == 0 && millis() - readyAt < 10000) {
    delay(10);
  }
  if (stream->peek() != 0xE9) {
    Serial.printf("OTA falhou: primeiro byte 0x%02X\n", stream->peek() < 0 ? 0 : stream->peek());
    Update.abort();
    http.end();
    return false;
  }
  size_t written = Update.writeStream(*stream);
  bool finished = written > 0 && Update.end();
  http.end();
  if (!finished || !Update.isFinished()) {
    Serial.printf("OTA falhou: %s\n", Update.errorString());
    Update.abort();
    return false;
  }
  Serial.println("OTA do Heltec aplicado, reiniciando");
  Serial.flush();
  ESP.restart();
  return true;
}

static void parseUpstream() {
  String base = API_BASE;
  upstream.https = base.startsWith("https://");
  int start = base.indexOf("://");
  start = start < 0 ? 0 : start + 3;
  int slash = base.indexOf('/', start);
  String hostport = slash < 0 ? base.substring(start) : base.substring(start, slash);
  int colon = hostport.indexOf(':');
  if (colon < 0) {
    upstream.host = hostport;
    upstream.port = upstream.https ? 443 : 80;
  } else {
    upstream.host = hostport.substring(0, colon);
    upstream.port = hostport.substring(colon + 1).toInt();
  }
}

static bool readHeaders(WiFiClient &client, String &headers) {
  uint32_t start = millis();
  while (millis() - start < 10000) {
    while (client.available()) {
      headers += (char)client.read();
      if (headers.length() > 4096) {
        return false;
      }
      if (headers.endsWith("\r\n\r\n")) {
        return true;
      }
    }
    if (!client.connected()) {
      return false;
    }
    delay(1);
  }
  return false;
}

static int headerInt(const String &headers, const char *name) {
  int index = headers.indexOf(name);
  if (index < 0) {
    return -1;
  }
  return headers.substring(index + strlen(name)).toInt();
}

static bool copyBytes(WiFiClient &from, WiFiClient &to, int count) {
  uint8_t buffer[512];
  uint32_t last = millis();
  while (count > 0) {
    int ready = from.available();
    if (ready <= 0) {
      if (!from.connected() || millis() - last > 65000) {
        return false;
      }
      delay(1);
      continue;
    }
    int take = ready < count ? ready : count;
    if (take > (int)sizeof(buffer)) {
      take = sizeof(buffer);
    }
    int got = from.read(buffer, take);
    if (got <= 0 || to.write(buffer, got) != (size_t)got) {
      return false;
    }
    count -= got;
    last = millis();
  }
  return true;
}

static void relayHttp(WiFiClient &client) {
  String headers;
  if (!readHeaders(client, headers)) {
    client.stop();
    return;
  }
  int path = headers.indexOf(' ');
  int pathEnd = path < 0 ? -1 : headers.indexOf(' ', path + 1);
  String target = path >= 0 && pathEnd > path ? headers.substring(path + 1, pathEnd) : "";
  if (!target.startsWith("/api/v1/ingest/")) {
    client.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    client.stop();
    return;
  }

  int hostAt = headers.indexOf("Host:");
  if (hostAt < 0) {
    hostAt = headers.indexOf("host:");
  }
  if (hostAt >= 0) {
    int lineEnd = headers.indexOf("\r\n", hostAt);
    String host = "Host: " + upstream.host;
    if ((!upstream.https && upstream.port != 80) || (upstream.https && upstream.port != 443)) {
      host += ":" + String(upstream.port);
    }
    headers = headers.substring(0, hostAt) + host + headers.substring(lineEnd);
  }

  WiFiClient plain;
  WiFiClientSecure secure;
  WiFiClient *upstreamClient = &plain;
  if (upstream.https) {
    secure.setInsecure();
    upstreamClient = &secure;
  }
  if (!upstreamClient->connect(upstream.host.c_str(), upstream.port, 10000)) {
    client.print("HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    client.stop();
    return;
  }
  upstreamClient->print(headers);
  int body = headerInt(headers, "Content-Length:");
  if (body < 0) {
    body = headerInt(headers, "content-length:");
  }
  if (body > 0 && !copyBytes(client, *upstreamClient, body)) {
    upstreamClient->stop();
    client.stop();
    return;
  }

  String responseHeaders;
  if (!readHeaders(*upstreamClient, responseHeaders)) {
    upstreamClient->stop();
    client.stop();
    return;
  }
  int responseBody = headerInt(responseHeaders, "Content-Length:");
  if (responseBody < 0) {
    responseBody = headerInt(responseHeaders, "content-length:");
  }
  uint8_t prefix[64];
  int prefixLen = 0;
  uint32_t prefixAt = millis();
  while (responseBody > prefixLen && prefixLen < (int)sizeof(prefix) && millis() - prefixAt < 10000) {
    int ready = upstreamClient->available();
    if (ready <= 0) {
      if (!upstreamClient->connected()) {
        break;
      }
      delay(1);
      continue;
    }
    int take = ready < responseBody - prefixLen ? ready : responseBody - prefixLen;
    if (take > (int)sizeof(prefix) - prefixLen) {
      take = sizeof(prefix) - prefixLen;
    }
    int got = upstreamClient->read(prefix + prefixLen, take);
    if (got <= 0) {
      break;
    }
    prefixLen += got;
  }
  size_t headerLen = responseHeaders.length();
  uint8_t *head = (uint8_t *)malloc(headerLen + prefixLen);
  if (head == nullptr) {
    upstreamClient->stop();
    client.stop();
    return;
  }
  memcpy(head, responseHeaders.c_str(), headerLen);
  if (prefixLen > 0) {
    memcpy(head + headerLen, prefix, prefixLen);
  }
  size_t sent = 0;
  size_t headTotal = headerLen + prefixLen;
  bool headOk = true;
  while (sent < headTotal) {
    int wrote = client.write(head + sent, headTotal - sent);
    if (wrote <= 0) {
      headOk = false;
      break;
    }
    sent += wrote;
  }
  free(head);
  if (!headOk) {
    upstreamClient->stop();
    client.stop();
    return;
  }
  if (responseBody > prefixLen) {
    copyBytes(*upstreamClient, client, responseBody - prefixLen);
  }
  upstreamClient->stop();
  client.stop();
}

static void serveProxy() {
  if (!apReady) {
    return;
  }
  WiFiClient incoming = proxyServer.available();
  if (incoming) {
    relayHttp(incoming);
  }
}

static void startAccessPoint() {
  int channel = WiFi.channel();
  if (channel < 1 || channel > 13) {
    channel = 1;
  }
  WiFi.softAP(AP_SSID, AP_PASSWORD, channel, 0, 4);
  proxyServer.begin();
  apReady = true;
  Serial.printf("AP %s em %s, encaminha para %s\n", AP_SSID, WiFi.softAPIP().toString().c_str(), API_BASE);
}

static void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!apReady) {
      configTime(0, 0, "pool.ntp.org");
      startAccessPoint();
    }
    return;
  }
  apReady = false;
  uint32_t now = millis();
  if (lastWifiTry != 0 && now - lastWifiTry < 20000) {
    return;
  }
  lastWifiTry = now;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.begin(UPSTREAM_SSID, UPSTREAM_PASSWORD);
  Serial.printf("Wi-Fi de subida \"%s\"\n", UPSTREAM_SSID);
}

static bool startLora() {
  if (loraReady) {
    return true;
  }
  pinMode(LORA_NSS, OUTPUT);
  digitalWrite(LORA_NSS, HIGH);
  if (!spiReady) {
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
    spiReady = true;
  }
  LoRa.setSPI(SPI);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("LoRa não iniciou");
    return false;
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH);
  LoRa.setCodingRate4(LORA_CODING_RATE);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.receive();
  loraReady = true;
  Serial.println("LoRa em escuta");
  return true;
}

static void sendOtaCommand() {
  SmartTrapDownlink command = {};
  command.magic = SMARTTRAP_MAGIC;
  command.kind = SMARTTRAP_KIND_OTA;
  LoRa.beginPacket();
  LoRa.write(reinterpret_cast<const uint8_t *>(&command), sizeof(command));
  LoRa.endPacket();
  LoRa.receive();
  Serial.println("Comando OTA enviado por LoRa");
}

static bool sendTimeCommand() {
  time_t now = time(nullptr);
  if (now <= 1700000000) {
    Serial.println("Relógio do Heltec indefinido; ajuste adiado");
    return false;
  }
  SmartTrapTimeDownlink command = {};
  command.magic = SMARTTRAP_MAGIC;
  command.kind = SMARTTRAP_KIND_TIME;
  command.unix_time = (uint32_t)now;
  LoRa.beginPacket();
  LoRa.write(reinterpret_cast<const uint8_t *>(&command), sizeof(command));
  LoRa.endPacket();
  LoRa.receive();
  Serial.printf("Horário %u enviado por LoRa\n", command.unix_time);
  return true;
}

static bool validCode(const char *code) {
  if (code[0] == '\0') {
    return false;
  }
  for (const char *cursor = code; *cursor != '\0'; cursor++) {
    char symbol = *cursor;
    bool ok = (symbol >= 'a' && symbol <= 'z') || (symbol >= 'A' && symbol <= 'Z') ||
              (symbol >= '0' && symbol <= '9') || symbol == '_' || symbol == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

static bool syncPending(const char *code) {
  for (int index = 0; index < syncCount; index++) {
    if (strcmp(syncCodes[index], code) == 0) {
      return true;
    }
  }
  return false;
}

static void pollClockSync() {
  String url = String(API_BASE) + "/api/v1/ingest/clock-sync";
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttp(http, plain, secure, url)) {
    return;
  }
  int code = http.GET();
  if (code != 200) {
    http.end();
    return;
  }
  String body = http.getString();
  http.end();
  syncCount = 0;
  int cursor = 0;
  while (syncCount < 16) {
    int key = body.indexOf("\"device_code\"", cursor);
    if (key < 0) {
      break;
    }
    int colon = body.indexOf(':', key);
    int open = colon < 0 ? -1 : body.indexOf('"', colon + 1);
    int close = open < 0 ? -1 : body.indexOf('"', open + 1);
    if (close < 0) {
      break;
    }
    String deviceCode = body.substring(open + 1, close);
    if (validCode(deviceCode.c_str())) {
      strncpy(syncCodes[syncCount], deviceCode.c_str(), sizeof(syncCodes[0]) - 1);
      syncCodes[syncCount][sizeof(syncCodes[0]) - 1] = '\0';
      syncCount++;
    }
    cursor = close + 1;
  }
  Serial.printf("Ajustes de relógio pendentes: %d\n", syncCount);
}

static String isoNow() {
  time_t now = time(nullptr);
  struct tm clock;
  gmtime_r(&now, &clock);
  char iso[32];
  strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &clock);
  return String(iso);
}

static void postMetric(const SmartTrapUplink &packet, int rssi, float snr, bool hasClock) {
  if (!validCode(packet.device_code) || WiFi.status() != WL_CONNECTED) {
    Serial.println("Métrica não enviada");
    return;
  }
  String json = "{\"device_code\":\"";
  json += packet.device_code;
  json += "\",\"recorded_at\":\"";
  json += isoNow();
  json += "\",\"pest_count\":";
  json += String(packet.pest_count);
  json += ",\"firmware_version\":";
  json += String(packet.fw_version);
  if (packet.confidence != SMARTTRAP_CONF_NONE) {
    json += ",\"confidence\":";
    json += String(packet.confidence / 100.0f, 2);
  }
  if (packet.battery_mv != 0) {
    json += ",\"battery_v\":";
    json += String(packet.battery_mv / 1000.0f, 2);
  }
  if (hasClock) {
    json += ",\"device_time\":";
    json += String(packet.unix_time);
  }
  json += ",\"rssi\":";
  json += String(rssi);
  json += ",\"snr\":";
  json += String(snr, 1);
  json += "}";

  String url = String(API_BASE) + "/api/v1/ingest/metrics";
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttp(http, plain, secure, url)) {
    return;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  Serial.printf("POST métrica -> HTTP %d\n", code);
  http.end();
}

static void postClock(const char *deviceCode, uint32_t unixTime) {
  if (!validCode(deviceCode) || WiFi.status() != WL_CONNECTED) {
    return;
  }
  String json = "{\"device_code\":\"";
  json += deviceCode;
  json += "\",\"device_time\":";
  json += String(unixTime);
  json += "}";
  String url = String(API_BASE) + "/api/v1/ingest/clock";
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  if (!beginHttp(http, plain, secure, url)) {
    return;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  Serial.printf("POST relógio -> HTTP %d\n", code);
  http.end();
}

static void handlePacket(SmartTrapUplink packet, int rssi, float snr, bool hasClock) {
  packet.device_code[sizeof(packet.device_code) - 1] = '\0';
  if (packet.magic != SMARTTRAP_MAGIC || packet.kind != SMARTTRAP_KIND_METRIC) {
    return;
  }
  Serial.printf("Pacote %s fw %u relógio %u\n", packet.device_code, packet.fw_version, hasClock ? packet.unix_time : 0);
  bool sentTime = false;
  if (hasClock && syncPending(packet.device_code)) {
    sentTime = sendTimeCommand();
  }
  if (!sentTime && publishedTrapVersion > packet.fw_version) {
    sendOtaCommand();
  }
  LoRa.receive();
  if (packet.model_ok == 0) {
    Serial.println("Sem modelo; métrica omitida");
    if (hasClock) {
      postClock(packet.device_code, packet.unix_time);
    }
    return;
  }
  postMetric(packet, rssi, snr, hasClock);
}

static void pollCloud() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  pollClockSync();
  int trapVersion = fetchVersion("smarttrap");
  if (trapVersion >= 0) {
    publishedTrapVersion = trapVersion;
    Serial.printf("Firmware da armadilha publicado: %d\n", publishedTrapVersion);
  }
  int gatewayVersion = fetchVersion("heltec");
  if (gatewayVersion > FW_VERSION) {
    applyOta("heltec");
  }
}

static void readLora() {
  if (!loraReady) {
    return;
  }
  int length = LoRa.parsePacket();
  if (length <= 0) {
    return;
  }
  uint8_t buffer[64];
  int cap = length < (int)sizeof(buffer) ? length : (int)sizeof(buffer);
  int got = LoRa.readBytes(buffer, cap);
  while (LoRa.available()) {
    LoRa.read();
  }
  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();
  if (got != (int)SMARTTRAP_UPLINK_LEGACY_SIZE && got != (int)sizeof(SmartTrapUplink)) {
    LoRa.receive();
    return;
  }
  SmartTrapUplink packet = {};
  memcpy(&packet, buffer, got);
  handlePacket(packet, rssi, snr, got == (int)sizeof(SmartTrapUplink));
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nHeltec SmartTrap fw %d\n", FW_VERSION);
  parseUpstream();
  WiFi.persistent(false);
  maintainWifi();
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  maintainWifi();
  if (!startLora()) {
    Serial.println("Segue sem LoRa até o próximo ciclo");
  }
  pollCloud();
  lastPoll = millis();
}

void loop() {
  maintainWifi();
  if (!loraReady && millis() - lastLoraTry > 10000) {
    lastLoraTry = millis();
    startLora();
  }
  readLora();
  serveProxy();
  if (millis() - lastPoll >= POLL_INTERVAL_MS) {
    pollCloud();
    lastPoll = millis();
  }
}
