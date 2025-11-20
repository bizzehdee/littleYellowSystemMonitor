#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Preferences.h>
#include <WebServer.h>
#include <cstring>
#include "constants.h"

#define GFXFF 1
#define FS9 &FreeSans9pt7b
#define MS9 &FreeMono9pt7b

Preferences prefs;

TFT_eSPI tft = TFT_eSPI();

WiFiServer server(TCP_PORT);
WiFiClient client;
WiFiUDP udp;

char broadcastMessage[64];

void switchToState(uint8_t newState);
void updateScreen();
uint8_t calcChecksum(PacketType packet, uint16_t length, uint8_t *payload);
void processPacket(PacketType packet, uint16_t length, uint8_t *payload);
void readDataWifi();
void drawInfo();
void drawStats();

uint64_t lastRefreshTime = 0, lastRefreshTime1hz = 0, lastRefreshBroadcast = 0, lastPacketRecieved = 0;
uint8_t screenState = 1;

uint8_t cpuCoreCount = 16;
uint8_t *cpuLoadsOld = new uint8_t[16];
uint8_t *cpuLoads = new uint8_t[16];
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
uint8_t packetReadState = 0;
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
uint8_t *payload = new uint8_t[MAX_PAYLOAD_SIZE + 1];

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

  memset(cpuLoads, 100, sizeof(uint8_t) * cpuCoreCount);
  memset(cpuLoadsOld, 0, sizeof(uint8_t) * cpuCoreCount);

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

  switchToState(0);
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
    switchToState(0);
  }
}

void readDataWifi()
{
  if (client && !client.connected())
  {
    // clean up a previous disconnected client
    client.stop();
  }

  if (!client || !client.connected())
  {
    delay(1);
    client = server.available();
  }

  // Handle incoming data
  while (client.available())
  {
    uint8_t byte = client.read();

    switch (packetReadState)
    {
    case 0:
      if (byte == 0xFA)
      {
        expectedPayloadLength = 0;
        packetType = PacketType::NOP;
        packetReadState = 1;
        payloadIdx = 0;
        memset(payload, 0, MAX_PAYLOAD_SIZE + 1);
      }
      break;
    case 1:
      expectedPayloadLength = byte;
      packetReadState = 2;
      break;
    case 2:
      expectedPayloadLength |= (byte << 8);

      if (expectedPayloadLength > MAX_PAYLOAD_SIZE)
      {
        packetReadState = 0;
        break;
      }

      if(expectedPayloadLength == 0)
      {
        packetReadState = 0;
        break;
      }

      packetReadState = 3;
      break;
    case 3:
      packetType = static_cast<PacketType>(byte);

      if (packetType >= 0 && packetType <= 5)
      {
        packetReadState = 4;
      }
      else // invalid packet type
      {
        packetReadState = 0;
      }

      break;
    case 4:
      payload[payloadIdx++] = byte;
      if (payloadIdx > MAX_PAYLOAD_SIZE)
      {
        // overflow: reset parser state and drop the current data
        packetReadState = 0;
        payloadIdx = 0;
        break;
      }

      // read an extra byte because we want the chk
      if (payloadIdx == expectedPayloadLength)
      {
        // we have the whole payload, check the checksum, and process the data, and then start waiting for new data
        uint8_t chk = calcChecksum(packetType, expectedPayloadLength, payload);
        uint8_t sentChk = payload[payloadIdx - 1];

        if (chk == sentChk)
        {
          processPacket(static_cast<PacketType>(packetType), expectedPayloadLength, payload);
          lastPacketRecieved = millis();
          switchToState(1);
        }

        packetReadState = 0;
      }

      break;
    }
  }
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

      if (cpuLoads != NULL)
      {
        delete[] cpuLoads;
        cpuLoads = nullptr;
      }
      if (cpuLoadsOld != NULL)
      {
        delete[] cpuLoadsOld;
        cpuLoadsOld = nullptr;
      }

      cpuLoads = new uint8_t[cpuCoreCount];
      cpuLoadsOld = new uint8_t[cpuCoreCount];
      memset(cpuLoadsOld, 0, cpuCoreCount);
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

    double ramPercentageD = ((double)usedRamInMb / (double)maxRamInMb) * 100;
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

void switchToState(uint8_t newState)
{
  if (screenState == newState)
    return;

  screenState = newState;

  tft.fillRect(0, 0, screenWidth, screenHeight, TFT_BLACK);

  if (screenState == 0)
  {
    tft.setFreeFont(MS9);
  }
  else if (screenState == 1)
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
  if (screenState == 0)
  {
    drawInfo();
  }
  else if (screenState == 1)
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

  // draw cpu load
  if (cpuLoadOverall != cpuLoadOverallOld)
  {
    snprintf(cpuLoadTextBuffer, sizeof(cpuLoadTextBuffer), "%d%%", cpuLoadOverall);

    drawGradientBar(&tft, 0, yOffset, screenWidth, barHeight, cpuLoadOverall, cpuLoadTextBuffer);

    cpuLoadOverallOld = cpuLoadOverall;
  }

  yOffset += barHeight + spacing + fontHeight;

  // draw cpu temp
  if (cpuTempOverall != cpuTempOverallOld)
  {
    snprintf(cpuTempTextBuffer, sizeof(cpuTempTextBuffer), "%dc", cpuTempOverall);

    double tempPercentOfMax = ((double)cpuTempOverall / (double)cpuTempMax) * 100;

    drawGradientBar(&tft, 0, yOffset, screenWidth, barHeight, (int)tempPercentOfMax, cpuTempTextBuffer);

    cpuTempOverallOld = cpuTempOverall;
  }

  yOffset += barHeight + spacing + fontHeight;

  // draw ram usage
  if (ramPercentage != ramPercentageOld)
  {
    snprintf(ramUsageTextBuffer, sizeof(ramUsageTextBuffer), "%dmb / %dmb", usedRamInMb, maxRamInMb);

    drawGradientBar(&tft, 0, yOffset, screenWidth, barHeight, (int)ramPercentage, ramUsageTextBuffer);

    ramPercentageOld = ramPercentage;
  }

  yOffset += barHeight + spacing + fontHeight;

  uint32_t coreCount = (cpuCoreCount == 0) ? 1 : cpuCoreCount;
  uint32_t coreWidth = (screenWidth / coreCount);

  // draw core usage
  for (uint32_t i = 0; i < cpuCoreCount; i++)
  {
    if (cpuLoadsOld[i] == cpuLoads[i])
    {
      continue;
    }
    // find the x value
    uint32_t x = i * coreWidth;

    double thisCoreBarHeightD = cpuBarHeight / 100.0 * (double)cpuLoads[i];

    uint32_t barYOffset = (cpuBarHeight - (uint32_t)thisCoreBarHeightD);
    uint32_t barYStart = yOffset + barYOffset;

    tft.fillRect(x, yOffset, coreWidth - 1, barYOffset, TFT_BLACK); // clear previous
    tft.fillRect(x, barYStart, coreWidth - 1, (uint32_t)thisCoreBarHeightD, tft.color565(0, 0, 255));

    cpuLoadsOld[i] = cpuLoads[i];
  }

  yOffset += cpuBarHeight + spacing;

  // draw diskio usage
  if (diskIOReadKbps != diskIOReadKbpsOld)
  {
    double diskIOR = diskIOReadKbps; // show kbps
    strncpy(diskIOReadSuffixText, "kb/s", sizeof(diskIOReadSuffixText));
    diskIOReadSuffixText[sizeof(diskIOReadSuffixText) - 1] = '\0';

    if (diskIOReadKbps > 10240)
    {
      diskIOR = (double)diskIOReadKbps / 1024.0; // show mbps above 10mb
      strncpy(diskIOReadSuffixText, "mb/s", sizeof(diskIOReadSuffixText));
      diskIOReadSuffixText[sizeof(diskIOReadSuffixText) - 1] = '\0';
    }
    if (diskIOReadKbps > 1024000)
    {
      diskIOR = (double)diskIOReadKbps / 1024.0 / 1024.0; // show gbps above 10gb
      strncpy(diskIOReadSuffixText, "gb/s", sizeof(diskIOReadSuffixText));
      diskIOReadSuffixText[sizeof(diskIOReadSuffixText) - 1] = '\0';
    }

    uint32_t w = tft.textWidth(diskIOReadTextBuffer);
    tft.fillRect(0, yOffset, w, 15, TFT_BLACK); // clear previous

    snprintf(diskIOReadTextBuffer, sizeof(diskIOReadTextBuffer), "Disk R: %0.1f%s", diskIOR, diskIOReadSuffixText);

    tft.drawString(diskIOReadTextBuffer, 0, yOffset, GFXFF);

    diskIOReadKbpsOld = diskIOReadKbps;
  }

  if (diskIOWriteKbps != diskIOWriteKbpsOld)
  {
    double diskIOW = diskIOWriteKbps; // show kbps
    strncpy(diskIOWriteSuffixText, "kb/s", sizeof(diskIOWriteSuffixText));
    diskIOWriteSuffixText[sizeof(diskIOWriteSuffixText) - 1] = '\0';
    if (diskIOWriteKbps > 10240)
    {
      diskIOW = (double)diskIOWriteKbps / 1024.0; // show mbps above 10mb
      strncpy(diskIOWriteSuffixText, "mb/s", sizeof(diskIOWriteSuffixText));
      diskIOWriteSuffixText[sizeof(diskIOWriteSuffixText) - 1] = '\0';
    }
    if (diskIOWriteKbps > 1024000)
    {
      diskIOW = (double)diskIOWriteKbps / 1024.0 / 1024.0; // show gbps above 10gb
      strncpy(diskIOWriteSuffixText, "gb/s", sizeof(diskIOWriteSuffixText));
      diskIOWriteSuffixText[sizeof(diskIOWriteSuffixText) - 1] = '\0';
    }

    uint32_t w = tft.textWidth(diskIOWriteTextBuffer);

    tft.fillRect(0, yOffset + 15 + spacing, w, 15, TFT_BLACK); // clear previous

    snprintf(diskIOWriteTextBuffer, sizeof(diskIOWriteTextBuffer), "Disk W: %0.1f%s", diskIOW, diskIOWriteSuffixText);

    tft.drawString(diskIOWriteTextBuffer, 0, yOffset + 15 + spacing, GFXFF);

    diskIOWriteKbpsOld = diskIOWriteKbps;
  }

  // draw uptime
  tft.drawString("Up Time", screenWidth - tft.textWidth("Up Time"), yOffset, GFXFF);

  if (uptimeCurrent != uptimeOld)
  {
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

  tft.end_nin_write();
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
    server.begin();
    udp.begin(BROADCAST_PORT);
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
