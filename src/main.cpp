// darksec-pager — WiFi IRC chat client for the LilyGo T-LoRa-Pager.
// Joins #DarksecHQ on irc.libera.chat (TLS); bridges to the darksec web chat.
//
// Features: on-device WiFi scan/connect, custom nickname, haptic + backlight
// notifications, NTP timestamps, colored nicks, word-wrap, battery gauge,
// DarkCell boot splash + idle screensaver (adjustable). Settings in NVS.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <Preferences.h>
#include <FS.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include <NimBLEDevice.h>
#include <TinyGPSPlus.h>
#include <driver/i2s.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <qrcode.h>
#include <math.h>
#include <esp_sleep.h>

#ifndef BOARD_TDECK
#include <Adafruit_TCA8418.h>
#include <RotaryEncoder.h>
#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>
#include <ExtensionIOXL9555.hpp>
#include <HapticDrivers.hpp>   // T-Deck has no haptic motor; this lib isn't needed there
#endif

#if __has_include("private_config.h")
#include "private_config.h"
#endif

#ifndef DEFAULT_SSID
#define DEFAULT_SSID ""
#endif
#ifndef DEFAULT_PASS
#define DEFAULT_PASS ""
#endif
// Optional fallback network (used when the primary/hotspot isn't in range).
#ifndef DEFAULT_SSID2
#define DEFAULT_SSID2 ""
#endif
#ifndef DEFAULT_PASS2
#define DEFAULT_PASS2 ""
#endif
#ifndef DEFAULT_NICK
#define DEFAULT_NICK "DarkSecPager"
#endif
// POSIX timezone string (auto-handles DST). Default = US Eastern.
// Central: "CST6CDT,M3.2.0,M11.1.0"  Mountain: "MST7MDT,..."  Pacific: "PST8PDT,..."  UTC: "UTC0"
#ifndef DEFAULT_TZ
#define DEFAULT_TZ "EST5EDT,M3.2.0,M11.1.0"
#endif
#ifndef DEFAULT_OTA_PASS
#define DEFAULT_OTA_PASS "changeme"
#endif
#ifndef DEFAULT_EMAIL
#define DEFAULT_EMAIL ""
#endif
#ifndef DEFAULT_EMAIL_PASS
#define DEFAULT_EMAIL_PASS ""
#endif

// ───────────────────────── config ─────────────────────────
static const char* IRC_HOST  = "irc.libera.chat";
static const uint16_t IRC_PORT = 6697;
static const char* IRC_CHAN  = "#DarksecHQ";
static const char* IMAP_HOST = "imap.gmail.com";
static const uint16_t IMAP_PORT = 993;
static const char* OTA_HOST = "darksec-pager";
static const char* OTA_PASS = DEFAULT_OTA_PASS;
#define DRV2605_ADDR 0x5A
// ────────────────────────────────────────────────────────────

#ifndef BOARD_TDECK
// ST7796 subclass: runtime offset adjustment + a corrected writeAddrWindow.
// (Arduino_GFX 1.3.9's ST7796 never applies _yStart to RASET, so the vertical
// offset had no effect — this override fixes it.)
class DarkGFX : public Arduino_ST7796 {
public:
  using Arduino_ST7796::Arduino_ST7796;
  void setOffsets(uint8_t xo,uint8_t yo){   // rot3: _xStart=ROW_OFFSET2, _yStart=COL_OFFSET1
    COL_OFFSET1=yo; COL_OFFSET2=yo; ROW_OFFSET1=xo; ROW_OFFSET2=xo;
    setRotation(TFT_ROTATION);
  }
  void writeAddrWindow(int16_t x,int16_t y,uint16_t w,uint16_t h) override {
    x += _xStart; y += _yStart;
    _bus->writeC8D16D16(0x2A, x, x + w - 1);   // CASET
    _bus->writeC8D16D16(0x2B, y, y + h - 1);   // RASET
    _bus->writeCommand(0x2C);                  // RAMWR
  }
};
#else
// ST7789 subclass: same offset-fix pattern as DarkGFX (ST7796) above, kept
// here preemptively so the on-device Calibrate Screen menu works the same
// way on T-Deck even if this particular controller doesn't need the fix.
class DarkGFX : public Arduino_ST7789 {
public:
  using Arduino_ST7789::Arduino_ST7789;
  void setOffsets(uint8_t xo,uint8_t yo){
    COL_OFFSET1=yo; COL_OFFSET2=yo; ROW_OFFSET1=xo; ROW_OFFSET2=xo;
    setRotation(TFT_ROTATION);
  }
  void writeAddrWindow(int16_t x,int16_t y,uint16_t w,uint16_t h) override {
    x += _xStart; y += _yStart;
    _bus->writeC8D16D16(0x2A, x, x + w - 1);   // CASET
    _bus->writeC8D16D16(0x2B, y, y + h - 1);   // RASET
    _bus->writeCommand(0x2C);                  // RAMWR
  }
};
#endif

Arduino_DataBus* bus = nullptr;
Arduino_GFX* gfx = nullptr;
DarkGFX* panel = nullptr;
#ifndef BOARD_TDECK
ExtensionIOXL9555 io;
PowersBQ25896 PPM;
Adafruit_TCA8418 keyboard;
RotaryEncoder* enc = nullptr;
HapticDriver_DRV2605 haptic;
IRAM_ATTR void encTick(){ if(enc) enc->tick(); }
#else
// 5-way trackball: ISR pulse counters per direction + click state.
static volatile uint32_t tbUp=0, tbDown=0, tbLeft=0, tbRight=0;
IRAM_ATTR void tbUpISR(){ tbUp++; }
IRAM_ATTR void tbDownISR(){ tbDown++; }
IRAM_ATTR void tbLeftISR(){ tbLeft++; }
IRAM_ATTR void tbRightISR(){ tbRight++; }
#endif
TinyGPSPlus gps;
HardwareSerial GPSser(1);
bool hapticOk = false;
Preferences prefs;

// ---- keyboard matrix ----
enum { KC_ENTER='\r', KC_BKSP='\b' };
#ifndef BOARD_TDECK
#define KB_ROWS 4
#define KB_COLS 10
enum { KC_FN=0x01, KC_SHIFT=0x02, KC_CAPS=0x03 };
struct KV { char a,b,c; };
static const KV KEYMAP[KB_ROWS][KB_COLS] = {
  {{'q','Q','1'},{'w','W','2'},{'e','E','3'},{'r','R','4'},{'t','T','5'},{'y','Y','6'},{'u','U','7'},{'i','I','8'},{'o','O','9'},{'p','P','0'}},
  {{'a','A','*'},{'s','S','/'},{'d','D','+'},{'f','F','-'},{'g','G','='},{'h','H',':'},{'j','J','\''},{'k','K','"'},{'l','L','@'},{KC_ENTER,KC_ENTER,KC_ENTER}},
  {{KC_FN,KC_FN,KC_FN},{'z','Z','_'},{'x','X','$'},{'c','C',';'},{'v','V','?'},{'b','B','!'},{'n','N',','},{'m','M','.'},{KC_SHIFT,KC_SHIFT,KC_CAPS},{KC_BKSP,KC_BKSP,KC_BKSP}},
  {{' ',' ',' '},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0}},
};
static bool fnDown=false, shiftDown=false, capsLock=false;
#endif

// ---- UI palette ----
static int SCR_W=480, SCR_H=222;
#define LCD_BG     BLACK
#define LCD_DARK   RGB565(2,14,5)
#define LCD_PANEL  RGB565(7,26,10)
#define LCD_GRID   RGB565(17,64,24)
#define LCD_GREEN  RGB565(95,245,105)
#define LCD_DIM    RGB565(48,125,58)
#define LCD_TEXT   RGB565(190,255,190)
#define LCD_AMBER  RGB565(245,190,65)
#define LCD_WARN   RGB565(255,80,60)
#define ARA_RED  LCD_GREEN
#define ARA_DIM  LCD_GRID
#define ARA_DEEP LCD_DARK
#define TXT      LCD_TEXT
#define DIMTXT   LCD_DIM
#define GRN      LCD_AMBER
static const int LINE_H=16, CHARW=12;
static bool otaActive=false;
static const uint16_t NICKPAL[]={LCD_GREEN,LCD_AMBER,RGB565(90,220,220),RGB565(160,220,90),
  RGB565(120,190,255),RGB565(220,220,110),RGB565(90,180,120),RGB565(210,255,160)};
static uint16_t nickColor(const String& n){uint32_t h=0;for(char c:n)h=h*31+(uint8_t)c;return NICKPAL[h%(sizeof(NICKPAL)/2)];}

// ---- messages ----
#define MAX_MSGS 80
struct Msg { String who, text, ts; bool self; };
static Msg msgs[MAX_MSGS];
static int msgCount=0, scrollOff=0, unread=0;
static String nowHHMM(){ struct tm t; if(getLocalTime(&t,5)){int h=t.tm_hour%12; if(h==0)h=12; char b[8];snprintf(b,8,"%d:%02d%c",h,t.tm_min,t.tm_hour<12?'a':'p');return String(b);} return "--:--"; }
static void pushMsg(const String& who,const String& text,bool self){
  Msg m{who,text,nowHHMM(),self};
  if(msgCount<MAX_MSGS) msgs[msgCount++]=m;
  else { for(int i=0;i<MAX_MSGS-1;i++)msgs[i]=msgs[i+1]; msgs[MAX_MSGS-1]=m; }
}

// ---- settings (NVS) ----
static String cfgSsid, cfgPass, cfgNick, cfgEmail, cfgEmailPass, cfgWigleName, cfgWigleToken;
static String cfgSsid2=DEFAULT_SSID2, cfgPass2=DEFAULT_PASS2;   // fallback network
static bool notifyEnabled=true;
static int saverSec=60;
static int cfgVolume=60;   // speaker volume 0-100              // 0 = off
static int dispX=0, dispY=0;         // display alignment offsets (NVS)
static const int SAVER_OPTS[]={0,30,60,120,300};
static bool firstRunNick=false;   // true on first boot (no nick saved yet) -> prompt user
static void cfgLoad(){
  prefs.begin("pager",true);
  cfgSsid=prefs.getString("ssid",DEFAULT_SSID);
  cfgPass=prefs.getString("pass",DEFAULT_PASS);
  firstRunNick=!prefs.isKey("nick");
  cfgNick=prefs.getString("nick",DEFAULT_NICK);
  cfgWigleName=prefs.getString("wgName","");
  cfgWigleToken=prefs.getString("wgTok","");
  cfgEmail=prefs.isKey("email")?prefs.getString("email"):"";
  cfgEmailPass=prefs.isKey("emailPass")?prefs.getString("emailPass"):"";
  if(strlen(DEFAULT_SSID)>0){ cfgSsid=DEFAULT_SSID; cfgPass=DEFAULT_PASS; }
  if(!cfgEmail.length() && strlen(DEFAULT_EMAIL)>0) cfgEmail=DEFAULT_EMAIL;
  if(!cfgEmailPass.length() && strlen(DEFAULT_EMAIL_PASS)>0) cfgEmailPass=DEFAULT_EMAIL_PASS;
  notifyEnabled=prefs.getBool("notify",true);
  saverSec=prefs.getInt("saver",60);
  cfgVolume=prefs.getInt("vol",60);
  dispX=prefs.getInt("dx2",0); dispY=prefs.getInt("dy2",0);   // fresh keys => clean baseline
  prefs.end();
}
static void cfgSaveDisp(){prefs.begin("pager",false);prefs.putInt("dx2",dispX);prefs.putInt("dy2",dispY);prefs.end();}
static void cfgSaveWifi(const String&s,const String&p){prefs.begin("pager",false);prefs.putString("ssid",s);prefs.putString("pass",p);prefs.end();cfgSsid=s;cfgPass=p;}
static void cfgSaveNick(const String&raw){
  String n; for(size_t i=0;i<raw.length();i++){ char c=raw[i]; if(c==' ')c='_';
    if(isalnum((unsigned char)c)||strchr("-_[]\\`^{}|",c)) n+=c; }   // IRC-legal nick chars only
  if(!n.length()) n="pager"; if(n.length()>16) n=n.substring(0,16);
  prefs.begin("pager",false);prefs.putString("nick",n);prefs.end();cfgNick=n;
}
static void cfgSaveWigleName(const String&v){prefs.begin("pager",false);prefs.putString("wgName",v);prefs.end();cfgWigleName=v;}
static void cfgSaveWigleTok(const String&v){prefs.begin("pager",false);prefs.putString("wgTok",v);prefs.end();cfgWigleToken=v;}
static void cfgSaveEmail(const String&e){prefs.begin("pager",false);prefs.putString("email",e);prefs.end();cfgEmail=e;}
static void cfgSaveEmailPass(const String&p){prefs.begin("pager",false);prefs.putString("emailPass",p);prefs.end();cfgEmailPass=p;}
static void cfgSaveNotify(bool b){prefs.begin("pager",false);prefs.putBool("notify",b);prefs.end();notifyEnabled=b;}
static void cfgSaveSaver(int s){prefs.begin("pager",false);prefs.putInt("saver",s);prefs.end();saverSec=s;}
static void cfgSaveVolume(int v){prefs.begin("pager",false);prefs.putInt("vol",v);prefs.end();cfgVolume=v;}
static bool emailReady(){ return cfgEmail.length() && cfgEmailPass.length(); }

// ---- app state ----
enum AppState { ST_HOME, ST_CHAT, ST_EMAIL, ST_INBOX, ST_TOOLS, ST_NOTES, ST_NOTE_VIEW, ST_MENU, ST_WIFI, ST_TEXTIN, ST_SAVER, ST_CALIB, ST_FLOCK, ST_WARDRIVE, ST_TRACKER, ST_NETRECON, ST_CLOCK, ST_HASH, ST_QR, ST_VOICE };
static AppState state=ST_HOME, prevState=ST_HOME;
static bool uiDirty=true;
static String inputLine="", pendingSsid="", emailStatus="";
static uint32_t lastActivity=0;

static String tiTitle, tiBuf; static bool tiPassword=false; static int tiPurpose=0;

static const char* HOME_TABS[]={"Chat","Email","WiFi","OTA","Setup","Tools"};
static const int HOME_N=6;
static int homeSel=0;

static const char* TOOL_MENU[]={"Flock Finder","Tracker Scan","Net Recon","Wardriver","New Note","View Notes","SD Status","Clock","Hash Tool","QR Code","Voice Memo","Back"};
static const int TOOL_N=12;
static int toolSel=0;

#define MAX_NOTES 20
struct NoteItem { String path, name; };
static NoteItem notes[MAX_NOTES];
static int noteCount=0, noteSel=0, noteViewScroll=0;
static String notesStatus="SD not checked", noteViewTitle="", noteViewBody="";
static bool sdReady=false, sdTried=false;

static const char* EMAIL_MENU[]={"Inbox","Compose","Set Address","Set App Pass","Back"};
static const int EMAIL_N=5;
static int emailSel=0;

#define MAX_EMAILS 8
struct EmailItem { String from, subj, date; };
static EmailItem emails[MAX_EMAILS];
static int emailCount=0, inboxSel=0;
static String inboxStatus="Open Inbox to sync";

static const char* MENU[]={"WiFi Setup","Set Nickname","Notifications","Volume","Screensaver","Calibrate Screen","OTA Update","Reconnect IRC","Back to Chat"};
static const int MENU_N=9;
static int menuSel=0;

#define MAX_SCAN 20
static String scanSsid[MAX_SCAN]; static int scanRssi[MAX_SCAN]; static bool scanEnc[MAX_SCAN];
static int scanN=0, scanSel=0;

// ---- IRC ----
WiFiClientSecure irc;
static String ircRx="", ircNick;
static bool ircRegistered=false, ircJoined=false;
static uint32_t lastIrcAttempt=0, lastRxMs=0, lastKeepMs=0;

// ---- battery ----
static int batteryPct(){
#ifndef BOARD_TDECK
  uint16_t mv = PPM.getBattVoltage();     // mV
#else
  uint16_t mv = analogReadMilliVolts(BAT_ADC_PIN) * 2;   // T-Deck's documented 2:1 divider formula
#endif
  if(mv<2500) return -1;                  // no battery / not reading
  int pct=(int)((mv-3300)*100/(4200-3300));
  return pct<0?0:pct>100?100:pct;
}

// ───────────────────────── backlight ─────────────────────────
// tftBacklight: T-Pager drives TFT_BL as plain PWM. T-Deck's backlight is a
// dedicated 16-level chip driven by toggling the pin a counted number of
// times (per LilyGo's own HelloWorld example) — analogWrite doesn't work.
static void tftBacklight(uint8_t v){
#ifndef BOARD_TDECK
  analogWrite(TFT_BL,v);
#else
  static int level=-1;                    // -1 forces the first call to sync
  int target=map(v,0,255,0,16);
  if(level<0){ pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_BL,LOW); delay(3); level=0; }
  if(target==0){ digitalWrite(TFT_BL,LOW); delay(3); level=0; return; }
  if(level==0){ digitalWrite(TFT_BL,HIGH); level=16; delayMicroseconds(30); }
  int from=16-level, to=16-target, steps=(16+to-from)%16;
  for(int i=0;i<steps;i++){ digitalWrite(TFT_BL,LOW); digitalWrite(TFT_BL,HIGH); }
  level=target;
#endif
}
// kbBacklight: per-key keyboard backlight only exists on the T-Pager.
static void kbBacklight(uint8_t v){
#ifndef BOARD_TDECK
  analogWrite(KEYBOARD_BL,v);
#endif
}

// ───────────────────────── activity / screensaver ─────────────────────────
static void wakeBacklight(){ tftBacklight(200); kbBacklight(120); }
static void markActivity(){
  lastActivity=millis();
  if(state==ST_SAVER){ state=prevState; wakeBacklight(); uiDirty=true; }
}

// ───────────────────────── haptic ─────────────────────────
// T-Deck has no vibration motor; hapticOk stays false there so buzz() is a no-op.
static void buzz(int ms,uint8_t amp){
  if(!hapticOk) return;
#ifndef BOARD_TDECK
  haptic.setMode(HapticMode::REAL_TIME_PLAYBACK);
  haptic.setRealtimeValue(amp);
  delay(ms);
  haptic.setRealtimeValue(0);
  haptic.setMode(HapticMode::INTERNAL_TRIGGER);
#endif
}
// ───────────────────────── Audio (ES8311 speaker) ─────────────────────────
// T-Deck has a different mic/speaker setup (ES7210 array, no ES8311 DAC) —
// stubbed rather than ported; audioOk stays false so beep()/voice memo no-op
// and the UI shows "not available on this hardware" (see drawVoice()).
static bool audioOk=false;
#ifndef BOARD_TDECK
static void esW(uint8_t r,uint8_t v){ Wire.beginTransmission(ES8311_ADDR); Wire.write(r); Wire.write(v); Wire.endTransmission(); }
#else
static void esW(uint8_t,uint8_t){}
#endif
static void audioSetVolume(int pct);
static void audioInit(){
#ifdef BOARD_TDECK
  Serial.println("[audio] n/a (T-Deck has no ES8311 codec)");
  return;
#else
  i2s_config_t c={};
  c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX|I2S_MODE_RX);
  c.sample_rate=16000; c.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT; c.communication_format=I2S_COMM_FORMAT_STAND_I2S;
  c.intr_alloc_flags=0; c.dma_buf_count=6; c.dma_buf_len=256;
  c.use_apll=true; c.tx_desc_auto_clear=true; c.fixed_mclk=4096000; c.mclk_multiple=I2S_MCLK_MULTIPLE_256;
  if(i2s_driver_install(I2S_NUM_0,&c,0,NULL)!=ESP_OK){ Serial.println("[audio] i2s install FAIL"); return; }
  i2s_pin_config_t pc={}; pc.mck_io_num=I2S_MCLK_PIN; pc.bck_io_num=I2S_BCK_PIN; pc.ws_io_num=I2S_WS_PIN;
  pc.data_out_num=I2S_DOUT_PIN; pc.data_in_num=I2S_SDIN_PIN;
  i2s_set_pin(I2S_NUM_0,&pc); i2s_zero_dma_buffer(I2S_NUM_0);
  // ES8311 slave, external MCLK (4.096MHz=256*16k), 16-bit, DAC playback
  static const uint8_t seq[][2]={
    {0x44,0x08},{0x44,0x08},{0x01,0x30},{0x02,0x00},{0x03,0x10},{0x16,0x24},{0x04,0x10},{0x05,0x00},
    {0x0B,0x00},{0x0C,0x00},{0x10,0x1F},{0x11,0x7F},{0x00,0x80},{0x00,0x80},{0x01,0x3F},{0x01,0x3F},
    {0x02,0x00},{0x05,0x00},{0x03,0x10},{0x04,0x20},{0x07,0x00},{0x08,0xFF},{0x06,0x03},
    {0x09,0x0C},{0x0A,0x0C},{0x13,0x10},{0x1B,0x0A},{0x1C,0x6A},
    {0x09,0x0C},{0x0A,0x4C},{0x17,0xBF},{0x0E,0x02},{0x12,0x00},{0x14,0x1A},{0x0D,0x01},{0x15,0x40},{0x37,0x08},
    {0x32,0xBF},{0x31,0x00},
  };
  for(auto&kv:seq) esW(kv[0],kv[1]);
  audioSetVolume(cfgVolume);
  audioOk=true; Serial.println("[audio] ES8311 ready");
#endif
}
static void audioSetVolume(int pct){ if(pct<0)pct=0; if(pct>100)pct=100; esW(0x32,(uint8_t)(pct*255/100)); }
static void beep(int freq,int ms){
  if(!audioOk) return;
#ifndef BOARD_TDECK
  const int sr=16000; int total=sr*ms/1000; int16_t buf[256]; int done=0;
  while(done<total){
    int cnt=0;
    while(cnt<128 && done<total){ int16_t v=(int16_t)(7000.0f*sinf(2.0f*3.14159265f*freq*done/sr)); buf[cnt*2]=v; buf[cnt*2+1]=v; cnt++; done++; }
    size_t bw; i2s_write(I2S_NUM_0,buf,cnt*2*sizeof(int16_t),&bw,50/portTICK_PERIOD_MS);
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
#endif
}
static void notify(){
  markActivity();
  if(!notifyEnabled) return;
  beep(1200,70); beep(1650,90);                 // two-tone chime
  buzz(130,0x70);                               // + haptic
  kbBacklight(255); delay(40); kbBacklight(120);
}

// ───────────────────────── rendering ─────────────────────────
static void headerBar(const char* title){
  gfx->fillRect(0,0,SCR_W,LINE_H,LCD_DARK);
  gfx->drawFastHLine(0,0,SCR_W,LCD_GREEN);
  gfx->drawFastHLine(0,LINE_H-1,SCR_W,LCD_GRID);
  gfx->setTextSize(2); gfx->setTextColor(LCD_GREEN,LCD_DARK);
  gfx->setCursor(2,0); gfx->print("["); gfx->print(title); gfx->print("]");
  // ---- right side: status text + graphical battery gauge ----
  int bp=batteryPct();
#ifndef BOARD_TDECK
  bool chg=PPM.isVbusIn();     // USB present -> charging
#else
  bool chg=false;              // T-Deck has no PMIC to query VBUS/charge state from
#endif
  const char* st=!WiFi.isConnected()?"NO RF":(otaActive?"OTA":(!ircRegistered?"DIAL":(!ircJoined?"SYNC":"LINK")));
  // battery glyph pinned to far right
  const int bw=22,bh=10; int bx=SCR_W-2-3-bw, by=(LINE_H-bh)/2;   // 3px for terminal nub
  uint16_t bcol = bp<0?LCD_DIM : (bp<=15?LCD_WARN : (bp<=40?LCD_AMBER:LCD_GREEN));
  gfx->drawRect(bx,by,bw,bh,bcol);
  gfx->fillRect(bx+bw,by+3,3,bh-6,bcol);                          // terminal nub
  if(bp>=0){ int fw=(bw-4)*bp/100; if(fw<1&&bp>0)fw=1; gfx->fillRect(bx+2,by+2,fw,bh-4,bcol); }
  if(chg){                                                        // charging bolt overlay
    int cx=bx+bw/2, cy=by+bh/2;
    gfx->drawLine(cx+1,by+2,cx-2,cy,LCD_DARK);
    gfx->drawLine(cx-2,cy,cx+2,cy,LCD_DARK);
    gfx->drawLine(cx+2,cy,cx-1,by+bh-3,LCD_DARK);
  }
  // status text right-aligned just left of the battery glyph
  char rb[48];
  if(bp>=0 && unread>0) snprintf(rb,sizeof(rb),"U%d %d%% %s %s",unread,bp,nowHHMM().c_str(),st);
  else if(bp>=0)        snprintf(rb,sizeof(rb),"%d%% %s %s",bp,nowHHMM().c_str(),st);
  else if(unread>0)     snprintf(rb,sizeof(rb),"U%d %s %s",unread,nowHHMM().c_str(),st);
  else                  snprintf(rb,sizeof(rb),"%s %s",nowHHMM().c_str(),st);
  int w=strlen(rb)*CHARW; gfx->setTextColor(LCD_AMBER,LCD_DARK); gfx->setCursor(bx-4-w,0); gfx->print(rb);
}

// draw one message ending at bottom row byBottom, growing upward; returns new bottom
static int drawMsgUp(int yBottom,int top,const Msg& m){
  int maxc=SCR_W/CHARW;
  String prefix=m.ts+" "+m.who+": ";
  int pc=prefix.length();
  int cap0=maxc-pc; if(cap0<4)cap0=4;
  int capN=maxc-2;
  // split text into chunks: first cap0, rest capN
  String chunks[10]; int nc=0;
  const String& t=m.text; int i=0,L=t.length();
  int cap=cap0;
  while(i<L && nc<10){ int len=min(cap,L-i); chunks[nc++]=t.substring(i,i+len); i+=len; cap=capN; }
  if(nc==0) chunks[nc++]="";
  // draw bottom-up
  for(int li=nc-1; li>=0 && yBottom>=top; li--){
    int y=yBottom;
    gfx->setTextSize(2);
    if(li==0){
      gfx->setCursor(2,y);
      gfx->setTextColor(DIMTXT,BLACK); gfx->print(m.ts); gfx->print(" ");
      gfx->setTextColor(m.self?LCD_AMBER:nickColor(m.who),BLACK); gfx->print(m.who);
      gfx->setTextColor(TXT,BLACK); gfx->print(": "); gfx->print(chunks[0]);
    } else {
      gfx->setCursor(2+CHARW,y);
      gfx->setTextColor(TXT,BLACK); gfx->print(chunks[li]);
    }
    yBottom-=LINE_H;
  }
  return yBottom;
}

static void drawHome(){
  gfx->fillScreen(LCD_BG); headerBar("DARKPAGER");
  gfx->drawRect(4,LINE_H+4,SCR_W-8,SCR_H-LINE_H-18,LCD_GRID);
  gfx->drawFastHLine(8,LINE_H+27,SCR_W-16,LCD_DIM);
  gfx->setTextSize(1); gfx->setTextColor(LCD_DIM,LCD_BG);
  gfx->setCursor(12,LINE_H+12); gfx->print("FREQ 6697KHZ  MODE TLS/IRC  CH #DARKSECHQ");
  const char* codes[]={"MSG","MAIL","NET","OTA","CFG","TOOL"};
  int top=LINE_H+33, rowH=24;
  for(int i=0;i<HOME_N;i++){
    int y=top+i*rowH; bool sel=(i==homeSel); uint16_t bg=sel?LCD_PANEL:LCD_BG;
    if(sel){ gfx->fillRect(8,y,SCR_W-16,rowH-2,bg); gfx->drawRect(8,y,SCR_W-16,rowH-2,LCD_GREEN); }
    else gfx->drawFastHLine(10,y+rowH-2,SCR_W-20,LCD_DARK);
    const char* sub="";
    if(i==0) sub=unread>0?"unread chat":"irc chat";
    else if(i==1) sub=emailReady()?"mailbox":(cfgEmail.length()?"need pass":"configure");
    else if(i==2) sub=WiFi.isConnected()?"connected":"setup";
    else if(i==3) sub=otaActive?"ready":"update";
    else if(i==4) sub="settings";
    else sub="notes + sd";
    gfx->setTextSize(2); gfx->setTextColor(sel?LCD_AMBER:LCD_GREEN,bg);
    gfx->setCursor(14,y+4); gfx->print(sel?">":" "); gfx->print(codes[i]);
    gfx->setTextColor(sel?LCD_TEXT:TXT,bg); gfx->setCursor(96,y+4); gfx->print(HOME_TABS[i]);
    gfx->setTextSize(1); gfx->setTextColor(sel?LCD_AMBER:DIMTXT,bg); gfx->setCursor(250,y+8); gfx->print("// "); gfx->print(sub);
    if(i==0 && unread>0){ gfx->setTextSize(2); gfx->setTextColor(LCD_AMBER,bg); gfx->setCursor(SCR_W-68,y+4); gfx->print("U"); gfx->print(unread); }
  }
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK);
  gfx->setCursor(6,SCR_H-12); gfx->print("ROTATE=SCAN  PRESS=OPEN  BK=MSG  :: retro packet terminal");
}

static int listStart(int sel,int n,int vis);
static void scrollArrows(int start,int vis,int n,int top,int bottom);

static void drawEmail(){
  gfx->fillScreen(LCD_BG); headerBar("MAILBOX");
  gfx->drawRect(4,LINE_H+4,SCR_W-8,SCR_H-LINE_H-18,LCD_GRID);
  int top=LINE_H+4, rowH=26, vis=(SCR_H-LINE_H-16)/rowH;
  for(int i=0;i<EMAIL_N && i<vis;i++){
    int y=top+i*rowH; bool sel=(i==emailSel); uint16_t bg=sel?ARA_DIM:BLACK;
    if(sel){ gfx->fillRect(8,y,SCR_W-16,rowH,ARA_DIM); gfx->drawRect(8,y,SCR_W-16,rowH,LCD_GREEN); }
    gfx->setTextSize(2); gfx->setTextColor(sel?LCD_AMBER:TXT,bg); gfx->setCursor(14,y+5);
    gfx->print(sel?"> ":"  "); gfx->print(EMAIL_MENU[i]);
  }
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK);
  gfx->setCursor(8,SCR_H-33); gfx->print("ADDR "); gfx->print(cfgEmail.length()?cfgEmail.c_str():"<unset>");
  gfx->setCursor(8,SCR_H-22); gfx->print(cfgEmailPass.length()?"AUTH app-password cached":"AUTH required");
  gfx->setCursor(8,SCR_H-11); gfx->print(emailStatus.length()?emailStatus.c_str():"Gmail IMAP expects an app password");
}

static void drawInbox(){
  gfx->fillScreen(LCD_BG); headerBar("INBOX");
  gfx->drawRect(4,LINE_H+4,SCR_W-8,SCR_H-LINE_H-18,LCD_GRID);
  const int top=LINE_H+3, footerY=SCR_H-11, rowH=42;
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK);
  gfx->setCursor(8,top); gfx->print("RX: "); gfx->print(inboxStatus.c_str());
  if(emailCount==0){
    gfx->setTextSize(2); gfx->setTextColor(TXT,BLACK);
    gfx->setCursor(20,top+38); gfx->print("-- NO PACKETS --");
  } else {
    const int listTop=top+12;
    const int vis=max(1,(footerY-listTop)/rowH);
    const int start=listStart(inboxSel,emailCount,vis);
    for(int r=0;r<vis && start+r<emailCount;r++){
      int i=start+r, y=listTop+r*rowH; bool sel=(i==inboxSel); uint16_t bg=sel?ARA_DIM:BLACK;
      if(sel){ gfx->fillRect(8,y,SCR_W-16,rowH,ARA_DIM); gfx->drawRect(8,y,SCR_W-16,rowH,LCD_GREEN); }
      String from=emails[i].from; if(from.length()>34)from=from.substring(0,34);
      String subj=emails[i].subj; if(subj.length()>38)subj=subj.substring(0,38);
      gfx->setTextSize(1); gfx->setTextColor(sel?LCD_AMBER:ARA_RED,bg); gfx->setCursor(12,y+3); gfx->print(sel?"> ":"  "); gfx->print(from.c_str());
      gfx->setTextColor(sel?WHITE:TXT,bg); gfx->setCursor(18,y+17); gfx->print(subj.c_str());
      gfx->setTextColor(DIMTXT,bg); gfx->setCursor(18,y+30); gfx->print(emails[i].date.c_str());
    }
    scrollArrows(start,vis,emailCount,listTop,footerY);
  }
  gfx->setTextColor(DIMTXT,BLACK); gfx->setTextSize(1);
  gfx->setCursor(6,SCR_H-10); gfx->print("turn=move  press=refresh  BK=email");
}

static void drawTools(){
  gfx->fillScreen(BLACK); headerBar("TOOLS");
  const int top=LINE_H+3, footerY=SCR_H-22, rowH=26, vis=max(1,(footerY-top)/rowH);
  const int start=listStart(toolSel,TOOL_N,vis);
  for(int r=0;r<vis && start+r<TOOL_N;r++){
    int i=start+r, ry=top+r*rowH, ty=ry+(rowH-16)/2; bool sel=(i==toolSel); uint16_t bg=sel?ARA_DIM:BLACK;
    if(sel) gfx->fillRect(0,ry,SCR_W,rowH,ARA_DIM);
    gfx->setTextSize(2); gfx->setTextColor(sel?LCD_AMBER:ARA_RED,bg); gfx->setCursor(6,ty); gfx->print(sel?">":" ");
    gfx->setTextColor(sel?WHITE:TXT,bg); gfx->setCursor(28,ty); gfx->print(TOOL_MENU[i]);
  }
  scrollArrows(start,vis,TOOL_N,top,footerY);
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK);
  gfx->setCursor(6,SCR_H-20); gfx->print(sdReady?"SD mounted":"SD idle");
  gfx->setCursor(6,SCR_H-10); gfx->print(notesStatus.length()>50?notesStatus.substring(0,50):notesStatus);
}

static void drawNotes(){
  gfx->fillScreen(LCD_BG); headerBar("NOTES");
  gfx->drawRect(4,LINE_H+4,SCR_W-8,SCR_H-LINE_H-18,LCD_GRID);
  const int top=LINE_H+8, footerY=SCR_H-12, rowH=18;
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK);
  gfx->setCursor(8,top); gfx->print(notesStatus.c_str());
  if(noteCount==0){
    gfx->setTextSize(2); gfx->setTextColor(TXT,BLACK);
    gfx->setCursor(20,top+38); gfx->print("-- NO NOTES --");
  } else {
    int listTop=top+14, vis=max(1,(footerY-listTop)/rowH), start=listStart(noteSel,noteCount,vis);
    for(int r=0;r<vis && start+r<noteCount;r++){
      int i=start+r, y=listTop+r*rowH; bool sel=(i==noteSel); uint16_t bg=sel?ARA_DIM:BLACK;
      if(sel){ gfx->fillRect(8,y,SCR_W-16,rowH,ARA_DIM); }
      String n=notes[i].name; if(n.length()>54)n=n.substring(0,54);
      gfx->setTextColor(sel?LCD_AMBER:TXT,bg); gfx->setCursor(14,y+4); gfx->print(sel?"> ":"  "); gfx->print(n.c_str());
    }
    scrollArrows(start,vis,noteCount,listTop,footerY);
  }
  gfx->setTextColor(DIMTXT,BLACK); gfx->setTextSize(1);
  gfx->setCursor(6,SCR_H-10); gfx->print("turn=move  press=open  BK=tools");
}

static void drawNoteView(){
  gfx->fillScreen(LCD_BG); headerBar("NOTE VIEW");
  gfx->drawRect(4,LINE_H+4,SCR_W-8,SCR_H-LINE_H-18,LCD_GRID);
  gfx->setTextSize(1); gfx->setTextColor(LCD_AMBER,BLACK);
  String title=noteViewTitle; if(title.length()>64)title=title.substring(0,64);
  gfx->setCursor(8,LINE_H+8); gfx->print(title.c_str());
  const int top=LINE_H+22, bottom=SCR_H-14, lineH=10, maxc=(SCR_W-20)/6;
  int totalLines=max(1,(int)((noteViewBody.length()+maxc-1)/maxc));
  int vis=max(1,(bottom-top)/lineH);
  if(noteViewScroll>totalLines-vis) noteViewScroll=max(0,totalLines-vis);
  int pos=noteViewScroll*maxc;
  gfx->setTextColor(TXT,BLACK);
  for(int r=0;r<vis && pos<(int)noteViewBody.length();r++){
    String line=noteViewBody.substring(pos,min(pos+maxc,(int)noteViewBody.length()));
    gfx->setCursor(8,top+r*lineH); gfx->print(line.c_str());
    pos+=maxc;
  }
  if(totalLines>vis) scrollArrows(noteViewScroll,vis,totalLines,top,bottom);
  gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,SCR_H-10); gfx->print("turn=scroll  BK=notes");
}

static void drawChat(){
  gfx->fillScreen(LCD_BG);
  headerBar(cfgNick.c_str());
  int top=LINE_H, inputY=SCR_H-LINE_H;
  int yBottom=inputY-LINE_H;
  for(int mi=msgCount-1-scrollOff; mi>=0 && yBottom>=top; mi--)
    yBottom=drawMsgUp(yBottom,top,msgs[mi]);
  // scrolled up? show a "more recent below" arrow (+unread count)
  if(scrollOff>0){
    gfx->fillTriangle(SCR_W-10,inputY-4,SCR_W-16,inputY-12,SCR_W-4,inputY-12,GRN);
    if(unread>0){ char u[8]; snprintf(u,8,"%d",unread); gfx->setTextSize(1); gfx->setTextColor(GRN,BLACK); gfx->setCursor(SCR_W-16-strlen(u)*6,inputY-11); gfx->print(u); }
  }
  // input
  gfx->fillRect(0,inputY,SCR_W,LINE_H,LCD_DARK);
  gfx->drawFastHLine(0,inputY,SCR_W,LCD_GRID);
  gfx->setTextSize(2); gfx->setTextColor(LCD_AMBER,LCD_DARK);
  gfx->setCursor(2,inputY); gfx->print("> ");
  gfx->setTextColor(LCD_TEXT,LCD_DARK);
  String v=inputLine; int mx=SCR_W/CHARW-3; if((int)v.length()>mx)v=v.substring(v.length()-mx);
  gfx->print(v); gfx->print("_");
}

// generic windowed-list start index that keeps `sel` visible within `vis` rows
static int listStart(int sel,int n,int vis){
  int start = sel>=vis ? sel-vis+1 : 0;
  if(start>n-vis) start = (n-vis>0)?n-vis:0;
  return start;
}
static void scrollArrows(int start,int vis,int n,int top,int bottom){
  if(start>0)        gfx->fillTriangle(SCR_W-9,top+1, SCR_W-15,top+9,  SCR_W-3,top+9,  ARA_RED);
  if(start+vis<n)    gfx->fillTriangle(SCR_W-9,bottom-1,SCR_W-15,bottom-9,SCR_W-3,bottom-9,ARA_RED);
}
static void drawMenu(){
  gfx->fillScreen(LCD_BG); headerBar("SETUP");
  gfx->drawRect(4,LINE_H+4,SCR_W-8,SCR_H-LINE_H-18,LCD_GRID);
  const int top=LINE_H+3, footerY=SCR_H-11, rowH=26;
  const int vis=max(1,(footerY-top)/rowH);
  const int start=listStart(menuSel,MENU_N,vis);
  for(int r=0;r<vis && start+r<MENU_N;r++){
    int i=start+r, ry=top+r*rowH, ty=ry+(rowH-16)/2;
    bool sel=(i==menuSel); uint16_t bg=sel?ARA_DIM:BLACK;
    if(sel){ gfx->fillRect(8,ry,SCR_W-16,rowH,ARA_DIM); gfx->drawRect(8,ry,SCR_W-16,rowH,LCD_GREEN); }
    gfx->setTextSize(2);
    gfx->setTextColor(sel?LCD_AMBER:ARA_RED,bg); gfx->setCursor(14,ty); gfx->print(sel?">":" ");
    gfx->setTextColor(sel?LCD_TEXT:TXT,bg); gfx->setCursor(36,ty); gfx->print(MENU[i]);
    char vb[10]; const char* val=nullptr; uint16_t vc=GRN;
    if(i==2){ val=notifyEnabled?"ON":"OFF"; vc=notifyEnabled?GRN:DIMTXT; }
    else if(i==3){ if(cfgVolume==0)strcpy(vb,"Off"); else snprintf(vb,10,"%d%%",cfgVolume); val=vb; }
    else if(i==4){ if(saverSec==0)strcpy(vb,"Off"); else if(saverSec<60)snprintf(vb,10,"%ds",saverSec); else snprintf(vb,10,"%dm",saverSec/60); val=vb; }
    else if(i==5){ val=otaActive?"ON":"Start"; vc=otaActive?GRN:ARA_RED; }
    if(val){ int w=strlen(val)*CHARW; gfx->setTextColor(vc,bg); gfx->setCursor(SCR_W-w-16,ty); gfx->print(val); }
  }
  scrollArrows(start,vis,MENU_N,top,footerY);
  gfx->setTextColor(DIMTXT,BLACK); gfx->setTextSize(1);
  gfx->setCursor(6,SCR_H-10); gfx->print("turn=move  press=select");
}

static void drawWifi(){
  gfx->fillScreen(LCD_BG); headerBar("RF SCAN");
  gfx->drawRect(4,LINE_H+4,SCR_W-8,SCR_H-LINE_H-18,LCD_GRID);
  int y=LINE_H+8; gfx->setTextSize(2);
  if(scanN==0){ gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(12,y); gfx->print("sweeping channels..."); return; }
  int rows=(SCR_H-LINE_H-24)/(LINE_H+2); int start=scanSel>=rows?scanSel-rows+1:0;
  for(int i=start;i<scanN&&i<start+rows;i++){
    bool sel=(i==scanSel);
    if(sel){ gfx->fillRect(8,y-2,SCR_W-16,LINE_H+3,ARA_DIM); gfx->drawRect(8,y-2,SCR_W-16,LINE_H+3,LCD_GREEN); }
    gfx->setTextColor(sel?LCD_AMBER:TXT,sel?ARA_DIM:BLACK);
    gfx->setCursor(14,y); gfx->print(sel?">":" ");
    String s=scanSsid[i]; if(s.length()>22)s=s.substring(0,22); gfx->print(s);
    char rb[16]; snprintf(rb,sizeof(rb),"%d%s",scanRssi[i],scanEnc[i]?" L":" "); int w=strlen(rb)*CHARW;
    gfx->setCursor(SCR_W-w-2,y); gfx->print(rb); y+=LINE_H+2;
  }
}

static void drawTextIn(){
  gfx->fillScreen(LCD_BG); headerBar(tiTitle.c_str());
  gfx->drawRect(8,LINE_H+18,SCR_W-16,44,LCD_GRID);
  gfx->setTextSize(2); gfx->setTextColor(LCD_TEXT,BLACK); gfx->setCursor(16,LINE_H+31);
  String show; if(tiPassword){for(size_t i=0;i<tiBuf.length();i++)show+='*';} else show=tiBuf;
  int mx=(SCR_W-34)/CHARW; if((int)show.length()>mx)show=show.substring(show.length()-mx);
  gfx->print(show); gfx->print("_");
  gfx->setTextColor(DIMTXT,BLACK); gfx->setTextSize(1);
  gfx->setCursor(8,SCR_H-22); if(tiPurpose==5){ gfx->print("NOTE CHARS "); gfx->print(tiBuf.length()); gfx->print("/480"); }
  gfx->setCursor(8,SCR_H-12); gfx->print("TYPE + ENTER=STORE  BK=ABORT");
}

// Simple diamond mark for the retro pager splash.
static void darkcellMark(int cx,int cy,int rad,uint16_t col){
  for(int o=0;o<2;o++){
    gfx->drawLine(cx,cy-rad+o,cx+rad-o,cy,col); gfx->drawLine(cx+rad-o,cy,cx,cy+rad-o,col);
    gfx->drawLine(cx,cy+rad-o,cx-rad+o,cy,col); gfx->drawLine(cx-rad+o,cy,cx,cy-rad+o,col);
  }
  gfx->fillRect(cx-1,cy-rad+8,2,rad*2-16,col);
  gfx->fillRect(cx-rad+8,cy-1,rad*2-16,2,col);
}
static void drawSplash(){
  gfx->fillScreen(LCD_BG);
  gfx->drawRect(4,4,SCR_W-8,SCR_H-8,LCD_GRID);
  gfx->drawRect(8,8,SCR_W-16,SCR_H-16,LCD_DARK);
  for(int y=14;y<SCR_H-14;y+=8) gfx->drawFastHLine(14,y,SCR_W-28,LCD_DARK);
  int cx=SCR_W/2, cy=SCR_H/2-14;
  darkcellMark(cx,cy,34,LCD_GREEN);
  gfx->setTextSize(3); gfx->setTextColor(LCD_GREEN,BLACK);
  const char* t="DARKPAGER"; int tw=strlen(t)*18;
  gfx->setCursor((SCR_W-tw)/2,cy+44); gfx->print(t);
  gfx->setTextSize(1); gfx->setTextColor(LCD_AMBER,BLACK);
  const char* s="PAGERNET NODE // TLS IRC // 6697KHZ";
  gfx->setCursor((SCR_W-strlen(s)*6)/2,cy+72); gfx->print(s);
  // loading bar
  int bx=40,bw=SCR_W-80,by=SCR_H-20;
  gfx->drawRect(bx-1,by-1,bw+2,6,LCD_GRID);
  gfx->fillRect(bx,by,bw,4,LCD_GREEN);
  if(gfx) gfx->flush();          // canvas -> push splash
  delay(1900);
}
// Bouncing "DARKPAGER" wordmark screensaver (changes color on each bounce).
static const int SV_TS=3;                 // text size
static const int SV_TW=9*6*SV_TS;         // "DARKPAGER" = 9 chars * 6px * size
static const int SV_TH=8*SV_TS;
static int svx,svy,svdx=4,svdy=3,psvx,psvy;
static uint16_t svcol=ARA_RED;
static void drawSaverStatus();
static void saverEnter(){
  gfx->fillScreen(BLACK);
  tftBacklight(70); kbBacklight(0);      // dim, but DARKPAGER still readable
  svx=(SCR_W-SV_TW)/2; svy=(SCR_H-SV_TH)/2; psvx=svx; psvy=svy; svcol=ARA_RED;
  drawSaverStatus(); if(gfx)gfx->flush();
}
static void drawSaverStatus(){
  gfx->fillRect(0,0,SCR_W,LINE_H,BLACK);
  gfx->setTextSize(2); gfx->setTextColor(unread?GRN:DIMTXT,BLACK); gfx->setCursor(2,0);
  if(unread>0) { gfx->print("UNREAD "); gfx->print(unread); }
  else gfx->print(WiFi.isConnected()?"PAGERNET":"NO SIGNAL");
  gfx->setTextColor(otaActive?GRN:DIMTXT,BLACK);
  const char* st=otaActive?"OTA":"LINK"; gfx->setCursor(SCR_W-strlen(st)*CHARW-2,0); gfx->print(st);
}
static void saverStep(){
  gfx->fillRect(psvx,psvy,SV_TW+2,SV_TH+2,BLACK);           // erase previous word
  drawSaverStatus();
  psvx=svx; psvy=svy;
  svx+=svdx; svy+=svdy;
  bool bounce=false;
  if(svx<0){svx=0;svdx=-svdx;bounce=true;}
  if(svx>SCR_W-SV_TW){svx=SCR_W-SV_TW;svdx=-svdx;bounce=true;}
  if(svy<LINE_H){svy=LINE_H;svdy=-svdy;bounce=true;}
  if(svy>SCR_H-SV_TH){svy=SCR_H-SV_TH;svdy=-svdy;bounce=true;}
  if(bounce){ static int ci=0; ci=(ci+1)%(int)(sizeof(NICKPAL)/2); svcol=NICKPAL[ci]; }
  gfx->setTextSize(SV_TS); gfx->setTextColor(svcol,BLACK);
  gfx->setCursor(svx,svy); gfx->print("DARKPAGER");
}

static void drawCalib(){
  gfx->fillScreen(LCD_BG);
  gfx->drawRect(0,0,SCR_W,SCR_H,LCD_AMBER);              // outer edge - align this to the panel
  gfx->drawRect(3,3,SCR_W-6,SCR_H-6,LCD_GREEN);
  int cx=SCR_W/2, cy=SCR_H/2;
  gfx->drawFastHLine(cx-14,cy,28,LCD_AMBER); gfx->drawFastVLine(cx,cy-14,28,LCD_AMBER);
  gfx->setTextSize(2); gfx->setTextColor(LCD_TEXT,BLACK);
  char b[28]; snprintf(b,28,"X=%d  Y=%d",dispX,dispY);
  gfx->setCursor(cx-strlen(b)*6,cy-42); gfx->print(b);
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK);
  gfx->setCursor(cx-135,cy+30); gfx->print("turn/w,s=up-down  a,d=left-right  press/Enter=SAVE");
}

// ───────────────────────── Flock Finder (BLE) ─────────────────────────
// Detects Flock Safety ALPR cameras / Penguin battery packs via BLE finger-
// printing: curated OUIs + Xuntong mfg id 0x09C8 + Penguin-style names. Same
// detection lineage as HaleHound's "Flock You" / Marauder. Hits -> SD CSV.
static const uint8_t FLOCK_OUIS[][3]={
  {0x58,0x8E,0x81},{0xCC,0xCC,0xCC},{0xEC,0x1B,0xBD},{0x90,0x35,0xEA},
  {0x04,0x0D,0x84},{0xF0,0x82,0xC0},{0x1C,0x34,0xF1},{0x38,0x5B,0x44},
  {0x94,0x34,0x69},{0xB4,0xE3,0xF9},{0x70,0xC9,0x4E},{0x3C,0x91,0x80},
  {0xD8,0xF3,0xBC},{0x80,0x30,0x49},{0x14,0x5A,0xFC},{0x74,0x4C,0xA1},
  {0x08,0x3A,0x88},{0x9C,0x2F,0x9D},{0x94,0x08,0x53},{0xE4,0xAA,0xEA},
  {0xF4,0x6A,0xDD},{0xF8,0xA2,0xD6},{0xE0,0x0A,0xF6},{0x00,0xF4,0x8D},
  {0xD0,0x39,0x57},{0xE8,0xD0,0xFC},{0xB4,0x1E,0x52},
};
static const int FLOCK_OUI_N=sizeof(FLOCK_OUIS)/sizeof(FLOCK_OUIS[0]);
static const uint16_t XUNTONG_ID=0x09C8;
struct FlockHit{ uint8_t mac[6]; char name[20]; char serial[20]; int8_t rssi; uint8_t reason; uint32_t lastMs; };
#define FLOCK_MAX 32
static FlockHit flocks[FLOCK_MAX]; static int flockN=0, flockSel=0;
static bool flockLogged[FLOCK_MAX]={false};
// --- BLE tracker (AirTag/Tile/SmartTag) shared scan state ---
struct TrackHit{ uint8_t mac[6]; char type[20]; int8_t rssi; uint16_t count; uint32_t firstMs,lastMs; };
#define TRACK_MAX 32
static TrackHit tracks[TRACK_MAX]; static int trackN=0, trackSel=0;
static bool trackLogged[TRACK_MAX]={false}; static volatile bool trackPending=false;
static const char* trackerType(NimBLEAdvertisedDevice* dev){
  if(dev->haveManufacturerData()){
    std::string m=dev->getManufacturerData();
    if(m.size()>=3 && (uint8_t)m[0]==0x4C && (uint8_t)m[1]==0x00){
      uint8_t t=(uint8_t)m[2];
      if(t==0x12) return "AirTag/FindMy";
      if(t==0x07) return "Apple FindMy";
    }
  }
  if(dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFEED))||dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFEEC))) return "Tile";
  if(dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFD5A))) return "Samsung Tag";
  return nullptr;
}
static int trackSlot(const uint8_t m[6]){ for(int i=0;i<trackN;i++) if(memcmp(tracks[i].mac,m,6)==0)return i; if(trackN>=TRACK_MAX){int o=0;for(int i=1;i<TRACK_MAX;i++)if(tracks[i].lastMs<tracks[o].lastMs)o=i;return o;} return trackN++; }
static void trackCsv(const TrackHit& h){ if(!sdReady)return; if(!SD.exists("/captures"))SD.mkdir("/captures"); bool fresh=!SD.exists("/captures/trackers.csv"); File f=SD.open("/captures/trackers.csv",FILE_APPEND); if(!f)return; if(fresh)f.println("time,mac,type,rssi,count"); char mac[18];snprintf(mac,18,"%02X:%02X:%02X:%02X:%02X:%02X",h.mac[0],h.mac[1],h.mac[2],h.mac[3],h.mac[4],h.mac[5]); f.printf("%s,%s,%s,%d,%d\n",nowHHMM().c_str(),mac,h.type,(int)h.rssi,h.count); f.close(); }
static volatile bool flockPending=false;   // BLE task -> serviced in loop()
static uint32_t flockAdvs=0; static bool flockScanning=false, bleInited=false, trackScanning=false;
static NimBLEScan* flockScan=nullptr;

static bool flockOui(const uint8_t m[6]){ for(int i=0;i<FLOCK_OUI_N;i++) if(memcmp(m,FLOCK_OUIS[i],3)==0)return true; return false; }
static bool penguinName(const String& n){
  if(!n.length())return false;
  if(n=="FS Ext Battery")return true;
  if(n.startsWith("Penguin-")&&n.length()==18){ for(int i=8;i<(int)n.length();i++){char c=n[i];if(c<'0'||c>'9')return false;} return true; }
  if(n.length()==10){ for(int i=0;i<10;i++){char c=n[i];if(c<'0'||c>'9')return false;} return true; }
  return false;
}
static bool xuntong(const std::string& mfg,const String& name){
  if(mfg.size()<2)return false;
  uint16_t cid=(uint8_t)mfg[0]|((uint16_t)(uint8_t)mfg[1]<<8);
  if(cid!=XUNTONG_ID)return false;
  return penguinName(name)||name.length()==0;
}
static void flockSerial(const std::string& mfg,char* out,size_t cap){
  out[0]=0; if(mfg.size()<4)return; bool st=false; size_t w=0;
  for(size_t k=2;k<mfg.size()&&w+1<cap;k++){ char c=(char)mfg[k];
    if(!st){ if(c=='T'&&k+1<mfg.size()&&(char)mfg[k+1]=='N'){out[w++]='T';out[w++]='N';st=true;k++;} }
    else { if(c>='0'&&c<='9')out[w++]=c; else if(c==' '||c=='#'||c=='-')continue; else break; } }
  out[w]=0;
}
static int flockSlot(const uint8_t m[6]){
  for(int i=0;i<flockN;i++) if(memcmp(flocks[i].mac,m,6)==0)return i;
  if(flockN>=FLOCK_MAX){ int o=0; for(int i=1;i<FLOCK_MAX;i++)if(flocks[i].lastMs<flocks[o].lastMs)o=i; return o; }
  return flockN++;
}
static void flockCsv(const FlockHit& h){
  if(!sdReady)return;
  if(!SD.exists("/captures"))SD.mkdir("/captures");
  bool fresh=!SD.exists("/captures/flock.csv");
  File f=SD.open("/captures/flock.csv",FILE_APPEND); if(!f)return;
  if(fresh)f.println("time,mac,name,serial,rssi,reason");
  char macS[18]; snprintf(macS,sizeof(macS),"%02X:%02X:%02X:%02X:%02X:%02X",h.mac[0],h.mac[1],h.mac[2],h.mac[3],h.mac[4],h.mac[5]);
  f.printf("%s,%s,%s,%s,%d,%u\n",nowHHMM().c_str(),macS,h.name,h.serial,(int)h.rssi,(unsigned)h.reason);
  f.close();
}
class FlockCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    flockAdvs++;
    uint8_t mac[6];
    if(sscanf(dev->getAddress().toString().c_str(),"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",&mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5])!=6)return;
    const char* tt=trackerType(dev);
    if(tt){ int ti=trackSlot(mac); TrackHit& th=tracks[ti]; bool tn=(memcmp(th.mac,mac,6)!=0);
      memcpy(th.mac,mac,6); strncpy(th.type,tt,sizeof(th.type)-1); th.type[sizeof(th.type)-1]=0;
      th.rssi=(int8_t)dev->getRSSI(); th.lastMs=millis();
      if(tn){ th.count=1; th.firstMs=millis(); trackLogged[ti]=false; trackPending=true; } else th.count++;
      uiDirty=true; }
    bool o=flockOui(mac);
    String name=dev->getName().c_str();
    std::string mfg=dev->haveManufacturerData()?dev->getManufacturerData():std::string();
    bool x=xuntong(mfg,name);
    if(!o&&!x)return;
    int i=flockSlot(mac); FlockHit& h=flocks[i];
    bool isNew=(memcmp(h.mac,mac,6)!=0);
    memcpy(h.mac,mac,6);
    strncpy(h.name,name.c_str(),sizeof(h.name)-1); h.name[sizeof(h.name)-1]=0;
    flockSerial(mfg,h.serial,sizeof(h.serial));
    h.rssi=(int8_t)dev->getRSSI(); h.reason=(o?1:0)|(x?2:0); h.lastMs=millis();
    if(isNew){ flockLogged[i]=false; flockPending=true; }
    uiDirty=true;
  }
};
static FlockCB flockCb;
static void flockStart(){
  WiFi.setSleep(true);   // BLE+WiFi coexistence REQUIRES modem sleep
  for(int i=0;i<FLOCK_MAX;i++) flockLogged[i]=false;
  if(!bleInited){
    NimBLEDevice::init("darkpager"); bleInited=true;
    flockScan=NimBLEDevice::getScan();
    flockScan->setAdvertisedDeviceCallbacks(&flockCb,false);
    flockScan->setActiveScan(true); flockScan->setInterval(97); flockScan->setWindow(37);
  }
  flockN=0; flockSel=0; flockAdvs=0;
  flockScan->start(0,nullptr,false); flockScanning=true; trackScanning=false;
  Serial.println("[flock] scan start");
}
static void flockStop(){
  if(flockScan){ flockScan->stop(); flockScan->clearResults(); }
  flockScanning=false; Serial.println("[flock] scan stop");
}
// Service deferred flock hits from the MAIN task (SD + haptic are unsafe from
// the BLE callback task — they share the SPI/I2C buses with the display).
static void flockService(){
  if(!flockPending) return;
  flockPending=false;
  for(int i=0;i<flockN;i++){
    if(!flockLogged[i]){ flockLogged[i]=true; flockCsv(flocks[i]); notify(); }
  }
}
static const char* flockReason(uint8_t r){ return r==3?"OUI+MFG":r==2?"MFG":"OUI"; }
static void drawFlock(){
  gfx->fillScreen(BLACK); headerBar("FLOCK FINDER");
  char sub[44]; snprintf(sub,44,"%d flock  %lu adv",flockN,(unsigned long)flockAdvs);
  gfx->setTextSize(1); gfx->setTextColor(flockN?ARA_RED:DIMTXT,BLACK); gfx->setCursor(6,LINE_H+3); gfx->print(sub);
  int top=LINE_H+16, footerY=SCR_H-11, rowH=26;
  int vis=max(1,(footerY-top)/rowH);
  if(flockN==0){ gfx->setTextSize(2); gfx->setTextColor(LCD_GREEN,BLACK); gfx->setCursor(8,top+10); gfx->print("scanning..."); }
  int start=listStart(flockSel,flockN,vis);
  for(int r=0;r<vis&&start+r<flockN;r++){
    int i=start+r, ry=top+r*rowH, ty=ry+(rowH-16)/2; bool sel=(i==flockSel); uint16_t bg=sel?ARA_DIM:BLACK;
    if(sel)gfx->fillRect(0,ry,SCR_W,rowH,ARA_DIM);
    const char* nm=flocks[i].name[0]?flocks[i].name:(flocks[i].serial[0]?flocks[i].serial:"Flock cam");
    gfx->setTextSize(2); gfx->setTextColor(sel?WHITE:ARA_RED,bg); gfx->setCursor(6,ty); gfx->print(nm);
    char rb[20]; snprintf(rb,20,"%ddB %s",flocks[i].rssi,flockReason(flocks[i].reason));
    gfx->setTextSize(1); gfx->setTextColor(sel?LCD_AMBER:DIMTXT,bg); gfx->setCursor(SCR_W-strlen(rb)*6-4,ty+4); gfx->print(rb);
  }
  scrollArrows(start,vis,flockN,top,footerY);
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,SCR_H-10); gfx->print("turn=scroll press=back r=rescan");
}

static bool sdEnsure(); static String noteTimestamp(); static bool gpsFix();
// ───────────────────────── Wardriver + WiGLE ─────────────────────────
static File wdFile; static bool wdActive=false;
static int wdNets=0, wdScanNets=0; static String wdPath, wdStatus="idle";
static uint32_t wdLastScan=0;
#define WD_SEEN_MAX 1000
static uint8_t wdSeen[WD_SEEN_MAX][6]; static int wdSeenN=0;
static bool wdSeenHas(const uint8_t* b){ for(int i=0;i<wdSeenN;i++) if(memcmp(wdSeen[i],b,6)==0)return true; return false; }
static void wdSeenAdd(const uint8_t* b){ if(wdSeenN<WD_SEEN_MAX) memcpy(wdSeen[wdSeenN++],b,6); }
static const char* wdAuth(wifi_auth_mode_t a){
  switch(a){ case WIFI_AUTH_OPEN:return "[ESS]"; case WIFI_AUTH_WEP:return "[WEP][ESS]";
    case WIFI_AUTH_WPA_PSK:return "[WPA-PSK-CCMP+TKIP][ESS]"; case WIFI_AUTH_WPA2_PSK:return "[WPA2-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA_WPA2_PSK:return "[WPA-PSK-CCMP+TKIP][WPA2-PSK-CCMP][ESS]"; case WIFI_AUTH_WPA3_PSK:return "[WPA3-SAE][ESS]";
    case WIFI_AUTH_WPA2_WPA3_PSK:return "[WPA2-PSK-CCMP][WPA3-SAE][ESS]"; default:return "[ESS]"; }
}
static String wdTime(){
  char b[24];
  if(gps.date.isValid()&&gps.time.isValid())
    snprintf(b,sizeof(b),"%04d-%02d-%02d %02d:%02d:%02d",gps.date.year(),gps.date.month(),gps.date.day(),gps.time.hour(),gps.time.minute(),gps.time.second());
  else { struct tm t; if(getLocalTime(&t,5)) snprintf(b,sizeof(b),"%04d-%02d-%02d %02d:%02d:%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec); else strcpy(b,"1970-01-01 00:00:00"); }
  return String(b);
}
static void wardriveStart(){
  if(!sdEnsure()){ wdStatus="no SD"; return; }
  if(!SD.exists("/captures")) SD.mkdir("/captures");
  wdPath="/captures/wigle_"+noteTimestamp()+".csv";
  wdFile=SD.open(wdPath,FILE_WRITE);
  if(!wdFile){ wdStatus="file open failed"; return; }
  wdFile.println("WigleWifi-1.4,appRelease=darkpager,model=T-LoRa-Pager,release=1,device=darkpager,display=,board=ESP32S3,brand=LilyGo");
  wdFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
  wdFile.flush(); wdNets=0; wdSeenN=0; wdActive=true; wdStatus="scanning"; irc.stop();
  Serial.printf("[wd] start %s\n",wdPath.c_str());
}
static void wardriveStop(){ wdActive=false; if(wdFile){ wdFile.flush(); wdFile.close(); } wdStatus="stopped"; Serial.println("[wd] stop"); }
static void wardriveScan(){
  if(!wdActive) return;
  int n=WiFi.scanComplete();          // NON-blocking: async scan
  if(n>=0){                            // results ready
    wdScanNets=n;
    bool haveFix=gpsFix();
#ifdef BOARD_TDECK
    // T-Deck has no GPS at all (gpsFix() is always false); log without
    // coordinates instead of never logging anything.
    bool logOk=true;
#else
    bool logOk=haveFix;
#endif
    if(logOk){
      double lat=haveFix?gps.location.lat():0.0, lng=haveFix?gps.location.lng():0.0;
      double alt=haveFix?gps.altitude.meters():0.0;
      double acc=haveFix?(gps.hdop.isValid()?gps.hdop.hdop()*5.0:10.0):9999.0;
      for(int i=0;i<n;i++){
        uint8_t* b=WiFi.BSSID(i); if(!b||wdSeenHas(b)) continue; wdSeenAdd(b);
        char mac[18]; snprintf(mac,sizeof(mac),"%02X:%02X:%02X:%02X:%02X:%02X",b[0],b[1],b[2],b[3],b[4],b[5]);
        String ssid=WiFi.SSID(i); ssid.replace(","," "); ssid.replace("\n"," "); ssid.replace("\r"," ");
        if(wdFile){
          wdFile.printf("%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%.1f,WIFI\n",mac,ssid.c_str(),wdAuth(WiFi.encryptionType(i)),
            wdTime().c_str(),WiFi.channel(i),WiFi.RSSI(i),lat,lng,alt,acc);
          wdNets++;
        }
      }
      if(wdFile) wdFile.flush();
    }
    WiFi.scanDelete(); uiDirty=true; n=WIFI_SCAN_FAILED;
  }
  if(n!=WIFI_SCAN_RUNNING && millis()-wdLastScan>1500){ wdLastScan=millis(); WiFi.scanNetworks(true,true); }
}
static const char WB64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static String b64(const String& in){
  String out; int val=0,bits=-6;
  for(size_t i=0;i<in.length();i++){ val=(val<<8)+(uint8_t)in[i]; bits+=8; while(bits>=0){ out+=WB64[(val>>bits)&0x3F]; bits-=6; } }
  if(bits>-6) out+=WB64[((val<<8)>>(bits+8))&0x3F];
  while(out.length()%4) out+='='; return out;
}
static String wigleUpload(){
  if(!cfgWigleName.length()||!cfgWigleToken.length()) return "set WiGLE name+token";
  if(!wdPath.length()||!SD.exists(wdPath)) return "no capture file";
  if(WiFi.status()!=WL_CONNECTED){
    WiFi.begin(cfgSsid.c_str(),cfgPass.c_str());
    uint32_t t0=millis(); while(WiFi.status()!=WL_CONNECTED&&millis()-t0<15000) delay(200);
    if(WiFi.status()!=WL_CONNECTED) return "no WiFi to upload";
  }
  File f=SD.open(wdPath,FILE_READ); if(!f) return "file read failed";
  size_t fsize=f.size();
  WiFiClientSecure c; c.setInsecure(); c.setTimeout(20000);
  if(!c.connect("api.wigle.net",443)){ f.close(); return "wigle connect failed"; }
  String bnd="----darkpager8842";
  String fn=wdPath.substring(wdPath.lastIndexOf('/')+1);
  String pre="--"+bnd+"\r\nContent-Disposition: form-data; name=\"donate\"\r\n\r\nfalse\r\n";
  pre+="--"+bnd+"\r\nContent-Disposition: form-data; name=\"file\"; filename=\""+fn+"\"\r\nContent-Type: text/csv\r\n\r\n";
  String post="\r\n--"+bnd+"--\r\n";
  size_t clen=pre.length()+fsize+post.length();
  String auth=b64(cfgWigleName+":"+cfgWigleToken);
  c.print("POST /api/v2/file/upload HTTP/1.1\r\nHost: api.wigle.net\r\n");
  c.print("Authorization: Basic "+auth+"\r\n");
  c.print("Accept: application/json\r\n");
  c.print("Content-Type: multipart/form-data; boundary="+bnd+"\r\n");
  c.printf("Content-Length: %u\r\nConnection: close\r\n\r\n",(unsigned)clen);
  c.print(pre);
  uint8_t buf[512]; while(f.available()){ int r=f.read(buf,sizeof(buf)); if(r>0) c.write(buf,r); }
  f.close(); c.print(post);
  String resp; uint32_t t0=millis();
  while(c.connected()&&millis()-t0<20000){ while(c.available()){ char ch=c.read(); if(resp.length()<3000)resp+=ch; } }
  c.stop();
  int code=0,sp=resp.indexOf(' '); if(sp>0)code=resp.substring(sp+1,sp+4).toInt();
  bool ok=resp.indexOf("\"success\":true")>=0;
  Serial.printf("[wd] upload http=%d ok=%d\n",code,ok);
  return ok?("uploaded! "+String(wdNets)+" nets"):("upload fail http "+String(code));
}
static void drawWardrive(){
  gfx->fillScreen(BLACK); headerBar("WARDRIVER");
  int y=LINE_H+8; char l[44]; gfx->setTextSize(2);
#ifdef BOARD_TDECK
  gfx->setTextColor(LCD_AMBER,BLACK); gfx->setCursor(6,y);
  gfx->print("no GPS - logging w/o coords"); y+=22;
  gfx->setTextColor(TXT,BLACK); gfx->setCursor(6,y);
  gfx->print("0.00000, 0.00000"); y+=22;
#else
  gfx->setTextColor(gpsFix()?LCD_GREEN:LCD_AMBER,BLACK); gfx->setCursor(6,y);
  snprintf(l,44,gpsFix()?"GPS FIX  sat %d":"GPS acquiring sat %d",(int)gps.satellites.value()); gfx->print(l); y+=22;
  gfx->setTextColor(TXT,BLACK); gfx->setCursor(6,y);
  snprintf(l,44,"%.5f, %.5f",gps.location.lat(),gps.location.lng()); gfx->print(l); y+=22;
#endif
  gfx->setTextColor(ARA_RED,BLACK); gfx->setCursor(6,y);
  snprintf(l,44,"nets logged: %d",wdNets); gfx->print(l); y+=24;
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,y);
  gfx->print(wdActive?"SCANNING":"paused"); gfx->print("  "); gfx->print(wdStatus.c_str()); y+=12;
  gfx->setCursor(6,y); gfx->print(cfgWigleName.length()?("wigle: "+cfgWigleName).c_str():"wigle: (no login)");
  gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,SCR_H-20); gfx->print("press=start/stop  u=upload WiGLE");
  gfx->setCursor(6,SCR_H-10); gfx->print("c=name  t=token  BKSP=back");
}

static void trackerStart(){
  WiFi.setSleep(true);
  for(int i=0;i<TRACK_MAX;i++) trackLogged[i]=false;
  if(!bleInited){ NimBLEDevice::init("darkpager"); bleInited=true; flockScan=NimBLEDevice::getScan();
    flockScan->setAdvertisedDeviceCallbacks(&flockCb,false); flockScan->setActiveScan(true); flockScan->setInterval(97); flockScan->setWindow(37); }
  trackN=0; trackSel=0;
  flockScan->start(0,nullptr,false); trackScanning=true; flockScanning=false;
  Serial.println("[track] scan start");
}
static void trackerStop(){ if(flockScan){ flockScan->stop(); flockScan->clearResults(); } trackScanning=false; Serial.println("[track] scan stop"); }
static void trackerService(){ if(!trackPending)return; trackPending=false; for(int i=0;i<trackN;i++) if(!trackLogged[i]){ trackLogged[i]=true; trackCsv(tracks[i]); notify(); } }
static void drawTracker(){
  gfx->fillScreen(BLACK); headerBar("TRACKER SCAN");
  char sub[44]; snprintf(sub,44,"%d trackers  %lu adv",trackN,(unsigned long)flockAdvs);
  gfx->setTextSize(1); gfx->setTextColor(trackN?ARA_RED:DIMTXT,BLACK); gfx->setCursor(6,LINE_H+3); gfx->print(sub);
  int top=LINE_H+16, footerY=SCR_H-11, rowH=26, vis=max(1,(footerY-top)/rowH);
  if(trackN==0){ gfx->setTextSize(2); gfx->setTextColor(LCD_GREEN,BLACK); gfx->setCursor(8,top+10); gfx->print("scanning..."); }
  int start=listStart(trackSel,trackN,vis);
  for(int r=0;r<vis&&start+r<trackN;r++){ int i=start+r,ry=top+r*rowH,ty=ry+(rowH-16)/2; bool sel=(i==trackSel); uint16_t bg=sel?ARA_DIM:BLACK;
    if(sel)gfx->fillRect(0,ry,SCR_W,rowH,ARA_DIM);
    gfx->setTextSize(2); gfx->setTextColor(sel?WHITE:ARA_RED,bg); gfx->setCursor(6,ty); gfx->print(tracks[i].type);
    char rb[24]; snprintf(rb,24,"%ddB x%d",tracks[i].rssi,tracks[i].count);
    gfx->setTextSize(1); gfx->setTextColor(sel?LCD_AMBER:DIMTXT,bg); gfx->setCursor(SCR_W-strlen(rb)*6-4,ty+4); gfx->print(rb); }
  scrollArrows(start,vis,trackN,top,footerY);
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,SCR_H-10); gfx->print("turn=scroll  press=back  r=rescan");
}
// ───────────────────────── Utility: QR generator ─────────────────────────
static String qrText="";
static void drawQR(){
  gfx->fillScreen(BLACK); headerBar("QR CODE");
  if(!qrText.length()){ gfx->setTextSize(2); gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(8,SCR_H/2-8); gfx->print("press e to enter text");
    gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,SCR_H-10); gfx->print("e=text  BKSP=back"); return; }
  uint8_t ver = qrText.length()>100?10 : qrText.length()>60?8 : qrText.length()>25?6 : 4;
  static uint8_t buf[512];   // fits QR version <=10
  QRCode qr; qrcode_initText(&qr, buf, ver, ECC_LOW, qrText.c_str());
  int n=qr.size, avail=(SCR_W<(SCR_H-LINE_H-16)?SCR_W:(SCR_H-LINE_H-16));
  int ms=avail/(n+2); if(ms<1)ms=1; int qw=n*ms;
  int ox=(SCR_W-qw)/2, oy=LINE_H+8+((SCR_H-LINE_H-16-qw)/2);
  gfx->fillRect(ox-ms,oy-ms,qw+2*ms,qw+2*ms,WHITE);
  for(int y=0;y<n;y++) for(int x=0;x<n;x++) if(qrcode_getModule(&qr,x,y)) gfx->fillRect(ox+x*ms,oy+y*ms,ms,ms,BLACK);
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,SCR_H-10); gfx->print("e=new  BKSP=back");
}
// ───────────────────────── Utilities: Clock / Stopwatch / Timer ─────────────────────────
static int clockMode=0;                 // 0 clock, 1 stopwatch, 2 timer
static uint32_t swStart=0, swElapsed=0; static bool swRun=false;
static int tmSet=5; static uint32_t tmEnd=0; static bool tmRun=false;
static void drawClock(){
  gfx->fillScreen(BLACK); headerBar(clockMode==0?"CLOCK":clockMode==1?"STOPWATCH":"TIMER");
  char l[32];
  if(clockMode==0){
    struct tm t;
    if(getLocalTime(&t,20)){
      gfx->setTextSize(4); gfx->setTextColor(LCD_GREEN,BLACK);
      { int h12=t.tm_hour%12; if(h12==0)h12=12; snprintf(l,32,"%d:%02d:%02d %s",h12,t.tm_min,t.tm_sec,t.tm_hour<12?"AM":"PM"); }
      gfx->setCursor((SCR_W-strlen(l)*24)/2,SCR_H/2-32); gfx->print(l);
      gfx->setTextSize(2); gfx->setTextColor(TXT,BLACK);
      snprintf(l,32,"%04d-%02d-%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday); gfx->setCursor((SCR_W-strlen(l)*12)/2,SCR_H/2+12); gfx->print(l);
    } else { gfx->setTextSize(2); gfx->setTextColor(ARA_RED,BLACK); gfx->setCursor(8,SCR_H/2-8); gfx->print("no time (join WiFi)"); }
  } else if(clockMode==1){
    uint32_t e=swRun?(swElapsed+millis()-swStart):swElapsed;
    gfx->setTextSize(4); gfx->setTextColor(swRun?LCD_GREEN:LCD_AMBER,BLACK);
    snprintf(l,32,"%02lu:%02lu.%1lu",(unsigned long)(e/60000),(unsigned long)((e/1000)%60),(unsigned long)((e/100)%10));
    gfx->setCursor((SCR_W-strlen(l)*24)/2,SCR_H/2-20); gfx->print(l);
  } else {
    long rem=tmRun?(long)(tmEnd-millis()):(long)tmSet*60000; if(rem<0)rem=0;
    gfx->setTextSize(4); gfx->setTextColor(tmRun?LCD_GREEN:LCD_AMBER,BLACK);
    snprintf(l,32,"%02ld:%02ld",rem/60000,(rem/1000)%60); gfx->setCursor((SCR_W-strlen(l)*24)/2,SCR_H/2-20); gfx->print(l);
  }
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,SCR_H-20);
  gfx->print(clockMode==1?"press=start/stop  r=reset":clockMode==2?"w/s=+/-min  press=start/stop":"turn=switch mode");
  gfx->setCursor(6,SCR_H-10); gfx->print("turn=Clock/Stopwatch/Timer   BKSP=back");
}
// ───────────────────────── Utilities: Hash / Encode ─────────────────────────
static String hashIn=""; static String hB64,hHex,hMd5,hSha;
static String toHex(const uint8_t* d,int n){ String o; char b[3]; for(int i=0;i<n;i++){ snprintf(b,3,"%02x",d[i]); o+=b; } return o; }
static void hashCompute(const String& in){
  hashIn=in;
  hB64=b64(in);
  hHex=toHex((const uint8_t*)in.c_str(),in.length());
  uint8_t m[16]; mbedtls_md5_ret((const unsigned char*)in.c_str(),in.length(),m); hMd5=toHex(m,16);
  uint8_t sh[32]; mbedtls_sha256_ret((const unsigned char*)in.c_str(),in.length(),sh,0); hSha=toHex(sh,32);
}
static void drawWrap(int&y,const char* label,const String& v,uint16_t col){
  gfx->setTextSize(1); gfx->setTextColor(LCD_AMBER,BLACK); gfx->setCursor(6,y); gfx->print(label); y+=10;
  gfx->setTextColor(col,BLACK); int maxc=SCR_W/6-1;
  for(int i=0;i<(int)v.length() && y<SCR_H-12;i+=maxc){ gfx->setCursor(6,y); gfx->print(v.substring(i,i+maxc)); y+=10; }
  y+=3;
}
static void drawHash(){
  gfx->fillScreen(BLACK); headerBar("HASH / ENCODE");
  int y=LINE_H+4;
  drawWrap(y,"input:",hashIn.length()?hashIn:"(press e to enter text)",TXT);
  if(hashIn.length()){
    drawWrap(y,"base64:",hB64,LCD_GREEN);
    drawWrap(y,"hex:",hHex,LCD_GREEN);
    drawWrap(y,"md5:",hMd5,LCD_GREEN);
    drawWrap(y,"sha256:",hSha,LCD_GREEN);
  }
  gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,SCR_H-10); gfx->print("e=new text   BKSP=back");
}
static void render();
// ───────────────────────── Network Recon (ARP-ish sweep + port scan) ─────────────────────────
static uint8_t netBase[3]={0,0,0}; static int netIdx=0, netN=0; static bool netScanning=false;
static uint8_t netHosts[64]; static int netSel=0, netMode=0; static uint8_t netTarget=0; static String netPorts="";
static const uint16_t COMMON_PORTS[]={21,22,23,25,53,80,110,139,143,443,445,554,1883,3306,3389,5900,8080,8443};
static bool tcpUp(IPAddress ip,uint16_t port,int to){ WiFiClient c; bool ok=c.connect(ip,port,to); c.stop(); return ok; }
static void netEnter(){
  netMode=0; netN=0; netSel=0; netScanning=false; netPorts="";
  if(WiFi.status()!=WL_CONNECTED) return;
  IPAddress ip=WiFi.localIP(); netBase[0]=ip[0]; netBase[1]=ip[1]; netBase[2]=ip[2];
  netIdx=1; netScanning=true;
}
static void netStep(){
  if(!netScanning) return;
  if(netIdx>254){ netScanning=false; uiDirty=true; return; }
  IPAddress ip(netBase[0],netBase[1],netBase[2],netIdx);
  if((tcpUp(ip,80,60)||tcpUp(ip,443,60)||tcpUp(ip,22,60)) && netN<64){ netHosts[netN++]=netIdx; }
  netIdx++; uiDirty=true;
}
static void netPortScan(uint8_t octet){
  netTarget=octet; netPorts=""; netMode=1; uiDirty=true; render();
  IPAddress ip(netBase[0],netBase[1],netBase[2],octet);
  for(uint16_t pp:COMMON_PORTS){ if(tcpUp(ip,pp,220)) netPorts+=String(pp)+" "; }
  if(!netPorts.length()) netPorts="(none open)";
  uiDirty=true;
}
static void drawNetRecon(){
  gfx->fillScreen(BLACK); headerBar("NET RECON");
  if(WiFi.status()!=WL_CONNECTED){ gfx->setTextSize(2); gfx->setTextColor(ARA_RED,BLACK); gfx->setCursor(8,LINE_H+22); gfx->print("connect WiFi first"); return; }
  char l[52]; IPAddress ip=WiFi.localIP();
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,LINE_H+3);
  snprintf(l,52,"me %s  gw %s",ip.toString().c_str(),WiFi.gatewayIP().toString().c_str()); gfx->print(l);
  if(netMode==1){
    gfx->setTextSize(2); gfx->setTextColor(WHITE,BLACK); gfx->setCursor(6,LINE_H+20);
    snprintf(l,52,"%d.%d.%d.%d",netBase[0],netBase[1],netBase[2],netTarget); gfx->print(l);
    gfx->setTextColor(LCD_GREEN,BLACK); gfx->setTextSize(1); gfx->setCursor(6,LINE_H+46); gfx->print("open ports:");
    gfx->setTextColor(TXT,BLACK); gfx->setCursor(6,LINE_H+62); gfx->print(netPorts.length()>44?netPorts.substring(0,44):netPorts);
    if(netPorts.length()>44){ gfx->setCursor(6,LINE_H+74); gfx->print(netPorts.substring(44)); }
    gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,SCR_H-10); gfx->print("press/BKSP=back to hosts");
    return;
  }
  char pr[28]; if(netScanning) snprintf(pr,28,"%d/254 f%d",netIdx,netN); else snprintf(pr,28,"done %d",netN);
  gfx->setTextColor(netScanning?LCD_AMBER:LCD_GREEN,BLACK); gfx->setCursor(SCR_W-strlen(pr)*6-4,LINE_H+3); gfx->print(pr);
  int top=LINE_H+16, footerY=SCR_H-11, rowH=22, vis=max(1,(footerY-top)/rowH);
  int start=listStart(netSel,netN,vis);
  for(int r=0;r<vis&&start+r<netN;r++){ int i=start+r,ry=top+r*rowH,ty=ry+(rowH-16)/2; bool sel=(i==netSel); uint16_t bg=sel?ARA_DIM:BLACK;
    if(sel)gfx->fillRect(0,ry,SCR_W,rowH,ARA_DIM); gfx->setTextSize(2); gfx->setTextColor(sel?WHITE:LCD_GREEN,bg); gfx->setCursor(6,ty);
    snprintf(l,52,"%d.%d.%d.%d",netBase[0],netBase[1],netBase[2],netHosts[i]); gfx->print(l); }
  scrollArrows(start,vis,netN,top,footerY);
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(6,SCR_H-10); gfx->print("turn=host  press=portscan  BKSP=back");
}
// ───────────────────────── Voice memo recorder ─────────────────────────
static File vfFile; static bool vRecording=false; static uint32_t vStartMs=0, vBytesRec=0; static String vLastPath="";
static void wavU32(File&f,uint32_t v){ uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; f.write(b,4); }
static void wavU16(File&f,uint16_t v){ uint8_t b[2]={(uint8_t)v,(uint8_t)(v>>8)}; f.write(b,2); }
static void wavHeader(File&f,uint32_t dataBytes){
  f.seek(0);
  f.write((const uint8_t*)"RIFF",4); wavU32(f,36+dataBytes); f.write((const uint8_t*)"WAVE",4);
  f.write((const uint8_t*)"fmt ",4); wavU32(f,16); wavU16(f,1); wavU16(f,1); wavU32(f,16000); wavU32(f,32000); wavU16(f,2); wavU16(f,16);
  f.write((const uint8_t*)"data",4); wavU32(f,dataBytes);
}
static void voiceRecStart(){
  if(!audioOk) return;
  if(!sdEnsure()) return;
  if(!SD.exists("/memos")) SD.mkdir("/memos");
  vLastPath="/memos/memo_"+noteTimestamp()+".wav";
  vfFile=SD.open(vLastPath,FILE_WRITE); if(!vfFile) return;
  wavHeader(vfFile,0); vBytesRec=0; vStartMs=millis(); vRecording=true;
  Serial.printf("[voice] rec %s\n",vLastPath.c_str());
}
static void voiceRecStop(){
  if(!vRecording) return; vRecording=false;
  wavHeader(vfFile,vBytesRec); vfFile.flush(); vfFile.close();
  Serial.printf("[voice] saved %u bytes\n",(unsigned)vBytesRec);
}
static void voiceRecStep(){
  if(!vRecording) return;
  static int16_t sbuf[512]; size_t br=0;
  i2s_read(I2S_NUM_0,sbuf,sizeof(sbuf),&br,20/portTICK_PERIOD_MS);
  if(br>0){ int fr=br/(2*sizeof(int16_t)); static int16_t mono[256]; for(int i=0;i<fr;i++)mono[i]=sbuf[i*2]; vfFile.write((uint8_t*)mono,fr*sizeof(int16_t)); vBytesRec+=fr*sizeof(int16_t); }
  if(millis()-vStartMs>120000) voiceRecStop();
  uiDirty=true;
}
static void voicePlayLast(){
  if(!audioOk) return;
  if(!vLastPath.length()||!SD.exists(vLastPath)) return;
  File f=SD.open(vLastPath,FILE_READ); if(!f) return; f.seek(44);
  static int16_t pb[256], st[512];
  while(f.available()){ int n=f.read((uint8_t*)pb,sizeof(pb)); int fr=n/2; for(int i=0;i<fr;i++){st[i*2]=pb[i];st[i*2+1]=pb[i];} size_t bw; i2s_write(I2S_NUM_0,st,fr*2*sizeof(int16_t),&bw,100/portTICK_PERIOD_MS); }
  f.close(); i2s_zero_dma_buffer(I2S_NUM_0);
}
static void drawVoice(){
  gfx->fillScreen(BLACK); headerBar("VOICE MEMO");
  gfx->setTextSize(3);
  if(!audioOk){ gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(20,LINE_H+30); gfx->print("N/A");
    gfx->setTextSize(1); gfx->setCursor(20,LINE_H+56); gfx->print("no mic/codec on this board"); }
  else if(vRecording){ gfx->setTextColor(ARA_RED,BLACK); gfx->setCursor(20,LINE_H+26); gfx->print("* REC");
    char l[16]; snprintf(l,16,"%lus",(unsigned long)((millis()-vStartMs)/1000)); gfx->setTextColor(WHITE,BLACK); gfx->setCursor(20,LINE_H+66); gfx->print(l); }
  else { gfx->setTextColor(LCD_GREEN,BLACK); gfx->setCursor(20,LINE_H+40); gfx->print("READY"); }
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK);
  gfx->setCursor(6,SCR_H-24); if(vLastPath.length()){ gfx->print("last: "); gfx->print(vLastPath.substring(vLastPath.lastIndexOf('/')+1)); }
  gfx->setCursor(6,SCR_H-12); gfx->print("press=rec/stop  p=play last  BKSP=back");
}
static void render();
static void render(){
  switch(state){
    case ST_HOME:drawHome();break; case ST_CHAT:drawChat();break; case ST_EMAIL:drawEmail();break; case ST_INBOX:drawInbox();break;
    case ST_TOOLS:drawTools();break; case ST_NOTES:drawNotes();break; case ST_NOTE_VIEW:drawNoteView();break; case ST_MENU:drawMenu();break;
    case ST_WIFI:drawWifi();break; case ST_TEXTIN:drawTextIn();break;
    case ST_CALIB:drawCalib();break;
    case ST_FLOCK:drawFlock();break;
    case ST_TRACKER:drawTracker();break;
    case ST_NETRECON:drawNetRecon();break;
    case ST_CLOCK:drawClock();break;
    case ST_HASH:drawHash();break;
    case ST_QR:drawQR();break;
    case ST_VOICE:drawVoice();break;
    case ST_WARDRIVE:drawWardrive();break;
    case ST_SAVER: break; }   // saver drawn incrementally via saverStep()
  if(gfx) gfx->flush();
  uiDirty=false;
}

// ───────────────────────── hardware ─────────────────────────
static void gpsInit();
#define STEP(m) do{Serial.println(m);Serial.flush();}while(0)
static void hwInit(){
  STEP("[hw] init");
#ifndef BOARD_TDECK
  pinMode(BK_BTN,INPUT_PULLUP); pinMode(ENCODER_KEY,INPUT_PULLUP);
  Wire.begin(I2C_SDA,I2C_SCL);
  { bool pmu=PPM.init(Wire,I2C_SDA,I2C_SCL,BQ25896_I2C_ADDRESS);
    if(pmu){
      PPM.setSysPowerDownVoltage(3300); PPM.setInputCurrentLimit(3250); PPM.disableCurrentLimitPin();
      PPM.setChargeTargetVoltage(4208); PPM.setPrechargeCurr(64); PPM.setChargerConstantCurr(832);
      PPM.enableMeasure(); PPM.enableCharge(); PPM.enableOTG(); PPM.disableOTG();
      // ---- STAY ON NO MATTER WHAT ----
      PPM.enableBatterPowerPath();  // clear any stuck ship/off state; keep battery feeding the system
      PPM.setFullSystemReset(0);    // disable the button-hold hardware power-off
      PPM.disableWatchdog();        // watchdog can never reset the charger/power config
    }
    Serial.printf("[pmu] init=%d batt=%dmV vbus=%dmV vbusIn=%d chgEn=%d\n",
      pmu,PPM.getBattVoltage(),PPM.getVbusVoltage(),PPM.isVbusIn(),PPM.isEnableCharge());
    if(pmu) Serial.printf("[pmu2] bus=%s chg=%s pg=%d charging=%d done=%d fault=0x%02X hiz=%d batLoad=%d ntc=%s\n",
      PPM.getBusStatusString(),PPM.getChargeStatusString(),PPM.isPowerGood(),PPM.isCharging(),PPM.isChargeDone(),
      PPM.getFaultStatus(),PPM.isHizMode(),PPM.isEnableBatLoad(),PPM.getNTCStatusString());
  }
  if(io.begin(Wire,XL9555_ADDR)){
    const uint8_t en[]={EXPANDS_KB_RST,EXPANDS_KB_EN,EXPANDS_SD_EN,EXPANDS_GPS_EN,EXPANDS_GPS_RST,EXPANDS_AMP_EN};
    for(uint8_t p:en){io.pinMode(p,OUTPUT);io.digitalWrite(p,HIGH);delay(2);}
  }
  if(keyboard.begin(KB_I2C_ADDRESS,&Wire)){ keyboard.matrix(KB_ROWS,KB_COLS); keyboard.flush(); }
  audioInit();
  hapticOk=haptic.begin(Wire,DRV2605_ADDR);
  if(hapticOk){ haptic.setMode(HapticMode::INTERNAL_TRIGGER); haptic.selectLibrary(1); }
  Serial.printf("[hw] haptic %s\n",hapticOk?"ok":"FAIL");
  enc=new RotaryEncoder(ENCODER_INA,ENCODER_INB,RotaryEncoder::LatchMode::FOUR3);
  attachInterrupt(digitalPinToInterrupt(ENCODER_INA),encTick,CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_INB),encTick,CHANGE);
  pinMode(KEYBOARD_BL,OUTPUT); kbBacklight(120);
  bus=new Arduino_HWSPI(TFT_DC,TFT_CS,TFT_SCLK,TFT_MOSI,TFT_MISO,&SPI);
  // Wipe the ENTIRE ST7796 controller RAM (full 320x480, no offsets) to erase
  // any leftover pixels from a previous firmware (e.g. old Meshtastic status bar).
  { Arduino_GFX* wipe=new Arduino_ST7796(bus,TFT_RST,0,TFT_IPS,320,480,0,0,0,0);
    wipe->begin(); wipe->fillScreen(BLACK); delete wipe; }
#else
  pinMode(PIN_POWER_ON,OUTPUT); digitalWrite(PIN_POWER_ON,HIGH);
  // SD/radio share the display's SPI bus — deselect them before the display talks.
  pinMode(SD_CS,OUTPUT); digitalWrite(SD_CS,HIGH);
  pinMode(RADIO_CS,OUTPUT); digitalWrite(RADIO_CS,HIGH);
  Wire.begin(I2C_SDA,I2C_SCL);
  pinMode(KB_INT_PIN,INPUT);
  pinMode(TB_UP,INPUT_PULLUP); pinMode(TB_DOWN,INPUT_PULLUP);
  pinMode(TB_LEFT,INPUT_PULLUP); pinMode(TB_RIGHT,INPUT_PULLUP); pinMode(TB_CLICK,INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TB_UP),tbUpISR,FALLING);
  attachInterrupt(digitalPinToInterrupt(TB_DOWN),tbDownISR,FALLING);
  attachInterrupt(digitalPinToInterrupt(TB_LEFT),tbLeftISR,FALLING);
  attachInterrupt(digitalPinToInterrupt(TB_RIGHT),tbRightISR,FALLING);
  audioInit();
  Serial.println("[hw] haptic n/a (T-Deck has no haptic motor)");
  bus=new Arduino_HWSPI(TFT_DC,TFT_CS,TFT_SCLK,TFT_MOSI,TFT_MISO,&SPI);
#endif
  panel=new DarkGFX(bus,TFT_RST,TFT_ROTATION,TFT_IPS,TFT_WIDTH,TFT_HEIGHT,TFT_COL_OFS1,TFT_ROW_OFS1,TFT_COL_OFS2,TFT_ROW_OFS2);
  panel->begin(); panel->setOffsets(dispX,dispY);
  // Double-buffer via a PSRAM framebuffer (eliminates scroll flicker); fall back to direct.
  Serial.printf("[gfx] psram=%d freePsram=%u freeHeap=%u\n",(int)psramFound(),(unsigned)ESP.getFreePsram(),(unsigned)ESP.getFreeHeap());
  if(psramFound() && ESP.getFreePsram()>320000){
    Arduino_Canvas* cv=new Arduino_Canvas(panel->width(),panel->height(),panel);
    if(cv->begin()){ gfx=cv; Serial.println("[gfx] canvas (PSRAM double-buffered)"); }
    else { gfx=panel; Serial.println("[gfx] direct (canvas failed)"); }
  } else { gfx=panel; Serial.println("[gfx] direct (no/low PSRAM - protecting internal RAM)"); }
  gfx->fillScreen(BLACK); SCR_W=gfx->width(); SCR_H=gfx->height();
  pinMode(TFT_BL,OUTPUT); tftBacklight(200);
  Serial.printf("[hw] display %dx%d\n",SCR_W,SCR_H);
  gpsInit();
}


// ───────────────────────── GPS ─────────────────────────
// T-Deck has no GPS module; gpsInit()/gpsPoll() no-op there so `gps` (a
// TinyGPSPlus object shared by both boards) never receives data and
// gpsFix() naturally stays false — no changes needed at any gps.* call site.
static void gpsInit(){
#ifndef BOARD_TDECK
  GPSser.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.printf("[gps] uart rx=%d tx=%d\n",GPS_RX_PIN,GPS_TX_PIN);
#else
  Serial.println("[gps] n/a (T-Deck has no GPS module)");
#endif
}
static void gpsPoll(){
#ifndef BOARD_TDECK
  while(GPSser.available()) gps.encode(GPSser.read());
#endif
}
static bool gpsFix(){ return gps.location.isValid() && gps.location.age()<5000; }

// ───────────────────────── input ─────────────────────────
#ifndef BOARD_TDECK
static char decodeKey(uint8_t keynum,bool pressed){
  int k=(keynum&0x7F)-1; if(k<0||k/10>=KB_ROWS||k%10>=KB_COLS) return 0;
  const KV& kv=KEYMAP[k/10][k%10]; char base=kv.a;
  if(base==KC_FN){ if(pressed)fnDown=!fnDown; return 0; }
  if(base==KC_SHIFT){ shiftDown=pressed; if(fnDown&&pressed)capsLock=!capsLock; return 0; }
  if(!pressed) return 0;
  if(fnDown) return kv.c; if(shiftDown^capsLock) return kv.b; return kv.a;
}
static int encDelta(){ static int last=0; int p=enc?enc->getPosition():0; int d=p-last; last=p; return d; }
static bool encBtnEdge(){ static bool was=true; bool now=digitalRead(ENCODER_KEY); bool e=(was&&!now); was=now; return e; }
static bool bkBtnEdge(){ static bool was=true; bool now=digitalRead(BK_BTN); bool e=(was&&!now); was=now; return e; }
#else
// Trackball nav: up/left = -1, down/right = +1, net since the last call.
static int encDelta(){
  static uint32_t lu=0,ld=0,ll=0,lr=0;
  uint32_t u=tbUp,d=tbDown,l=tbLeft,r=tbRight;
  int delta=(int)(d-ld)+(int)(r-lr)-(int)(u-lu)-(int)(l-ll);
  lu=u; ld=d; ll=l; lr=r;
  return delta;
}
// TB_CLICK doubles as both nav buttons: a short press/release selects, a
// press held >=600ms fires "back" once (while still held).
static uint32_t tbDownMs=0; static bool tbHeld=false, tbLongFired=false;
static bool encBtnEdge(){
  bool now=!digitalRead(TB_CLICK); bool selectEdge=false;
  if(now && !tbHeld){ tbDownMs=millis(); tbLongFired=false; }
  if(!now && tbHeld && !tbLongFired) selectEdge=true;
  tbHeld=now; return selectEdge;
}
static bool bkBtnEdge(){
  bool now=!digitalRead(TB_CLICK); bool backEdge=false;
  if(now && !tbLongFired && millis()-tbDownMs>=600){ backEdge=true; tbLongFired=true; }
  return backEdge;
}
#endif

static void beginTextIn(const char* title,bool pw,int purpose,const String& initial=""){
  state=ST_TEXTIN; tiTitle=title; tiPassword=pw; tiPurpose=purpose; tiBuf=initial; uiDirty=true;
}
static void startWifiScan();
static void startOta();
static void ircConnect();
static void fetchInbox();
static void loadNotes();
static void openNote();
static void saveNote(const String& body);
static void openHomeTab(){
  switch(homeSel){
    case 0: state=ST_CHAT; unread=0; uiDirty=true; break;
    case 1: state=ST_EMAIL; emailSel=0; uiDirty=true; break;
    case 2: startWifiScan(); break;
    case 3: startOta(); break;
    case 4: state=ST_MENU; menuSel=0; uiDirty=true; break;
    case 5: state=ST_TOOLS; toolSel=0; uiDirty=true; break;
  }
}
static void startWifiScan(){
  state=ST_WIFI; scanN=0; scanSel=0; uiDirty=true; render();
  irc.stop(); WiFi.disconnect(); delay(50);
  int n=WiFi.scanNetworks(); scanN=n>MAX_SCAN?MAX_SCAN:(n<0?0:n);
  for(int i=0;i<scanN;i++){ scanSsid[i]=WiFi.SSID(i); scanRssi[i]=WiFi.RSSI(i); scanEnc[i]=(WiFi.encryptionType(i)!=WIFI_AUTH_OPEN); }
  uiDirty=true;
}
static void startOta(){
  if(!WiFi.isConnected()){
    pushMsg("*","OTA needs WiFi first",false); state=ST_CHAT; uiDirty=true; return;
  }
  if(!otaActive){
    ArduinoOTA.setHostname(OTA_HOST);
    ArduinoOTA.setPassword(OTA_PASS);
    ArduinoOTA.onStart([](){ Serial.println("[ota] start"); });
    ArduinoOTA.onEnd([](){ Serial.println("[ota] done"); });
    ArduinoOTA.onError([](ota_error_t e){ Serial.printf("[ota] error %u\n",e); });
    ArduinoOTA.begin();
    otaActive=true;
    Serial.printf("[ota] ready %s host=%s\n",WiFi.localIP().toString().c_str(),OTA_HOST);
    pushMsg("*","OTA ready: "+WiFi.localIP().toString()+" pass "+String(OTA_PASS),false);
  } else {
    pushMsg("*","OTA already ready: "+WiFi.localIP().toString(),false);
  }
  state=ST_CHAT; scrollOff=0; uiDirty=true;
}

static bool sdEnsure(){
  if(sdReady) return true;
  if(!sdTried){
    sdTried=true;
    Serial.println("[sd] mounting SD (shared SPI bus)");
#ifndef BOARD_TDECK
    // SD is on the SPI bus shared with the display (SCLK35/MISO33/MOSI34), CS=21.
    // Power rail is enabled via the XL9555 expander (EXPANDS_SD_EN) in hwInit.
    // The SD shares the SPI bus with LoRa (CS 36), NFC/ST25R3916 (CS 39) and the
    // display (CS 38). Deselect the other devices (CS HIGH) or they corrupt SD I/O.
    pinMode(36,OUTPUT); digitalWrite(36,HIGH);       // LoRa CS
    pinMode(39,OUTPUT); digitalWrite(39,HIGH);       // NFC CS
#else
    // T-Deck's SD shares its SPI bus only with the (optional) LoRa radio.
    pinMode(RADIO_CS,OUTPUT); digitalWrite(RADIO_CS,HIGH);
#endif
    pinMode(TFT_CS,OUTPUT); digitalWrite(TFT_CS,HIGH);
    pinMode(SDCARD_CS,OUTPUT); digitalWrite(SDCARD_CS,HIGH);
    SPI.begin(TFT_SCLK,TFT_MISO,TFT_MOSI);           // ensure MISO is configured for reads
    delay(10);
    sdReady=SD.begin(SDCARD_CS,SPI,20000000);
    if(!sdReady) sdReady=SD.begin(SDCARD_CS,SPI,4000000);   // retry slower
    if(sdReady && SD.cardType()==CARD_NONE) sdReady=false;
    if(sdReady){
      Serial.printf("[sd] mounted %lluMB\n",SD.cardSize()/1024/1024);
      if(!SD.exists("/notes")) SD.mkdir("/notes");
      notesStatus="SD ready /notes";
    } else {
      Serial.println("[sd] mount failed");
      notesStatus="SD mount failed - check card";
    }
  }
  return sdReady;
}

static String noteTimestamp(){
  struct tm t;
  if(getLocalTime(&t,5)){
    char b[32]; snprintf(b,sizeof(b),"%04d%02d%02d_%02d%02d%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
    return String(b);
  }
  return String(millis());
}

static String baseName(const String& path){
  int slash=path.lastIndexOf('/');
  return slash>=0?path.substring(slash+1):path;
}

static void saveNote(const String& body){
  if(!body.length()){ notesStatus="empty note discarded"; uiDirty=true; return; }
  if(!sdEnsure()){ uiDirty=true; return; }
  String path="/notes/note_"+noteTimestamp()+".txt";
  File f=SD.open(path,FILE_WRITE);
  if(!f){ notesStatus="note open failed"; uiDirty=true; return; }
  f.println(body); f.close();
  notesStatus="saved "+baseName(path);
  Serial.printf("[notes] saved %s\n",path.c_str());
  uiDirty=true;
}

static void loadNotes(){
  noteCount=0; noteSel=0;
  if(!sdEnsure()){ state=ST_NOTES; uiDirty=true; return; }
  File root=SD.open("/notes");
  if(!root || !root.isDirectory()){ notesStatus="/notes missing"; state=ST_NOTES; uiDirty=true; return; }
  File f=root.openNextFile();
  while(f && noteCount<MAX_NOTES){
    if(!f.isDirectory()){
      String p=f.name();
      if(!p.startsWith("/")) p="/notes/"+p;
      if(p.endsWith(".txt")){
        notes[noteCount].path=p;
        notes[noteCount].name=baseName(p);
        noteCount++;
      }
    }
    f.close();
    f=root.openNextFile();
  }
  root.close();
  notesStatus=noteCount?String(noteCount)+" notes on SD":"no notes on SD";
  state=ST_NOTES; uiDirty=true;
}

static void openNote(){
  if(noteCount<=0 || noteSel<0 || noteSel>=noteCount) return;
  if(!sdEnsure()){ uiDirty=true; return; }
  File f=SD.open(notes[noteSel].path,FILE_READ);
  if(!f){ notesStatus="note read failed"; uiDirty=true; return; }
  noteViewTitle=notes[noteSel].name;
  noteViewBody="";
  while(f.available() && noteViewBody.length()<1600){
    char c=(char)f.read();
    if(c=='\r'||c=='\n') c=' ';
    noteViewBody+=c;
  }
  f.close();
  if(!noteViewBody.length()) noteViewBody="<empty note>";
  noteViewScroll=0; state=ST_NOTE_VIEW; uiDirty=true;
}

static String imapQuote(const String& s){
  String q="\"";
  for(size_t i=0;i<s.length();i++){ char c=s[i]; if(c=='\\'||c=='\"')q+='\\'; q+=c; }
  q+='\"'; return q;
}
static bool imapReadLine(WiFiClientSecure& c,String& out,uint32_t timeout=12000){
  out=""; uint32_t start=millis();
  while(millis()-start<timeout){
    while(c.available()){
      char ch=c.read();
      if(ch=='\n') return true;
      if(ch!='\r' && out.length()<1200) out+=ch;
    }
    if(!c.connected() && !c.available()) return out.length()>0;
    delay(5);
  }
  return out.length()>0;
}
static bool imapWaitOk(WiFiClientSecure& c,const String& tag,String& err,uint32_t timeout=15000){
  String line; uint32_t start=millis(); err="";
  while(millis()-start<timeout && imapReadLine(c,line,timeout)){
    Serial.printf("[imap<] %s\n",line.c_str());
    if(line.startsWith(tag+" ")){ err=line; return line.startsWith(tag+" OK"); }
  }
  err="IMAP timeout"; return false;
}
static int parseExistsLine(const String& line){
  if(!line.startsWith("* ") || !line.endsWith(" EXISTS")) return 0;
  int p=2, q=line.indexOf(' ',p);
  if(q<0) return 0;
  return line.substring(p,q).toInt();
}
static String cleanHeaderVal(String v){
  v.trim();
  if(v.startsWith("\"") && v.endsWith("\"") && v.length()>1) v=v.substring(1,v.length()-1);
  return v;
}
static bool fetchOneHeader(WiFiClientSecure& c,int id,EmailItem& item,int& tagNo){
  item.from="(unknown)"; item.subj="(no subject)"; item.date="";
  char tb[8]; snprintf(tb,sizeof(tb),"A%03d",tagNo++); String tag(tb);
  c.printf("%s FETCH %d BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)]\r\n",tag.c_str(),id);
  String line;
  while(imapReadLine(c,line,15000)){
    Serial.printf("[imap<] %s\n",line.c_str());
    if(line.startsWith("From:")) item.from=cleanHeaderVal(line.substring(5));
    else if(line.startsWith("Subject:")) item.subj=cleanHeaderVal(line.substring(8));
    else if(line.startsWith("Date:")) item.date=cleanHeaderVal(line.substring(5));
    else if(line.startsWith(tag+" ")) return line.startsWith(tag+" OK");
  }
  return false;
}
static void fetchInbox(){
  state=ST_INBOX; emailCount=0; inboxSel=0; inboxStatus="Preparing inbox..."; uiDirty=true; render();
  if(!cfgEmail.length()){ inboxStatus="Set email address first"; uiDirty=true; return; }
  if(!cfgEmailPass.length()){ inboxStatus="Set email app password first"; uiDirty=true; return; }
  if(!WiFi.isConnected()){ inboxStatus="WiFi offline"; uiDirty=true; return; }

  WiFiClientSecure imap; imap.setInsecure(); imap.setTimeout(12000);
  inboxStatus="Connecting to IMAP..."; uiDirty=true; render();
  if(!imap.connect(IMAP_HOST,IMAP_PORT)){ inboxStatus="IMAP connect failed"; uiDirty=true; return; }

  String line, err; int tagNo=1;
  imapReadLine(imap,line,12000); Serial.printf("[imap<] %s\n",line.c_str());

  inboxStatus="Logging in..."; uiDirty=true; render();
  char tb[8]; snprintf(tb,sizeof(tb),"A%03d",tagNo++); String tag(tb);
  imap.print(tag+" LOGIN "+imapQuote(cfgEmail)+" "+imapQuote(cfgEmailPass)+"\r\n");
  if(!imapWaitOk(imap,tag,err)){ inboxStatus="IMAP login failed"; imap.stop(); uiDirty=true; return; }

  int exists=0;
  int ids[MAX_EMAILS], idCount=0;
  snprintf(tb,sizeof(tb),"A%03d",tagNo++); tag=tb;
  imap.print(tag+" SELECT INBOX\r\n");
  while(imapReadLine(imap,line,15000)){
    Serial.printf("[imap<] %s\n",line.c_str());
    int ex=parseExistsLine(line); if(ex>0) exists=ex;
    if(line.startsWith(tag+" ")){
      if(!line.startsWith(tag+" OK")){ inboxStatus="Could not open INBOX"; imap.stop(); uiDirty=true; return; }
      break;
    }
  }
  if(exists<=0){ inboxStatus="Inbox empty"; imap.print("A999 LOGOUT\r\n"); imap.stop(); uiDirty=true; return; }
  idCount=exists<MAX_EMAILS?exists:MAX_EMAILS;
  for(int i=0;i<idCount;i++) ids[i]=exists-idCount+1+i;

  inboxStatus="Fetching headers..."; uiDirty=true; render();
  for(int i=idCount-1;i>=0 && emailCount<MAX_EMAILS;i--){
    if(fetchOneHeader(imap,ids[i],emails[emailCount],tagNo)) emailCount++;
  }
  imap.print("A999 LOGOUT\r\n"); imap.stop();
  inboxStatus=emailCount?String(emailCount)+" newest messages":"No headers loaded";
  uiDirty=true;
}
static void handleMenuInput(){
  switch(menuSel){
    case 0: startWifiScan(); break;
    case 1: beginTextIn("Set Nickname",false,2,cfgNick); break;
    case 2: cfgSaveNotify(!notifyEnabled); if(notifyEnabled)buzz(120,0x70); uiDirty=true; break;
    case 3: { static const int VS[]={0,20,40,60,80,100}; int idx=0; for(int i=0;i<6;i++)if(VS[i]==cfgVolume)idx=i;
             cfgSaveVolume(VS[(idx+1)%6]); audioSetVolume(cfgVolume); beep(1400,140); uiDirty=true; } break;
    case 4: { int idx=0; for(int i=0;i<5;i++)if(SAVER_OPTS[i]==saverSec)idx=i; cfgSaveSaver(SAVER_OPTS[(idx+1)%5]); uiDirty=true; } break;
    case 5: state=ST_CALIB; uiDirty=true; break;
    case 6: startOta(); break;
    case 7: irc.stop(); state=ST_CHAT; uiDirty=true; break;
    case 8: state=ST_CHAT; uiDirty=true; break;
  }
}
static void handleToolInput(){
  switch(toolSel){
    case 0: flockStart(); state=ST_FLOCK; uiDirty=true; break;
    case 1: trackerStart(); state=ST_TRACKER; uiDirty=true; break;
    case 2: netEnter(); state=ST_NETRECON; uiDirty=true; break;
    case 3: state=ST_WARDRIVE; wdStatus="press to scan"; uiDirty=true; break;
    case 4: beginTextIn("New Note",false,5,""); break;
    case 5: loadNotes(); break;
    case 6:
      if(sdReady) SD.end();
      sdTried=false; sdReady=false;
      if(sdEnsure()){
        char b[32]; snprintf(b,sizeof(b),"SD %lluMB ready",SD.cardSize()/1024/1024);
        notesStatus=b;
      } else notesStatus="SD mount failed";
      state=ST_TOOLS; uiDirty=true; break;
    case 7: clockMode=0; state=ST_CLOCK; uiDirty=true; break;
    case 8: state=ST_HASH; uiDirty=true; break;
    case 9: state=ST_QR; uiDirty=true; break;
    case 10: state=ST_VOICE; uiDirty=true; break;
    case 11: state=ST_HOME; uiDirty=true; break;
  }
}
static void handleEmailInput(){
  switch(emailSel){
    case 0: fetchInbox(); break;
    case 1:
      emailStatus=!cfgEmail.length()?"Set email address first":(!cfgEmailPass.length()?"Set app password first":"Compose/SMTP is next");
      state=ST_EMAIL; uiDirty=true; break;
    case 2: beginTextIn("Email Address",false,3,cfgEmail); break;
    case 3: beginTextIn(cfgEmailPass.length()?"App Pass Saved - Type New":"Email App Pass",true,4,""); break;
    case 4: state=ST_HOME; uiDirty=true; break;
  }
}

static void handleKeyChar(char c){
    if(!c) return; markActivity();
    if(state==ST_CHAT && scrollOff==0 && unread>0){ unread=0; uiDirty=true; }
    if(state==ST_HOME){
      if(c==KC_ENTER) openHomeTab();
      else if(c=='a'||c=='w'){ homeSel=(homeSel+HOME_N-1)%HOME_N; uiDirty=true; }
      else if(c=='d'||c=='s'){ homeSel=(homeSel+1)%HOME_N; uiDirty=true; }
    } else if(state==ST_CHAT){
      if(c==KC_ENTER){ if(inputLine.length()){ String o=inputLine; inputLine=""; if(ircJoined){irc.printf("PRIVMSG %s :%s\r\n",IRC_CHAN,o.c_str()); pushMsg(cfgNick,o,true);} else { if(!WiFi.isConnected()) pushMsg("*","wifi offline - check WiFi tab/password",false); else if(!irc.connected()){ pushMsg("*","irc connecting - try again",false); ircConnect(); } else pushMsg("*","joining channel - try again",false); } scrollOff=0; uiDirty=true; } }
      else if(c==KC_BKSP){ if(inputLine.length()){inputLine.remove(inputLine.length()-1);uiDirty=true;} }
      else if(c>=' '&&c<127){ if(inputLine.length()<400){inputLine+=c;uiDirty=true;} }
    } else if(state==ST_TEXTIN){
      if(c==KC_ENTER){
        if(tiPurpose==2){ if(tiBuf.length()){cfgSaveNick(tiBuf);ircNick=cfgNick;irc.stop();} state=ST_CHAT; uiDirty=true; }
        else if(tiPurpose==1){ cfgSaveWifi(pendingSsid,tiBuf); WiFi.disconnect(); WiFi.begin(cfgSsid.c_str(),cfgPass.c_str()); state=ST_CHAT; uiDirty=true; }
        else if(tiPurpose==3){ cfgSaveEmail(tiBuf); emailStatus="address saved"; state=ST_EMAIL; uiDirty=true; }
        else if(tiPurpose==4){ if(tiBuf.length()){ cfgSaveEmailPass(tiBuf); emailStatus="app password saved"; } else if(cfgEmailPass.length()) emailStatus="app password unchanged"; state=ST_EMAIL; uiDirty=true; }
        else if(tiPurpose==5){ saveNote(tiBuf); state=ST_TOOLS; uiDirty=true; }
        else if(tiPurpose==6){ cfgSaveWigleName(tiBuf); state=ST_WARDRIVE; uiDirty=true; }
        else if(tiPurpose==7){ cfgSaveWigleTok(tiBuf); state=ST_WARDRIVE; uiDirty=true; }
        else if(tiPurpose==8){ hashCompute(tiBuf); state=ST_HASH; uiDirty=true; }
        else if(tiPurpose==9){ qrText=tiBuf; state=ST_QR; uiDirty=true; }
      } else if(c==KC_BKSP){ if(tiBuf.length()){tiBuf.remove(tiBuf.length()-1);uiDirty=true;} }
      else if(c>=' '&&c<127){ size_t lim=(tiPurpose==5)?480:63; if(tiBuf.length()<lim){tiBuf+=c;uiDirty=true;} }
    } else if(state==ST_CALIB){
      if(c=='a'&&dispX>0){dispX--;panel->setOffsets(dispX,dispY);uiDirty=true;}
      else if(c=='d'&&dispX<120){dispX++;panel->setOffsets(dispX,dispY);uiDirty=true;}
      else if(c=='w'&&dispY>0){dispY--;panel->setOffsets(dispX,dispY);uiDirty=true;}
      else if(c=='s'&&dispY<120){dispY++;panel->setOffsets(dispX,dispY);uiDirty=true;}
      else if(c==KC_ENTER){ cfgSaveDisp(); state=ST_MENU; uiDirty=true; }
    } else if(state==ST_INBOX){
      if(c==KC_ENTER) fetchInbox();
      else if(c==KC_BKSP){ state=ST_EMAIL; uiDirty=true; }
    } else if(state==ST_TOOLS){
      if(c==KC_ENTER) handleToolInput();
      else if(c=='w'||c=='a'){ toolSel=(toolSel+TOOL_N-1)%TOOL_N; uiDirty=true; }
      else if(c=='s'||c=='d'){ toolSel=(toolSel+1)%TOOL_N; uiDirty=true; }
      else if(c==KC_BKSP){ state=ST_HOME; uiDirty=true; }
    } else if(state==ST_FLOCK){
      if(c==KC_BKSP){ flockStop(); state=ST_TOOLS; uiDirty=true; }
      else if(c=='r'||c==KC_ENTER){ flockStop(); flockStart(); uiDirty=true; }   // rescan
      else if(c=='w'||c=='a'){ if(flockN>0){flockSel=(flockSel+flockN-1)%flockN; uiDirty=true;} }
      else if(c=='s'||c=='d'){ if(flockN>0){flockSel=(flockSel+1)%flockN; uiDirty=true;} }
    } else if(state==ST_TRACKER){
      if(c==KC_BKSP){ trackerStop(); state=ST_TOOLS; uiDirty=true; }
      else if(c=='r'||c==KC_ENTER){ trackerStop(); trackerStart(); uiDirty=true; }
      else if(c=='w'||c=='a'){ if(trackN>0){trackSel=(trackSel+trackN-1)%trackN; uiDirty=true;} }
      else if(c=='s'||c=='d'){ if(trackN>0){trackSel=(trackSel+1)%trackN; uiDirty=true;} }
    } else if(state==ST_NETRECON){
      if(c==KC_BKSP){ if(netMode==1){netMode=0;uiDirty=true;} else { netScanning=false; state=ST_TOOLS; uiDirty=true; } }
      else if(c=='r'){ netEnter(); uiDirty=true; }
    } else if(state==ST_CLOCK){
      if(c==KC_BKSP){ state=ST_TOOLS; uiDirty=true; }
      else if(clockMode==1 && c=='r'){ swElapsed=0; swRun=false; uiDirty=true; }
      else if(clockMode==2 && (c=='w'||c=='+')){ if(tmSet<99)tmSet++; uiDirty=true; }
      else if(clockMode==2 && (c=='s'||c=='-')){ if(tmSet>1)tmSet--; uiDirty=true; }
      else if(c==KC_ENTER){ if(clockMode==1){ if(swRun){swElapsed+=millis()-swStart;swRun=false;} else {swStart=millis();swRun=true;} } else if(clockMode==2){ if(tmRun)tmRun=false; else {tmEnd=millis()+(uint32_t)tmSet*60000;tmRun=true;} } uiDirty=true; }
    } else if(state==ST_HASH){
      if(c==KC_BKSP){ state=ST_TOOLS; uiDirty=true; }
      else if(c=='e'){ beginTextIn("Hash/Encode text",false,8,hashIn); }
    } else if(state==ST_QR){
      if(c==KC_BKSP){ state=ST_TOOLS; uiDirty=true; }
      else if(c=='e'){ beginTextIn("QR text/URL",false,9,qrText); }
    } else if(state==ST_VOICE){
      if(c==KC_BKSP){ if(vRecording)voiceRecStop(); state=ST_TOOLS; uiDirty=true; }
      else if(c==KC_ENTER){ if(vRecording)voiceRecStop(); else voiceRecStart(); uiDirty=true; }
      else if(c=='p'){ if(!vRecording) voicePlayLast(); }
    } else if(state==ST_WARDRIVE){
      if(c=='u'){ wdStatus="uploading..."; render(); wdStatus=wigleUpload(); uiDirty=true; }
      else if(c=='c'){ beginTextIn("WiGLE API Name",false,6,cfgWigleName); }
      else if(c=='t'){ beginTextIn("WiGLE API Token",true,7,cfgWigleToken); }
      else if(c==KC_ENTER){ if(wdActive)wardriveStop(); else wardriveStart(); uiDirty=true; }
      else if(c==KC_BKSP||c=='b'){ wardriveStop(); state=ST_TOOLS; uiDirty=true; }
    } else if(state==ST_NOTES){
      if(c==KC_ENTER) openNote();
      else if((c=='w'||c=='a') && noteCount>0){ noteSel=(noteSel+noteCount-1)%noteCount; uiDirty=true; }
      else if((c=='s'||c=='d') && noteCount>0){ noteSel=(noteSel+1)%noteCount; uiDirty=true; }
      else if(c==KC_BKSP){ state=ST_TOOLS; uiDirty=true; }
    } else if(state==ST_NOTE_VIEW){
      if(c=='w'||c=='a'){ if(noteViewScroll>0)noteViewScroll--; uiDirty=true; }
      else if(c=='s'||c=='d'){ noteViewScroll++; uiDirty=true; }
      else if(c==KC_BKSP){ state=ST_NOTES; uiDirty=true; }
    }
}
static void pumpInput(){
#ifndef BOARD_TDECK
  while(keyboard.available()>0){
    int ev=keyboard.getEvent(); bool pressed=ev&0x80; char c=decodeKey((uint8_t)ev,pressed);
    handleKeyChar(c);
  }
#else
  // T-Deck's ESP32-C3 keyboard co-processor sends decoded ASCII directly.
  if(digitalRead(KB_INT_PIN)==LOW){
    Wire.requestFrom((uint8_t)KB_I2C_ADDR,(uint8_t)1);
    if(Wire.available()){
      char c=Wire.read();
      static bool loggedOnce=false;
      if(!loggedOnce && c){ Serial.printf("[hw] kb raw byte: 0x%02X\n",(uint8_t)c); loggedOnce=true; }
      handleKeyChar(c);
    }
  }
#endif
  int d=encDelta(); if(d!=0)markActivity();
  bool eb=encBtnEdge(), bb=bkBtnEdge(); if(eb||bb)markActivity();
  if((d!=0||eb||bb) && state==ST_CHAT && scrollOff==0 && unread>0){ unread=0; uiDirty=true; }
  if(state==ST_HOME){
    if(d!=0){ homeSel=(homeSel+(d>0?1:-1)+HOME_N)%HOME_N; uiDirty=true; }
    if(eb) openHomeTab();
    if(bb){ state=ST_CHAT; unread=0; uiDirty=true; }
  } else if(state==ST_CHAT){
    if(d!=0){ scrollOff+=d>0?-1:1; int mx=msgCount>0?msgCount-1:0; if(scrollOff<0)scrollOff=0; if(scrollOff>mx)scrollOff=mx; if(scrollOff==0)unread=0; uiDirty=true; }
    if(eb){ state=ST_HOME; uiDirty=true; }
    if(bb){ state=ST_HOME; uiDirty=true; }
  } else if(state==ST_EMAIL){
    if(d!=0){ emailSel=(emailSel+(d>0?1:-1)+EMAIL_N)%EMAIL_N; uiDirty=true; }
    if(eb) handleEmailInput();
    if(bb){ state=ST_HOME; uiDirty=true; }
  } else if(state==ST_INBOX){
    if(d!=0 && emailCount>0){ inboxSel=(inboxSel+(d>0?1:-1)+emailCount)%emailCount; uiDirty=true; }
    if(eb) fetchInbox();
    if(bb){ state=ST_EMAIL; uiDirty=true; }
  } else if(state==ST_TOOLS){
    if(d!=0){ toolSel=(toolSel+(d>0?1:-1)+TOOL_N)%TOOL_N; uiDirty=true; }
    if(eb) handleToolInput();
    if(bb){ state=ST_HOME; uiDirty=true; }
  } else if(state==ST_FLOCK){
    if(d!=0 && flockN>0){ flockSel=(flockSel+(d>0?1:-1)+flockN)%flockN; uiDirty=true; }
    if(eb||bb){ flockStop(); state=ST_TOOLS; uiDirty=true; }     // encoder press or BK = back
  } else if(state==ST_TRACKER){
    if(d!=0 && trackN>0){ trackSel=(trackSel+(d>0?1:-1)+trackN)%trackN; uiDirty=true; }
    if(eb||bb){ trackerStop(); state=ST_TOOLS; uiDirty=true; }
  } else if(state==ST_NETRECON){
    if(netMode==0 && d!=0 && netN>0){ netSel=(netSel+(d>0?1:-1)+netN)%netN; uiDirty=true; }
    if(eb){ if(netMode==1){ netMode=0; uiDirty=true; } else if(netN>0){ netPortScan(netHosts[netSel]); } }
    if(bb){ if(netMode==1){ netMode=0; uiDirty=true; } else { netScanning=false; state=ST_TOOLS; uiDirty=true; } }
  } else if(state==ST_CLOCK){
    if(d!=0){ clockMode=(clockMode+(d>0?1:-1)+3)%3; uiDirty=true; }
    if(eb){ if(clockMode==1){ if(swRun){swElapsed+=millis()-swStart;swRun=false;} else {swStart=millis();swRun=true;} } else if(clockMode==2){ if(tmRun)tmRun=false; else {tmEnd=millis()+(uint32_t)tmSet*60000;tmRun=true;} } uiDirty=true; }
    if(bb){ state=ST_TOOLS; uiDirty=true; }
  } else if(state==ST_HASH){
    if(eb||bb){ state=ST_TOOLS; uiDirty=true; }
  } else if(state==ST_QR){
    if(bb){ state=ST_TOOLS; uiDirty=true; }
  } else if(state==ST_VOICE){
    if(eb){ if(vRecording)voiceRecStop(); else voiceRecStart(); uiDirty=true; }
    if(bb){ if(vRecording)voiceRecStop(); state=ST_TOOLS; uiDirty=true; }
  } else if(state==ST_WARDRIVE){
    if(eb){ if(wdActive)wardriveStop(); else wardriveStart(); uiDirty=true; }
    if(bb){ wardriveStop(); state=ST_TOOLS; uiDirty=true; }
  } else if(state==ST_NOTES){
    if(d!=0 && noteCount>0){ noteSel=(noteSel+(d>0?1:-1)+noteCount)%noteCount; uiDirty=true; }
    if(eb) openNote();
    if(bb){ state=ST_TOOLS; uiDirty=true; }
  } else if(state==ST_NOTE_VIEW){
    if(d!=0){ noteViewScroll+=d>0?1:-1; if(noteViewScroll<0)noteViewScroll=0; uiDirty=true; }
    if(bb||eb){ state=ST_NOTES; uiDirty=true; }
  } else if(state==ST_MENU){
    if(d!=0){ menuSel=(menuSel+(d>0?1:-1)+MENU_N)%MENU_N; uiDirty=true; }
    if(eb) handleMenuInput();
    if(bb){ state=ST_HOME; uiDirty=true; }
  } else if(state==ST_WIFI){
    if(d!=0&&scanN>0){ scanSel=(scanSel+(d>0?1:-1)+scanN)%scanN; uiDirty=true; }
    if(eb&&scanN>0){ pendingSsid=scanSsid[scanSel]; beginTextIn("WiFi Password",true,1); }
    if(bb){ state=ST_HOME; uiDirty=true; }
  } else if(state==ST_TEXTIN){
    if(bb){ state=(tiPurpose==1?ST_WIFI:(tiPurpose==3||tiPurpose==4?ST_EMAIL:(tiPurpose==5?ST_TOOLS:(tiPurpose==6||tiPurpose==7?ST_WARDRIVE:(tiPurpose==8?ST_HASH:(tiPurpose==9?ST_QR:ST_MENU)))))); uiDirty=true; }
  } else if(state==ST_CALIB){
    if(d!=0){ dispY+=d; if(dispY<0)dispY=0; if(dispY>120)dispY=120; panel->setOffsets(dispX,dispY); uiDirty=true; }
    if(eb||bb){ cfgSaveDisp(); state=ST_MENU; uiDirty=true; }   // encoder press or BK saves
  }
}

// ───────────────────────── IRC ─────────────────────────
static void ircConnect(){
  lastIrcAttempt=millis(); ircRegistered=false; ircJoined=false; ircRx="";
  irc.stop(); irc.setInsecure();
  Serial.printf("[irc] connecting as %s\n",ircNick.c_str());
  if(irc.connect(IRC_HOST,IRC_PORT)){
    lastRxMs=millis(); lastKeepMs=millis();
    irc.printf("NICK %s\r\n",ircNick.c_str());
    irc.printf("USER %s 0 * :DarkSec Pager\r\n",ircNick.c_str());
  }
}
static void handleIrcLine(String line){
  if(line.endsWith("\r"))line.remove(line.length()-1); if(!line.length())return;
  Serial.printf("[irc<] %s\n",line.c_str());
  if(line.startsWith("PING ")){ irc.printf("PONG %s\r\n",line.substring(5).c_str()); return; }
  int cs=0; String prefix="";
  if(line[0]==':'){ int sp=line.indexOf(' '); prefix=line.substring(1,sp); cs=sp+1; }
  String rest=line.substring(cs); int sp=rest.indexOf(' '); String cmd=sp<0?rest:rest.substring(0,sp);
  if(cmd=="376"||cmd=="422"){ irc.printf("JOIN %s\r\n",IRC_CHAN); ircRegistered=true; uiDirty=true; }
  else if(cmd=="433"){ ircNick+=String((int)(millis()%1000)); irc.printf("NICK %s\r\n",ircNick.c_str()); }
  else if(cmd=="JOIN"){ if(prefix.startsWith(ircNick)){ ircJoined=true; uiDirty=true; } }
  else if(cmd=="PRIVMSG"){
    String who=prefix.substring(0,prefix.indexOf('!'));
    int colon=line.indexOf(" :",cs);
    if(colon>=0){ String text=line.substring(colon+2); if(who!=ircNick){ bool countUnread=(state!=ST_CHAT||scrollOff!=0); pushMsg(who,text,false); if(countUnread)unread++; notify(); uiDirty=true; } }
  }
}
static void pumpIrc(){ while(irc.available()){ char c=irc.read(); lastRxMs=millis(); if(c=='\n'){handleIrcLine(ircRx);ircRx="";} else if(ircRx.length()<1024)ircRx+=c; } }
static void ircKeepAlive(){ if(!irc.connected())return; uint32_t n=millis(); if(n-lastRxMs>60000&&n-lastKeepMs>60000){irc.print("PING :keepalive\r\n");lastKeepMs=n;} }

// ───────────────────────── WiFi ─────────────────────────
static uint32_t wifiTryMs=0;
static void wifiConnectTo(const char* s,const char* p){
  if(!s || !*s) return;
  Serial.printf("[wifi] connect %s pass=%s\n",s,(p&&*p)?"set":"empty");
  WiFi.begin(s,p); wifiTryMs=millis();
}
static void wifiEnsure(){
  if(WiFi.status()==WL_CONNECTED) return;
  if(!cfgSsid.length() && !cfgSsid2.length()) return;
  if(millis()-wifiTryMs<9000) return;
  wifiTryMs=millis();                     // throttle no matter what the scan does
  WiFi.disconnect(false,false);           // stop the pending begin() so the scan can run
  int n=WiFi.scanNetworks(false,true); bool see1=false,see2=false;
  for(int i=0;i<n;i++){ String s=WiFi.SSID(i);
    if(cfgSsid.length() && s==cfgSsid) see1=true;
    if(cfgSsid2.length() && s==cfgSsid2) see2=true; }
  WiFi.scanDelete();
  Serial.printf("[wifi] scan n=%d primary=%d fallback=%d\n",n,see1,see2);
  // prefer the primary hotspot; fall back to the secondary network when it's off
  if(see1)      wifiConnectTo(cfgSsid.c_str(),cfgPass.c_str());
  else if(see2) wifiConnectTo(cfgSsid2.c_str(),cfgPass2.c_str());
}

// ───────────────────────── setup / loop ─────────────────────────
void setup(){
  // NOTE: T-LoRa-Pager has NO firmware power latch (per LilyGo docs) — power-on
  // is the hardware PWR button + BQ25896. GPIO15 is not a power pin here.
  Serial.begin(115200); delay(300);
  Serial.println("\n== darksec-pager boot ==");
  cfgLoad(); ircNick=cfgNick;
  hwInit();
  sdEnsure();          // mount SD at boot so notes are ready
  drawSplash();
  beep(880,90); beep(1320,120);   // boot chime
  pushMsg("*","darkpager pagernet online",false);
  state=ST_HOME; lastActivity=millis(); render();
  WiFi.mode(WIFI_STA); WiFi.persistent(false); WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm); WiFi.setAutoReconnect(false);  // wifiEnsure() manages reconnect + fallback
  wifiConnectTo(cfgSsid.c_str(),cfgPass.c_str());
  configTzTime(DEFAULT_TZ,"pool.ntp.org","time.nist.gov");
  // First boot on a fresh device: ask the user for their own IRC nickname.
  if(firstRunNick) beginTextIn("Enter your IRC nick",false,2,"");
}
void loop(){
  if(state!=ST_WARDRIVE) wifiEnsure();
  static bool wifiWas=false; bool up=WiFi.isConnected();
  if(up!=wifiWas){ wifiWas=up; uiDirty=true;
    if(up){ Serial.printf("[wifi] up %s\n",WiFi.localIP().toString().c_str()); pushMsg("*","wifi: "+WiFi.localIP().toString(),false); configTzTime(DEFAULT_TZ,"pool.ntp.org","time.nist.gov"); } }
  if(up && state!=ST_WIFI && state!=ST_WARDRIVE){
    if(!irc.connected() && millis()-lastIrcAttempt>8000) ircConnect();
    if(irc.connected()){ pumpIrc(); ircKeepAlive(); }
  }
  pumpInput();
  if(flockScanning) flockService();
  if(trackScanning) trackerService();
  if(netScanning) netStep();
  if(vRecording) voiceRecStep();
  if(state==ST_CLOCK){ static uint32_t lc=0; if(millis()-lc>200){ lc=millis(); if(tmRun&&(long)(tmEnd-millis())<=0){ tmRun=false; beep(1800,300); beep(1800,300); } uiDirty=true; } }
  if(wdActive) wardriveScan();
  gpsPoll();
  // screensaver: enter after idle
  if(state!=ST_SAVER && state!=ST_WIFI && state!=ST_CALIB && state!=ST_FLOCK && state!=ST_WARDRIVE && state!=ST_TRACKER && state!=ST_NETRECON && state!=ST_CLOCK && state!=ST_HASH && state!=ST_QR && state!=ST_VOICE && saverSec>0 && millis()-lastActivity > (uint32_t)saverSec*1000){
    prevState=(state==ST_MENU||state==ST_TEXTIN)?ST_CHAT:state; state=ST_SAVER; saverEnter();
  }
  if(state==ST_SAVER){ static uint32_t last=0; if(millis()-last>140){ saverStep(); if(gfx)gfx->flush(); last=millis(); } }
  else if(uiDirty) render();
  delay(5);
}
