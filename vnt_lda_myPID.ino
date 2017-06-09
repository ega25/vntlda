/* 
 VNT LDA Controller - based on "Standalone VNT Controller" by DMN - http://dmn.kuulalaakeri.org/vnt-lda/
 - Rewritten PID loop
 - Support added for EGT probe with LDA control based on EGTs
 - Support for EMP sensor to display EMP values (no control map yet)
 - Other various small changes
 
 PID loop code based on https://mbed.org/users/aberk/code/PID/docs/6e12a3e5af19/classPID.html which ironically
 is based on the Arduino PID lib that I tried earlier with poor results
 */

#include <avr/pgmspace.h>
#include <EEPROM.h>
#include <Wire.h> 
#include <SoftwareSerial.h>  
#include <SPI.h>
#include <MAX31855.h>    // Use library written for a faster read time

#define PIN_BUTTON A5
#define PIN_HEARTBEAT 13

#define PIN_TEMP2 A3
#define PIN_MAP A1
#define PIN_TPS A0
#define PIN_EMP A2

#define PIN_RPM_TRIGGER 2
#define PIN_VNT_N75 11    
#define PIN_AUX_N75 3
#define PIN_OUTPUT1 7
#define PIN_OUTPUT2 12

#define PIN_LCD 8

#define LCD_FORCE_INIT 1

// Pins for the EGT MAX31855
#define doPin 4
#define csPin 5
#define clPin 6

#define EGT_COOL 140
#define EGT_WARN 700
#define EGT_ALARM 775
#define EGT_MAX_READ 1101

#define IDLE_MAX_RPM 1150

/* Scaling factor for your sensors - 255 divided by this should equal the full scale deflection of your sensor */
#define MAP_SCALING_KPA 0.977 
#define EMP_SCALING_KPA 1.953 

/* Change this if you need to adjust the scaling of the PID outputs - ie if you need finer control at smaller fractional numbers increase this
 or if you need to have large multipliers then decrease this */
#define PIDControlRatio 50

/* Help reduce overshoot by increasing the falloff rate of the integral when we have a large error.  Whenever the positive error (overboost) exceeds this
 value the integral gain will be doubled causing the integral to decrease faster. */
#define maxPosErrorPct 0.10

/* If your turbo boosts higher than your sensor then the system will not be able to respond in a proportional manner.  If boost is higher than
 PIDMaxBoost% then the controller will double the proportional response to reduce boost faster.  This value is a percentage so it should be
 fine with different sensors and boost ranges - the value is "% of the maximum value of your sensor" so 0.95 * 250kPa = 238kPa for a 2.5 bar
 sensor. */
#define PIDMaxBoost 0.95

/* The resolution we use to calculate RPM - we are only going to calculate RPM ever 'n' number of teeth that pass by; otherwise we are going to have
 a jittery value.  Divide this value by the 'Teeth per Rotation' setting to know how many revolutions before we caculate RPM. */
#define rpmResolution 10

/* If boost is below spoolMinBoost then the turbo hasn't spooled yet - we don't start integrating till we see some signs of life otherwise we
 get all wound up.  preSpoolInt is a static value that the system will use till we see enough boost to start actually controlling things. 
 preSpoolProp is a static value */
#define preSpoolInt 0.75
#define preSpoolProp 0.25
#define spoolMinBoost 10 // kpa

/* Overshoot reduction - when we have a steep upwards slope and we're approaching the setpoint we'll start hacking away at the integral early */
#define rampThreshold 0.025
#define rampFactor 1.5
#define rampActive 0.20 // kPa value divided by max to yield percentage

/* More overshoot reduction - when we have a steep upwards slope but we're below setpoint multiply Kp by underGain.  When we're over then use
   overGain */
#define underGain 0.7
#define overGain 1.3

/* Fine control ratios */
#define fineBand 0.03
#define fineGain 0.5

/* RPM Smoothing control */
#define rpmSmoothing 0.5 // Value between >0 and 1.0 - the closer to 1.0 the less dampening and the faster the RPM values will respond

// Set up the LCD pin
SoftwareSerial lcd = SoftwareSerial(0,PIN_LCD); 

// Set up the thermocouple pins (Adafruit MAX31855 thermocouple interface)
//Adafruit_MAX31855 thermocouple(clPin, csPin, doPin);
MAX31855 temp(doPin, csPin, clPin );

// Set loop delay times
#define SERIAL_DELAY 257 // ms
#define EXEC_DELAY 80 //ms
#define DISPLAY_DELAY 250 // ms


// Calculate avarage values 
#define AVG_MAX 5 

#define MAP_AXIS_TPS 0xDE
#define MAP_AXIS_RPM 0xAD
#define MAP_AXIS_KPA 0xDD
#define MAP_AXIS_CELSIUS 0xAA
#define MAP_AXIS_VOLTAGE 0xAB
#define MAP_AXIS_DUTY_CYCLE 0xAC
#define MAP_AXIS_RAW 0x0
#define MAP_AXIS_EGT 0xAE

unsigned char *auxMap,*boostRequest,*boostDCMax,*boostDCMin;

/*
MAP format:
 
 'M','2','D'   // D - interpolated maps, d - nearest neighbor
 xsize,ysize,x-axis-type,y-axis-type,output-type,
 data[xsize,ysize],
 lastX,lastY,lastRet // automatically filled when used mapLookup
 
 */

unsigned char auxMap1[] = {
  'M','2','D',
  0x5,0x8,MAP_AXIS_RPM,MAP_AXIS_EGT,MAP_AXIS_DUTY_CYCLE,     // 01 - new version 
  0,0,0,0,0,
  0,0,0,0,0,
  0,0,0,0,0,
  0,0,0,0,0,
  0,0,0,0,0,
  0,0,0,0,0,
  60,60,60,60,255,
  255,255,255,255,255,
  00,00,00,                  // lastX,lastY,lastRet
};


unsigned char boostRequest1[] = {
  'M','2','D',
  0xA,0xA,MAP_AXIS_RPM,MAP_AXIS_TPS,MAP_AXIS_KPA,     // 01 - new version 
  0,20,20,20,20,20,20,20,20,20,
  0,20,20,20,20,20,20,20,20,20,
  0,40,40,40,40,40,40,40,40,20,
  0,65,65,65,65,65,65,65,65,40,
  0,65,65,65,65,65,65,65,65,40,
  0,83,83,83,83,83,83,83,83,80,
  0,83,83,83,83,83,83,83,83,90,
  0,83,100,100,100,100,100,100,100,90,
  0,50,85,190,185,200,200,200,200,90,
  0,50,85,190,185,200,200,200,200,90,
  00,00,00,                  // lastX,lastY,lastRet
};

unsigned char boostDCMax1[] = {
  'M','2','D',
  0x8,0xA,MAP_AXIS_RPM,MAP_AXIS_TPS,MAP_AXIS_DUTY_CYCLE,
  200,200,200,200,200,200,200,80,
  200,200,200,200,200,200,200,80,
  200,200,200,200,200,200,200,80,
  200,200,200,200,200,200,200,80,
  200,200,200,200,200,200,200,80,
  200,200,200,200,200,200,200,80,
  200,200,200,200,200,200,200,80,
  200,200,200,200,200,200,200,80,
  200,185,180,180,180,180,180,80,
  200,185,170,170,170,170,170,80,
  00,00,00,                  // lastX,lastY,lastRet
};

unsigned char boostDCMin1[] = {
  'M','2','D',
  0x8,0xA,MAP_AXIS_RPM,MAP_AXIS_TPS,MAP_AXIS_DUTY_CYCLE,
  0,0,70,70,70,70,70,70,
  0,70,70,70,70,70,70,70,
  0,70,70,70,70,70,70,70,
  0,70,70,70,70,70,70,70,
  0,70,70,70,70,70,70,70,
  0,70,70,70,70,70,70,70,
  0,70,70,70,70,70,70,70,
  0,70,70,70,70,70,70,70,
  0,70,70,70,70,70,70,70,
  0,70,70,70,70,70,70,70,
  00,00,00,                  // lastX,lastY,lastRet
};

// R1 = 2400ohm, R2 = old style Bosch temp sensor
unsigned char tempCalibrationMap[] = {
  'M','1','D',
  0x8,0x1,0xAB,0x00,0xAA,
  // values at -64,-32,0,32, 64,96,128,160 °C
  175+64,120+64,90+64,62+64,44+64,30+64,6+64,-55+64,
  //    255,227,179,109,51,19,9,0,  Calculated from bosch reference, does not seem to be accurate?
  00,00,00,                  // lastX,lastY,lastRet
};

// Also used in NVRAM data store magic header
const unsigned char versionString[] PROGMEM  = "DMN-Vanbcguy Boost Ctrl v2.5a."; 
const unsigned char statusString1[] PROGMEM  = " Active view: ";

#define OPTIONS_VANESOPENIDLE 1
#define OPTIONS_VNTOUTPUTINVERTED 2

#define METHOD_DUTYCYCLEMAP 0
#define METHOD_PID 1
#define METHOD_SIMULATE_ACTUATOR 2

#define TEMP_HYSTERESIS 3

// contains configurable data. Can be stored in eeprom
struct settingsStruct {
  int tpsMin;
  int tpsMax;
  int mapMin;
  int mapMax;
  int egtMin;
  int egtMax;
  int empMin;
  int empMax;
  int rpmMax; 	
  int rpmTeethsPerRotation;
  unsigned char mode;
  unsigned char inDamp;
  unsigned char outDamp;
  int output1EnableTemp;
  int output2EnableTemp;
  char options;
  int boostKp;
  int boostKi;
  int boostKd;
  int boostBias;
};

settingsStruct settings;

//  contains calculated output data. calculated each run of mainloop
struct controlsStruct {
  // inputs
  volatile int tpsInput;
  unsigned char tpsCorrected;
  volatile int mapInput;
  unsigned char mapCorrected;
  volatile int egtInput;
  unsigned char egtCorrected;
  volatile int empInput;
  unsigned char empCorrected;
  char mode; // operating mode

    // outputs

  unsigned char vntTargetPressure;
  unsigned char vntPositionRemapped;  
  unsigned char vntPositionDC;
  unsigned char vntMinDc;
  unsigned char vntMaxDc;

  // calculated value
  volatile int rpmActual;
  volatile unsigned char rpmCorrected;
  unsigned char statusBits;

  bool idling;
  int temp1;
  int temp2;
  char output1Enabled;
  char output2Enabled;
  unsigned char auxOutput;

  float boostCalculatedP;
  float boostCalculatedI;
  float boostCalculatedD;

  float pidOutput;
  float prevPidOutput;

  unsigned long lastTime;
  float lastInput;
};

controlsStruct controls;

struct avgStruct {
  unsigned char pos;
  unsigned char size;
  volatile unsigned int avgData[AVG_MAX]; 
};

avgStruct tpsAvg;
avgStruct mapAvg;
avgStruct empAvg;

char buffer[100]; // general purpose buffer, mainly used for string storage when printing from flash
unsigned long lastPacketTime;
const unsigned char mapVisualitionHelp[] PROGMEM  = "Top Left is 0,0 (press: L - toggle live mode)";

unsigned char page=0;
char *pages[] = {
  "About","Adaptation","Actuator Fine-tune","Edit map: boostRequest","Edit map: boostDCMin","Edit map: boostDCMax","Edit map: Aux. device PWM map","Edit map: tempCalibration","Output Tests"};

unsigned char **editorMaps;
unsigned char *editorMaps1[]={
  boostRequest1,boostDCMin1,boostDCMax1,auxMap1,tempCalibrationMap};

unsigned char clearScreen[] =  { 
  27,'[','2','J',27,'[','H'};

const unsigned char ANSIclearEol[] PROGMEM = {
  27,'[','K',0};

const unsigned char ANSIclearEolAndLf[] PROGMEM = {
  27,'[','K','\r','\n',0};
const unsigned char ANSIgoHome[] PROGMEM = {
  27,'[','1',';','1','H',0};
const unsigned char ANSIclearEos[] PROGMEM = {
  27,'[','J',0};

void setPwmFrequency(int pin, int divisor) {
  byte mode;
  if(pin == 5 || pin == 6 || pin == 9 || pin == 10) {
    switch(divisor) {
    case 1: 
      mode = 0x01; 
      break;
    case 8: 
      mode = 0x02; 
      break;
    case 64: 
      mode = 0x03; 
      break;
    case 256: 
      mode = 0x04; 
      break;
    case 1024: 
      mode = 0x05; 
      break;
    default: 
      return;
    }
    if(pin == 5 || pin == 6) {
      TCCR0B = TCCR0B & 0b11111000 | mode;
    } 
    else {
      TCCR1B = TCCR1B & 0b11111000 | mode;
    }
  } 
  else if(pin == 3 || pin == 11) {
    switch(divisor) {
    case 1: 
      mode = 0x01; 
      break;
    case 8: 
      mode = 0x02; 
      break;
    case 32: 
      mode = 0x03; 
      break;
    case 64: 
      mode = 0x04; 
      break;
    case 128: 
      mode = 0x05; 
      break;
    case 256: 
      mode = 0x06; 
      break;
    case 1024: 
      mode = 0x7; 
      break;
    default: 
      return;
    }
    // TCCR2A = 0xA3;
    TCCR2B = TCCR2B & 0b11111000 | mode;
  }
}


volatile unsigned int teethNo = 0;

void rpmTrigger() {
  // increase the tooth count whenever we see a tooth go by
  teethNo++;
}

unsigned long rpmMicros = 0;
unsigned long teethSeconds = 0;

void calcRpm() {
  if (teethNo > rpmResolution) 
  {
    int rpm;
    
    teethSeconds = 60000000 / settings.rpmTeethsPerRotation;

    // teethSeconds is one second in microseconds / number of teeth per revolution - avoid overflowing by pre-dividing a second by the number of teeth
    rpm = (teethSeconds * teethNo)/(micros() - rpmMicros);

    // Set time to now, reset tooth count to zero to start incrementing again
    rpmMicros = micros();
    teethNo = 0;
    
    controls.rpmActual = (rpmSmoothing * rpm) + ((1.0-rpmSmoothing)*controls.rpmActual);
  }
}

void gotoXY(char x,char y) {
  Serial.print("\e[");
  Serial.print(y,DEC);
  Serial.print(";");
  Serial.print(x,DEC);
  Serial.print("H");
}

void modeSelect() {
  // This needs to be removed, we don't have a dual mode switch and most of the rest of the code is gone
  
  editorMaps = editorMaps1;

  auxMap = auxMap1;

  boostRequest = boostRequest1;

  boostDCMax = boostDCMax1;

  boostDCMin = boostDCMin1;

}

void setup_lcd() {
  // Put all the LCD setup stuff here, we will call this from
  // the "setup" loop

  lcd.begin(9600);        

  // set the contrast, 200 is a good place to start, adjust as desired
  lcd.write(0xFE);
  lcd.write(0x50);
  lcd.write(200);
  delay(10);       

  // set the brightness - we'll max it (255 is max brightness)
  lcd.write(0xFE);
  lcd.write(0x99);
  lcd.write(255);
  delay(10);     

  // set background colour - r/g/b 0-255
  lcd.write(0xFE);
  lcd.write(0xD0);
  lcd.write(64);
  lcd.write(255);
  lcd.write((uint8_t)0);
  delay(10);

  // turn off cursors
  lcd.write(0xFE);
  lcd.write(0x4B);
  lcd.write(0xFE);
  lcd.write(0x54);

  // turn off autoscroll
  lcd.write(0xFE);
  lcd.write(0x52); 

  // clear screen
  lcd.write(0xFE);
  lcd.write(0x58);
  delay(10);   // we suggest putting delays after each command 

  // go 'home'
  lcd.write(0xFE);
  lcd.write(0x48);
  delay(10);   // we suggest putting delays after each command 
}


float Kp;
float Ki;
float Kd;

void calcKp() {
  Kp = (float)(settings.boostKp)/PIDControlRatio;
}

void calcKi() {
  Ki = (float)(settings.boostKi)/(PIDControlRatio * 500);
}

void calcKd() {
  Kd = (float)(settings.boostKd * 100)/PIDControlRatio; 
}

void setup() {
  modeSelect();
  setup_lcd();

  // Print a message to the LCD.
  strcpy_P(buffer, (PGM_P)&versionString);   
  lcd.print(buffer);  

  Serial.begin(115200);
  //	Serial.begin(19200);
  Serial.print("Boot:");

  pinMode(PIN_HEARTBEAT,OUTPUT); // DEBUG led

  pinMode(PIN_BUTTON,INPUT); // Reset switch
  digitalWrite(PIN_BUTTON, HIGH);  // activate pull up resistor

  pinMode(PIN_RPM_TRIGGER,INPUT); // Reset switch
  digitalWrite(PIN_RPM_TRIGGER,HIGH); // pullup for honeywell

  attachInterrupt(0, rpmTrigger, RISING); // or rising!
    
  setPwmFrequency(PIN_VNT_N75, 1024); // was 1024
  setPwmFrequency(PIN_AUX_N75, 1024); // was 1024

  pinMode(PIN_VNT_N75,OUTPUT);
  pinMode(PIN_AUX_N75,OUTPUT);

  pinMode(PIN_TPS,INPUT);
  pinMode(PIN_MAP,INPUT);

  pinMode(PIN_TEMP2,INPUT);

  digitalWrite(PIN_TPS,LOW); // safety unconnected TPS
  digitalWrite(PIN_MAP,HIGH); // safety unconnected MAP

  // clear screen
  lcd.write(0xFE);
  lcd.write(0x58);

  lcd.print("Load:");
  Serial.print("OK, Load:");
  if (loadFromEEPROM(false) == false) {
    Serial.print("invalid conf.");
    lcd.print("INVALID CONF");
    loadDefaults();
    delay(2000);
  } 
  else {
    Serial.println("OK");
    lcd.print("OK.");
    delay(500);
  }
  Serial.println("\r\n");
  Serial.write(clearScreen,sizeof(clearScreen));
  tpsAvg.size=AVG_MAX;
  mapAvg.size=AVG_MAX;

  //initial setup of kp/ki/kd
  calcKp();
  calcKi();
  calcKd();

  digitalWrite(PIN_HEARTBEAT,LOW);  

  // set up screen
  layoutLCD();

  pageAbout(1); // force output
}

void loadDefaults() {
  memset(&settings,0,sizeof(settingsStruct));
  settings.tpsMax = 1023;
  settings.mapMax = 1023;
  settings.empMax = 1023;
  settings.egtMax = EGT_ALARM;
  settings.egtMin = 0;
  settings.rpmTeethsPerRotation = 4;
  settings.rpmMax = 6000;
  settings.options = 0;
  settings.inDamp = 0;
  settings.outDamp = 0;
  settings.output1EnableTemp = 85;
  settings.output2EnableTemp = 85; 
  settings.boostKp = 30;
  settings.boostKi = 22;
  settings.boostKd = 35;
  settings.boostBias = 10;
}


unsigned char mapValues(int raw,int mapMin,int mapMax) {
  if (raw < mapMin)
    return 0;
  if (raw >= mapMax)
    return 0xff;

  return map(raw,mapMin,mapMax,0,255);
}

unsigned char empValues(int raw,int empMin,int empMax) {
  if (raw < empMin)
    return 0;
  if (raw >= empMax)
    return 0xff;

  return map(raw,empMin,empMax,0,255);
}

unsigned char egtValues(int raw,int egtMin,int egtMax) {
  if (raw < egtMin)
    return 0;
  if (raw >= egtMax)
    return 0xff;

  return map(raw,egtMin,egtMax,0,255);
}

unsigned char mapValuesSqueeze(int raw,int mapMin,int mapMax) {
  return map(raw,0,255,mapMin,mapMax);
}

unsigned char mapInterpolate(unsigned char p1,unsigned char p2, unsigned char pos) {
  return (p1*(100-pos)+p2*pos)/100;
}

unsigned char mapLookUp(unsigned char *mapData,unsigned char x,unsigned char y) {
  unsigned char isInterpolated = *(mapData+2);
  unsigned char tableSizeX = *(mapData+3);
  unsigned char tableSizeY = *(mapData+4);
  unsigned char yPos;
  *(mapData+8+tableSizeX*tableSizeY) = x;
  *(mapData+8+tableSizeX*tableSizeY+1) = y;

  if (tableSizeY) {
    yPos = y / (256/(tableSizeY-1));
  } 
  else {
    yPos = 0;
  }
  unsigned char xPos = (x / (256/(tableSizeX-1)));
  int ofs = 8; // skip headers

  unsigned char p1 = *(mapData+ofs+(yPos*tableSizeX)+xPos);
  unsigned char p2 = *(mapData+ofs+(yPos*tableSizeX)+(((xPos+1)>=tableSizeX)?xPos:xPos+1));
  ;
  unsigned char p3 = *(mapData+ofs+((((yPos+1)>=tableSizeX)?yPos:yPos+1)*tableSizeX)+xPos);
  unsigned char p4 = *(mapData+ofs+((((yPos+1)>=tableSizeX)?yPos:yPos+1)*tableSizeX)+(((xPos+1)>=tableSizeX)?xPos:xPos+1));

  unsigned char ret;
  if (isInterpolated == 'D') {
    int amountX = (x % (256/(tableSizeX-1)))*(10000/(256/(tableSizeX-1)));
    if (tableSizeY) {
      // 2D
      int amountY = (y % (256/(tableSizeY-1)))*(10000/(256/(tableSizeY-1)));
      char y1 = mapInterpolate(p1,p2,amountX /100);
      char y2 = mapInterpolate(p3,p4,amountX /100);
      ret = mapInterpolate(y1,y2,amountY /100);
    } 
    else {
      // 1D
      ret = mapInterpolate(p1,p2,amountX /100);
    }
  } 
  else {
    ret = p1;
  }
  *(mapData+8+tableSizeX*tableSizeY+2) = ret;
  return ret;
}


char mapDebugCharValue(unsigned char c) {
  if (c<5) {
    return ' ';
  } 
  else if (c<20) {
    return '.';
  } 
  else if (c<60) {
    return ':';
  } 
  else if (c<128) {
    return '!';
  } 
  else if (c<180) {
    return 'o';
  } 
  else if (c<220) {
    return 'O';
  } 
  else  {
    return '@';
  }
}


// Fetches and print string from flash to preserve some ram!
void printFromFlash(const unsigned char *str) {
  strcpy_P(buffer, (PGM_P)str);   
  Serial.print(buffer);
}

int EEPROMwriteData(int offset, byte *ptr,int size) {
  int i;
  for (i = 0; i < size; i++)
    EEPROM.write(offset++, *(ptr++));
  return i;
}

int EEPROMreadData(int offset, byte *ptr,int size) {
  int i;
  for (i = 0; i < size; i++)
    *(ptr++) = EEPROM.read(offset++);
  return i;
}

void saveToEEPROM() {
  int ofs=0;
  // write magic header
  strcpy_P(buffer, (PGM_P)&versionString);   
  ofs += EEPROMwriteData(0,(byte*)&buffer,strlen(buffer));
  // write control struct
  ofs += EEPROMwriteData(ofs,(byte*)&settings,sizeof(settingsStruct));

  ofs += EEPROMwriteData(ofs,(byte*)&auxMap1,sizeof(auxMap1));    
  ofs += EEPROMwriteData(ofs,(byte*)&boostRequest1,sizeof(boostRequest1));
  ofs += EEPROMwriteData(ofs,(byte*)&boostDCMin1,sizeof(boostDCMin1));
  ofs += EEPROMwriteData(ofs,(byte*)&boostDCMax1,sizeof(boostDCMax1));

  ofs += EEPROMwriteData(ofs,(byte*)&tempCalibrationMap,sizeof(tempCalibrationMap));

  printFromFlash(ANSIclearEolAndLf);
  Serial.print(ofs,DEC);
  Serial.print("SAVED ");
  Serial.print(ofs);
  Serial.print(" BYTES.");

  delay(1000); 
}

bool loadFromEEPROM(bool force) {
  int ofs=0;
  // if reset pin is active, no not load anything from eeprom
  if (digitalRead(PIN_BUTTON) == 0) {
    Serial.print("PIN_BUTTON active..");
    delay(2000);
    return false;
  }
  // Check magic header to prevent data corruption of blank board or wrong version save file
  if (!force) {
    strcpy_P(buffer, (PGM_P)&versionString);   
    for (ofs=0;ofs<strlen(buffer);ofs++) {
      if (EEPROM.read(ofs) != buffer[ofs])
        return false;
    }
  }
  ofs = strlen(buffer);
  ofs += EEPROMreadData(ofs,(byte*)&settings,sizeof(settingsStruct));

  ofs += EEPROMreadData(ofs,(byte*)&auxMap1,sizeof(auxMap1));
  ofs += EEPROMreadData(ofs,(byte*)&boostRequest1,sizeof(boostRequest1));   
  ofs += EEPROMreadData(ofs,(byte*)&boostDCMin1,sizeof(boostDCMin1));
  ofs += EEPROMreadData(ofs,(byte*)&boostDCMax1,sizeof(boostDCMax1));

  ofs += EEPROMreadData(ofs,(byte*)&tempCalibrationMap,sizeof(tempCalibrationMap));

  return true;
}


int toKpaMAP(int raw) {
  return raw*MAP_SCALING_KPA;
}

int toKpaEMP(int raw) {
  return raw*EMP_SCALING_KPA;
}

int toTemperature( int rawValue) {
  int ret = mapLookUp(tempCalibrationMap,rawValue,0);
  return ret-64;
}

int toVoltage(int raw) {
  // mVolt
  return int(raw*19.608);
}

int toRpm(int raw) {
  return round(((float)settings.rpmMax/255)*(float)raw);
}

int toEgt(int raw) {
  return round(((float)settings.egtMax/255)*(float)raw);
}

int toTps(int raw) {
  // percent
  return int(raw/2.55);
}

void printIntWithPadding(int val,unsigned char width,char padChar) {
  // print enough leading zeroes!
  memset(buffer,padChar,30);
  // append string presentation of number to end
  itoa(val,buffer+30,10);
  // print string with given width
  Serial.print(buffer+30+strlen(buffer+30)-width);
}

void printStringWithPadding(const unsigned char *str,unsigned char width,char padChar) {
  // print enough leading zeroes!
  memset(buffer,padChar,30);
  // append string presentation of number to end
  strcpy_P(buffer+30, (PGM_P)str);   

  // print string with given width
  Serial.print(buffer+30+strlen(buffer+30)-width);
}

void printPads(unsigned char n, char padChar) {
  memset(buffer,padChar,n);
  buffer[n] = 0;
  Serial.print(buffer);
}

// User interface functions
void pageHeader() {
  //Serial.write(clearScreen,sizeof(clearScreen));    
  printFromFlash(ANSIgoHome);
  printFromFlash((const unsigned char*)versionString);  
  Serial.print(" Mode:");
  Serial.print(controls.mode,DEC);
  Serial.print(" "); 
  printFromFlash(statusString1);   
  Serial.print(pages[page]);
  printFromFlash(ANSIclearEolAndLf);
  printFromFlash(ANSIclearEolAndLf);
}

// Stored in the 32kB FLASH
const unsigned char aboutString1[] PROGMEM  = "(c) 2011-2014 Juho Pesonen. Visit http://dmn.kuulalaakeri.org/dmn-boost-control/";
const unsigned char aboutString2[] PROGMEM  = "Press: <space> to jump next view, or press ...";
const unsigned char aboutString3[] PROGMEM  = "Questions? Or feedback? Send mail to dmn@qla.fi";

void pageAbout(char key) {

  if (key) {
    // update only if key pressed
    pageHeader();
    printFromFlash(aboutString1);   
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(ANSIclearEolAndLf);

    printFromFlash(aboutString2);   
    printFromFlash(ANSIclearEolAndLf);	
    for (char i=0;i<9;i++) {
      printPads(11,' ');
      Serial.print("<");
      Serial.print(i,DEC);
      Serial.print("> ");
      Serial.print(pages[i]);			
      printFromFlash(ANSIclearEolAndLf);
    }
    printFromFlash(ANSIclearEolAndLf);

    printFromFlash(aboutString3);   
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(ANSIclearEolAndLf);


    printFromFlash(ANSIclearEos);
  }
} 

// TPS input: 200 Corrected: 0 (low:200, high:788);
const unsigned char statusRPM[] PROGMEM  = "RPM actual:";
const unsigned char statusCorrected[] PROGMEM  = " Corrected:";
const unsigned char statusTPSinput[] PROGMEM  = "TPS input:";
const unsigned char statusLow[] PROGMEM  = ":";
const unsigned char statusMAPinput[] PROGMEM  = "MAP input:";
const unsigned char statusVNTactOutput[] PROGMEM  = "VNT actuator output:";
const unsigned char statusHeader[] PROGMEM  = "Sensor values and adaptation map limits (l=live, y=save, p/P=load, R=reset)";
//                                          0123456789012345678901234567890123456789012345678901234567890123456789
const unsigned char statusTableHeader[] PROGMEM  = "        Raw val.  Corr.val. Map min.  Map max.  Mode";
const unsigned char statusRowTPS[] PROGMEM = "TPS";
const unsigned char statusRowMAP[] PROGMEM = "MAP";
const unsigned char statusRowEMP[] PROGMEM = "EMP";
const unsigned char statusRowEGT[] PROGMEM = "EGT";
const unsigned char statusRowRPM[] PROGMEM = "RPM";

//                                          0123456789012345678901234567890123456789012345678901234567890123456789
const unsigned char statusVNTtableStyleDC[] PROGMEM =  "Duty cycle";
const unsigned char statusVNTtableStyleMAP[] PROGMEM = "Target press.";
const unsigned char statusOpenAtIdle[] PROGMEM = "VNT Open blades when idling C";

const unsigned char statusNone[] PROGMEM = "-";

const unsigned char statusSelected[] PROGMEM = "[X] ";
const unsigned char statusUnSelected[] PROGMEM = "[ ] ";
const unsigned char statusVNTOutputInverted[] PROGMEM = "VNT Output Inverted J";
const unsigned char statusControlMethodDC[] PROGMEM =       "[X] BoostDCMin = output, [ ] PID, [ ] Simulate Actuator   M";
const unsigned char statusControlMethodPID[] PROGMEM =      "[ ] BoostDCMin = output, [X] PID, [ ] Simulate Actuator   M";
const unsigned char statusControlMethodActuator[] PROGMEM = "[ ] BoostDCMin = output, [ ] PID, [X] Simulate Actuator   M";

const unsigned char statusTemp1[] PROGMEM = "Temp1";
const unsigned char statusTemp2[] PROGMEM = "Temp2";
const unsigned char statusC[] PROGMEM = "°C";
const unsigned char statusOn[] PROGMEM = " (on)  ";
const unsigned char statusOff[] PROGMEM = " (off) ";
const unsigned char statusFooter[] PROGMEM = "To change adaptation value, press the letter after value.";
const unsigned char statusFooter2[] PROGMEM = "For example: q = decrease 'Map Low' for TPS / Q increase 'Map Low' for TPS";

char oldKey;
bool isLive = true;

void pageStatusAndAdaption(char key) {
  int x = 1;

  // Boost for hurry people!
  if (oldKey == key)
    x = 10;

  switch (key) {
  case 'l':
  case 'L': 
    isLive=!isLive; 
    break;
  case 'i': 
    if (settings.inDamp-x>0) settings.inDamp -= x; 
    break;
  case 'I': 
    if (settings.inDamp+x<64) settings.inDamp += x; 
    break;
  case 'o': 
    if (settings.outDamp-x>0) settings.outDamp -= x; 
    break;
  case 'O': 
    if (settings.outDamp+x<64) settings.outDamp += x; 
    break;
  case 'q': 
    if (settings.tpsMin-x>0) settings.tpsMin -= x; 
    break;
  case 'Q': 
    if (settings.tpsMin+x<settings.tpsMax) settings.tpsMin += x; 
    break;
  case 'w': 
    if (settings.tpsMax-x>settings.tpsMin) settings.tpsMax -= x; 
    break;
  case 'W': 
    if (settings.tpsMax+x<1024) settings.tpsMax += x; 
    break;
  case 'e':
    if (settings.egtMax+x<EGT_MAX_READ) settings.egtMax += x;
    break;
  case 'E':
    if (settings.egtMax-x>0) settings.egtMax -= x;
    break;
  case 'a': 
    if (settings.mapMin-x>0) settings.mapMin -= x; 
    break;
  case 'A': 
    if (settings.mapMin+x<settings.mapMax) settings.mapMin += x; 
    break;
  case 's': 
    if (settings.mapMax-x>settings.mapMin) settings.mapMax -= x; 
    break;
  case 'S': 
    if (settings.mapMax+x<1024) settings.mapMax += x; 
    break;
  case 'f': 
    if (settings.rpmTeethsPerRotation>1) settings.rpmTeethsPerRotation -= 1; 
    teethSeconds = 60000000 / settings.rpmTeethsPerRotation;
    break;				
  case 'F': 
    if (settings.rpmTeethsPerRotation<99) settings.rpmTeethsPerRotation += 1; 
    teethSeconds = 60000000 / settings.rpmTeethsPerRotation;
    break;				
  case 'd': 
    if (settings.rpmMax-100>1000) settings.rpmMax -= 100; 
    break;		
  case 'D': 
    if (settings.rpmMax+100<9999) settings.rpmMax += 100; 
    break;		

  case 'c':
  case 'C': 
    settings.options = settings.options ^ OPTIONS_VANESOPENIDLE; 
    break;		
  case 'm':
  case 'M': 			
  case 'j':
  case 'J': 
    settings.options = settings.options ^ OPTIONS_VNTOUTPUTINVERTED; 
    break;				
  case 'y': 
    saveToEEPROM(); 
    break;
  case 'p': 
    loadFromEEPROM(false); 
    break;
  case 'R': 
    loadDefaults(); 
    break;		
  }
  if (key) {
    lastPacketTime = millis();
  }
  oldKey = key;	

  if (!key && !isLive)
    return;

  // update always if live
  pageHeader(); 

  printFromFlash(statusHeader);
  printFromFlash(ANSIclearEolAndLf);
  printFromFlash(ANSIclearEolAndLf);

  printFromFlash(statusTableHeader);
  printFromFlash(ANSIclearEolAndLf);

  printStringWithPadding(statusRowTPS,7,' ');
  printPads(1,' ');
  printIntWithPadding(controls.tpsInput,4,'0'); // RAW
  printPads(6,' ');
  printIntWithPadding(controls.tpsCorrected,3,'0'); // Corr.
  printPads(7,' ');	
  printIntWithPadding(settings.tpsMin,4,'0'); // Map low
  printPads(1,' ');	
  Serial.print("Q");
  printPads(4,' ');	
  printIntWithPadding(settings.tpsMax,4,'0'); // Map high
  Serial.print(" W");
  printFromFlash(ANSIclearEolAndLf);

  printStringWithPadding(statusRowMAP,7,' ');
  printPads(1,' ');
  printIntWithPadding(controls.mapInput,4,'0'); // RAW
  printPads(6,' ');
  printIntWithPadding(controls.mapCorrected,3,'0'); // Corr.
  printPads(7,' ');	
  printIntWithPadding(settings.mapMin,4,'0'); // Map low
  printPads(1,' ');	
  Serial.print("A");
  printPads(4,' ');	
  printIntWithPadding(settings.mapMax,4,'0'); // Map high
  Serial.print(" S");
  printFromFlash(ANSIclearEolAndLf);

  printStringWithPadding(statusRowRPM,7,' ');
  printPads(1,' ');
  printIntWithPadding(controls.rpmActual,4,'0'); // RAW
  printPads(6,' ');
  printIntWithPadding(controls.rpmCorrected,3,'0'); // Corrected
  printPads(17,' ');
  printIntWithPadding(settings.rpmMax,4,'0'); 
  Serial.print(" D");
  printPads(4,' ');	
  Serial.print("No.teeths=");
  printIntWithPadding(settings.rpmTeethsPerRotation,2,'0'); 
  Serial.print(" F");

  printFromFlash(ANSIclearEolAndLf);

  printStringWithPadding(statusTemp1,7,' ');
  printPads(1,' ');  
  printIntWithPadding(controls.temp1,4,' ');
  printFromFlash(statusC);
  printPads(3,' ');
  Serial.print("Max EGT=");
  printIntWithPadding(settings.egtMax,3,' ');
  Serial.print(" E");

  printFromFlash(ANSIclearEolAndLf);

  printStringWithPadding(statusTemp2,7,' ');
  printPads(1,' ');    
  printIntWithPadding(controls.temp2,4,' ');
  printFromFlash(statusC);
  printPads(3,' ');
  if (controls.output2Enabled) {
    printStringWithPadding(statusOn,7,' ');
  } 
  else {
    printStringWithPadding(statusOff,7,' ');
  }

  printFromFlash(ANSIclearEolAndLf);
  printFromFlash(ANSIclearEolAndLf);

  printPads(8,' '); 

  if (settings.options & OPTIONS_VANESOPENIDLE) {
    printFromFlash(statusSelected);
  } 
  else {
    printFromFlash(statusUnSelected);
  }
  printFromFlash(statusOpenAtIdle);

  printFromFlash(ANSIclearEolAndLf);

  printPads(8,' ');
  if (settings.options & OPTIONS_VNTOUTPUTINVERTED) {
    printFromFlash(statusSelected);
  } 
  else {
    printFromFlash(statusUnSelected);
  }
  printFromFlash(statusVNTOutputInverted);
  printFromFlash(ANSIclearEolAndLf);

  printFromFlash(ANSIclearEolAndLf);
  printFromFlash(ANSIclearEolAndLf);

  printFromFlash(statusFooter);
  printFromFlash(ANSIclearEolAndLf);

  printFromFlash(statusFooter2);

  printFromFlash(ANSIclearEos);
}


const unsigned char statusOutput1[] PROGMEM = "Output tests:";
const unsigned char statusOutput2[] PROGMEM = "<Q> Set output VNT output to Map min. for 2 seconds, value=";
const unsigned char statusOutput3[] PROGMEM = "<W> Set output VNT output to Map max. for 2 seconds, value=";
const unsigned char statusOutput4[] PROGMEM = "<E> Sweep output VNT output between min & max";


const unsigned char statusOutput8[] PROGMEM = "<Z> Enable OUTPUT1 for 2 seconds";
const unsigned char statusOutput9[] PROGMEM = "<X> Enable OUTPUT2 for 2 seconds";

void pageOutputTests(char key) {

  if (key) {
    switch(key) {
    case 'q':
    case 'Q':
      controls.vntPositionRemapped = 0;
      updateOutputValues(true);
      updateLCD();
      delay(2000);				
      break;
    case 'w':
    case 'W':
      controls.vntPositionRemapped = 255;
      updateOutputValues(true);				
      updateLCD();
      delay(2000);							
      break;
    case 'e':
    case 'E':
      for (controls.vntPositionRemapped = 0;
                       controls.vntPositionRemapped<255;
                       controls.vntPositionRemapped++) {
          updateOutputValues(true);
        updateLCD();
        delay(20);
      }
      for (controls.vntPositionRemapped = 255;
                       controls.vntPositionRemapped>0;
                       controls.vntPositionRemapped--) {
          updateOutputValues(true);
        updateLCD();
        delay(20);
      }				
      break;
    case 'z':			
    case 'Z':
      controls.output1Enabled = true;
      updateOutputValues(true);				
      delay(2000);							
      break;
    case 'x':			
    case 'X':
      controls.output2Enabled = true;
      updateOutputValues(true);				
      delay(2000);							
      break;
    }
    pageHeader();

    printFromFlash(statusOutput1); 		
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(statusOutput2); 
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(statusOutput4); 
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(statusOutput8); 
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(statusOutput9); 
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(ANSIclearEos);
  }
}

const unsigned char exportConf[] PROGMEM = "Configuration dump:";
const unsigned char exportVntMap[] PROGMEM = "VNT Map dump:";
const unsigned char exportLdaMap[] PROGMEM = "LDA Map dump:";
const unsigned char exportBoostDCMax[] PROGMEM = "VNT Max DC Map dump:";
const unsigned char exportBoostDCMin[] PROGMEM = "VNT Min DC Map dump:";

void pageExport(char key) {
  if (key) {
    pageHeader();
    printFromFlash(exportConf);
    printFromFlash(ANSIclearEolAndLf);
    Serial.print("!AA");
    for (int i=0;i<sizeof(settingsStruct);i++) {
      if (i%32 == 31) 
        printFromFlash(ANSIclearEolAndLf);
      unsigned char v = (unsigned char)*(i+((unsigned char*)&settings));
      if (v<16)
        Serial.print("0");
      Serial.print(v,HEX);
    }
    Serial.print("!");
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(ANSIclearEolAndLf);

    printFromFlash(exportVntMap);
    printFromFlash(ANSIclearEolAndLf);
    Serial.print("!AA");
    for (int i=0;i<sizeof(boostRequest1);i++) {
      if (i && i%16 == 0) 
        printFromFlash(ANSIclearEolAndLf);
      unsigned char v = (unsigned char)*(i+((unsigned char*)&boostRequest1));
      if (v<16)
        Serial.print("0");
      Serial.print(v,HEX);
    }
    Serial.print("!");
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(ANSIclearEolAndLf);

    printFromFlash(exportBoostDCMax);
    printFromFlash(ANSIclearEolAndLf);
    Serial.print("!AB");
    for (int i=0;i<sizeof(boostDCMax1);i++) {
      if (i && i%16 == 0) 
        printFromFlash(ANSIclearEolAndLf);
      unsigned char v = (unsigned char)*(i+((unsigned char*)&boostDCMax1));
      if (v<16)
        Serial.print("0");
      Serial.print(v,HEX);
    }
    Serial.print("!");
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(ANSIclearEolAndLf);

    printFromFlash(exportBoostDCMin);
    printFromFlash(ANSIclearEolAndLf);
    Serial.print("!AC");
    for (int i=0;i<sizeof(boostDCMin1);i++) {
      if (i && i%16 == 0) 
        printFromFlash(ANSIclearEolAndLf);
      unsigned char v = (unsigned char)*(i+((unsigned char*)&boostDCMin1));
      if (v<16)
        Serial.print("0");
      Serial.print(v,HEX);
    }
    Serial.print("!");
    printFromFlash(ANSIclearEolAndLf);
    printFromFlash(ANSIclearEolAndLf);       

    printFromFlash(exportLdaMap);
    printFromFlash(ANSIclearEolAndLf);
    Serial.print("!AD");
    for (int i=0;i<sizeof(auxMap1);i++) {
      if (i && i%16 == 0) 
        printFromFlash(ANSIclearEolAndLf);
      unsigned char v = (unsigned char)*(i+((unsigned char*)&auxMap1));
      if (v<16)
        Serial.print("0");
      Serial.print(v,HEX);
    }
    Serial.print("!");

    printFromFlash(ANSIclearEolAndLf);

    printFromFlash(ANSIclearEos);	
  }
}

unsigned int execTimeRead = 0;
unsigned int execTimeAct = 0;
unsigned int execTimeLcd = 0;

void pageDataLogger(char key) {
  Serial.write(2); // stx
  Serial.print(toKpaMAP(controls.mapCorrected),DEC);
  Serial.print(",");
  Serial.print(toKpaMAP(controls.vntTargetPressure),DEC);
  Serial.print(",");
  Serial.print(controls.vntPositionRemapped,DEC);
  Serial.print(",");
  Serial.print(controls.tpsCorrected,DEC);
  Serial.print(",");
  Serial.print(controls.rpmActual,DEC);
  Serial.print(",");
  Serial.print(controls.temp1,DEC);
  Serial.print(",");
  Serial.print(controls.boostCalculatedP,DEC);
  Serial.print(",");
  Serial.print(controls.boostCalculatedI,DEC);
  Serial.print(",");
  Serial.print(controls.boostCalculatedD,DEC);
  Serial.print(",");
  Serial.print(controls.pidOutput,DEC);
  Serial.print(",");
  Serial.print(controls.mode,DEC);
  Serial.print(",");
  Serial.print(controls.auxOutput,DEC);
  Serial.print(",");
  Serial.print(execTimeRead,DEC);
  Serial.print(",");
  Serial.print(execTimeAct,DEC);
  Serial.print(",");
  Serial.print(execTimeLcd,DEC);
  Serial.print(",");
  Serial.print(millis()/10,DEC); 
  Serial.write(3);
}

void printMapAxis(unsigned char axisType,unsigned char idx,bool verbose) {
  switch (axisType) {
  case MAP_AXIS_RPM:
    Serial.print(toRpm(idx),DEC);
    if (verbose) Serial.print(" RPM");
    break;
  case MAP_AXIS_TPS:
    Serial.print(toTps(idx),DEC);
    if (verbose) Serial.print("% TPS");
    break;
  case MAP_AXIS_KPA:
    Serial.print(toKpaMAP(idx),DEC);
    if (verbose) Serial.print(" kPa");
    break;
  case MAP_AXIS_VOLTAGE:
    Serial.print(toVoltage(idx),DEC);
    if (verbose) Serial.print(" mV");
    break;
  case MAP_AXIS_CELSIUS:
    Serial.print(idx-64,DEC);
    if (verbose) Serial.print(" °C");
    break;
  case MAP_AXIS_EGT:
    Serial.print(toEgt(idx),DEC);
    if (verbose) Serial.print(" °C");
    break;
  default:
    Serial.print(idx,DEC);
    if (verbose) Serial.print(" Raw");
  }
}
struct mapEditorDataStruct {
  char cursorX;
  char cursorY;
  unsigned char lastX;
  unsigned char lastY;
  char currentMap;
  unsigned char clipboard;
} 
mapEditorData;


const unsigned char mapCurrentOutput[] PROGMEM = "Current output:";
const unsigned char mapEditorHelp[] PROGMEM = "Press: cursor keys to move, - / + dec/inc, c/v copy/paste cell, y save";

void pageMapEditor(unsigned char mapIdx,int key,boolean showCurrent=false) {    
  unsigned char *mapData = editorMaps[mapIdx];
  unsigned char tableSizeX = *(mapData+3);
  unsigned char tableSizeY = *(mapData+4);
  unsigned char axisTypeX = *(mapData+5);
  unsigned char axisTypeY = *(mapData+6);
  unsigned char axisTypeResult = *(mapData+7);
  unsigned char lastXpos = *(mapData+8+tableSizeX*tableSizeY);
  unsigned char lastYpos = *(mapData+8+tableSizeX*tableSizeY+1);
  unsigned char lastValue = *(mapData+8+tableSizeX*tableSizeY+2);

  const char xPad = 5;
  const char xSpace = 7;
  const char yPad = 5;
  const char ySpace = 2;

  switch (key) {
  case -2:
    gotoXY(xPad+xSpace+mapEditorData.cursorX*xSpace+1,yPad+ySpace+mapEditorData.cursorY*ySpace);
    printPads(xSpace-2,' ');
    gotoXY(xPad+xSpace+mapEditorData.cursorX*xSpace+1,yPad+ySpace+mapEditorData.cursorY*ySpace);
    printMapAxis(axisTypeResult,*(mapData+8+mapEditorData.cursorX+mapEditorData.cursorY*tableSizeX),0);

    return;
    break;
  case -1:
    // erase cursor
    gotoXY(xPad+xSpace+mapEditorData.cursorX*xSpace,yPad+ySpace+mapEditorData.cursorY*ySpace);
    Serial.print(" ");
    gotoXY(xPad+xSpace+mapEditorData.cursorX*xSpace+xSpace-1,yPad+ySpace+mapEditorData.cursorY*ySpace);
    Serial.print(" ");
    return;
    break;
  case 'c':
    mapEditorData.clipboard = *(mapData+8+mapEditorData.cursorX+mapEditorData.cursorY*tableSizeX);
    return;
    break;
  case 'v':
    (*(mapData+8+mapEditorData.cursorX+mapEditorData.cursorY*tableSizeX)) = mapEditorData.clipboard;
    pageMapEditor(mapIdx,-2);
    return;
    break;
  case 'h':
    pageMapEditor(mapIdx,-1);
    if (mapEditorData.cursorX>0)
      mapEditorData.cursorX--;
    pageMapEditor(mapIdx,0);
    return;
    break;
  case 'l':
    pageMapEditor(mapIdx,-1);
    if (mapEditorData.cursorX<tableSizeX-1)
      mapEditorData.cursorX++;
    pageMapEditor(mapIdx,0);
    return;
    break;
  case 'k':
    pageMapEditor(mapIdx,-1);
    if (mapEditorData.cursorY>0)
      mapEditorData.cursorY--;
    pageMapEditor(mapIdx,0);
    return;
    break;
  case 'j':
    pageMapEditor(mapIdx,-1);
    if (mapEditorData.cursorY<tableSizeY-1)
      mapEditorData.cursorY++;
    pageMapEditor(mapIdx,0);
    return;
    break;
  case '+':
    if (*(mapData+8+mapEditorData.cursorX+mapEditorData.cursorY*tableSizeX)<0xff)
      (*(mapData+8+mapEditorData.cursorX+mapEditorData.cursorY*tableSizeX))++;
    pageMapEditor(mapIdx,-2);
    return;
    break;
  case '-':
    if (*(mapData+8+mapEditorData.cursorX+mapEditorData.cursorY*tableSizeX)>0)
      (*(mapData+8+mapEditorData.cursorX+mapEditorData.cursorY*tableSizeX))--;
    pageMapEditor(mapIdx,-2);
    return;
    break;
  case 'y':
    saveToEEPROM();
  case 0:
    if (showCurrent) {
      // Current interpreted value
      gotoXY(xPad+xSpace,yPad+tableSizeY*ySpace+2);
      printFromFlash(mapCurrentOutput);
      printMapAxis(axisTypeResult,lastValue,1);
      printFromFlash(ANSIclearEolAndLf);
    }

    // update cursors only:
    gotoXY(2,yPad+ySpace+round((float)mapEditorData.lastY*(float)((float)(tableSizeY-1)*(float)ySpace/255)));
    Serial.print("  ");
    gotoXY(xPad+xSpace+round((float)mapEditorData.lastX*(float)((float)tableSizeX*(float)xSpace/255)),4);
    Serial.print(" ");

    mapEditorData.lastY = lastYpos;
    mapEditorData.lastX = lastXpos;

    gotoXY(2,yPad+ySpace+round((float)lastYpos*(float)((float)(tableSizeY-1)*(float)ySpace/255)));
    Serial.print(">>");
    gotoXY(xPad+xSpace+round((float)lastXpos*(float)((float)tableSizeX*(float)xSpace/255)),4);
    Serial.print("v"); 

    gotoXY(xPad+xSpace+mapEditorData.cursorX*xSpace,yPad+ySpace+mapEditorData.cursorY*ySpace);
    Serial.print(">");
    gotoXY(xPad+xSpace+mapEditorData.cursorX*xSpace+xSpace-1,yPad+ySpace+mapEditorData.cursorY*ySpace);
    Serial.print("<");

    return;
    break;
  default:
    if (mapEditorData.currentMap!=mapIdx) {
      mapEditorData.cursorX=0;
      mapEditorData.cursorY=0;
      mapEditorData.currentMap = mapIdx;
    }
    pageHeader();
    printFromFlash(ANSIclearEos);
    gotoXY(0,3);
    printFromFlash(mapEditorHelp);
  }
  // Table X header

  for (int x=0;x<tableSizeX;x++) {
    gotoXY(xPad+(1+x)*xSpace,yPad);
    int idx = round((float)((255/(float)(tableSizeX-1)))*(float)x);
    printPads(1,' ');
    printMapAxis(axisTypeX,idx, ((x==0||x==(tableSizeX-1))?true:false));
  }
  gotoXY(xPad+xSpace,yPad+1);
  printPads(tableSizeX*xSpace,'-');

  // Table Y header

  for (int y=0;y<tableSizeY;y++) {
    gotoXY(xPad-1,yPad+(1+y)*ySpace);
    int idx = round((float)((255/(float)(tableSizeY-1)))*(float)y);

    printMapAxis(axisTypeY,idx,true);
    gotoXY(xPad+xSpace-1,yPad+(1+y)*ySpace);
    Serial.print("|");
    if (y<tableSizeY-1) {
      gotoXY(xPad+xSpace-1,yPad+(1+y)*ySpace+1); // works for ySpace=2
      Serial.print("|");
    }

  }
  for (int y=0;y<tableSizeY;y++) {
    for (int x=0;x<tableSizeX;x++) {
      gotoXY(xPad+(1+x)*xSpace,yPad+(1+y)*ySpace);
      printPads(1,' ');
      //Serial.print(*(mapData+8+x*y),DEC);
      printMapAxis(axisTypeResult,*(mapData+8+x+y*tableSizeX),0);
    }
  }

}

const unsigned char debugHeader[] PROGMEM = "Target pres.   Actual press.  Actuator pos.  RPM  TPS";
// 0123456789012345678901234567890123456789012345678901234567890123456789


void pageHelp(char key) {
  //pageHeader();
  //Serial.print("help!");
  //printFromFlash(ANSIclearEos);	
  if (key) {
    printFromFlash(debugHeader);
    Serial.print("\r\n");
    printIntWithPadding(toKpaMAP(controls.vntTargetPressure),3,'0');
    printPads(12,' ');
    printIntWithPadding(toKpaMAP(controls.mapCorrected),3,'0');
    printPads(12,' ');
    printIntWithPadding(controls.vntPositionDC,3,'0');
    printPads(12,' ');
    printIntWithPadding(controls.rpmActual,4,'0');
    printPads(1,' ');
    printIntWithPadding(controls.tpsCorrected,4,'0');
    Serial.print("\r\n");
  }
}

const unsigned char ServoFineTunePosition[] PROGMEM = "Act. pos.: ";
const unsigned char ServoFineTunePositionScale[] PROGMEM = "0% ----------- 25% ----------- 50% ----------- 75% -------- 100%";

const unsigned char ServoFineTuneChargePressureRequest[] PROGMEM = "Charge pressure, request:";
const unsigned char ServoFineTuneChargePressureActual[] PROGMEM = "Charge pressure, actual:";
const unsigned char ServoFineTuneTPS[] PROGMEM = "TPS:";
const unsigned char ServoOutputDC[] PROGMEM = "N75 Duty Cycle:";
const unsigned char ServoFineTuneInDamp[] PROGMEM = "In damp.:";
const unsigned char ServoFineTuneOutDamp[] PROGMEM = "Out damp.:";
const unsigned char ServoFineTuneP[] PROGMEM = "PID Kp:";
const unsigned char ServoFineTuneI[] PROGMEM = "PID Ki:";
const unsigned char ServoFineTuneD[] PROGMEM = "PID Kd:";
const unsigned char ServoFineTuneBias[] PROGMEM = "PID Bias (10 default):";

void visualizeActuator(char y) {
  gotoXY(1,y);

  printFromFlash(ServoFineTunePosition); 
  // append string presentation of number to end
  strcpy_P(buffer, (PGM_P)ServoFineTunePositionScale); 
  // Visualize actuator
  memset(buffer,'*',controls.vntPositionDC /4);
  Serial.print(buffer);
  printFromFlash(ANSIclearEol);
  gotoXY(1,y+1);
  printFromFlash(ANSIclearEol);
  gotoXY(1,y+1);
  gotoXY(12+controls.vntMaxDc/4,y+1);
  Serial.print("^Max");
  gotoXY(12+controls.vntMinDc/4,y+1);
  Serial.print("^Min"); 
}

void pageServoFineTune(char key) {
  pageHeader();


  switch (key) {
  case 'p':
    if (settings.boostKp>0) settings.boostKp--;
    calcKp();
    break;
  case 'P':
    if (settings.boostKp<1000) settings.boostKp++;
    calcKp();
    break;
  case 'i':
    if (settings.boostKi>0) settings.boostKi--;
    calcKi();
    break;
  case 'I':
    if (settings.boostKi<500) settings.boostKi++;
    calcKi();
    break;
  case 'd':
    if (settings.boostKd>0) settings.boostKd--;
    calcKd();
    break;
  case 'D':
    if (settings.boostKd<255) settings.boostKd++;
    calcKd();
    break;

  case 'b':
    if (settings.boostBias>0) settings.boostBias--;
    break;
  case 'B':
    if (settings.boostBias<50) settings.boostBias++;
    break;

  case 'y': 
    saveToEEPROM(); 
    break;
  }


  visualizeActuator(3);

  gotoXY(1,5);

  printStringWithPadding(ServoFineTuneChargePressureRequest,25,' ');
  printPads(1,' ');
  printIntWithPadding(toKpaMAP(controls.vntTargetPressure),3,'0');
  Serial.print(" kPa");
  printFromFlash(ANSIclearEolAndLf);

  printStringWithPadding(ServoFineTuneChargePressureActual,25,' ');
  printPads(1,' ');
  printIntWithPadding(toKpaMAP(controls.mapCorrected),3,'0');
  Serial.print(" kPa");
  printFromFlash(ANSIclearEolAndLf);

  printStringWithPadding(ServoFineTuneTPS,25,' ');
  printPads(1,' ');  
  printIntWithPadding(controls.tpsCorrected,3,'0');
  printFromFlash(ANSIclearEolAndLf);

  printStringWithPadding(ServoOutputDC,25,' ');
  printPads(1,' ');  
  printIntWithPadding(controls.vntPositionRemapped,3,'0');
  printFromFlash(ANSIclearEolAndLf);


  printStringWithPadding(ServoFineTuneP,25,' ');
  printPads(1,' ');  
  printIntWithPadding(settings.boostKp,3,'0');
  Serial.print(" P (");
  Serial.print(controls.boostCalculatedP*(100*settings.boostKp/PIDControlRatio));
  Serial.print(")");      
  printFromFlash(ANSIclearEolAndLf);

  printStringWithPadding(ServoFineTuneI,25,' ');
  printPads(1,' ');  
  printIntWithPadding(settings.boostKi,3,'0');
  Serial.print(" I (");
  Serial.print(controls.boostCalculatedI*(100*settings.boostKi/PIDControlRatio));      
  Serial.print(")");            
  printFromFlash(ANSIclearEolAndLf);


  printStringWithPadding(ServoFineTuneD,25,' ');
  printPads(1,' ');  
  printIntWithPadding(settings.boostKd,3,'0');
  Serial.print(" D (");
  Serial.print(controls.boostCalculatedD*(settings.boostKd/PIDControlRatio));      
  Serial.print(")");            
  printFromFlash(ANSIclearEolAndLf);

  printStringWithPadding(ServoFineTuneBias,25,' ');
  printPads(1,' ');  
  printIntWithPadding(settings.boostBias,3,'0');
  Serial.print(" B");
  printFromFlash(ANSIclearEolAndLf);

  printFromFlash(ANSIclearEos);	
}


int getFilteredAvarage(struct avgStruct *a) {
  int minVal = 0;
  int maxVal = 255;
  long int avgAll = 0;

  for (int i=0; i < a->size;i++) {

    if (a->avgData[i] < minVal) {
      minVal = a->avgData[i];
    } 
    if (a->avgData[i] > maxVal) {
      maxVal = a->avgData[i];
    } 

    avgAll += a->avgData[i];
  }
  avgAll = (int)(avgAll / a->size);
  return avgAll;

}

void readValuesTps() {
  tpsAvg.pos++;
  if (tpsAvg.pos>=tpsAvg.size)
    tpsAvg.pos=0;
  tpsAvg.avgData[tpsAvg.pos] = analogRead(PIN_TPS);   
}

void readValuesMap() {
  mapAvg.pos++;
  if (mapAvg.pos>=mapAvg.size)
    mapAvg.pos=0;
  mapAvg.avgData[mapAvg.pos] = analogRead(PIN_MAP);
}

void readValuesEgt() {
  controls.temp1 = temp.readThermocouple(CELSIUS);
  // controls.temp2 = toTemperature(analogRead(PIN_TEMP2)/4); 
  controls.temp2 = 0; // disabled for now as we aren't using it

  if (controls.temp1<=0) {
    controls.temp1 = 0;
  } 
  else if (controls.temp1 >= settings.egtMax) {
    controls.temp1 = settings.egtMax;
  }

}

void processValues() {

  controls.vntMaxDc = mapLookUp(boostDCMax,controls.rpmCorrected,controls.tpsCorrected);
  controls.vntMinDc = mapLookUp(boostDCMin,controls.rpmCorrected,controls.tpsCorrected);


  controls.rpmCorrected = mapValues(controls.rpmActual,0,settings.rpmMax);
  controls.mapInput = getFilteredAvarage(&mapAvg);
  controls.mapCorrected = mapValues(controls.mapInput,settings.mapMin,settings.mapMax);
  controls.tpsInput = getFilteredAvarage(&tpsAvg);
  controls.tpsCorrected = mapValues(controls.tpsInput,settings.tpsMin,settings.tpsMax);
  controls.empInput = getFilteredAvarage(&empAvg);
  controls.empCorrected = mapValues(controls.empInput,settings.empMin,settings.empMax);
  controls.egtCorrected = mapValues(controls.temp1,settings.egtMin,settings.egtMax);

  // EGT controls
  controls.auxOutput = mapLookUp(auxMap,controls.rpmCorrected,controls.egtCorrected);

  // TODO add RPM hysterisis
  if ( controls.tpsCorrected > 0 ) {
    controls.idling = false;
  } 
  else if ( controls.rpmActual < IDLE_MAX_RPM ) {    
    controls.idling = true;
  }
  else {
    controls.idling = false;
  }
  unsigned long now = millis();
  int timeChange = now - controls.lastTime;

  static float error;
  static float integral;
  static float derivate;

  float controlSpan;
  float inputSpan;

  float scaledInput;
  float scaledTarget;
  float scaledBias;

  float toControlVNT;

  /* This is the available span of our DC - we can only go between min and max */
  controlSpan = controls.vntMaxDc - controls.vntMinDc;

  /* This is the available span of our input - we can only go between 0-255 */
  inputSpan = 255.0;

  /* Make the input a percentage of the availble input span */
  scaledInput = (float)controls.mapCorrected / inputSpan; 

  if (scaledInput>1.0) {
    scaledInput = 1.0;
  } 
  else if (scaledInput<0) {
    scaledInput = 0;
  }

  if ((controls.idling)) {
    // If we are at idle then we don't want any boost regardless of map 

    controls.vntTargetPressure = 0;                    // We don't want any pressure
    controls.lastInput = scaledInput;                  // Keep the derivative loop primed
    integral = 0;                                      // Only you can prevent integral windup at idle
    error = 0;                                         // Not strictly necessary but it keeps my ADD in check on the datalogs
    derivate = 0;                                      // See above
    controls.pidOutput=0;                              // Final output is zero - we aren't trying to do anything
    controls.mode = 0;                                 // System status = idling
    
  } 
  else if ( controls.rpmActual <= settings.rpmMax ) {         // Only calculate if RPM is less than rpmMax or we start doing bad things
    // Normal running mode

    /* Look up the requested boost */
    controls.vntTargetPressure = mapLookUp(boostRequest,controls.rpmCorrected,controls.tpsCorrected);

    /* Now make the target a percentage of the same input span */
    scaledTarget = (float)controls.vntTargetPressure / inputSpan;

    /* It should not be possible to have >100% or <0% targets but hey, let's be sure */
    if (scaledTarget>1.0) {
      scaledTarget = 1.0;
    } 
    else if (scaledTarget<0) {
      scaledTarget = 0;
    }
    
    /* Determine the slope of the signal - we need to know this to determine everything else we want to do */
    derivate = Kd * (scaledInput - controls.lastInput) / timeChange;
    controls.lastInput = scaledInput;
    
    /* Since we do a bunch of comparisons with this value lets just calculate it once */
    float scaledError = scaledTarget - scaledInput;

    if ( toKpaMAP(controls.mapCorrected) < spoolMinBoost && integral < preSpoolInt ) {
      // We haven't spooled up yet - use a static value for the integral
      // May want to add a case here for 'spooled but still at low boost'
      controls.mode = 1;              // idling
      integral = preSpoolInt;
      // Still add proportional control like normal
      error = Kp * scaledError;
    } else {
      // Turbo is producing pressure - now we can start actually controlling it
      if ( derivate > rampThreshold ) {
        // Boost is building extremely quickly, we need to take corrective action - multiply the derivative by rampFactor
        derivate = derivate * rampFactor;
        if (scaledError > 0) {
          // We are below setpoint
          if (scaledError < rampActive) {
            // We haven't overshot yet but we are approaching setpoint. Reduce upwards momentum
            controls.mode = 5;                 // Under but accelerating upwards rapidly
            integral += Ki * underGain * scaledError * timeChange; 
            error = Kp * underGain * scaledError;
          } else {
            // We haven't overshot and we're still somewhat far from the target. We will continue as normal. 
            controls.mode = 8;                    // Normal PID
            if (!(controls.prevPidOutput>=0.99 && error > 0) && !(controls.prevPidOutput <= 0 && error < 0)) {
              // If we are at the upper or lower limit then don't integrate, otherwise go ahead
              integral += Ki * scaledError * timeChange; 
            }
            error = Kp * scaledError;
          }
        } else {
          // We've overshot and we're still building fast, pull back hard with proportional control, chop off the integral fast
          controls.mode = 4;                  // Overshot and still accelerating upwards
          if (!(controls.prevPidOutput>=0.99 && error > 0) && !(controls.prevPidOutput <= 0 && error < 0)) {
            integral += Ki * overGain * scaledError * timeChange;
          }
          error = Kp * overGain * scaledError;
        }
      } else {
        if (-scaledError > maxPosErrorPct) {
          //We're quite a bit over
          controls.mode = 3;                   // Over but not steeply accelerating
          if (!(controls.prevPidOutput>=0.99 && error > 0) && !(controls.prevPidOutput <= 0 && error < 0)) {
            // If we are at the upper or lower limit then don't integrate, otherwise go ahead
            integral += Ki * overGain * scaledError * timeChange; 
          } 
          error = Kp * overGain * scaledError;
        } else if (abs(scaledError) < fineBand) {
          // We're not on a steep upwards slope and we're close to the setpoint. Switch to fine control mode, disable derivative.
          controls.mode = 7;
          if (!(controls.prevPidOutput>=0.99 && error > 0) && !(controls.prevPidOutput <= 0 && error < 0)) {
            integral += Ki * fineGain * scaledError * timeChange;
          }
          error = Kp * fineGain * scaledError;
          derivate = fineGain * derivate;
        } else {
          // We are spooled but everything is normal; we can use normal PID
          controls.mode = 2;                    // Normal PID
          if (!(controls.prevPidOutput>=0.99 && error > 0) && !(controls.prevPidOutput <= 0 && error < 0)) {
            // If we are at the upper or lower limit then don't integrate, otherwise go ahead
            integral += Ki * scaledError * timeChange; 
          }
          error = Kp * scaledError;
        }
      }
    }

    /* Can't have a value below zero... */
    if ( integral < 0 ) {
      integral = 0;
    } else if ( integral > 1 ) {
      integral = 1;
    }

    /* We can bias the signal when requesting boost - do we want boost to come on faster or slower */
    if (error>0) {
      error = (error * (float)settings.boostBias) / 10; 
    } 
    else if ((error<0) && (scaledInput > PIDMaxBoost)) {
      controls.mode = 6;              // We're in emergency pullback mode
      error = (error * 2);        // If we are over PIDMaxBoost% then double the proportional response to pulling off boost - turbo saver
    }

    /* PID Output */
    controls.pidOutput = error + integral - derivate;

  } 
  else {
    // We must be over max RPM, open the vanes!
    controls.pidOutput = 0;
  }


  // now = millis();   // This was set above, if we set it again here we aren't counting the time we spent processing?
  controls.lastTime = now;

  /* If our loop goes over 100% or under 0% weird things happen!*/
  if (controls.pidOutput > 1.0) {
    controls.pidOutput = 1.0;
  } 
  else if (controls.pidOutput < 0) {
    controls.pidOutput = 0;
  }

  controls.prevPidOutput = controls.pidOutput;

  // Not sure why I used two variables here? 
  toControlVNT =  controls.pidOutput * controlSpan + controls.vntMinDc;

  controls.vntPositionDC = toControlVNT;

  /* This loop should never ever be true - a 100% output should be diff between min and max + min which should equal max
   but I'm not quite ready to remove this */
  if (controls.vntPositionDC>controls.vntMaxDc)
    controls.vntPositionDC=controls.vntMaxDc;

  if ((settings.options & OPTIONS_VANESOPENIDLE)) {  // Open the vanes fully if set that way
    controls.vntPositionDC=0;
  }

  /* Display these as real numbers - will make the logs more useful as we can try different values */
  controls.boostCalculatedP=(error);
  controls.boostCalculatedI=(integral);
  controls.boostCalculatedD=(derivate);

  unsigned char finalPos;
  finalPos = controls.vntPositionDC;

  if (settings.options & OPTIONS_VNTOUTPUTINVERTED) {
    controls.vntPositionRemapped = 255-finalPos;
  } 
  else {
    controls.vntPositionRemapped = finalPos;  
  } 
}

void updateOutputValues(bool showDebug) {
  // PWM output pins
  analogWrite(PIN_VNT_N75,controls.vntPositionRemapped);
  analogWrite(PIN_AUX_N75,controls.auxOutput);    
  /*  digitalWrite(PIN_OUTPUT1,controls.output1Enabled?HIGH:LOW);   -- Disable unused outputs for the time being
   digitalWrite(PIN_OUTPUT2,controls.output2Enabled?HIGH:LOW); */
}

void layoutLCD() {
  // Set up the LCD for later writing - clear the screen, put all the stuff that doesn't change up
  lcd.write(0xFE);
  lcd.write(0x58);
  delay(10);

  position_lcd(3,0);
  lcd.print("/");
  position_lcd(7,0);
  lcd.print("k");
  position_lcd(13,0);
  lcd.print("rpm");

  position_lcd(3,1);
  lcd.print("C");

  position_lcd(5,1);
  lcd.print("T");

  position_lcd(12,1);
  lcd.print("A");
}


byte egtState = 0;
byte lcdFlipFlop = 0;

void updateLCD() { 
  if (lcdFlipFlop == 1 ) {
    // Only update half the LCD each cycle. Allow more frequent updates without disturbing control loop.
    position_lcd(0,0);
    printn_lcd(toKpaMAP(controls.mapCorrected),3);

    position_lcd(4,0);
    /*    printn_lcd(toKpaEMP(controls.empCorrected),3); */
    /*  Print the target pressure beside the actual pressure */
    printn_lcd(toKpaMAP(controls.vntTargetPressure),3);

    position_lcd(9,0);
    printn_lcd(controls.rpmActual,4);
    lcdFlipFlop = 0;
  } else {
    position_lcd(0,1);
    printn_lcd(controls.temp1,3);

    position_lcd(6,1);
    printn_lcd(controls.tpsCorrected,3);

    position_lcd(13,1);
    printn_lcd(controls.vntPositionRemapped,3);
    lcdFlipFlop = 1;
  }
  
  if (controls.temp1 < EGT_COOL) {
    if (egtState != 1); 
    {
      // Make the screen green if it isn't already
      // set background colour - r/g/b 0-255
      lcd.write(0xFE);
      lcd.write(0xD0);
      lcd.write((uint8_t)0);
      lcd.write(50);
      lcd.write(255);
      egtState = 1;
    }
  } 
  else if (controls.temp1 < EGT_WARN) {
    if (egtState != 2); 
    {
      // Make the screen green if it isn't already
      // set background colour - r/g/b 0-255
      lcd.write(0xFE);
      lcd.write(0xD0);
      lcd.write(64);
      lcd.write(255);
      lcd.write((uint8_t)0);
      egtState = 2;
    }
  } 
  else if (controls.temp1 < EGT_ALARM) {
    if (egtState != 3); 
    {
      // Make the screen orange if it isn't already
      lcd.write(0xFE);
      lcd.write(0xD0);
      lcd.write(255);
      lcd.write(32);
      lcd.write((uint8_t)0);
      egtState = 3;
    }
  } 
  else {
    if (egtState != 4); 
    {
      // Make the screen red if it isn't already
      lcd.write(0xFE);
      lcd.write(0xD0);
      lcd.write(255);
      lcd.write((uint8_t)0);
      lcd.write((uint8_t)0);
      egtState = 4;
    }
  }        
}

void position_lcd(int col, int row) {
  // allows a simple way of specifying a particular column and row for the
  // cursor to move to on the LCD. Specify starting from 0,0
  col = col + 1;
  row = row + 1;
  lcd.write(0xFE);
  lcd.write(0x47);
  lcd.write(col);
  lcd.write(row);
}

void printn_lcd(int lcdnumber, int lcddigits) {
  int padding = 0;
  if (lcdnumber < 10) {
    padding = lcddigits - 1;
  }
  else if (lcdnumber < 100) {
    padding = lcddigits - 2;
  }
  else if (lcdnumber < 1000) {
    padding = lcddigits - 3;
  }
  int i;
  for (i = 0; i < padding; i++) {
    lcd.print("0");
  }
  lcd.print(lcdnumber);
}

void displayPage(char page,char data) {
  switch(page) {
  case 1:
    pageStatusAndAdaption(data);
    break;
  case 2:
    pageServoFineTune(data);
    break;    
  case 3:
    pageMapEditor(0,data);
    visualizeActuator(28);

    break;
  case 4:
    pageMapEditor(1,data);
    visualizeActuator(28);

    break; 
  case 5:
    pageMapEditor(2,data);
    visualizeActuator(28);
    break;
  case 6:
    pageMapEditor(3,data);
    break;			
  case 7:
    pageMapEditor(4,data);
    break;            			
  case 8:
    pageOutputTests(data);
    visualizeActuator(28);
    break;
  case 9:
    pageExport(data);
    break;
  case 10:
    pageDataLogger(data);
    break;
  case 0:
  default:
    pageAbout(data);
  }
}

bool freezeModeEnabled=false;

unsigned long serialLoop = 0;
unsigned long execLoop = 0;
unsigned long displayLoop = 0;


void loop() {

  static char lastPage;

  //We started executing at...
  execTimeRead=millis();

  readValuesTps();
  readValuesMap();

  execTimeRead=millis() - execTimeRead;

  /* Actual execution will happen every EXEC_DELAY - this is where we do our actual calculations */

  if ((millis() - execLoop) >= EXEC_DELAY) {

    execTimeAct = millis();

    // Reading the thermocouple takes a bit and the signal is quite clean; reading it a few times per second is sufficient
    readValuesEgt();

    // We will actually process our values and change actuators every EXEC_DELAY milliseconds
    if (freezeModeEnabled) {
      Serial.print("\rFREEZE ");
    } 
    else {
      // update output values according to input
      calcRpm();
      processValues();
      updateOutputValues(false);
    }  
    execLoop = millis();

    execTimeAct = execLoop - execTimeAct;
  }


  /* we are only going to actualy process every SERIAL_LOOP_DELAY milliseconds though we will read from our sensors every loop
   This way we can get high resolution readings from the sensors without waiting for the actual calculations to occur every
   single time */

  else if ((millis() - serialLoop) >= SERIAL_DELAY) {  

    unsigned char data = 0;

    // User interface for configuration and monitoring
    if (Serial.available()) {
      data = Serial.read();
      if (data >= '0' && data <= '9') {
        page = data - '0';
      }   
      else if (data == ':') {
        freezeModeEnabled = !freezeModeEnabled;
      } 
      else if (data == '#') {
        page = 10;
      } 
      else if (data == 27) {
        data = Serial.read();
        if (data == '[') {
          data = Serial.read();
          switch (data) {
          case 'A':
            data = 'k';
            break;
          case 'B':
            data = 'j';
            break;
          case 'C':
            data = 'l';
            break;
          case 'D':
            data = 'h';
            break;

          default:
            data = 0;
          }
        }
      }
      lastPage = page;
    }
    displayPage(page,data);     
    serialLoop = millis();
  }
  
  /* The LCD also takes a while to update; only do the update every DISPLAY_DELAY ms as we don't need a blur where numbers should be */
  
  else if ((millis() - displayLoop) >= DISPLAY_DELAY) {
    execTimeLcd = millis();
    // We will only update the LCD every DISPLAY_DELAY milliseconds
    updateLCD();
    displayLoop = millis();
    execTimeLcd = displayLoop - execTimeLcd;
  }
}














