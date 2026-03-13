#include "DS3231.h"
#include <EEPROM.h>
#define __BAUD__ 115200
#define __FF__ 0.02
#define _FuelPin_ A0
#define _OilPin_ A1
#define PRESS 1
#define RELEASE 0
#define ERR_LOW 100
#define ERR_HIGH 1000
struct calib
{
    uint16_t OilCalib[6];
    bool OilCalib2Points=false;
    uint16_t FuelCalib[6];
    bool FuelCalib2Points=false;
};

HardwareSerial *nextion = &Serial2;
RTClib rtc;
DS3231 myRTC;
DateTime now;
calib Calibration;
float pres = 0;
float oilPres = 0;
float tempPresFuel = 0;
float tempPresOil = 0;
//double oilPres = 0, oilPresAVG=0;
//float presAVG = 0;
enum TIME_SET_BUTTON_IDS {addH=6,addM,addS,subH,subM,subS};
enum CALIB_BUTTON_IDS {BTN_PCT0=1, BTN_PCT20, BTN_PCT40, BTN_PCT60, BTN_PCT80, BTN_PCT100, BTN_RESET, BTN_BACK, BTN_SAVE, BTN_2P6P=21};
enum MAIN_PAGE_BUTTON_IDS {FUEL_GAUGE=1,OIL_GAUGE=9};
enum PAGES {Page_Main, Page_Time_Setting, Page_Fuel_Calib, Page_Oil_Calib};
// DBG - timer for periodic debug printout
// LPS - timer for loops per second
enum TIMERS { LOOP, LPS, __DEAD_MEMORY__, TIME_OUT, DBG};

uint32_t timer[sizeof(TIMERS)];
uint32_t longPress[10];
char buff[20];
char inbuffer[20];
bool dummy=true;

uint16_t calibPressure(int pin){
    uint16_t total=0;
    uint8_t nbr=10;
    for(uint8_t i=0; i<nbr;i++)
    {
        total+=analogRead(pin);
        delay(10);
    }
    return total/nbr;
}

void initTimers(uint32_t *t, uint8_t count)
{
    uint32_t currMill= millis();
    for(uint8_t i=0;i<count;i++)
    {
        t[i]= currMill;
    }
}

uint16_t _map(uint16_t input, uint16_t inLow, uint16_t inHigh, uint16_t outLow, uint16_t outHigh)
{
    if(input<inLow) input = inLow;
    if(input>inHigh) input = inHigh;
    return (float)(input-inLow)/(inHigh-inLow) * (outHigh-outLow) + outLow;
}

void sendCmd(HardwareSerial* display, char* buff)
{
            //sprintf(buff,"\"%dC\"",(int)myRTC.getTemperature());
            //nextion->write("temp.txt=");
            display->print(buff);
            display->write(0xFF);
            display->write(0xFF);
            display->write(0xFF);
}

void displayCalib(uint16_t* thing,uint8_t size, bool mode2P)
{
    for(uint8_t i=0;i<size/sizeof(thing);i++)
    {
        if(!mode2P || i==0 || i==((size/sizeof(thing))-1))
        {
            memset(buff,0,sizeof(buff));
            sprintf(buff,"n%d.val=%d",i,thing[i]);
            sendCmd(nextion,buff);
        }
        else
        {
            memset(buff,0,sizeof(buff));
            sprintf(buff,"n%d.val=%d",i,-1);
            sendCmd(nextion,buff);
        }
    }
}

uint8_t mapToCalib(uint16_t input,uint16_t* calib,uint16_t calSize,bool mode2P)
{
    if(mode2P)
    {
        return _map(input, calib[0], calib[(calSize/sizeof(calib))-1], 0, 100);
    }
    else
    {
        float stepSize = 100/(calSize/sizeof(calib)-1);
        //Serial.print("stepSize: "); Serial.println(stepSize);
        for(uint8_t i=0;i<calSize/sizeof(calib);i++)
        {
            if(input<calib[i])
            {
                //Serial.print("input: "); Serial.println(input);
                //Serial.print("i: "); Serial.println(i);
                //Serial.print("calib[i-1]: "); Serial.println(calib[i-1]);
                //Serial.print("calib[i]: "); Serial.println(calib[i]);
                //Serial.print("uint16_t((i-1)*stepSize): "); Serial.println(uint16_t((i-1)*stepSize));
                //Serial.print("uint16_t(i*stepSize): "); Serial.println(uint16_t(i*stepSize));
                if(i==0)
                { return 0; }
                else
                { return _map(input,  calib[i-1], calib[i], uint16_t((i-1)*stepSize), uint16_t(i*stepSize)); }
            }
        }
        return 100;
    }
}
void reset_watchdog()
{
  asm( "wdr ");
}

void SetUpWDT()
{
  reset_watchdog();
  // enable watchdog settings modification - set WDCE
  // enable system reset watchdog - set WDE to 1
  // both of these must be set on one line otherwise it does't work...
  WDTCSR |= 1<<WDCE|1<<WDE; 
  // set watchdog prescaler to 64K cycles 0.5s , WDP3 = 0, WDP2 = 1, WDP1 =0, WDP0=1, set WDE also, because using |= operator doesn't work at all on this line...
  WDTCSR = 1<<WDE|0<<WDP3|1<<WDP2|0<<WDP1|1<<WDP0; 
}

void SetVref5V()
{
    analogReference(DEFAULT);
    analogRead(_OilPin_); // this one will probably be wrong
    analogRead(_OilPin_); // this one will probably be wrong
    analogRead(_OilPin_); // this one will probably be wrong
    analogRead(_FuelPin_); // this one will probably be wrong
    analogRead(_FuelPin_); // this one will probably be wrong
    analogRead(_FuelPin_); // this one will probably be wrong
}

void SetVref1_1V()
{
    analogReference(INTERNAL1V1);
    analogRead(_OilPin_); // this one will probably be wrong
    analogRead(_OilPin_); // this one will probably be wrong
    analogRead(_OilPin_); // this one will probably be wrong
    analogRead(_FuelPin_); // this one will probably be wrong
    analogRead(_FuelPin_); // this one will probably be wrong
    analogRead(_FuelPin_); // this one will probably be wrong
}

void setup() {
    SetUpWDT();
    // initialize both serial ports:
    Serial.begin(__BAUD__);
    nextion->begin(__BAUD__);
    Wire.begin();
    Serial.println("serial, OK");
// Init timers
    initTimers(timer,sizeof(TIMERS));
// Init calibration points
    EEPROM.get(0,Calibration);
// Init pullups
    pinMode(_OilPin_,INPUT_PULLUP);
    pinMode(_FuelPin_,INPUT_PULLUP);
//  Hide useless buttons
    //sendCmd(nextion, "vis 3,0");
    //sendCmd(nextion, "vis 4,0");
    //sendCmd(nextion, "vis 11,0");
//  Print current time from RTC
    memset(buff,sizeof(buff),20);
    sprintf(buff,"20%02d-%02d-%02d %02d:%02d:%02d",myRTC.getYear(),myRTC.getMonth(dummy),myRTC.getDate(),myRTC.getHour(dummy,dummy), myRTC.getMinute(), myRTC.getSecond());
    Serial.print("current Time:");
    Serial.println(buff);
// Setup ADC 
    SetVref1_1V();
    //SetVref5V();
// Init Reading
    //pres = calibPressure(_FuelPin_);
    //oilPres = calibPressure(_OilPin_);
}
void loop() {
    //Serial.print("timer[DBG]: "); Serial.println(timer[DBG]);
    timer[LPS]=micros();

    memset(inbuffer,0,sizeof(inbuffer));
    if (nextion->available()) 
    {
        // sendme response, what current page is being displayed
        if(0x66 == nextion->peek())
        {
            memset(inbuffer,0,sizeof(inbuffer));
            for(int i=0;i<5;i++)
            {
                timer[TIME_OUT] = millis();
                while(!nextion->available() && millis()-timer[TIME_OUT]<=10)
                {
                    delay(1);
                }
                inbuffer[i] = nextion->read();
            }

            if(inbuffer[1] == Page_Oil_Calib)
            {
                //calibPressure(_OilPin_);
                //sprintf(buff,"n6.val=%d",calibPressure(_OilPin_));
                sprintf(buff,"n6.val=%d",(uint16_t)oilPres);
                sendCmd(nextion,buff);
            }
            else if(inbuffer[1] == Page_Fuel_Calib)
            {
                //calibPressure(_FuelPin_);
                //sprintf(buff,"n6.val=%d",calibPressure(_FuelPin_));
                sprintf(buff,"n6.val=%d",(uint16_t)pres);
                sendCmd(nextion,buff);
            }

        }
        // touch event 0x65 page id press/release 0xff 0xff 0xff
        else if(0x65 == nextion->peek())
        {
            memset(inbuffer,0,sizeof(inbuffer));
            for(int i=0;i<7;i++)
            {
                timer[TIME_OUT] = millis();
                while(!nextion->available() && millis()-timer[TIME_OUT]<=10)
                {
                    delay(1);
                }
                inbuffer[i] = nextion->read();
            }
            // Parse time setting buttons
            if(inbuffer[1]==Page_Time_Setting && inbuffer[2]==addH && inbuffer[3]==RELEASE)
            { myRTC.setHour(myRTC.getHour(dummy,dummy)+1); }
            else if(inbuffer[1]==Page_Time_Setting && inbuffer[2]==subH && inbuffer[3]==RELEASE)
            { myRTC.setHour(myRTC.getHour(dummy,dummy)-1); }
            else if(inbuffer[1]==Page_Time_Setting && inbuffer[2]==addM && inbuffer[3]==RELEASE)
            { myRTC.setMinute(myRTC.getMinute()+1); }
            else if(inbuffer[1]==Page_Time_Setting && inbuffer[2]==subM && inbuffer[3]==RELEASE)
            { myRTC.setMinute(myRTC.getMinute()-1); }
            else if(inbuffer[1]==Page_Time_Setting && inbuffer[2]==addS && inbuffer[3]==RELEASE)
            { myRTC.setSecond(myRTC.getSecond()+1); }
            else if(inbuffer[1]==Page_Time_Setting && inbuffer[2]==subS && inbuffer[3]==RELEASE)
            { myRTC.setSecond(myRTC.getSecond()-1); }

            // Long press for fuel calibration screen
            if(inbuffer[1]==Page_Main && inbuffer[2]==FUEL_GAUGE && inbuffer[3]==PRESS)
            { longPress[0]=millis(); }
            else if(inbuffer[1]==Page_Main && inbuffer[2]==FUEL_GAUGE && inbuffer[3]==RELEASE)
            { 
                if(millis()-longPress[0]>3000 && millis()-longPress[0]<10000)
                { 
                    sendCmd(nextion, "page Fuel_calib");
                    displayCalib(Calibration.FuelCalib,sizeof(Calibration.FuelCalib),Calibration.FuelCalib2Points);
                }
            }

            // Long press for oil calibration screen
            if(inbuffer[1]==Page_Main && inbuffer[2]==OIL_GAUGE && inbuffer[3]==PRESS)
            { longPress[0]=millis(); }
            else if(inbuffer[1]==Page_Main && inbuffer[2]==OIL_GAUGE && inbuffer[3]==RELEASE)
            { 
                if(millis()-longPress[0]>3000 && millis()-longPress[0]<10000)
                {
                    sendCmd(nextion, "page Oil_calib");
                    displayCalib(Calibration.OilCalib,sizeof(Calibration.OilCalib),Calibration.OilCalib2Points);
                }
            }

            // Fuel calibration
            if(inbuffer[1]==Page_Fuel_Calib && inbuffer[2]>=BTN_PCT0 && inbuffer[2]<=BTN_PCT100 && inbuffer[3]==PRESS)
            { 
                //Calibration.FuelCalib[inbuffer[2]-BTN_PCT0] = calibPressure(_FuelPin_);
                Calibration.FuelCalib[inbuffer[2]-BTN_PCT0] = (uint16_t)pres;
                displayCalib(Calibration.FuelCalib,sizeof(Calibration.FuelCalib),Calibration.FuelCalib2Points);
            }
            else if(inbuffer[1]==Page_Fuel_Calib && inbuffer[2]==BTN_RESET && inbuffer[3]==PRESS) //Reset
            {
                for(uint8_t i=0;i<sizeof(Calibration.FuelCalib)/sizeof(Calibration.FuelCalib[0]);i++)
                {
                    Calibration.FuelCalib[i] = i*1023/((sizeof(Calibration.FuelCalib)/sizeof(Calibration.FuelCalib[0])-1));
                }
                displayCalib(Calibration.FuelCalib,sizeof(Calibration.FuelCalib),Calibration.FuelCalib2Points);
            }
            else if(inbuffer[1]==Page_Fuel_Calib && inbuffer[2]==BTN_2P6P && inbuffer[3]==PRESS) //2P - 6P
            {
                if(Calibration.FuelCalib2Points)
                {Calibration.FuelCalib2Points=0;}
                else
                {Calibration.FuelCalib2Points=1;}
                displayCalib(Calibration.FuelCalib,sizeof(Calibration.FuelCalib),Calibration.FuelCalib2Points);
            }
            else if(inbuffer[1]==Page_Fuel_Calib && inbuffer[2]==BTN_SAVE && inbuffer[3]==PRESS) //Save
            {
                EEPROM.put(0,Calibration);
            }

            // Oil calibration
            if(inbuffer[1]==Page_Oil_Calib && inbuffer[2]>=BTN_PCT0 && inbuffer[2]<=BTN_PCT100 && inbuffer[3]==PRESS)
            { 
                //Calibration.OilCalib[inbuffer[2]-BTN_PCT0] = calibPressure(_OilPin_);
                Calibration.OilCalib[inbuffer[2]-BTN_PCT0] = (uint16_t)oilPres;
                displayCalib(Calibration.OilCalib,sizeof(Calibration.OilCalib),Calibration.OilCalib2Points);
            }
            else if(inbuffer[1]==Page_Oil_Calib && inbuffer[2]==BTN_RESET && inbuffer[3]==PRESS) //Reset
            {
                for(uint8_t i=0;i<sizeof(Calibration.OilCalib)/sizeof(Calibration.OilCalib[0]);i++)
                {
                    Calibration.OilCalib[i] = i*1023/((sizeof(Calibration.OilCalib)/sizeof(Calibration.OilCalib[0])-1));
                }
                displayCalib(Calibration.OilCalib,sizeof(Calibration.OilCalib),Calibration.OilCalib2Points);
            }
            else if(inbuffer[1]==Page_Oil_Calib && inbuffer[2]==BTN_2P6P && inbuffer[3]==PRESS) //2P - 6P
            {
                if(Calibration.OilCalib2Points)
                {Calibration.OilCalib2Points=0;}
                else
                {Calibration.OilCalib2Points=1;}

                displayCalib(Calibration.OilCalib,sizeof(Calibration.OilCalib),Calibration.OilCalib2Points);
            }
            else if(inbuffer[1]==Page_Oil_Calib && inbuffer[2]==BTN_SAVE && inbuffer[3]==PRESS) //Save
            {
                EEPROM.put(0,Calibration);
            }
                
        }
        else
        {
            Serial.print(nextion->read(),HEX);
        }

        //int inByte = nextion->read();
        //Serial.write(inByte);
        //Serial.print(inByte, HEX);
      }

    //Main update loop every 100ms update main indicators
    if(millis()-timer[LOOP]>100)
    {
            timer[LOOP]=millis();
            sendCmd(nextion, "sendme"); //get current page number

    //      Set fuel level and if input is out of range display error messages

            pres -= (pres-analogRead(_FuelPin_))*__FF__;
            if(tempPresFuel>ERR_HIGH)
            {
                sendCmd(nextion,"j0.bpic=4"); //ERR: OPEN
                sendCmd(nextion,"j0.val=0");
            }
            else if(tempPresFuel<ERR_LOW)
            {
                sendCmd(nextion,"j0.bpic=5"); //ERR: SHORT
                sendCmd(nextion,"j0.val=0");
            }
            else
            {
                sendCmd(nextion,"j0.bpic=0");
                sprintf(buff,"j0.val=%d",mapToCalib((uint16_t)pres, Calibration.FuelCalib,sizeof(Calibration.FuelCalib),Calibration.FuelCalib2Points));
                sendCmd(nextion,buff);
            }

    //      Set oil level
            oilPres -= (oilPres-analogRead(_OilPin_))*__FF__;
            if(tempPresOil>ERR_HIGH)
            {
                sendCmd(nextion,"j1.bpic=4"); //ERR: OPEN
                sendCmd(nextion,"j1.val=0");
            }
            else if(tempPresOil<ERR_LOW)
            {
                sendCmd(nextion,"j1.bpic=5"); //ERR: SHORT
                sendCmd(nextion,"j1.val=0");
            }
            else
            {
                sendCmd(nextion,"j1.bpic=2");
                sprintf(buff,"j1.val=%d",mapToCalib((uint16_t)oilPres, Calibration.OilCalib, sizeof(Calibration.OilCalib), Calibration.OilCalib2Points));
                sendCmd(nextion,buff);
            }

    //      Set time
            memset(buff,sizeof(buff),20);
            sprintf(buff,"time.txt=\"%02d:%02d:%02d\"",myRTC.getHour(dummy,dummy), myRTC.getMinute(), myRTC.getSecond());
            sendCmd(nextion,buff);

    //      Set temperature
            memset(buff,sizeof(buff),20);
            sprintf(buff,"temp.txt=\"%dC\"",(int)myRTC.getTemperature());
            sendCmd(nextion,buff);
    }
    //Debug loop runs every 1000ms print debug info to serial
    if(millis()-timer[DBG]>1000)
    {
        SetVref5V();
        tempPresFuel = analogRead(_FuelPin_);
        tempPresOil = analogRead(_OilPin_);
        SetVref1_1V();
       // Serial.print("millis()-timer[DBG]: "); Serial.println(millis()-timer[DBG]);
       // Serial.print("timer[DBG]: "); Serial.println(timer[DBG]);
       // Serial.print("millis(): "); Serial.println(millis());
        timer[DBG]=millis();
        //Serial.print("after timer[DBG]=millis(): "); Serial.println(timer[DBG]);
        Serial.print("LPS: "); Serial.println(1000000/(micros()-timer[LPS]));
        //Serial.print("Calibration.FuelCalib2Points: "); Serial.println(Calibration.FuelCalib2Points);
        //Serial.print("DGB: "); Serial.println(DBG);
    }
    reset_watchdog();
}
