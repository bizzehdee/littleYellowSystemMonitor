#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Preferences.h>
#include <WebServer.h>
#include <cstring>
#include <vector>
#include <algorithm>
#include "constants.h"

#define GFXFF 1
#define FS9 &FreeSans9pt7b
#define MS9 &FreeMono9pt7b

Preferences prefs;

TFT_eSPI tft = TFT_eSPI();

WiFiUDP udp;

char broadcastMessage[64];

enum class ScreenState : uint8_t {
    Info,
    Stats
};

/*
  Packet read states:
  - WaitingForStart: expecting 0xFA
  - ReadingLengthLow: reading low byte of payload length
  - ReadingLengthHigh: reading high byte of payload length
  - ReadingPacketType: reading the packet type
  - ReadingPayload: reading payload bytes and checksum
*/
enum class PacketReadState : uint8_t {
    WaitingForStart,
    ReadingLengthLow,
    ReadingLengthHigh,
    ReadingPacketType,
    ReadingPayload
};


void switchToState(ScreenState newState);
void updateScreen();
uint8_t calcChecksum(PacketType packet, uint16_t length, uint8_t *payload);
void processPacket(PacketType packet, uint16_t length, uint8_t *payload);
void readDataWifi();
void drawInfo();
void drawStats();
void drawCpuLoad(uint32_t &yOffset);
void drawCpuTemp(uint32_t &yOffset);
void drawRamUsage(uint32_t &yOffset);
void drawCoreUsage(uint32_t &yOffset);
void drawDiskIO(uint32_t &yOffset);
void drawUptime(uint32_t &yOffset);
void formatSpeed(double &value, char* suffix, size_t suffixSize, uint64_t kbps);


unsigned long lastRefreshTime = 0, lastRefreshTime1hz = 0, lastRefreshBroadcast = 0, lastPacketRecieved = 0;
uint32_t lastSequence = 0;
ScreenState screenState = ScreenState::Stats;

uint8_t cpuCoreCount = 16;
std::vector<uint8_t> cpuLoadsOld(16, 0);
std::vector<uint8_t> cpuLoads(16, 100);
uint8_t cpuLoadOverall = 90;
uint8_t cpuLoadOverallOld = 0;
char cpuLoadTextBuffer[5];

uint8_t cpuTempMax = 105;
uint8_t cpuTempOverall = 79;
uint8_t cpuTempOverallOld = 0;
char cpuTempTextBuffer[5];

uint16_t maxRamInMb = 32 * 1024;
uint16_t usedRamInMb = 16 * 1024;
uint32_t ramPercentage = 50;
uint32_t ramPercentageOld = 0;
char ramUsageTextBuffer[20];

uint64_t diskIOReadKbps = 0;
uint64_t diskIOReadKbpsOld = 1;
uint64_t diskIOWriteKbps = 0;
uint64_t diskIOWriteKbpsOld = 1;
char diskIOReadTextBuffer[32];
char diskIOWriteTextBuffer[32];
char diskIOReadSuffixText[5];
char diskIOWriteSuffixText[5];

uint32_t uptimeCurrent = 0;
uint32_t uptimeOld = 1;
char uptimeTextBuffer[20];

/*
  0 = waiting for start
  1 = packet type
  2 = data length low byte (int16)
  3 = data length high byte (int16)
  4 = actual payload
*/
PacketReadState packetReadState = PacketReadState::WaitingForStart;
/*
  0 = nop
  1 = cpu load (with cores)
  2 = cpu temp
  3 = ram usage
  4 = disk i/o
  5 = uptime
  6 = ...
*/
PacketType packetType = PacketType::NOP;
uint16_t expectedPayloadLength = 0;
uint16_t payloadIdx = 0;

// payload buffer: allow indices 0..MAX_PAYLOAD_SIZE (last byte is checksum)
// Allocate +1 so a full-size payload (MAX_PAYLOAD_SIZE) can be stored including checksum
std::vector<uint8_t> payload(MAX_PAYLOAD_SIZE + 1);

String prefSsid, prefPassword;
WebServer webServer(80);

void startAccessPoint();
void startConfigServer();
void handleRoot();
void handleSave();
void connectToWiFi();

void setup()
{
  prefs.begin("wifi", true);
  prefSsid = prefs.getString("ssid");
  prefPassword = prefs.getString("pass");
  prefs.end();

  memset(diskIOReadTextBuffer, 0, sizeof(diskIOReadTextBuffer));
  memset(diskIOWriteTextBuffer, 0, sizeof(diskIOWriteTextBuffer));
  memset(diskIOReadSuffixText, 0, 5);
  memset(diskIOWriteSuffixText, 0, 5);

  if (prefSsid.length() > 0)
  {
    connectToWiFi();
  }
  else
  {
    startAccessPoint();
  }

  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);

  switchToState(ScreenState::Info);
}

void loop()
{
  if (WiFi.getMode() == WIFI_AP)
  {
    webServer.handleClient();
  }
  else
  {
    readDataWifi();
  }

  unsigned long currentTime = millis();

  if (currentTime - lastRefreshTime > targetRefreshInterval) // every 1/targetRefreshRate'th of a second
  {
    lastRefreshTime = currentTime;

    updateScreen();
  }

  if (currentTime - lastRefreshTime1hz > 1000)
  {

    lastRefreshTime1hz = currentTime;
  }

  if (currentTime - lastRefreshBroadcast > 5000)
  {
    lastRefreshBroadcast = currentTime;

    snprintf(broadcastMessage, sizeof(broadcastMessage),
             "SYSMN_INFO %s %u",
             WiFi.localIP().toString().c_str(),
             TCP_PORT);

    // compute broadcast from local IP and subnet mask
    IPAddress broadcastIP = (WiFi.localIP() & WiFi.subnetMask()) | ~WiFi.subnetMask();
    udp.beginPacket(broadcastIP, BROADCAST_PORT);
    // send only the used portion of the buffer
    udp.write((const uint8_t *)broadcastMessage, strlen(broadcastMessage));
    udp.endPacket();
  }

  if (currentTime - lastPacketRecieved > 10000)
  {
    switchToState(ScreenState::Info);
  }
}

void readDataWifi()
{
  int packetSize = udp.parsePacket();
  if (packetSize <= 0)
  {
    return;
  }

  std::vector<uint8_t> buf;
  buf.resize(packetSize);
  int readLen = udp.read(buf.data(), packetSize);
  if (readLen != packetSize)
  {
    return;
  }

  if (packetSize < 4)
  {
    return;
  }
  uint8_t header = buf[0];
  if (header != 0xFA)
  {
    return;
  }
  uint16_t length = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
  PacketType pType = (PacketType)buf[3];
  if (length == 0 || length > MAX_PAYLOAD_SIZE)
  {
    return;
  }
  if ((int)(4 + length) > packetSize)
  {
    return;
  }
  uint8_t *pl = buf.data() + 4;
  uint8_t chkCalc = calcChecksum(pType, length, pl);
  uint8_t chkSent = pl[length - 1];
  if (chkCalc != chkSent)
  {
    return;
  }

  processPacket(pType, length, pl);
  lastPacketRecieved = millis();
  switchToState(ScreenState::Stats);
}

uint8_t calcChecksum(PacketType pType, uint16_t length, uint8_t *payload)
{
  uint8_t xChk = 0;
  xChk ^= (uint8_t)(length);
  xChk ^= (uint8_t)(length >> 8);
  xChk ^= (uint8_t)pType;

  if (length > 0)
  {
    //XOR all payload bytes except the last byte (which is the checksum byte)
    for (uint16_t x = 0; x + 1 < length; x++)
    {
      xChk ^= payload[x];
    }
  }

  return xChk;
}

void processPacket(PacketType pType, uint16_t length, uint8_t *payload)
{
  uint8_t *p = payload;

  switch (pType)
  {
  case NOP:
    break;

  case STATE:
  {
    uint16_t dataLen = (length > 0) ? (length - 1) : 0;
    if (dataLen < 4)
      return;
    uint32_t seq = 0;
    memcpy(&seq, p, sizeof(seq));
    p += 4;
    if (seq <= lastSequence)
    {
      return;
    }
    lastSequence = seq;

    if ((payload + dataLen) - p < 2)
      return;
    cpuLoadOverall = CLAMP(p[0], 0, 100);
    uint8_t newCoreCount = CLAMP(p[1], 1, 64);
    p += 2;

    if ((payload + dataLen) - p < newCoreCount)
      return;
    if (newCoreCount != cpuCoreCount)
    {
      cpuCoreCount = newCoreCount;
      cpuLoads.resize(cpuCoreCount);
      cpuLoadsOld.assign(cpuCoreCount, 0);
    }
    for (uint8_t i = 0; i < cpuCoreCount; i++)
    {
      cpuLoads[i] = CLAMP(p[i], 0, 100);
    }
    p += cpuCoreCount;

    if ((payload + dataLen) - p < 1)
      return;
    cpuTempOverall = *p;
    p += 1;

    if ((payload + dataLen) - p < 4)
      return;
    uint16_t maxRam = 0;
    memcpy(&maxRam, p, sizeof(maxRam));
    p += 2;
    uint16_t usedRam = 0;
    memcpy(&usedRam, p, sizeof(usedRam));
    p += 2;
    maxRamInMb = maxRam;
    usedRamInMb = MIN(usedRam, maxRamInMb);
    double ramPercentageD = (maxRamInMb > 0) ? ((double)usedRamInMb / (double)maxRamInMb) * 100.0 : 0.0;
    ramPercentage = (uint32_t)ramPercentageD;

    if ((payload + dataLen) - p < 16)
      return;
    uint64_t r = 0, w = 0;
    memcpy(&r, p, sizeof(r));
    p += 8;
    memcpy(&w, p, sizeof(w));
    p += 8;
    diskIOReadKbps = r / 1024;
    diskIOWriteKbps = w / 1024;

    if ((payload + dataLen) - p < 4)
      return;
    uint32_t up = 0;
    memcpy(&up, p, sizeof(up));
    uptimeCurrent = up;
  }
  break;

  case CPU:
  {
    if (length == 0)
      return;
    uint16_t dataLen = (length > 0) ? (length - 1) : 0; // excluding checksum
    if (dataLen < 2)
      return; // need at least overall + corecount
    cpuLoadOverall = CLAMP(p[0], 0, 100);

    uint8_t newCoreCount = p[1];
    // clamp core count to sensible range [1,64]
    newCoreCount = CLAMP(newCoreCount, 1, 64);

    if (newCoreCount != cpuCoreCount)
    {
      cpuCoreCount = newCoreCount;

      cpuLoads.resize(cpuCoreCount);
      cpuLoadsOld.assign(cpuCoreCount, 0);
    }

    if (dataLen < (uint16_t)(2 + cpuCoreCount))
      return; // not enough data for cores

    for (int i = 0; i < cpuCoreCount; i++)
    {
      cpuLoads[i] = CLAMP(p[2 + i], 0, 100);
    }
  }
  break;

  case TEMP:
  {
    if (length < 2)
      return;
    
    cpuTempOverall = payload[0];
  }
  break;

  case RAM:
  {
    if (length < 5)
      return;

    uint16_t dataLen = (length > 0) ? (length - 1) : 0;
    if (dataLen < 4)
      return;

      uint16_t maxRam = 0;
      memcpy(&maxRam, p, sizeof(maxRam));
      p += 2;
      uint16_t usedRam = 0;
      memcpy(&usedRam, p, sizeof(usedRam));

    maxRamInMb = maxRam;
    usedRamInMb = usedRam;
    if (usedRamInMb > maxRamInMb)
    {
      usedRamInMb = maxRamInMb;
    }

    double ramPercentageD = 0.0;
    if (maxRamInMb > 0) {
      ramPercentageD = ((double)usedRamInMb / (double)maxRamInMb) * 100;
    }
    ramPercentage = (uint32_t)ramPercentageD;
  }
  break;

  case DISK:
  {
    if (length < 17)
      return;

    uint16_t dataLen = (length > 0) ? (length - 1) : 0;
    if (dataLen < 16)
      return;

    uint64_t r = 0;
    memcpy(&r, p, sizeof(r));
    p += 8;
    uint64_t w = 0;
    memcpy(&w, p, sizeof(w));

    diskIOReadKbps = r / 1024;
    diskIOWriteKbps = w / 1024;
  }
  break;

  case UPTIME:
  {
    if (length < 5)
      return;

    uint32_t up = 0;
    memcpy(&up, payload, sizeof(up));
    uptimeCurrent = up;
  }
  break;
  }
}

void switchToState(ScreenState newState)
{
  if (screenState == newState)
    return;

  screenState = newState;

  tft.fillRect(0, 0, screenWidth, screenHeight, TFT_BLACK);

  if (screenState == ScreenState::Info)
  {
    tft.setFreeFont(MS9);
  }
  else if (screenState == ScreenState::Stats)
  {
    tft.setFreeFont(FS9);

    // these areas only ever need to be drawn once, never overdrawn
    tft.drawString("CPU Load", 0, 0, GFXFF);
    tft.drawString("CPU Temp (c)", 0, fontHeight + barHeight + spacing, GFXFF);
    tft.drawString("RAM", 0, fontHeight + barHeight + spacing + fontHeight + barHeight + spacing, GFXFF);
    tft.drawString("Core Usage", 0, fontHeight + barHeight + spacing + fontHeight + barHeight + spacing + fontHeight + barHeight + spacing, GFXFF);

    tft.setFreeFont(MS9);
  }
}

void drawGradientBar(TFT_eSPI *tft, int x, int y, int w, int h, int percent, char *text)
{
  uint16_t color1 = tft->color565(0, 255, 0);
  uint16_t color2 = tft->color565(255, 0, 0);

  double barWidth = CLAMP((CLAMP(percent,0,100) / 100.0) * w, 0, w);
  int barWidthInt = (int)barWidth;

  float delta = -255.0 / w;
  float alpha = 255.0;
  uint32_t color = color1;

  uint16_t w2 = 0;

  while (w2++ < barWidthInt)
  {
    tft->drawFastVLine(x++, y, h, color);
    alpha += delta;
    color = fastBlend((uint8_t)alpha, color1, color2);
  }

  tft->fillRect(x, y, w - w2, h, TFT_BLACK);

  uint32_t startX = (w / 2) - (tft->textWidth(text) / 2);
  tft->drawString(text, startX, y + 2, GFXFF);
}

void updateScreen()
{
  if (screenState == ScreenState::Info)
  {
    drawInfo();
  }
  else if (screenState == ScreenState::Stats)
  {
    drawStats();
  }
}

void drawInfo()
{
  if (WiFi.getMode() == WIFI_AP)
  {
    // if we are an AP, show the details and how to
    tft.drawString("Access Point Mode", 0, 0);
    tft.drawString(softAPName, 0, 25);
    tft.drawString(WiFi.softAPIP().toString().c_str(), 0, 50);
  }
  else
  {
    // if we are connected to wifi, show IP and port
    tft.drawString("Connected Mode", 0, 0);
    tft.drawString("Waiting for data...", 0, 25);
    tft.drawString(prefSsid.c_str(), 0, 75);
    tft.drawString(WiFi.localIP().toString().c_str(), 0, 100);
  }
}

void drawStats()
{
  uint32_t yOffset = fontHeight;

  tft.begin_nin_write();

  drawCpuLoad(yOffset);
  drawCpuTemp(yOffset);
  drawRamUsage(yOffset);
  drawCoreUsage(yOffset);
  drawDiskIO(yOffset);
  drawUptime(yOffset);

  tft.end_nin_write();
}

void formatSpeed(double &value, char* suffix, size_t suffixSize, uint64_t kbps) {
    if (kbps > 1024000) {
        value = (double)kbps / 1024.0 / 1024.0;
        strncpy(suffix, "gb/s", suffixSize);
    } else if (kbps > 10240) {
        value = (double)kbps / 1024.0;
        strncpy(suffix, "mb/s", suffixSize);
    } else {
        value = (double)kbps;
        strncpy(suffix, "kb/s", suffixSize);
    }
    suffix[suffixSize - 1] = '\0';
}

void drawCpuLoad(uint32_t &yOffset) {
  if (cpuLoadOverall != cpuLoadOverallOld) {
    snprintf(cpuLoadTextBuffer, sizeof(cpuLoadTextBuffer), "%d%%", cpuLoadOverall);
    drawGradientBar(&tft, 0, yOffset, screenWidth, barHeight, cpuLoadOverall, cpuLoadTextBuffer);
    cpuLoadOverallOld = cpuLoadOverall;
  }
  yOffset += barHeight + spacing + fontHeight;
}

void drawCpuTemp(uint32_t &yOffset) {
  if (cpuTempOverall != cpuTempOverallOld) {
    snprintf(cpuTempTextBuffer, sizeof(cpuTempTextBuffer), "%dc", cpuTempOverall);
    double tempPercentOfMax = ((double)cpuTempOverall / (double)cpuTempMax) * 100;
    drawGradientBar(&tft, 0, yOffset, screenWidth, barHeight, (int)tempPercentOfMax, cpuTempTextBuffer);
    cpuTempOverallOld = cpuTempOverall;
  }
  yOffset += barHeight + spacing + fontHeight;
}

void drawRamUsage(uint32_t &yOffset) {
  if (ramPercentage != ramPercentageOld) {
    snprintf(ramUsageTextBuffer, sizeof(ramUsageTextBuffer), "%dmb / %dmb", usedRamInMb, maxRamInMb);
    drawGradientBar(&tft, 0, yOffset, screenWidth, barHeight, (int)ramPercentage, ramUsageTextBuffer);
    ramPercentageOld = ramPercentage;
  }
  yOffset += barHeight + spacing + fontHeight;
}

void drawCoreUsage(uint32_t &yOffset) {
  uint32_t coreCount = (cpuCoreCount == 0) ? 1 : cpuCoreCount;
  uint32_t coreWidth = (screenWidth / coreCount);

  for (uint32_t i = 0; i < cpuCoreCount; i++) {
    if (cpuLoadsOld[i] == cpuLoads[i]) {
      continue;
    }
    uint32_t x = i * coreWidth;
    double thisCoreBarHeightD = cpuBarHeight / 100.0 * (double)cpuLoads[i];
    uint32_t barYOffset = (cpuBarHeight - (uint32_t)thisCoreBarHeightD);
    uint32_t barYStart = yOffset + barYOffset;
    tft.fillRect(x, yOffset, coreWidth - 1, barYOffset, TFT_BLACK); // clear previous
    tft.fillRect(x, barYStart, coreWidth - 1, (uint32_t)thisCoreBarHeightD, tft.color565(0, 0, 255));
    cpuLoadsOld[i] = cpuLoads[i];
  }
  yOffset += cpuBarHeight + spacing;
}

void drawDiskIO(uint32_t &yOffset) {
  if (diskIOReadKbps != diskIOReadKbpsOld) {
    double diskIOR = 0;
    formatSpeed(diskIOR, diskIOReadSuffixText, sizeof(diskIOReadSuffixText), diskIOReadKbps);

    uint32_t w = tft.textWidth(diskIOReadTextBuffer);
    tft.fillRect(0, yOffset, w, 15, TFT_BLACK); // clear previous

    snprintf(diskIOReadTextBuffer, sizeof(diskIOReadTextBuffer), "Disk R: %0.1f%s", diskIOR, diskIOReadSuffixText);
    tft.drawString(diskIOReadTextBuffer, 0, yOffset, GFXFF);
    diskIOReadKbpsOld = diskIOReadKbps;
  }

  if (diskIOWriteKbps != diskIOWriteKbpsOld) {
    double diskIOW = 0;
    formatSpeed(diskIOW, diskIOWriteSuffixText, sizeof(diskIOWriteSuffixText), diskIOWriteKbps);
    
    uint32_t w = tft.textWidth(diskIOWriteTextBuffer);
    tft.fillRect(0, yOffset + 15 + spacing, w, 15, TFT_BLACK); // clear previous

    snprintf(diskIOWriteTextBuffer, sizeof(diskIOWriteTextBuffer), "Disk W: %0.1f%s", diskIOW, diskIOWriteSuffixText);
    tft.drawString(diskIOWriteTextBuffer, 0, yOffset + 15 + spacing, GFXFF);
    diskIOWriteKbpsOld = diskIOWriteKbps;
  }
}

void drawUptime(uint32_t &yOffset) {
  tft.drawString("Up Time", screenWidth - tft.textWidth("Up Time"), yOffset, GFXFF);
  if (uptimeCurrent != uptimeOld) {
    uint32_t total_seconds = uptimeCurrent;
    uint32_t days = total_seconds / 86400;
    total_seconds %= 86400;
    uint32_t hours = total_seconds / 3600;
    total_seconds %= 3600;
    uint32_t minutes = total_seconds / 60;

    snprintf(uptimeTextBuffer, sizeof(uptimeTextBuffer), "%dd%02dh%02dm", days, hours, minutes);

    uint32_t w = tft.textWidth(uptimeTextBuffer);
    uint32_t startX = screenWidth - w;
    uint32_t startY = yOffset + 15 + spacing;
    tft.fillRect(startX, startY, w, 15, TFT_BLACK); // clear the previous text
    tft.drawString(uptimeTextBuffer, startX, startY, GFXFF);
    uptimeOld = uptimeCurrent;
  }
}

void connectToWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(prefSsid.c_str(), prefPassword.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    // Listen for UDP state frames on data port
    udp.begin(TCP_PORT);
  }
  else
  {
    startAccessPoint();
  }
}

void startAccessPoint()
{
  WiFi.mode(WIFI_AP);
  WiFi.softAP(softAPName);

  startConfigServer();
}

void startConfigServer()
{
  webServer.on("/", handleRoot);
  webServer.on("/save", handleSave);
  webServer.begin();
}

void handleRoot()
{
  String html = R"HTML(
  <html>
  <head><title>ESP32 WiFi Config</title></head>
  <body style='font-family:sans-serif;'>
    <h2>Configure WiFi</h2>
    <form action='/save' method='post'>
      SSID:<br><input type='text' name='ssid'><br>
      Password:<br><input type='password' name='pass'><br><br>
      <input type='submit' value='Save'>
    </form>
  </body>
  </html>
  )HTML";
  webServer.send(200, "text/html", html);
}

void handleSave()
{
  String newSSID = webServer.arg("ssid");
  String newPASS = webServer.arg("pass");

  if (newSSID.length() > 0)
  {
    prefs.begin("wifi", false);
    prefs.putString("ssid", newSSID);
    prefs.putString("pass", newPASS);
    prefs.end();

    String html = "<html><body><h3>Saved! Restarting...</h3></body></html>";
    webServer.send(200, "text/html", html);

    delay(2000);
    ESP.restart();
  }
  else
  {
    webServer.send(400, "text/plain", "SSID cannot be empty!");
  }
}
