/* 
 *    ESP Nixie Clock 
 *	  Copyright (C) 2019  Larry McGovern
 *	
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License <http://www.gnu.org/licenses/> for more details.
 */
	
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <Timezone.h>
#include <Wire.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <avdweb_Switch.h>

#define AP_NAME "ESPCLOCK"
#define AP_PASSWORD "PASSWORD"

#define CLOCK_COLON //  Comment out if not using colon separator PCB

#define D0 16 // LED_BUILTIN
#define D1 5 // I2C Bus SCL (clock)
#define D2 4 // I2C Bus SDA (data)
#define D3 0 // 
#define D4 2 //  Blue LED 
#define D5 14 // SPI Bus SCK (clock)
#define D6 12 // SPI Bus MISO 
#define D7 13 // SPI Bus MOSI
#define D8 15 // SPI Bus SS (CS) 
#define D9 3 // RX0 (Serial console) 
#define D10 1 // TX0 (Serial console) 

const int dataPin  = D5; // SER (pin 14)
const int latchPin = D6; // RCLK (pin 12)
const int clockPin = D7; // SRCLK (pin 11)

const int encoderPinA = D9;
const int encoderPinB = D10;
const int encoderButtonPin = D0;

const int colonPin = D8;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.nist.gov", 0, 7200000); // Update time every two hours

// Set timezone rules.  Offsets set to zero, since they will be loaded from EEPROM
TimeChangeRule myDST = {"DST", Second, Sun, Mar, 2, 0};    
TimeChangeRule mySTD = {"STD", First, Sun, Nov, 2, 0};     
Timezone myTZ(myDST, mySTD);

#define OLED_RESET  LED_BUILTIN
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int debouncePeriod = 50;
const int longPressPeriod = 3000;  // Set longPressPeriod to 3 seconds.  Will manually turn on/off Nixies if button held for longer than 3 sec.
Switch encoderButton = Switch(encoderButtonPin, INPUT_PULLUP, LOW, debouncePeriod, longPressPeriod);

 
int encoderPos, encoderPosPrev;

enum Menu {
  TOP,
  SETTINGS1,
  SETTINGS2,
  SET_UTC_OFFSET,
  ENABLE_DST,
  SET_12_24,
  BLINK_COLON,
  CATHODE_PROTECT,
  AUTO_SHUTOFF,
  AUTO_SHUTOFF_ENABLE,
  AUTO_SHUTOFF_OFFTIME,
  AUTO_SHUTOFF_ONTIME,
  SHOW_ZERO,
  RESET_WIFI
} menu;

// EEPROM addresses
const int EEPROM_addr_UTC_offset  = 0; 
const int EEPROM_addr_DST         = 1;  
const int EEPROM_addr_12_24       = 2; 
const int EEPROM_addr_protect     = 3; 
const int EEPROM_addr_shutoff_en  = 4;
const int EEPROM_addr_shutoff_off = 5;
const int EEPROM_addr_shutoff_on  = 6;
const int EEPROM_addr_showzero    = 7;
const int EEPROM_addr_blink_colon = 8;

bool enableDST;   // Flag to enable DST
bool set12_24;    // Flag indicating 12 vs 24 hour time (0 = 12, 1 = 24);
bool showZero;    // Flag to indicate whether to show zero in the hour ten's place
bool enableBlink; // Flag to indicate whether center colon should blink

uint8_t interval_indx; // Cathode protection interval index
const int num_intervals = 6;
const int intervals[num_intervals] = {0, 15, 30, 60, 120, 240};  // Intervals in minutes, with 0 = off

bool enableAutoShutoff; // Flag to enable/disable nighttime shut off
int autoShutoffOfftime, autoShutoffOntime;  // On and off times from 0 to 95 in 15 minute intervals

time_t protectTimer = 0, menuTimer = 0;
bool nixieOn = true, manualOverride = false;
bool initProtectionTimer = false;  // Set true at the top of the hour to synchronize protection timer with clock

void setup() {
  pinMode(dataPin, OUTPUT);
  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT); 
  pinMode(encoderPinA, INPUT_PULLUP);
  pinMode(encoderPinB, INPUT_PULLUP);
  pinMode(colonPin, OUTPUT);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // OLED I2C Address, may need to change for different device,
                                              // Check with I2C_Scanner
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.clearDisplay();
  
  display.setCursor(0,0);
  display.println("Connecting...");
  display.println();
  display.println("Cycle Power after a");
  display.println("few minutes if no");
  display.print("connection.");
  display.display();

    // Setup WiFiManager
  WiFiManager MyWifiManager;
  MyWifiManager.setAPCallback(configModeCallback);
  MyWifiManager.autoConnect(AP_NAME,AP_PASSWORD); // Default password is PASSWORD, change as needed
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("Wifi Connected");
  display.display();

  timeClient.begin();

  display.setCursor(0,28);
  display.println("Updating local time");
  display.display();
  while (!timeClient.update()) {
    delay(500);
    display.print(".");
    display.display();
  }

  EEPROM.begin(9);
  
  // Read UTC offset from EEPROM
  int utc_offset = (((int)EEPROM.read(EEPROM_addr_UTC_offset)+12) % 24) - 12;

  // Read Daylight Savings Time setting from EEPROM
  enableDST = EEPROM.read(EEPROM_addr_DST) != 0;

  mySTD.offset = utc_offset * 60;
  myDST.offset = mySTD.offset;
  if (enableDST) {
    myDST.offset += 60;
  }
  myTZ = Timezone(myDST, mySTD);

  // Read 12/24 hour setting from EEPROM
  set12_24 = EEPROM.read(EEPROM_addr_12_24) != 0;

  // Read cathode proection interval from EEPROM
  interval_indx = EEPROM.read(EEPROM_addr_protect);
  if (interval_indx >= num_intervals) interval_indx = 0;

  // Read auto shutoff settings
  enableAutoShutoff  = EEPROM.read(EEPROM_addr_shutoff_en) != 0;
  autoShutoffOfftime = (int)EEPROM.read(EEPROM_addr_shutoff_off);
  autoShutoffOntime  = (int)EEPROM.read(EEPROM_addr_shutoff_on);
  if (autoShutoffOfftime > 95) autoShutoffOfftime = 0;  // 96 15-minute intervals in day
  if (autoShutoffOntime > 95) autoShutoffOntime = 0;

  // Read show zero setting from EEPROM
  showZero = EEPROM.read(EEPROM_addr_showzero) != 0;

  // Read colon blink setting from EEPROM
  enableBlink = EEPROM.read(EEPROM_addr_blink_colon) != 0;
  
  menu = TOP;
  updateSelection();

  timeClient.update();
  setTime(myTZ.toLocal(timeClient.getEpochTime()));
  displayTime();
}

void loop() {  
  updateEncoderPos();
  encoderButton.poll();

  // If button held for longer than 3 seconds, toggle Nixie tubes on/off
  if (encoderButton.longPress()){
    manualOverride = true;  // Overrides auto shutoff for this period
    nixieOn = !nixieOn;
    menu = TOP;
    updateSelection(); 
  }

  static bool initial_loop = 1;
  if (encoderButton.pushed()) {
    menuTimer = now();  
    if (initial_loop == 1) {
      initial_loop = 0;  // Ignore first push
    }
    else {
      updateMenu();
    }
   }

  timeClient.update();
  setTime(myTZ.toLocal(timeClient.getEpochTime()));

  // Cathode protection logic
  if (interval_indx > 0) {  // Cathode protection is enabled
    if ((menu != SET_UTC_OFFSET) && (menu != ENABLE_DST)) { // Changing UTC offset or DST setting can disrupt protection timer
      // At the first top of the hour, initialize cathode protection logic timer
      if (!initProtectionTimer && (minute() == 0)) {  
        protectTimer = 0;   // Ensures protection logic will be immediately triggered                                     
        initProtectionTimer = true;
      }
      if ((now() - protectTimer) >= 60 * intervals[interval_indx]) {
        protectTimer = now();
        // The current time can drift slightly relative to the protectTimer when NIST time is updated
        // Need to make a small adjustment to the timer to ensure it is triggered at the minute change
        protectTimer -= ((second()+30)%60 - 30);  
        if (nixieOn) cathodeProtect();
      }      
    }
  }
  
  static time_t prevTime = 0;
  if (now() != prevTime) {
    prevTime = now();
    evalShutoffTime();
    displayTime();
  }
  // Reset screen to top level if encoder inactive for more than 60 seconds
  // Note that UTC offset and DST setting changes can change now() by one hour
  // (3600 seconds), so modulus operator is needed
  if ((mod(now() - menuTimer, 3600) > 60) && (menu != TOP)) { 
    menu = TOP;
    updateSelection();      
  }
}

void cathodeProtect() {
  int hour12_24 = set12_24 ? (unsigned char)hour() : (unsigned char)hourFormat12();
  unsigned char hourBcd = decToBcd((unsigned char)hour12_24);
  unsigned char minBcd  = decToBcd((unsigned char)minute());
  unsigned char dh1 = (hourBcd >> 4), dh2 = (hourBcd & 15); 
  unsigned char dm1 = (minBcd >> 4),  dm2 = (minBcd & 15);

  // All four digits will increment up at 10 Hz.
  // At T=2 sec, individual digits will stop at 
  // the correct time every second starting from 
  // the right and ending on the left
  for (int i = 0; i <= 50; i++){
    if (i >= 20) dm2 = (minBcd & 15); 
    if (i >= 30) dm1 = (minBcd >> 4); 
    if (i >= 40) dh2 = (hourBcd & 15);
    if (i == 50) dh1 = (hourBcd >> 4);
    digitalWrite(latchPin, LOW);
    shiftOut(dataPin, clockPin, MSBFIRST, (dh1 << 4) | dh2);
    shiftOut(dataPin, clockPin, MSBFIRST, (dm1 << 4) | dm2);
    digitalWrite(latchPin, HIGH);
    incMod10(dh1); incMod10(dh2);
    incMod10(dm1); incMod10(dm2);
    delay(100);
  }
}

inline void incMod10(unsigned char &x) { x = (x + 1 == 10 ? 0: x + 1); };

void displayTime(){
   char tod[10], time_str[20], date_str[20];
   const char* am_pm[] = {"AM", "PM"};
   const char* month_names[] = {"Jan", "Feb", "March", "April", "May", "June", "July", "Aug", "Sept", "Oct", "Nov", "Dec"};
   int hour12_24 = set12_24 ? (unsigned char)hour() : (unsigned char)hourFormat12();
   unsigned char hourBcd = decToBcd((unsigned char)hour12_24);
   unsigned char minBcd  = decToBcd((unsigned char)minute());
   bool colonBlinkState = (bool)(second() % 2);

   if (!showZero && ((hourBcd >> 4) == 0)) { // If 10's digit is zero, we don't want to display a zero
    hourBcd |= (15 << 4); 
   }

   if (!nixieOn) {
    hourBcd = 255; 
    minBcd = 255;
    colonBlinkState = false;
   }
   else if (!enableBlink) {
    colonBlinkState = true;
   }
   
   // Write to shift register
   digitalWrite(latchPin, LOW);
   shiftOut(dataPin, clockPin, MSBFIRST, hourBcd);
   shiftOut(dataPin, clockPin, MSBFIRST, minBcd);
   digitalWrite(latchPin, HIGH);   

   digitalWrite(colonPin, colonBlinkState); // Blink colon pin
   
   if ((menu == TOP) || (menu == SET_UTC_OFFSET)) {
      formattedTime(tod, hour12_24, minute(), second());
      sprintf(time_str, "%s %s", tod, am_pm[isPM()]);
      sprintf(date_str, "%s %d, %d", month_names[month() - 1], day(), year());
      display.fillRect(20,28,120,8,BLACK);
      display.setCursor(20,28);
      display.print(time_str);
      if (enableDST) {
        if (myTZ.utcIsDST(timeClient.getEpochTime())) {
          display.print(" DST");
        }
        else {
          display.print(" STD");
        }
      }
      display.setCursor(20,36);
      display.print(date_str);
      display.display();
   }
}

unsigned char decToBcd(unsigned char val)
{
  return ( ((val/10)*16) + (val%10) );
}

#define colonDigit(digit) digit < 10 ? ":0" : ":"
void formattedTime(char *tod, int hours, int minutes, int seconds)
{
  sprintf(tod, "%d%s%d%s%d", hours, colonDigit(minutes), minutes, colonDigit(seconds), seconds);  // Hours, minutes, seconds
}

void evalShutoffTime() {  // Determine whether Nixie tubes should be turned off
  
  if (!enableAutoShutoff) return;
  
  int mn = 60*hour() + minute();
  int mn_on = 15*autoShutoffOntime;  
  int mn_off = 15*autoShutoffOfftime;

  static bool prevShutoffState = true;  
  if ( ((mn_off < mn_on) &&  (mn > mn_off) && (mn < mn_on)) ||  // Nixies should be off
        (mn_off > mn_on) && ((mn > mn_off) || (mn < mn_on))) { 
     if (!manualOverride) nixieOn = false;
     if (prevShutoffState == true) manualOverride = false; 
     prevShutoffState = false;
  }
  else {  // Nixies should be on
    if (!manualOverride) nixieOn = true;
    if (prevShutoffState == false) manualOverride = false; 
    prevShutoffState = true;
  }
    
}

void updateEncoderPos() {
    static int encoderA, encoderB, encoderA_prev;   

    encoderA = digitalRead(encoderPinA); 
    encoderB = digitalRead(encoderPinB);
 
    if((!encoderA) && (encoderA_prev)){ // A has gone from high to low 
      encoderPosPrev = encoderPos;
      encoderB ? encoderPos++ : encoderPos--;  
      menuTimer = now();
      if (menu != TOP) {
        updateSelection();
      }    
    }
    encoderA_prev = encoderA;     
}

#ifdef CLOCK_COLON
  const int n_set1 = 6;  // 6 menu items on Settings 1
#else
  const int n_set1 = 5;  // Otherwise, only 5 items
#endif

void updateMenu() {  // Called whenever button is pushed

  switch (menu) {
    case TOP:
      menu = SETTINGS1;
      break;
    case SETTINGS1:
      switch (mod(encoderPos,n_set1)) {
        case 0: // Timezone Offset
          menu = SET_UTC_OFFSET;
          break;
        case 1: // Enable DST
          menu = ENABLE_DST;
          break;
        case 2: // 12/24 Hours
          menu = SET_12_24;
          break;
          
#ifdef CLOCK_COLON
        case 3: // Blink Colon
          menu = BLINK_COLON;
          break;  
        case 4: // More Options
          menu = SETTINGS2;
          break;
        case 5: // Return
          menu = TOP;
          break;
#else
        case 3: // More Options
          menu = SETTINGS2;
          break;
        case 4: // Return
          menu = TOP;
          break;
#endif
      }
      break;
    case SETTINGS2:
      switch (mod(encoderPos,5)) {
        case 0: // Cathod Protection
          menu = CATHODE_PROTECT;
          break;
        case 1: // Auto Shut off
          menu = AUTO_SHUTOFF;
          break;
        case 2: // Show Zero
          menu = SHOW_ZERO;
          break;
        case 3: // Reset Wifi
          menu = RESET_WIFI;
          break;
        case 4: // Return
          menu = SETTINGS1;
          break;
      }
      break;
      
    case RESET_WIFI:
      if (mod(encoderPos, 2) == 1){  // Selection = YES
        resetWiFi();
        menu = TOP; 
      }
      else {  // Selection = NO
        menu = SETTINGS1;
      }
      break;
      
    case SET_UTC_OFFSET:
      EEPROM.write(EEPROM_addr_UTC_offset, (unsigned char)(mod(mySTD.offset/60,24))); 
      EEPROM.commit();
      initProtectionTimer = false;
      menu = SETTINGS1;
      break;
      
    case ENABLE_DST:
      EEPROM.write(EEPROM_addr_DST, (unsigned char)enableDST);
      EEPROM.commit();
      initProtectionTimer = false;
      menu = SETTINGS1; 
      break;
      
    case SET_12_24:
      EEPROM.write(EEPROM_addr_12_24, (unsigned char)set12_24);
      EEPROM.commit();
      menu = SETTINGS1;
      break; 

    case BLINK_COLON:
      EEPROM.write(EEPROM_addr_blink_colon, (unsigned char)enableBlink);
      EEPROM.commit();
      menu = SETTINGS1;
      break;
     
    case CATHODE_PROTECT: 
      EEPROM.write(EEPROM_addr_protect, interval_indx);
      EEPROM.commit();
      initProtectionTimer = false;
      protectTimer = 0;
      menu = SETTINGS2;
      break;
      
    case AUTO_SHUTOFF: 
      switch (mod(encoderPos,4)) {
        case 0: 
          menu = AUTO_SHUTOFF_ENABLE;
          break;
        case 1: 
          menu = AUTO_SHUTOFF_OFFTIME;
          break;
        case 2: 
          menu = AUTO_SHUTOFF_ONTIME;
          break;
        case 3: 
          menu = SETTINGS2;
          break;
      }
      break;
      
    case AUTO_SHUTOFF_ENABLE:
      EEPROM.write(EEPROM_addr_shutoff_en, (unsigned char)enableAutoShutoff);
      EEPROM.commit();
      menu = AUTO_SHUTOFF;
      break;
      
    case AUTO_SHUTOFF_OFFTIME:  
      EEPROM.write(EEPROM_addr_shutoff_off, (unsigned char)autoShutoffOfftime);
      EEPROM.commit();
      menu = AUTO_SHUTOFF;
      break;
      
    case AUTO_SHUTOFF_ONTIME: 
      EEPROM.write(EEPROM_addr_shutoff_on, (unsigned char)autoShutoffOntime);
      EEPROM.commit();
      menu = AUTO_SHUTOFF;
      break;
      
    case SHOW_ZERO: 
      EEPROM.write(EEPROM_addr_showzero, (unsigned char)showZero);
      EEPROM.commit();
      menu = SETTINGS2;
      break;
  }
  encoderPos = 0;  // Reset encoder position
  encoderPosPrev = 0;
  updateSelection(); // Refresh screen
}

void updateSelection() { // Called whenever encoder is turned
  int UTC_STD_Offset, dispOffset;
  
  display.clearDisplay();
  switch (menu) {
    case TOP:
      display.setTextColor(WHITE,BLACK);
      display.setCursor(0,0);
      display.print("Wifi Connected");
      display.setCursor(0,56);
      display.print("Click for settings");
      break;
      
    case ENABLE_DST:
      if (encoderPos != encoderPosPrev) // Encoder has turned
        enableDST = !enableDST;
      myDST.offset = mySTD.offset;
      if (enableDST) {
        myDST.offset += 60;
      }
      myTZ = Timezone(myDST, mySTD);   
      // No break statement, continue through next case
      
    case SET_12_24:
      if (menu == SET_12_24 && encoderPos != encoderPosPrev) 
        set12_24 = !set12_24;
      displayTime();
      // No break statement, continue through next case

     case BLINK_COLON:
      if (menu == BLINK_COLON && encoderPos != encoderPosPrev) 
        enableBlink = !enableBlink;
      // No break statement, continue through next case
      
    case SETTINGS1:
      display.setCursor(0,0); 
      display.setTextColor(WHITE,BLACK);
      display.print("SETTINGS (1 of 2)");
      display.setCursor(0,16);
      
      if (menu == SETTINGS1) setHighlight(0,n_set1);
      display.println("Set UTC Offset  ");
      
      if (menu == SETTINGS1) setHighlight(1,n_set1);
      display.print("Auto DST        ");  
      if (menu == ENABLE_DST) display.setTextColor(BLACK,WHITE);
      else display.setTextColor(WHITE,BLACK);
      display.println( enableDST ? "On " : "Off" );
      
      if (menu == SETTINGS1) setHighlight(2,n_set1);
      else display.setTextColor(WHITE,BLACK);
      display.print("12/24 Hours     ");
      if (menu == SET_12_24) display.setTextColor(BLACK,WHITE);
      else display.setTextColor(WHITE,BLACK);
      display.println( set12_24 ? "24" : "12" );

#ifdef CLOCK_COLON
      if (menu == SETTINGS1) setHighlight(3,n_set1);
      display.print("Blink Colon     ");  
      if (menu == BLINK_COLON) display.setTextColor(BLACK,WHITE);
      else display.setTextColor(WHITE,BLACK);
      display.println( enableBlink ? "On " : "Off" );
#endif
      
      if (menu == SETTINGS1) setHighlight(n_set1-2,n_set1);
      else display.setTextColor(WHITE,BLACK);
      display.println("More Options    ");
      
      if (menu == SETTINGS1) setHighlight(n_set1-1,n_set1);
      display.println("Return          ");
      break;

    case CATHODE_PROTECT: 
      if (encoderPos == 0 && encoderPosPrev == 0) // Encoder position was initialized
        encoderPos = interval_indx;
      interval_indx = mod(encoderPos, num_intervals);
      // No break statement, continue through next case
        
    case SHOW_ZERO: 
      if (menu == SHOW_ZERO && encoderPos != encoderPosPrev) {
        showZero = !showZero;
        displayTime();
      }
      // No break statement, continue through next case
    case SETTINGS2:
      display.setCursor(0,0); 
      display.setTextColor(WHITE,BLACK);
      display.print("SETTINGS (2 of 2)");
      display.setCursor(0,16);
      
      if (menu == SETTINGS2) setHighlight(0,5);
      display.print("Protect Cathode ");
      if (menu == CATHODE_PROTECT) display.setTextColor(BLACK,WHITE);
      else display.setTextColor(WHITE,BLACK);
      if (interval_indx == 0)
        display.println("Off");
      else
        display.println(intervals[interval_indx]);
      
      if (menu == SETTINGS2) setHighlight(1,5);
      else display.setTextColor(WHITE,BLACK);
      display.print("Auto Shut Off   ");
      display.setTextColor(WHITE,BLACK);
      display.println( enableAutoShutoff ? "On " : "Off" );
      
      if (menu == SETTINGS2) setHighlight(2,5);
      else display.setTextColor(WHITE,BLACK);
      display.print("Show Zero       ");
      if (menu == SHOW_ZERO) display.setTextColor(BLACK,WHITE);
      else display.setTextColor(WHITE,BLACK);
      display.println( showZero ? "On " : "Off" );
      
      if (menu == SETTINGS2) setHighlight(3,5);
      else display.setTextColor(WHITE,BLACK);
      display.println("Reset Wifi      ");
      
      if (menu == SETTINGS2) setHighlight(4,5);
      display.println("Return          ");
      break;

    case RESET_WIFI:
      display.setCursor(0,0); 
      display.setTextColor(WHITE,BLACK);
      display.print("RESET WIFI?");
      display.setCursor(0,16);
      setHighlight(0,2);
      display.println("No                   ");
      setHighlight(1,2);
      display.println("Yes                  ");
      break; 
       
    case SET_UTC_OFFSET:
      UTC_STD_Offset = mySTD.offset/60;
      if (encoderPos > encoderPosPrev) {
        UTC_STD_Offset = ((UTC_STD_Offset + 12 + 1) % 24) - 12;
      } else if (encoderPos < encoderPosPrev) {
        UTC_STD_Offset = ((UTC_STD_Offset + 12 - 1) % 24) - 12;
      }
      mySTD.offset = UTC_STD_Offset * 60;
      myDST.offset = mySTD.offset;
      if (enableDST) {
        myDST.offset += 60;
      }
      myTZ = Timezone(myDST, mySTD);
      
      display.setCursor(0,0);
      display.setTextColor(WHITE,BLACK);
      display.println("SET TIMEZONE OFFSET");
      display.println();
      display.print("    UTC ");
      display.print(UTC_STD_Offset >= 0 ? "+ " : "- ");
      dispOffset = UTC_STD_Offset;
      if (enableDST) {
        if (myTZ.utcIsDST(timeClient.getEpochTime())) {
          dispOffset += 1;  // Include DST in UTC offset
        }
      }
      display.print(abs(dispOffset));
      display.print(" hours");
      displayTime();

      display.setCursor(0,48);
      display.println("Press knob to");
      display.print("confirm offset");
      break;

    case AUTO_SHUTOFF_ENABLE:
      if (encoderPos != encoderPosPrev) // Encoder has turned
        enableAutoShutoff = !enableAutoShutoff;
      // No break statement, continue through next case
      
    case AUTO_SHUTOFF_OFFTIME:
      if (menu == AUTO_SHUTOFF_OFFTIME) {
        if (encoderPos == 0 && encoderPosPrev == 0) // Encoder position was initialized
          encoderPos = autoShutoffOfftime;
        autoShutoffOfftime = mod(encoderPos, 96);
      }
      // No break statement, continue through next case
      
    case AUTO_SHUTOFF_ONTIME:
      if (menu == AUTO_SHUTOFF_ONTIME) {
        if (encoderPos == 0 && encoderPosPrev == 0) // Encoder position was initialized
          encoderPos = autoShutoffOntime;
        autoShutoffOntime = mod(encoderPos, 96);
      }
      // No break statement, continue through next case
      
    case AUTO_SHUTOFF: 
      display.setCursor(0,0);
      display.setTextColor(WHITE,BLACK);
      display.println("AUTO SHUT-OFF");
      display.setCursor(0,16);
      
      if (menu == AUTO_SHUTOFF) setHighlight(0,4);
      display.print("Enable        ");
      if (menu == AUTO_SHUTOFF_ENABLE) display.setTextColor(BLACK,WHITE);
      else display.setTextColor(WHITE,BLACK);
      display.println( enableAutoShutoff ? "On " : "Off" );
      
      if (menu == AUTO_SHUTOFF) setHighlight(1,4);
      else display.setTextColor(WHITE,BLACK);
      display.print("Turn Off Time ");
      if (menu == AUTO_SHUTOFF_OFFTIME) display.setTextColor(BLACK,WHITE);
      else display.setTextColor(WHITE,BLACK);
      char timestr[7];
      int hr, mn;
      fifteenMinToHM(hr, mn, autoShutoffOfftime);
      sprintf(timestr, "%d%s%d", hr, colonDigit(mn), mn);  
      display.println(timestr);
      
      if (menu == AUTO_SHUTOFF) setHighlight(2,4);
      else display.setTextColor(WHITE,BLACK);
      display.print("Turn On Time  ");
      if (menu == AUTO_SHUTOFF_ONTIME) display.setTextColor(BLACK,WHITE);
      else display.setTextColor(WHITE,BLACK);
      fifteenMinToHM(hr, mn, autoShutoffOntime);
      sprintf(timestr, "%d%s%d", hr, colonDigit(mn), mn);  
      display.println(timestr);
            
      if (menu == AUTO_SHUTOFF) setHighlight(3,4);
      else display.setTextColor(WHITE,BLACK);
      display.println("Return          ");
      break;
      
      break;
  }
  display.display(); 
}

void fifteenMinToHM(int &hours, int &minutes, int fifteenMin)
{
  hours = fifteenMin/4;
  minutes = (fifteenMin % 4) * 15; 
}

void resetWiFi(){
  WiFiManager MyWifiManager;
  MyWifiManager.resetSettings();
  delay(1000);
  ESP.restart();
}

void setHighlight(int menuItem, int numMenuItems) {
  if (mod(encoderPos, numMenuItems) == menuItem) {
    display.setTextColor(BLACK,WHITE);
  }
  else {
    display.setTextColor(WHITE,BLACK);
  }
}

inline int mod(int a, int b)
{
    int r = a % b;
    return r < 0 ? r + b : r;
}

void configModeCallback (WiFiManager *myWiFiManager) {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("To configure Wifi,  ");
  display.println("connect to Wifi ");
  display.print("network: ");
  display.println(AP_NAME);
  display.print("password: ");
  display.println(AP_PASSWORD);
  display.println("Open 192.168.4.1");
  display.println("in web browser");
  display.display(); 
}
