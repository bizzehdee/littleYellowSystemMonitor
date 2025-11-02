#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Preferences.h>
#include <WebServer.h>

#define GFXFF 1
#define FS9 &FreeSans9pt7b
#define MS9 &FreeMono9pt7b

#define MAX_PAYLOAD_SIZE 64

#define BACKLIGHT_PIN 21

#define PTYPE_NOP 0
#define PTYPE_CPU 1
#define PTYPE_TEMP 2
#define PTYPE_RAM 3
#define PTYPE_DISK 4
#define PTYPE_UPTIME 5

const char *softAPName = "ESP32_Config_AP";

const uint16_t BROADCAST_PORT = 33333;
const uint16_t TCP_PORT = 3333;

Preferences prefs;

TFT_eSPI tft = TFT_eSPI();

WiFiServer server(TCP_PORT);
WiFiClient client;
WiFiUDP udp;

char broadcastMessage[64];

void switchToState(uint8_t newState);
void updateScreen();
uint8_t calcChecksum(uint8_t packet, uint16_t length, uint8_t *payload);
void processPacket(uint8_t packet, uint16_t length, uint8_t *payload);
void readDataWifi();
void drawInfo();
void drawStats();

const uint32_t screenWidth = 320;
const uint32_t screenHeight = 240;

const uint32_t barHeight = 20;
const uint32_t fontHeight = 22;
const uint32_t cpuBarHeight = 35;
const uint32_t spacing = 5;

const uint32_t targetRefreshRate = 10; // fps
const uint32_t targetRefreshInterval = (1000 / targetRefreshRate);
uint64_t lastRefreshTime = 0, lastRefreshTime1hz = 0, lastRefreshBroadcast = 0, lastPacketRecieved = 0;
uint8_t screenState = -1;

uint8_t cpuCoreCount = 16;
uint8_t *cpuLoadsOld = new uint8_t[16];
uint8_t *cpuLoads = new uint8_t[16];
uint8_t cpuLoadOverall = 90;
uint8_t cpuLoadOverallOld = 0;
char *cpuLoadTextBuffer = new char[5];

uint8_t cpuTempMax = 105;
uint8_t cpuTempOverall = 79;
uint8_t cpuTempOverallOld = 0;
char *cpuTempTextBuffer = new char[5];

uint16_t maxRamInMb = 32 * 1024;
uint16_t usedRamInMb = 16 * 1024;
uint32_t ramPercentage = 50;
uint32_t ramPercentageOld = 0;
char *ramUsageTextBuffer = new char[20];

uint32_t cpuMaxTemp = 95;

uint64_t diskIOReadKbps = 0;
uint64_t diskIOReadKbpsOld = 1;
uint64_t diskIOWriteKbps = 0;
uint64_t diskIOWriteKbpsOld = 1;
char *diskIOReadTextBuffer = new char[22];
char *diskIOWriteTextBuffer = new char[22];
char *diskIOReadSuffixText = new char[5];
char *diskIOWriteSuffixText = new char[5];

uint32_t uptimeCurrent = 0;
uint32_t uptimeOld = 1;
char *uptimeTextBuffer = new char[12];

/*
  0 = waiting for start
  1 = packet type
  2 = data length low byte (int16)
  3 = data length high byte (int16)
  4 = actual payload
*/
uint8_t state = 0;
/*
  0 = nop
  1 = cpu load (with cores)
  2 = cpu temp
  3 = ram usage
  4 = disk i/o
  5 = uptime
  6 = ...
*/
uint8_t packetType = 0;
uint16_t expectedPayloadLength = 0;
uint16_t payloadIdx = 0;

uint8_t *payload = new uint8_t[MAX_PAYLOAD_SIZE+1];

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

  memset(diskIOReadTextBuffer, 0, 22);
  memset(diskIOWriteTextBuffer, 0, 22);
  memset(diskIOReadSuffixText, 0, 5);
  memset(diskIOWriteSuffixText, 0, 5);

  memset(cpuLoads, 100, sizeof(uint8_t) * cpuCoreCount);

  Serial.begin(115200);

  if (prefSsid.length() > 0) {
    Serial.println("Found stored WiFi credentials, connecting...");
    connectToWiFi();
  } else {
    Serial.println("No WiFi credentials found, starting AP...");
    startAccessPoint();
  }

  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH); // Enable backlight

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);

  switchToState(0);
}

void loop()
{
  if (WiFi.getMode() == WIFI_AP) {
    webServer.handleClient();
  } else {
    readDataWifi();
  }

  unsigned long currentTime = millis();

  if (currentTime - lastRefreshTime > targetRefreshInterval) // every 1/targetRefreshRate'th of a second
  {
    lastRefreshTime = currentTime;

    updateScreen();
  }

  if(currentTime - lastRefreshTime1hz > 1000)
  {

    lastRefreshTime1hz = currentTime;
  }

  if(currentTime - lastRefreshBroadcast > 5000)
  {
    lastRefreshBroadcast = currentTime;
       
    snprintf(broadcastMessage, sizeof(broadcastMessage),
             "SYSMN_INFO %s %u",
             WiFi.localIP().toString().c_str(),
             TCP_PORT);

    IPAddress broadcastIP = ~WiFi.subnetMask() | WiFi.gatewayIP();
    udp.beginPacket(broadcastIP, BROADCAST_PORT);
    udp.write((uint8_t *)broadcastMessage, sizeof(broadcastMessage));
    udp.endPacket();
  }

  if(currentTime - lastPacketRecieved > 10000)
  {
    switchToState(0);
  }

}
  
void readDataWifi()
{
  uint8_t checksum = 0;

  if (!client || !client.connected())
  {
    delay(1);
    client = server.available(); // Wait for connection
  }

  // Handle incoming data
  while (client.available())
  {
    uint8_t byte = client.read();

    switch (state)
    {
    case 0:
      if (byte == 0xFA)
      {
        expectedPayloadLength = 0;
        packetType = 0;
        state = 1;
        payloadIdx = 0;
        memset(payload, 0, MAX_PAYLOAD_SIZE + 1);
      }
      break;
    case 1:
      expectedPayloadLength = byte;
      state = 2;
      break;
    case 2:
      expectedPayloadLength |= (byte << 8);

      if(expectedPayloadLength > MAX_PAYLOAD_SIZE) 
      {
        state = 0;
        break;
      }

      state = 3;
      break;
    case 3:
      packetType = byte;

      if(packetType >= 0 && packetType <= 5)
      {
        state = 4;
      }
      else // invalid packet type
      {
        state = 0;
      }

      break;
    case 4:
      payload[payloadIdx++] = byte;
      if(payloadIdx >= MAX_PAYLOAD_SIZE) 
      {
        state = 0;
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
          processPacket(packetType, expectedPayloadLength, payload);
          lastPacketRecieved = millis();
          switchToState(1);
        }

        state = 0;
      }

      break;
    }
  }
}

uint8_t calcChecksum(uint8_t packet, uint16_t length, uint8_t *payload)
{
  uint8_t xChk = 0;
  xChk ^= (uint8_t)(length);
  xChk ^= (uint8_t)(length >> 8);
  xChk ^= packet;

  for (uint16_t x = 0; x < length - 1; x++)
  {
    xChk ^= payload[x];
  }

  return xChk;
}

void processPacket(uint8_t packet, uint16_t length, uint8_t *payload)
{
  // do nothing for now
  switch (packet)
  {
  case PTYPE_NOP:

    break;
  case PTYPE_CPU:
  {
    cpuLoadOverall = max(min(payload[0], (uint8_t)100), (uint8_t)0);

    uint8_t newCoreCount = payload[1];
    if (newCoreCount != cpuCoreCount)
    {
      cpuCoreCount = max(min(payload[1], (uint8_t)100), (uint8_t)0);

      if(cpuLoads != NULL) delete cpuLoads;
      if(cpuLoadsOld != NULL) delete cpuLoadsOld;

      cpuLoads = new uint8_t[cpuCoreCount];
      cpuLoadsOld = new uint8_t[cpuCoreCount];
    }

    for (int i = 0; i < cpuCoreCount; i++)
    {
      cpuLoads[i] = max(min(payload[2+i], (uint8_t)100), (uint8_t)0);
    }
  }
  break;
  case PTYPE_TEMP:
  {
    cpuTempOverall = payload[0];
  }
  break;
  case PTYPE_RAM: // ram
  {

    maxRamInMb = *((uint16_t*)payload);
    payload += 2;
    
    usedRamInMb = *((uint16_t *)payload);
    if(usedRamInMb > maxRamInMb)
    {
      usedRamInMb = maxRamInMb;
    }

    double ramPercentageD = ((double)usedRamInMb / (double)maxRamInMb) * 100;
    ramPercentage = (uint32_t)ramPercentageD;
  }
  break;
  case PTYPE_DISK:
  {
    diskIOReadKbps = *((uint64_t *)payload);
    payload += 8;

    diskIOReadKbps = diskIOReadKbps / 1024;

    diskIOWriteKbps = *((uint64_t *)payload);

    diskIOWriteKbps = diskIOWriteKbps / 1024;
  }

  break;
  case PTYPE_UPTIME:
  {
    uptimeCurrent = *((uint32_t *)payload);
  }
  break;
  }
}

void switchToState(uint8_t newState)
{
  if(screenState == newState)
    return;

  screenState = newState;

  Serial.printf("Switching state to %d\n", newState);

  tft.fillRect(0, 0, screenWidth, screenHeight, TFT_BLACK);

  if (screenState == 0)
  {
    tft.setFreeFont(MS9);
  }
  else if(screenState == 1)
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

  double barWidth = (percent / 100.0) * screenWidth;
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

  uint32_t startX = (screenWidth / 2) - (tft->textWidth(text) / 2);
  tft->drawString(text, startX, y + 2, GFXFF);
}

void updateScreen()
{
  if(screenState == 0)
  {
    drawInfo();
  }
  else if(screenState == 1)
  {
    drawStats();
  }
}

void drawInfo()
{
  if (WiFi.getMode() == WIFI_AP) 
  {
    //if we are an AP, show the details and how to
    tft.drawString("Access Point Mode", 0, 0);
    tft.drawString(softAPName, 0, 25);
    tft.drawString(WiFi.softAPIP().toString().c_str(), 0, 50);
  }
  else
  {
    //if we are connected to wifi, show IP and port
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
    sprintf(cpuLoadTextBuffer, "%d%%", cpuLoadOverall);

    drawGradientBar(&tft, 0, yOffset, screenWidth, barHeight, cpuLoadOverall, cpuLoadTextBuffer);

    cpuLoadOverallOld = cpuLoadOverall;
  }

  yOffset += barHeight + spacing + fontHeight;

  // draw cpu temp
  if (cpuTempOverall != cpuTempOverallOld)
  {
    sprintf(cpuTempTextBuffer, "%dc", cpuTempOverall);

    double tempPercentOfMax = ((double)cpuTempOverall / (double)cpuTempMax) * 100;

    drawGradientBar(&tft, 0, yOffset, screenWidth, barHeight, (int)tempPercentOfMax, cpuTempTextBuffer);

    cpuTempOverallOld = cpuTempOverall;
  }

  yOffset += barHeight + spacing + fontHeight;

  // draw ram usage
  if (ramPercentage != ramPercentageOld)
  {
    sprintf(ramUsageTextBuffer, "%dmb / %dmb", usedRamInMb, maxRamInMb);

    drawGradientBar(&tft, 0, yOffset, screenWidth, barHeight, (int)ramPercentage, ramUsageTextBuffer);

    ramPercentageOld = ramPercentage;
  }

  yOffset += barHeight + spacing + fontHeight;

  uint32_t coreWidth = (screenWidth / cpuCoreCount);

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
    memccpy(diskIOReadSuffixText, "kb/s", 5, 5);

    if (diskIOReadKbps > 10240)
    {
      diskIOR = (double)diskIOReadKbps / 1024.0; // show mbps above 10mb
      memccpy(diskIOReadSuffixText, "mb/s", 5, 5);
    }
    if (diskIOReadKbps > 1024000)
    {
      diskIOR = (double)diskIOReadKbps / 1024.0 / 1024.0; // show gbps above 10gb
      memccpy(diskIOReadSuffixText, "gb/s", 5, 5);
    }

    uint32_t w = tft.textWidth(diskIOReadTextBuffer);
    tft.fillRect(0, yOffset, w, 15, TFT_BLACK); // clear previous

    sprintf(diskIOReadTextBuffer, "Disk R: %0.1f%s", diskIOR, diskIOReadSuffixText);

    tft.drawString(diskIOReadTextBuffer, 0, yOffset, GFXFF);

    diskIOReadKbps = diskIOReadKbpsOld;
  }

  if (diskIOWriteKbps != diskIOWriteKbpsOld)
  {
    double diskIOW = diskIOWriteKbps; // show kbps
    memccpy(diskIOWriteSuffixText, "kb/s", 5, 5);
    if (diskIOWriteKbps > 10240)
    {
      diskIOW = (double)diskIOWriteKbps / 1024.0; // show mbps above 10mb
      memccpy(diskIOWriteSuffixText, "mb/s", 5, 5);
    }
    if (diskIOWriteKbps > 1024000)
    {
      diskIOW = (double)diskIOWriteKbps / 1024.0 / 1024.0; // show gbps above 10gb
      memccpy(diskIOWriteSuffixText, "gb/s", 5, 5);
    }

    uint32_t w = tft.textWidth(diskIOWriteTextBuffer);

    tft.fillRect(0, yOffset + 15 + spacing, w, 15, TFT_BLACK); // clear previous

    sprintf(diskIOWriteTextBuffer, "Disk W: %0.1f%s", diskIOW, diskIOWriteSuffixText);

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

    sprintf(uptimeTextBuffer, "%dd%02dh%02dm", days, hours, minutes);

    uint32_t w = tft.textWidth(uptimeTextBuffer);
    uint32_t startX = screenWidth - w;
    uint32_t startY = yOffset + 15 + spacing;

    tft.fillRect(startX, startY, w, 15, TFT_BLACK); // clear the previous text
    tft.drawString(uptimeTextBuffer, startX, startY, GFXFF);

    uptimeOld = uptimeCurrent;
  }

  tft.end_nin_write();
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(prefSsid.c_str(), prefPassword.c_str());

  Serial.printf("Connecting to %s", prefSsid.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    server.begin();
    udp.begin(BROADCAST_PORT);
    Serial.println("TCP server started on port 3333");
  } else {
    Serial.println("\nFailed to connect, starting AP instead...");
    startAccessPoint();
  }
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(softAPName);
  Serial.printf("AP started: %s\n", softAPName);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  startConfigServer();
}

void startConfigServer() {
  webServer.on("/", handleRoot);
  webServer.on("/save", handleSave);
  webServer.begin();
  Serial.println("HTTP server started for WiFi config");
}

void handleRoot() {
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

void handleSave() {
  String newSSID = webServer.arg("ssid");
  String newPASS = webServer.arg("pass");

  if (newSSID.length() > 0) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", newSSID);
    prefs.putString("pass", newPASS);
    prefs.end();

    String html = "<html><body><h3>Saved! Restarting...</h3></body></html>";
    webServer.send(200, "text/html", html);

    delay(2000);
    ESP.restart();
  } else {
    webServer.send(400, "text/plain", "SSID cannot be empty!");
  }
}