// Declaraciones compartidas entre main.cpp y console.cpp.
#pragma once

#include <Arduino.h>

#include "access_controller.h"
#include "block_list.h"
#include "dy50_sensor.h"
#include "relay_pulse.h"

struct Settings {
  uint32_t pulseMs;
  uint16_t minScore;
  bool accessOn;
  uint32_t fpBaud;
};

extern Dy50Sensor sensor;
extern AccessController accessCtl;
extern RelayPulse relayLogic;
extern Settings settings;
extern BlockList blockList;  // huellas guardadas pero sin permiso de abrir
extern bool readerOnline;
extern fp::SysParams readerParams;
extern bool readerParamsValid;

void settingsSave();
void blockListSave();
bool readerConnect(bool verbose);
void requestOpen(const char* reason);
void ledSignalDenied();
const char* resetReasonText();
void printChipLine();

// console.cpp
void consolePoll();
void printHelp();
void printStatus();
