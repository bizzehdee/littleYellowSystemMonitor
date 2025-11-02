#ifndef __LYSM_CONSTANTS_H_
#define __LYSM_CONSTANTS_H_

#include <Arduino.h>

const char *softAPName = "ESP32_Config_AP";
const uint16_t BROADCAST_PORT = 33333;
const uint16_t TCP_PORT = 3333;

const uint32_t screenWidth = 320;
const uint32_t screenHeight = 240;

const uint32_t barHeight = 20;
const uint32_t fontHeight = 22;
const uint32_t cpuBarHeight = 35;
const uint32_t spacing = 5;

const uint32_t targetRefreshRate = 10; // fps
const uint32_t targetRefreshInterval = (1000 / targetRefreshRate);

#endif // __LYSM_CONSTANTS_H_