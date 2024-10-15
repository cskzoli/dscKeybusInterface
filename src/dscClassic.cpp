/*
    DSC Keybus Interface

    https://github.com/taligentx/dscKeybusInterface

    This library is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dscClassic.h"

#if defined(ESP32)
portMUX_TYPE dscClassicInterface::timer1Mux = portMUX_INITIALIZER_UNLOCKED;
hw_timer_t * dscClassicInterface::timer1 = NULL;
#endif

dscClassicInterface::dscClassicInterface(byte setClockPin, byte setReadPin, byte setPC16Pin, byte setWritePin, const char * setAccessCode) {
  dscClockPin = setClockPin;
  dscReadPin = setReadPin;
  dscPC16Pin = setPC16Pin;
  dscWritePin = setWritePin;
  if (dscWritePin != 255) virtualKeypad = true;
  writeReady = false;
  writePartition = 1;
  pauseStatus = false;
  statusByte = 1;  // Sets default data structure for PC1500/PC1550
  accessCodeStay = setAccessCode;
  strcpy(accessCodeAway, accessCodeStay);
  strcat(accessCodeAway, "*1");
  strcpy(accessCodeNight, "*9");
  strcat(accessCodeNight, accessCodeStay);
}


void dscClassicInterface::begin(Stream &_stream) {
  pinMode(dscClockPin, INPUT);
  pinMode(dscReadPin, INPUT);
  pinMode(dscPC16Pin, INPUT);
  if (virtualKeypad) pinMode(dscWritePin, OUTPUT);
  stream = &_stream;

  // Platform-specific timers trigger a read of the data line 250us after the Keybus clock changes:

  // Arduino/AVR Timer1 calls ISR(TIMER1_OVF_vect) from dscClockInterrupt() and is disabled in the ISR for a one-shot timer
  #if defined(__AVR__)
  TCCR1A = 0;
  TCCR1B = 0;
  TIMSK1 |= (1 << TOIE1);

  // esp8266 timer1 calls dscDataInterrupt() from dscClockInterrupt() as a one-shot timer
  #elif defined(ESP8266)
  timer1_isr_init();
  timer1_attachInterrupt(dscDataInterrupt);
  timer1_enable(TIM_DIV16, TIM_EDGE, TIM_SINGLE);

  // esp32 timer1 calls dscDataInterrupt() from dscClockInterrupt()
  
  #elif defined(ESP32)
  #if ESP_IDF_VERSION_MAJOR >= 5
  timer1 = timerBegin(1000000);
  timerStop(timer1);
  timerAttachInterrupt(timer1, &dscDataInterrupt);
  timerAlarm(timer1, 250, true, 0);
  #elif
  timer1 = timerBegin(1, 80, true);
  timerStop(timer1);
  timerAttachInterrupt(timer1, &dscDataInterrupt, true);
  timerAlarmWrite(timer1, 250, true);
  timerAlarmEnable(timer1);
  #endif
  #endif

  // Generates an interrupt when the Keybus clock rises or falls - requires a hardware interrupt pin on Arduino/AVR
  attachInterrupt(digitalPinToInterrupt(dscClockPin), dscClockInterrupt, CHANGE);
}


void dscClassicInterface::stop() {

  // Disables Arduino/AVR Timer1 interrupts
  #if defined(__AVR__)
  TIMSK1 = 0;

  // Disables esp8266 timer1
  #elif defined(ESP8266)
  timer1_disable();
  timer1_detachInterrupt();

  // Disables esp32 timer1
  #elif defined(ESP32)
  #if ESP_IDF_VERSION_MAJOR < 5
  timerAlarmDisable(timer1);
  #endif
  timerEnd(timer1);
  #endif

  // Disables the Keybus clock pin interrupt
  detachInterrupt(digitalPinToInterrupt(dscClockPin));

  // Resets the panel capture data and counters
  panelBufferLength = 0;
  for (byte i = 0; i < dscDataSize; i++) {
    isrPanelData[i] = 0;
    isrPC16Data[i] = 0;
    isrModuleData[i] = 0;
  }
  isrPanelBitTotal = 0;
  isrPanelBitCount = 0;
  isrPanelByteCount = 0;

  // Resets the keypad and module data counters
  isrModuleBitTotal = 0;
  isrModuleBitCount = 0;
  isrModuleByteCount = 0;
}


bool dscClassicInterface::loop() {

  #if defined(ESP8266) || defined(ESP32)
  yield();
  #endif

  // Checks if Keybus data is detected and sets a status flag if data is not detected for 3s
  #if defined(ESP32)
  portENTER_CRITICAL(&timer1Mux);
  #else
  noInterrupts();
  #endif

  if (millis() - keybusTime > 3000) keybusConnected = false;  // keybusTime is set in dscDataInterrupt() when the clock resets
  else keybusConnected = true;

  #if defined(ESP32)
  portEXIT_CRITICAL(&timer1Mux);
  #else
  interrupts();
  #endif

  if (previousKeybus != keybusConnected) {
    previousKeybus = keybusConnected;
    keybusChanged = true;
    if (!pauseStatus) statusChanged = true;
    if (!keybusConnected) return true;
  }

  // Writes keys when multiple keys are sent as a char array
  if (writeKeysPending) writeKeys(writeKeysArray);

  // Skips processing if the panel data buffer is empty
  if (panelBufferLength == 0) return false;

  // Copies data from the buffer to panelData[]
  static byte panelBufferIndex = 1;
  byte dataIndex = panelBufferIndex - 1;
  for (byte i = 0; i < dscDataSize; i++) {
    panelData[i] = panelBuffer[dataIndex][i];
    pc16Data[i] = pc16Buffer[dataIndex][i];
  }
  panelBitCount = panelBufferBitCount[dataIndex];
  panelByteCount = panelBufferByteCount[dataIndex];
  panelBufferIndex++;

  // Resets counters when the buffer is cleared
  #if defined(ESP32)
  portENTER_CRITICAL(&timer1Mux);
  #else
  noInterrupts();
  #endif

  if (panelBufferIndex > panelBufferLength) {
    panelBufferIndex = 1;
    panelBufferLength = 0;
  }

  #if defined(ESP32)
  portEXIT_CRITICAL(&timer1Mux);
  #else
  interrupts();
  #endif

  // Waits at startup for valid data
  static bool startupCycle = true;
  if (startupCycle) {
    if (panelByteCount < 2 || pc16Data[0] == 0xFF) return false;
    else {
      startupCycle = false;
      writeReady = true;
      if (panelByteCount == 3) statusByte = 2; // Sets data structure for PC3000
    }
  }

  // Sets writeReady status
  if (!writeKeyPending && !writeKeysPending) writeReady = true;
  else writeReady = false;

  processPanelStatus();
  return true;
}


// Resets the state of all status components as changed for sketches to get the current status
void dscClassicInterface::resetStatus() {
  statusChanged = true;
  keybusChanged = true;
  troubleChanged = true;
  readyChanged[0] = true;
  armedChanged[0] = true;
  alarmChanged[0] = true;
  fireChanged[0] = true;
  openZonesStatusChanged = true;
  alarmZonesStatusChanged = true;
  openZonesChanged[0] = 0xFF;
  alarmZonesChanged[0] = 0xFF;
  pgmOutputsChanged[0] = true;
}


// Processes status
void dscClassicInterface::processPanelStatus() {

  // Keypad lights - maps Classic series keypad lights to PowerSeries keypad light order for sketch compatibility
  readyLight = bitRead(panelData[statusByte], 7);
  bitWrite(lights[0], 0, readyLight);

  armedLight = bitRead(panelData[statusByte], 6);
  bitWrite(lights[0], 1, armedLight);

  memoryLight = bitRead(panelData[statusByte], 5);
  bitWrite(lights[0], 2, memoryLight);

  bypassLight = bitRead(panelData[statusByte], 4);
  bitWrite(lights[0], 3, bypassLight);

  troubleLight = bitRead(panelData[statusByte], 3);
  bitWrite(lights[0], 4, troubleLight);

  programLight = bitRead(panelData[statusByte], 2);
  bitWrite(lights[0], 5, programLight);

  fireLight = bitRead(panelData[statusByte], 1);
  bitWrite(lights[0], 6, fireLight);

  if (lights[0] != previousLights) {
    previousLights = lights[0];
    if (!pauseStatus) statusChanged = true;
  }

  // Keypad beep
  beep = bitRead(panelData[statusByte], 0);

  // PC16 status
  troubleBit = bitRead(pc16Data[statusByte], 7);
  armedBypassBit = bitRead(pc16Data[statusByte], 6);
  armedBitA = bitRead(pc16Data[statusByte], 5);
  armedBitB = bitRead(pc16Data[statusByte], 4);
  alarmBit = bitRead(pc16Data[statusByte], 0);

  // Checks for memory light blinking
  static unsigned long memoryLightTimeOn = 0;
  static unsigned long memoryLightTimeOff = 0;
  if (memoryLight) {
    memoryLightTimeOn = millis();
    if (millis() - memoryLightTimeOff < 600) {
        memoryBlink = true;
        lightBlink = true;
      }
    else {
      memoryBlink = false;
      if (!armedBlink && !bypassBlink && !troubleBlink) lightBlink = false;
    }
  }
  else {
    memoryLightTimeOff = millis();
    if (millis() - memoryLightTimeOn > 1200) {
      memoryBlink = false;
      if (!armedBlink && !bypassBlink && !troubleBlink) lightBlink = false;
    }
  }

  // Checks for armed light blinking
  static unsigned long armedLightTimeOn = 0;
  static unsigned long armedLightTimeOff = 0;
  if (armedLight) {
    armedLightTimeOn = millis();
    if (millis() - armedLightTimeOff < 600) {
        armedBlink = true;
        lightBlink = true;
      }
    else {
      armedBlink = false;
      if (!memoryBlink && !bypassBlink && !troubleBlink) lightBlink = false;
    }
  }
  else {
    armedLightTimeOff = millis();
    if (millis() - armedLightTimeOn > 1200) {
      armedBlink = false;
      if (!memoryBlink && !bypassBlink && !troubleBlink) lightBlink = false;
    }
  }

  // Checks for bypass light blinking
  static unsigned long bypassLightTimeOn = 0;
  static unsigned long bypassLightTimeOff = 0;
  if (bypassLight) {
    bypassLightTimeOn = millis();
    if (millis() - bypassLightTimeOff < 600) {
        bypassBlink = true;
        lightBlink = true;
      }
    else {
      bypassBlink = false;
      if (!memoryBlink && !armedBlink && !troubleBlink) lightBlink = false;
    }
  }
  else {
    bypassLightTimeOff = millis();
    if (millis() - bypassLightTimeOn > 1200) {
      bypassBlink = false;
      if (!memoryBlink && !armedBlink && !troubleBlink) lightBlink = false;
    }
  }

  // Checks for trouble light blinking
  static unsigned long troubleLightTimeOn = 0;
  static unsigned long troubleLightTimeOff = 0;
  if (troubleLight) {
    troubleLightTimeOn = millis();
    if (millis() - troubleLightTimeOff < 600) {
        troubleBlink = true;
        lightBlink = true;
      }
    else {
      troubleBlink = false;
      if (!memoryBlink && !armedBlink && !bypassBlink) lightBlink = false;
    }
  }
  else {
    troubleLightTimeOff = millis();
    if (millis() - troubleLightTimeOn > 1200) {
      troubleBlink = false;
      if (!memoryBlink && !armedBlink && !bypassBlink) lightBlink = false;
    }
  }

  // Checks for beep status
  static unsigned long beepTimeStart = millis();
  static unsigned long beepTimeOff = millis();
  static bool beepOn = false;
  if (beep) {
    beepTimeStart = millis();
    beepOn = true;
  }
  else if (beepOn) {
    unsigned long beepTime = millis() - beepTimeStart;
    beepOn = false;
    if (beepTime <= 90) {  // Normal keypress beep
      if (status[0] == 0x0E) readyChanged[0] = true;
    }
    else if (!lights[0] && beepTime > 90 && beepTime <= 800) {
      status[0] = 0x10;  // Keypad lockout
      readyChanged[0] = false;
    }
    else if (beepTime > 800 && beepTime < 1200) {
      if (!lights[0]) {
        status[0] = 0x8F;  // Invalid access code
        readyChanged[0] = false;
      }
      else {
        status[0] = 0x0E;  // Function not available
        readyChanged[0] = false;
      }
    }
  }

  // Armed status
  static bool armedStayTriggered;
  if (armedBitA) {
    armed[0] = true;
    exitDelayArmed = true;
    if (bypassLight || armedBypassBit) {
      armedStay[0] = true;
      armedStayTriggered = true;
      armedAway[0] = false;
    }
    else if (armedStayTriggered) {
      if (!beep && !alarmBit && millis() - beepTimeOff > 2000) {
        armedStay[0] = false;
        armedAway[0] = true;
      }
    }
    else {
      armedStay[0] = false;
      armedAway[0] = true;
    }

    if (armedBlink && armedBitA == armedBitB) {
      noEntryDelay[0] = true;
      exitState[0] = DSC_EXIT_NO_ENTRY_DELAY;
    }

    // Reset ready status
    processReadyStatus(false);
  }
  else {
    armedStayTriggered = false;
    processArmedStatus(false);
    processAlarmStatus(false);
  }

  if (armed[0] != previousArmed || armedStay[0] != previousArmedStay || armedAway[0] != previousArmedAway) {
    previousArmed = armed[0];
    previousArmedStay = armedStay[0];
    previousArmedAway = armedAway[0];
    armedChanged[0] = true;
    if (!pauseStatus) statusChanged = true;
  }

  // Ready status
  if (readyLight && !armedBitA) {
    processReadyStatus(true);
    processArmedStatus(false);
    processAlarmStatus(false);
    exitDelayArmed = false;
    previousAlarmTriggered = false;
    starKeyDetected = false;
    if (!armedBlink) noEntryDelay[0] = false;

    if (armedLight) {
      processExitDelayStatus(true);
      exitDelayTriggered = true;
      if (exitState[0] != DSC_EXIT_NO_ENTRY_DELAY) {
        if (bypassLight) exitState[0] = DSC_EXIT_STAY;
        else exitState[0] = DSC_EXIT_AWAY;
        if (exitState[0] != previousExitState) {
          previousExitState = exitState[0];
          exitDelayChanged[0] = true;
          exitStateChanged[0] = true;
          if (!pauseStatus) statusChanged = true;
        }
      }
    }

    // Reset armed status
    else if (!exitDelayArmed && !armedBlink && millis() - armedLightTimeOn > 400) {
      processExitDelayStatus(false);
      exitState[0] = 0;
    }
  }

  else {
    // Sets ready status if zones lights are on
    if (panelData[statusByte - 1] || (statusByte == 3 && (panelData[statusByte - 1] || panelData[statusByte - 2]))) processReadyStatus(false);

    if (exitDelayArmed && !armedBitA) {
      processReadyStatus(false);
      exitDelayArmed = false;
    }
    if (exitDelay[0] && armedBitA) {
      processExitDelayStatus(false);
    }
  }

  // Zones status
  byte zonesChanged;
  if (!previousAlarmTriggered && !memoryBlink && !bypassBlink && !troubleBlink && !starKeyDetected) {

    for (byte i = 1; i <= statusByte; i++) {
      for (byte bit = 7; bit <= 7; bit--) {
        if ((!bitRead(zonesTriggered[statusByte - 1], bit) && !alarmBit) || exitDelay[0]) {
          if (bitRead(panelData[statusByte - i], bit)) {
            bitWrite(openZones[i - 1], 7 - bit, 1);
          }
          else bitWrite(openZones[i - 1], 7 - bit, 0);
        }
      }

      zonesChanged = openZones[i - 1] ^ previousOpenZones[i - 1];

      if (zonesChanged) {
        previousOpenZones[i - 1] = openZones[i - 1];
        openZonesStatusChanged = true;
        if (!pauseStatus) statusChanged = true;

        for (byte zoneBit = 0; zoneBit < 8; zoneBit++) {
          if (bitRead(zonesChanged, zoneBit)) {
            bitWrite(openZonesChanged[i - 1], zoneBit, 1);
          }
        }
      }
    }
  }

  // Alarm zones status
  for (byte bit = 7; bit > 1; bit--) {
    if (bitRead(pc16Data[statusByte - 1], bit)) {
      bitWrite(alarmZones[0], 7 - bit, 1);
      bitWrite(zonesTriggered[0], 7 - bit, 1);
    }
    else bitWrite(alarmZones[0], 7 - bit, 0);
  }
  zonesChanged = alarmZones[0] ^ previousAlarmZones[0];
  if (zonesChanged != 0) {
    previousAlarmZones[0] = alarmZones[0];
    alarmZonesStatusChanged = true;
    if (!pauseStatus) statusChanged = true;

    for (byte zoneBit = 0; zoneBit < 8; zoneBit++) {
      if (bitRead(zonesChanged, zoneBit)) {
        bitWrite(alarmZonesChanged[0], zoneBit, 1);

        // Handles zones closed during an alarm
        if (alarmBit) {
          if (bitRead(alarmZones[0], zoneBit)) {
            bitWrite(openZones[0], zoneBit, 1);
            bitWrite(openZonesChanged[0], zoneBit, 1);
            openZonesStatusChanged = true;
          }
          else {
            bitWrite(openZones[0], zoneBit, 0);
            bitWrite(openZonesChanged[0], zoneBit, 1);
            openZonesStatusChanged = true;
          }
          previousOpenZones[0] = openZones[0];
        }
      }
    }
  }

  // Alarm status - requires PGM output section 24 configured to option 08: Strobe Output
  if ((panelData[statusByte] & 0xFE) != 0) {
    if (alarmBit && !memoryBlink) {
      processReadyStatus(false);
      processAlarmStatus(true);
      alarmTriggered = true;
    }
    else if (!memoryBlink && !armedChanged[0]) {
      processAlarmStatus(false);
      if (alarmTriggered) {
        alarmTriggered = false;
        previousAlarmTriggered = true;
      }
    }
  }

  // Trouble status
  trouble = troubleBit;
  if (trouble != previousTrouble) {
    previousTrouble = trouble;
    troubleChanged = true;
    if (!pauseStatus) statusChanged = true;
  }

  // Fire status
  fire[0] = bitRead(pc16Data[statusByte - 1], 0);
  if (fire[0] != previousFire) {
    previousFire = fire[0];
    fireChanged[0] = true;
    if (!pauseStatus) statusChanged = true;
  }

  // Keypad Fire alarm
  if (bitRead(pc16Data[statusByte], 1)) {
    static unsigned long previousFireAlarm = 0;
    if (millis() - previousFireAlarm > 1000) {
      keypadFireAlarm = true;
      previousFireAlarm = millis();
      if (!pauseStatus) statusChanged = true;
    }
  }

  // Keypad Aux alarm
  if (bitRead(pc16Data[statusByte], 2)) {
    static unsigned long previousAuxAlarm = 0;
    if (millis() - previousAuxAlarm > 1000) {
      keypadAuxAlarm = true;
      previousAuxAlarm = millis();
      if (!pauseStatus) statusChanged = true;
    }
  }

  // Keypad Panic alarm
  if (bitRead(pc16Data[statusByte], 3)) {
    static unsigned long previousPanicAlarm = 0;
    if (millis() - previousPanicAlarm > 1000) {
      keypadPanicAlarm = true;
      previousPanicAlarm = millis();
      if (!pauseStatus) statusChanged = true;
    }
  }

  // Status - sets the status to match PowerSeries status codes for sketch compatibility
  if (memoryBlink && bypassBlink && troubleBlink) {
    status[0] = 0xE4;  // Programming
  }
  else if (lights[0]) {
    if (readyChanged[0] || status[0] == 0x8F || status[0] == 0x10 || status[0] == 0xE4) {
      if (ready[0]) status[0] = 0x01;                                     // Partition ready
      else if (openZonesStatusChanged && (openZones[0] || openZones[1])) status[0] = 0x03;  // Zones open
    }

    if (armedChanged[0]) {
      if (armed[0]) {
        if (armedAway[0]) status[0] = 0x05;         // Armed away
        else if (armedStay[0]) status[0] = 0x04;    // Armed stay

        if (noEntryDelay[0]) status[0] = 0x06;      // Armed with no entry delay
      }
      else status[0] = 0x3E;                        // Disarmed
    }

    if (alarmChanged[0]) {
      if (alarm[0]) status[0] = 0x11;               // Alarm
      else if (!armedChanged[0]) status[0] = 0x3E;  // Disarmed
    }

    if (exitDelayChanged[0]) {
      if (exitDelay[0]) status[0] = 0x08;           // Exit delay in progress
      else if (!armed[0]) status[0] = 0x3E;         // Disarmed
    }

    if (status[0] == 0x3E) {
      if (ready[0]) status[0] = 0x01;
      else if (openZones[0] || openZones[1]) status[0] = 0x03;
    }
  }

  if (status[0] != previousStatus) {
    previousStatus = status[0];
    if (!pauseStatus) statusChanged = true;
  }
}


void dscClassicInterface::processReadyStatus(bool status) {
  ready[0] = status;
  if (ready[0] != previousReady) {
    previousReady = ready[0];
    readyChanged[0] = true;
    if (!pauseStatus) statusChanged = true;
  }
}


void dscClassicInterface::processAlarmStatus(bool status) {
  alarm[0] = status;
  if (alarm[0] != previousAlarm) {
    previousAlarm = alarm[0];
    alarmChanged[0] = true;
    if (!pauseStatus) statusChanged = true;
  }
}


void dscClassicInterface::processExitDelayStatus(bool status) {
  exitDelay[0] = status;
  if (exitDelay[0] != previousExitDelay) {
    previousExitDelay = exitDelay[0];
    exitDelayChanged[0] = true;
    if (!pauseStatus) statusChanged = true;
  }
}


void dscClassicInterface::processArmedStatus(bool armedStatus) {
  armedStay[0] = armedStatus;
  armedAway[0] = armedStatus;
  armed[0] = armedStatus;

  if (armed[0] != previousArmed) {
    previousArmed = armed[0];
    armedChanged[0] = true;
    if (!pauseStatus) statusChanged = true;
  }
}


bool dscClassicInterface::handleModule() {
  if (!moduleDataCaptured) return false;
  moduleDataCaptured = false;

  if (moduleBitCount < 8) return false;

  return true;
}

// Sets up writes for a single key
void dscClassicInterface::write(const char receivedKey) {

  // Blocks if a previous write is in progress
  while(writeKeyPending || writeKeysPending) {
    loop();
    #if defined(ESP8266)
    yield();
    #endif
  }

  if (strlen(accessCodeStay) < 4) {
    setWriteKey(receivedKey);
  }
  else {
    switch(receivedKey) {
      case 's':
      case 'S': write(accessCodeStay); break;
      case 'w':
      case 'W': write(accessCodeAway); break;
      case 'n':
      case 'N': write(accessCodeNight); break;
      default: setWriteKey(receivedKey);
    }
  }
}


// Sets up writes for multiple keys sent as a char array
void dscClassicInterface::write(const char *receivedKeys, bool blockingWrite) {

  // Blocks if a previous write is in progress
  while(writeKeyPending || writeKeysPending) {
    loop();
    #if defined(ESP8266)
    yield();
    #endif
  }

  if (strlen(receivedKeys) == 1) {
    write(receivedKeys[0]);
    return;
  }

  writeKeysArray = receivedKeys;

  if (writeKeysArray[0] != '\0') {
    writeKeysPending = true;
    writeReady = false;
  }

  // Optionally blocks until the write is complete, necessary if the received keys char array is ephemeral
  if (blockingWrite) {
    while (writeKeysPending) {
      writeKeys(writeKeysArray);
      loop();
      #if defined(ESP8266)
      yield();
      #endif
    }
  }
  else writeKeys(writeKeysArray);
}


// Writes multiple keys from a char array
void dscClassicInterface::writeKeys(const char *writeKeysArray) {
  static byte writeCounter = 0;

  if (!writeKeyPending && writeKeysPending && writeCounter < strlen(writeKeysArray)) {
    if (writeKeysArray[writeCounter] != '\0') {
      setWriteKey(writeKeysArray[writeCounter]);
      writeCounter++;
      if (writeKeysArray[writeCounter] == '\0') {
        writeKeysPending = false;
        writeCounter = 0;
      }
    }
  }
}


// Specifies the key value to be written by dscClockInterrupt() and selects the write partition.  This includes a 500ms
// delay after alarm keys to resolve errors when additional keys are sent immediately after alarm keys.
void dscClassicInterface::setWriteKey(const char receivedKey) {
  static unsigned long previousTime;

  // Sets the binary to write for virtual keypad keys
  if (!writeKeyPending && (millis() - previousTime > 500 || millis() <= 500)) {
    bool validKey = true;

    // Sets binary for virtual keypad keys
    switch (receivedKey) {
      case '0': writeKey = 0xD7; break;
      case '1': writeKey = 0xBE; break;
      case '2': writeKey = 0xDE; break;
      case '3': writeKey = 0xEE; break;
      case '4': writeKey = 0xBD; break;
      case '5': writeKey = 0xDD; break;
      case '6': writeKey = 0xED; break;
      case '7': writeKey = 0xBB; break;
      case '8': writeKey = 0xDB; break;
      case '9': writeKey = 0xEB; break;
      case '*': writeKey = 0xB7; break;
      case '#': writeKey = 0xE7; break;
      case 'F':
      case 'f': writeKey = 0x3F; writeAlarm = true; break;                    // Keypad fire alarm
      case 'A':
      case 'a': writeKey = 0x5F; writeAlarm = true; break;                    // Keypad auxiliary alarm
      case 'P':
      case 'p': writeKey = 0x6F; writeAlarm = true; break;                    // Keypad panic alarm
      default: {
        validKey = false;
        break;
      }
    }

    if (writeAlarm) previousTime = millis();  // Sets a marker to time writes after keypad alarm keys
    if (validKey) {
      writeKeyPending = true;                 // Sets a flag indicating that a write is pending, cleared by dscClockInterrupt()
      writeReady = false;
    }
  }
}


/*
 *  printPanelMessage() decodes and prints panel data from
 *  panelData[] and pc16Data[]
 */
void dscClassicInterface::printPanelMessage() {

  stream->print(F("Lights: "));
  if (panelData[statusByte]) {
    if (bitRead(panelData[statusByte], 7)) stream->print(F("Ready "));
    if (bitRead(panelData[statusByte], 6)) stream->print(F("Armed "));
    if (bitRead(panelData[statusByte], 5)) stream->print(F("Memory "));
    if (bitRead(panelData[statusByte], 4)) stream->print(F("Bypass "));
    if (bitRead(panelData[statusByte], 3)) stream->print(F("Trouble "));
    if (bitRead(panelData[statusByte], 2)) stream->print(F("Program "));
    if (bitRead(panelData[statusByte], 1)) stream->print(F("Fire "));
  }
  else stream->print(F("none "));

  if (bitRead(panelData[statusByte], 0)) stream->print(F("| Beep "));

  stream->print(F("| Status: "));
  if (pc16Data[statusByte]) {
    if (bitRead(pc16Data[statusByte], 7)) stream->print(F("Trouble "));
    if (bitRead(pc16Data[statusByte], 6)) stream->print(F("Bypassed zones "));
    if (bitRead(pc16Data[statusByte], 5)) stream->print(F("Armed (Side A) "));
    if (bitRead(pc16Data[statusByte], 4)) stream->print(F("Armed (Side B) "));
    if (bitRead(pc16Data[statusByte], 3)) stream->print(F("Keypad Panic alarm "));
    if (bitRead(pc16Data[statusByte], 2)) stream->print(F("Keypad Aux alarm "));
    if (bitRead(pc16Data[statusByte], 1)) stream->print(F("Keypad Fire alarm "));
    if (bitRead(pc16Data[statusByte], 0)) stream->print(F("Alarm "));
  }
  else stream->print(F("none "));

  stream->print(F("| Zone lights: "));
  if ((statusByte == 1 && panelData[statusByte - 1]) == 0 || (statusByte == 2 && panelData[statusByte - 1] == 0 && panelData[statusByte - 2] == 0)) stream->print(F("none "));
  else {
    for (byte i = 1; i <= statusByte; i++) {
      for (byte bit = 7; bit <= 7; bit--) {
        if (bitRead(panelData[statusByte - i], bit)) {
          stream->print((8 - bit) + ((i - 1) * 8));
          stream->print(F(" "));
        }
      }
    }
  }

  if (pc16Data[statusByte - 1] & 0xFE) {
    stream->print(F("| Zone alarm: "));
    for (byte bit = 7; bit > 1; bit--) {
      if (bitRead(pc16Data[statusByte - 1], bit)) {
        if (statusByte == 2 && bit == 3) stream->print("5-8");        // PC3000 alarm zones range
        else if (statusByte == 2 && bit == 2) stream->print("9-16");
        else stream->print(8 - bit);
        stream->print(F(" "));
      }
    }
  }

  if (bitRead(pc16Data[statusByte - 1], 0)) {
    stream->print(F("| Fire alarm"));
  }
}



// Processes keypad and module notifications and responses to panel queries
void dscClassicInterface::printModuleMessage() {

  stream->print(F("[Keypad] "));
  if (hideKeypadDigits) {
    switch (moduleData[0]) {
      case 0xBE:
      case 0xDE:
      case 0xEE:
      case 0xBD:
      case 0xDD:
      case 0xED:
      case 0xBB:
      case 0xDB:
      case 0xEB:
      case 0xD7: Serial.print(F("[Digit]")); break;
      case 0xB7: Serial.print("*"); break;
      case 0xE7: Serial.print("#"); break;
      case 0x3F: Serial.print(F("Fire alarm")); break;
      case 0x5F: Serial.print(F("Aux alarm")); break;
      case 0x6F: Serial.print(F("Panic alarm")); break;
    }
  }

  else {
    switch (moduleData[0]) {
      case 0xBE: Serial.print("1"); break;
      case 0xDE: Serial.print("2"); break;
      case 0xEE: Serial.print("3"); break;
      case 0xBD: Serial.print("4"); break;
      case 0xDD: Serial.print("5"); break;
      case 0xED: Serial.print("6"); break;
      case 0xBB: Serial.print("7"); break;
      case 0xDB: Serial.print("8"); break;
      case 0xEB: Serial.print("9"); break;
      case 0xD7: Serial.print("0"); break;
      case 0xB7: Serial.print("*"); break;
      case 0xE7: Serial.print("#"); break;
      case 0x3F: Serial.print(F("Fire alarm")); break;
      case 0x5F: Serial.print(F("Aux alarm")); break;
      case 0x6F: Serial.print(F("Panic alarm")); break;
    }
  }
}


// Prints the panel message as binary with an optional parameter to print spaces between bytes
void dscClassicInterface::printPanelBinary(bool printSpaces) {
  for (byte panelByte = 0; panelByte < panelByteCount; panelByte++) {
    for (byte mask = 0x80; mask; mask >>= 1) {
      if (mask & panelData[panelByte]) stream->print("1");
      else stream->print("0");
    }
    if (printSpaces && panelByte != panelByteCount - 1) stream->print(" ");
  }

  if (printSpaces) stream->print(" ");

  for (byte panelByte = 0; panelByte < panelByteCount; panelByte++) {
    for (byte mask = 0x80; mask; mask >>= 1) {
      if (mask & pc16Data[panelByte]) stream->print("1");
      else stream->print("0");
    }
    if (printSpaces && panelByte != panelByteCount - 1) stream->print(" ");
  }
}


// Prints the panel message as binary with an optional parameter to print spaces between bytes
void dscClassicInterface::printModuleBinary(bool printSpaces) {
  bool keypadDigit = false;
  if (hideKeypadDigits) {
    switch (moduleData[0]) {
      case 0xBE:
      case 0xDE:
      case 0xEE:
      case 0xBD:
      case 0xDD:
      case 0xED:
      case 0xBB:
      case 0xDB:
      case 0xEB: keypadDigit = true; break;
      default: keypadDigit = false;
    }
  }

  for (byte moduleByte = 0; moduleByte < moduleByteCount; moduleByte++) {
    if (hideKeypadDigits && keypadDigit && moduleByte == 0) stream->print(F("........"));
    else {
      for (byte mask = 0x80; mask; mask >>= 1) {
        if (mask & moduleData[moduleByte]) stream->print("1");
        else stream->print("0");
      }
    }
    if (printSpaces && moduleByte != panelByteCount - 1) stream->print(" ");
  }
}


// Prints the panel command as hex
void dscClassicInterface::printPanelCommand() {
  stream->print(F("Panel"));
}


#if defined(__AVR__)
bool dscClassicInterface::redundantPanelData(byte previousCmd[], volatile byte currentCmd[], byte checkedBytes) {
#elif defined(ESP8266) || defined(ESP32)
bool IRAM_ATTR dscClassicInterface::redundantPanelData(byte previousCmd[], volatile byte currentCmd[], byte checkedBytes) {
#endif

  bool redundantData = true;
  for (byte i = 0; i < checkedBytes; i++) {
    if (previousCmd[i] != currentCmd[i]) {
      redundantData = false;
      break;
    }
  }
  if (redundantData) return true;
  else {
    for (byte i = 0; i < dscDataSize; i++) previousCmd[i] = currentCmd[i];
    return false;
  }
}


// Called as an interrupt when the DSC clock changes to write data for virtual keypad and setup timers to read
// data after an interval.
#if defined(__AVR__)
void dscClassicInterface::dscClockInterrupt() {
#elif defined(ESP8266) || defined(ESP32)
void IRAM_ATTR dscClassicInterface::dscClockInterrupt() {
#endif

  // Data sent from the panel and keypads/modules has latency after a clock change (observed up to 160us for
  // keypad data).  The following sets up a timer for each platform that will call dscDataInterrupt() in
  // 250us to read the data line.

  // AVR Timer1 calls dscDataInterrupt() via ISR(TIMER1_OVF_vect) when the Timer1 counter overflows
  #if defined(__AVR__)
  TCNT1=61535;            // Timer1 counter start value, overflows at 65535 in 250us
  TCCR1B |= (1 << CS10);  // Sets the prescaler to 1

  // esp8266 timer1 calls dscDataInterrupt() in 250us
  #elif defined(ESP8266)
  timer1_write(1250);

  // esp32 timer1 calls dscDataInterrupt() in 250us
  #elif defined(ESP32)
  timerStart(timer1);
  portENTER_CRITICAL(&timer1Mux);
  #endif

  static unsigned long previousClockHighTime;
  if (digitalRead(dscClockPin) == HIGH) {
    if (virtualKeypad) digitalWrite(dscWritePin, LOW);  // Restores the data line after a virtual keypad write
    previousClockHighTime = micros();
  }

  else {
    clockHighTime = micros() - previousClockHighTime;  // Tracks the clock high time to find the reset between commands

    // Virtual keypad
    if (virtualKeypad) {
      static bool writeStart = false;

      if (writeKeyPending && millis() - writeCompleteTime > 50) {
        writeKeyWait = false;
      }

      // Writes a regular key
      if (writeKeyPending && !writeKeyWait) {

        if (clockHighTime > 2000) {
          if (!((writeKey >> 7) & 0x01)) digitalWrite(dscWritePin, HIGH);
          writeStart = true;
        }
        else if (writeStart && isrPanelBitTotal <= 7) {
          if (!((writeKey >> (7 - isrPanelBitCount)) & 0x01)) digitalWrite(dscWritePin, HIGH);
          if (isrPanelBitTotal == 7) {
            writeKeyPending = false;
            writeKeyWait = true;
            writeCompleteTime = millis();
            writeStart = false;
          }
        }
      }
    }
  }
  #if defined(ESP32)
  portEXIT_CRITICAL(&timer1Mux);
  #endif
}


// Interrupt function called by AVR Timer1, esp8266 timer1, and esp32 timer1 after 250us to read the data line
#if defined(__AVR__)
void dscClassicInterface::dscDataInterrupt() {
#elif defined(ESP8266)
void IRAM_ATTR dscClassicInterface::dscDataInterrupt() {
#elif defined(ESP32)
void IRAM_ATTR dscClassicInterface::dscDataInterrupt() {
  timerStop(timer1);
  portENTER_CRITICAL(&timer1Mux);
#endif

  static bool skipData = false;

  // Panel sends data while the clock is high
  if (digitalRead(dscClockPin) == HIGH) {

    // Stops processing Keybus data at the dscDataSize limit
    if (isrPanelByteCount >= dscDataSize) skipData = true;

    else {
      if (isrPanelBitCount < 8) {
        // Data is captured in each byte by shifting left by 1 bit and writing to bit 0
        isrPanelData[isrPanelByteCount] <<= 1;
        isrPC16Data[isrPanelByteCount] <<= 1;

        if (digitalRead(dscReadPin) == HIGH) isrPanelData[isrPanelByteCount] |= 1;
        if (digitalRead(dscPC16Pin) == HIGH) isrPC16Data[isrPanelByteCount] |= 1;
      }

      // Increments the bit counter if the byte is incomplete
      if (isrPanelBitCount < 7) isrPanelBitCount++;

      // Byte is complete, set the counters for the next byte
      else {
        isrPanelBitCount = 0;
        isrPanelByteCount++;
      }

      isrPanelBitTotal++;
    }
  }

  // Keypads and modules send data while the clock is low
  else {
    static bool moduleDataDetected = false;

    // Saves data and resets counters after the clock cycle is complete (high for at least 2ms)
    if (clockHighTime > 2000) {
      keybusTime = millis();

      // Skips incomplete data and redundant data
      if (isrPanelBitTotal < 8) skipData = true;
      else {
        static byte previousPanelData[dscDataSize];
        static byte previousPC16Data[dscDataSize];

        if (lightBlink && readyLight) skipData = false;
        else if (redundantPanelData(previousPanelData, isrPanelData, isrPanelByteCount) &&
                 redundantPanelData(previousPC16Data, isrPC16Data, isrPanelByteCount)) {
          skipData = true;
        }
      }

      // Stores new panel data in the panel buffer
      if (panelBufferLength == dscBufferSize) bufferOverflow = true;
      else if (!skipData && panelBufferLength < dscBufferSize) {
        for (byte i = 0; i < dscDataSize; i++) {
          panelBuffer[panelBufferLength][i] = isrPanelData[i];
          pc16Buffer[panelBufferLength][i] = isrPC16Data[i];
        }
        panelBufferBitCount[panelBufferLength] = isrPanelBitTotal;
        panelBufferByteCount[panelBufferLength] = isrPanelByteCount;
        panelBufferLength++;
      }

      // Stores new keypad and module data - this data is not buffered
      if (processModuleData) {
        if (moduleDataDetected) {
          moduleDataDetected = false;
          moduleDataCaptured = true;  // Sets a flag for handleModule()
          for (byte i = 0; i < dscDataSize; i++) moduleData[i] = isrModuleData[i];
          moduleBitCount = isrModuleBitTotal;
          moduleByteCount = isrModuleByteCount;
        }

        // Resets the keypad and module capture data and counters
        for (byte i = 0; i < dscDataSize; i++) isrModuleData[i] = 0;
        isrModuleBitTotal = 0;
        isrModuleBitCount = 0;
        isrModuleByteCount = 0;
      }

      // Resets the panel capture data and counters
      for (byte i = 0; i < dscDataSize; i++) {
        isrPanelData[i] = 0;
        isrPC16Data[i] = 0;
      }
      isrPanelBitTotal = 0;
      isrPanelBitCount = 0;
      isrPanelByteCount = 0;
      skipData = false;
    }

    // Keypad and module data is not buffered and skipped if the panel data buffer is filling
    if (processModuleData && isrModuleByteCount < dscDataSize && panelBufferLength <= 1) {

      // Data is captured in each byte by shifting left by 1 bit and writing to bit 0
      if (isrModuleBitCount < 8) {
        isrModuleData[isrModuleByteCount] <<= 1;
        if (digitalRead(dscReadPin) == HIGH) {
          isrModuleData[isrModuleByteCount] |= 1;
        }
        else moduleDataDetected = true;  // Keypads and modules send data by pulling the data line low
      }

      // Byte is complete, set the counters for the next byte
      if (isrModuleBitCount == 7) {
        isrModuleBitCount = 0;
        isrModuleByteCount++;
        if (moduleDataDetected && isrModuleData[0] == 0xB7) {
          starKeyDetected = true;
        }
      }

      // Increments the bit counter if the byte is incomplete
      else if (isrModuleBitCount < 7) {
        isrModuleBitCount++;
      }

      isrModuleBitTotal++;
    }

  }
  #if defined(ESP32)
  portEXIT_CRITICAL(&timer1Mux);
  #endif
}
