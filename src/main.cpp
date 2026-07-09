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
#include <time.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_TCA8418.h>
#include <RotaryEncoder.h>

#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>
#include <ExtensionIOXL9555.hpp>
#include <HapticDrivers.hpp>

#if __has_include("private_config.h")
#include "private_config.h"
#endif

#ifndef DEFAULT_SSID
#define DEFAULT_SSID ""
#endif
#ifndef DEFAULT_PASS
#define DEFAULT_PASS ""
#endif
#ifndef DEFAULT_NICK
#define DEFAULT_NICK "DarkSecPager"
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
static const char* OTA_HOST = "darksec-pager";
static const char* OTA_PASS = DEFAULT_OTA_PASS;
#define DRV2605_ADDR 0x5A
// ────────────────────────────────────────────────────────────

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

Arduino_DataBus* bus = nullptr;
DarkGFX* gfx = nullptr;
ExtensionIOXL9555 io;
PowersBQ25896 PPM;
Adafruit_TCA8418 keyboard;
RotaryEncoder* enc = nullptr;
HapticDriver_DRV2605 haptic;
bool hapticOk = false;
Preferences prefs;
IRAM_ATTR void encTick(){ if(enc) enc->tick(); }

// ---- keyboard matrix ----
#define KB_ROWS 4
#define KB_COLS 10
enum { KC_ENTER='\r', KC_BKSP='\b', KC_FN=0x01, KC_SHIFT=0x02, KC_CAPS=0x03 };
struct KV { char a,b,c; };
static const KV KEYMAP[KB_ROWS][KB_COLS] = {
  {{'q','Q','1'},{'w','W','2'},{'e','E','3'},{'r','R','4'},{'t','T','5'},{'y','Y','6'},{'u','U','7'},{'i','I','8'},{'o','O','9'},{'p','P','0'}},
  {{'a','A','*'},{'s','S','/'},{'d','D','+'},{'f','F','-'},{'g','G','='},{'h','H',':'},{'j','J','\''},{'k','K','"'},{'l','L','@'},{KC_ENTER,KC_ENTER,KC_ENTER}},
  {{KC_FN,KC_FN,KC_FN},{'z','Z','_'},{'x','X','$'},{'c','C',';'},{'v','V','?'},{'b','B','!'},{'n','N',','},{'m','M','.'},{KC_SHIFT,KC_SHIFT,KC_CAPS},{KC_BKSP,KC_BKSP,KC_BKSP}},
  {{' ',' ',' '},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0}},
};
static bool fnDown=false, shiftDown=false, capsLock=false;

// ---- UI palette ----
static int SCR_W=480, SCR_H=222;
#define ARA_RED  RGB565(210,30,40)
#define ARA_DIM  RGB565(110,12,18)
#define ARA_DEEP RGB565(45,4,8)
#define TXT      RGB565(215,215,215)
#define DIMTXT   RGB565(120,120,120)
#define GRN      RGB565(90,220,120)
static const int LINE_H=16, CHARW=12;
static bool otaActive=false;
static const uint16_t NICKPAL[]={RGB565(90,200,120),RGB565(80,170,255),RGB565(230,180,60),
  RGB565(220,110,220),RGB565(90,220,220),RGB565(240,120,90),RGB565(160,220,90),RGB565(200,160,255)};
static uint16_t nickColor(const String& n){uint32_t h=0;for(char c:n)h=h*31+(uint8_t)c;return NICKPAL[h%(sizeof(NICKPAL)/2)];}

// ---- messages ----
#define MAX_MSGS 80
struct Msg { String who, text, ts; bool self; };
static Msg msgs[MAX_MSGS];
static int msgCount=0, scrollOff=0, unread=0;
static String nowHHMM(){ struct tm t; if(getLocalTime(&t,5)){char b[6];snprintf(b,6,"%02d:%02d",t.tm_hour,t.tm_min);return String(b);} return "--:--"; }
static void pushMsg(const String& who,const String& text,bool self){
  Msg m{who,text,nowHHMM(),self};
  if(msgCount<MAX_MSGS) msgs[msgCount++]=m;
  else { for(int i=0;i<MAX_MSGS-1;i++)msgs[i]=msgs[i+1]; msgs[MAX_MSGS-1]=m; }
}

// ---- settings (NVS) ----
static String cfgSsid, cfgPass, cfgNick, cfgEmail, cfgEmailPass;
static bool notifyEnabled=true;
static int saverSec=60;              // 0 = off
static int dispX=0, dispY=0;         // display alignment offsets (NVS)
static const int SAVER_OPTS[]={0,30,60,120,300};
static void cfgLoad(){
  prefs.begin("pager",true);
  cfgSsid=prefs.getString("ssid",DEFAULT_SSID);
  cfgPass=prefs.getString("pass",DEFAULT_PASS);
  cfgNick=prefs.getString("nick",DEFAULT_NICK);
  cfgEmail=prefs.isKey("email")?prefs.getString("email"):"";
  cfgEmailPass=prefs.isKey("emailPass")?prefs.getString("emailPass"):"";
  if(strlen(DEFAULT_SSID)>0){ cfgSsid=DEFAULT_SSID; cfgPass=DEFAULT_PASS; }
  if(!cfgEmail.length() && strlen(DEFAULT_EMAIL)>0) cfgEmail=DEFAULT_EMAIL;
  if(!cfgEmailPass.length() && strlen(DEFAULT_EMAIL_PASS)>0) cfgEmailPass=DEFAULT_EMAIL_PASS;
  notifyEnabled=prefs.getBool("notify",true);
  saverSec=prefs.getInt("saver",60);
  dispX=prefs.getInt("dx2",0); dispY=prefs.getInt("dy2",0);   // fresh keys => clean baseline
  prefs.end();
}
static void cfgSaveDisp(){prefs.begin("pager",false);prefs.putInt("dx2",dispX);prefs.putInt("dy2",dispY);prefs.end();}
static void cfgSaveWifi(const String&s,const String&p){prefs.begin("pager",false);prefs.putString("ssid",s);prefs.putString("pass",p);prefs.end();cfgSsid=s;cfgPass=p;}
static void cfgSaveNick(const String&n){prefs.begin("pager",false);prefs.putString("nick",n);prefs.end();cfgNick=n;}
static void cfgSaveEmail(const String&e){prefs.begin("pager",false);prefs.putString("email",e);prefs.end();cfgEmail=e;}
static void cfgSaveEmailPass(const String&p){prefs.begin("pager",false);prefs.putString("emailPass",p);prefs.end();cfgEmailPass=p;}
static void cfgSaveNotify(bool b){prefs.begin("pager",false);prefs.putBool("notify",b);prefs.end();notifyEnabled=b;}
static void cfgSaveSaver(int s){prefs.begin("pager",false);prefs.putInt("saver",s);prefs.end();saverSec=s;}

// ---- app state ----
enum AppState { ST_HOME, ST_CHAT, ST_EMAIL, ST_MENU, ST_WIFI, ST_TEXTIN, ST_SAVER, ST_CALIB };
static AppState state=ST_HOME, prevState=ST_HOME;
static bool uiDirty=true;
static String inputLine="", pendingSsid="";
static uint32_t lastActivity=0;

static String tiTitle, tiBuf; static bool tiPassword=false; static int tiPurpose=0;

static const char* HOME_TABS[]={"Chat","Email","WiFi","OTA","Setup"};
static const int HOME_N=5;
static int homeSel=0;

static const char* EMAIL_MENU[]={"Inbox","Compose","Set Address","Set App Pass","Back"};
static const int EMAIL_N=5;
static int emailSel=0;

static const char* MENU[]={"WiFi Setup","Set Nickname","Notifications","Screensaver","Calibrate Screen","OTA Update","Reconnect IRC","Back to Chat"};
static const int MENU_N=8;
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
  uint16_t mv = PPM.getBattVoltage();     // mV
  if(mv<2500) return -1;                  // no battery / not reading
  int pct=(int)((mv-3300)*100/(4200-3300));
  return pct<0?0:pct>100?100:pct;
}

// ───────────────────────── activity / screensaver ─────────────────────────
static void wakeBacklight(){ analogWrite(TFT_BL,200); analogWrite(KEYBOARD_BL,120); }
static void markActivity(){
  lastActivity=millis();
  if(state==ST_SAVER){ state=prevState; wakeBacklight(); uiDirty=true; }
}

// ───────────────────────── haptic ─────────────────────────
static void buzz(int ms,uint8_t amp){
  if(!hapticOk) return;
  haptic.setMode(HapticMode::REAL_TIME_PLAYBACK);
  haptic.setRealtimeValue(amp);
  delay(ms);
  haptic.setRealtimeValue(0);
  haptic.setMode(HapticMode::INTERNAL_TRIGGER);
}
static void notify(){
  markActivity();                          // a message wakes the screen
  if(!notifyEnabled) return;
  buzz(130,0x70); delay(70); buzz(130,0x70);    // double buzz
  analogWrite(KEYBOARD_BL,255); delay(50); analogWrite(KEYBOARD_BL,120);
}

// ───────────────────────── rendering ─────────────────────────
static void headerBar(const char* title){
  gfx->fillRect(0,0,SCR_W,LINE_H,ARA_DIM);
  gfx->setTextSize(2); gfx->setTextColor(WHITE,ARA_DIM);
  gfx->setCursor(2,0); gfx->print(title);
  // right side: battery + clock + status
  char rb[48]; int bp=batteryPct();
  const char* st=!WiFi.isConnected()?"WiFi":(otaActive?"OTA":(!ircRegistered?"IRC":(!ircJoined?"join":"#HQ")));
  if(bp>=0 && unread>0) snprintf(rb,sizeof(rb),"U%d %d%% %s %s",unread,bp,nowHHMM().c_str(),st);
  else if(bp>=0)        snprintf(rb,sizeof(rb),"%d%% %s %s",bp,nowHHMM().c_str(),st);
  else if(unread>0)     snprintf(rb,sizeof(rb),"U%d %s %s",unread,nowHHMM().c_str(),st);
  else                  snprintf(rb,sizeof(rb),"%s %s",nowHHMM().c_str(),st);
  int w=strlen(rb)*CHARW; gfx->setCursor(SCR_W-w-2,0); gfx->print(rb);
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
      gfx->setTextColor(m.self?ARA_RED:nickColor(m.who),BLACK); gfx->print(m.who);
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
  gfx->fillScreen(BLACK); headerBar("PAGER HOME");
  int top=LINE_H+10, cardW=SCR_W/HOME_N, cardH=116;
  for(int i=0;i<HOME_N;i++){
    int x=i*cardW+3; bool sel=(i==homeSel); uint16_t bg=sel?ARA_DIM:ARA_DEEP;
    gfx->fillRoundRect(x,top,cardW-6,cardH,8,bg);
    gfx->drawRoundRect(x,top,cardW-6,cardH,8,sel?ARA_RED:ARA_DIM);
    gfx->setTextSize(2); gfx->setTextColor(sel?WHITE:TXT,bg);
    int tx=x+((cardW-6)-(int)strlen(HOME_TABS[i])*CHARW)/2; if(tx<x+4)tx=x+4;
    gfx->setCursor(tx,top+16); gfx->print(HOME_TABS[i]);
    gfx->setTextSize(1); gfx->setTextColor(sel?WHITE:DIMTXT,bg);
    const char* sub="";
    if(i==0) sub=unread>0?"unread chat":"irc chat";
    else if(i==1) sub=cfgEmail.length()?"mailbox":"configure";
    else if(i==2) sub=WiFi.isConnected()?"connected":"setup";
    else if(i==3) sub=otaActive?"ready":"update";
    else sub="settings";
    gfx->setCursor(x+8,top+52); gfx->print(sub);
    if(i==0 && unread>0){ gfx->setTextSize(2); gfx->setTextColor(GRN,bg); gfx->setCursor(x+8,top+78); gfx->print("U"); gfx->print(unread); }
  }
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK);
  gfx->setCursor(6,SCR_H-12); gfx->print("turn=tab  press=open  BK=chat");
}

static void drawEmail(){
  gfx->fillScreen(BLACK); headerBar("EMAIL");
  int top=LINE_H+4, rowH=26, vis=(SCR_H-LINE_H-16)/rowH;
  for(int i=0;i<EMAIL_N && i<vis;i++){
    int y=top+i*rowH; bool sel=(i==emailSel); uint16_t bg=sel?ARA_DIM:BLACK;
    if(sel) gfx->fillRect(0,y,SCR_W,rowH,ARA_DIM);
    gfx->setTextSize(2); gfx->setTextColor(sel?WHITE:TXT,bg); gfx->setCursor(8,y+5);
    gfx->print(sel?"> ":"  "); gfx->print(EMAIL_MENU[i]);
  }
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK);
  gfx->setCursor(8,SCR_H-33); gfx->print(cfgEmail.length()?cfgEmail.c_str():"No email account configured");
  gfx->setCursor(8,SCR_H-22); gfx->print(cfgEmailPass.length()?"App password: saved":"App password: not set");
  gfx->setCursor(8,SCR_H-11); gfx->print("Gmail requires an app password");
}

static void drawChat(){
  gfx->fillScreen(BLACK);
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
  gfx->fillRect(0,inputY,SCR_W,LINE_H,RGB565(22,22,22));
  gfx->setTextSize(2); gfx->setTextColor(ARA_RED,RGB565(22,22,22));
  gfx->setCursor(2,inputY); gfx->print("> ");
  gfx->setTextColor(WHITE,RGB565(22,22,22));
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
  gfx->fillScreen(BLACK); headerBar("MENU");
  const int top=LINE_H+3, footerY=SCR_H-11, rowH=26;
  const int vis=max(1,(footerY-top)/rowH);
  const int start=listStart(menuSel,MENU_N,vis);
  for(int r=0;r<vis && start+r<MENU_N;r++){
    int i=start+r, ry=top+r*rowH, ty=ry+(rowH-16)/2;
    bool sel=(i==menuSel); uint16_t bg=sel?ARA_DIM:BLACK;
    if(sel) gfx->fillRect(0,ry,SCR_W,rowH,ARA_DIM);
    gfx->setTextSize(2);
    gfx->setTextColor(sel?WHITE:ARA_RED,bg); gfx->setCursor(6,ty); gfx->print(sel?">":" ");
    gfx->setTextColor(sel?WHITE:TXT,bg); gfx->setCursor(28,ty); gfx->print(MENU[i]);
    char vb[10]; const char* val=nullptr; uint16_t vc=GRN;
    if(i==2){ val=notifyEnabled?"ON":"OFF"; vc=notifyEnabled?GRN:DIMTXT; }
    else if(i==3){ if(saverSec==0)strcpy(vb,"Off"); else if(saverSec<60)snprintf(vb,10,"%ds",saverSec); else snprintf(vb,10,"%dm",saverSec/60); val=vb; }
    else if(i==5){ val=otaActive?"ON":"Start"; vc=otaActive?GRN:ARA_RED; }
    if(val){ int w=strlen(val)*CHARW; gfx->setTextColor(vc,bg); gfx->setCursor(SCR_W-w-16,ty); gfx->print(val); }
  }
  scrollArrows(start,vis,MENU_N,top,footerY);
  gfx->setTextColor(DIMTXT,BLACK); gfx->setTextSize(1);
  gfx->setCursor(6,SCR_H-10); gfx->print("turn=move  press=select");
}

static void drawWifi(){
  gfx->fillScreen(BLACK); headerBar("WIFI SETUP");
  int y=LINE_H+8; gfx->setTextSize(2);
  if(scanN==0){ gfx->setTextColor(DIMTXT,BLACK); gfx->setCursor(8,y); gfx->print("scanning..."); return; }
  int rows=(SCR_H-LINE_H-24)/(LINE_H+2); int start=scanSel>=rows?scanSel-rows+1:0;
  for(int i=start;i<scanN&&i<start+rows;i++){
    bool sel=(i==scanSel);
    if(sel) gfx->fillRect(0,y-2,SCR_W,LINE_H+3,ARA_DIM);
    gfx->setTextColor(sel?WHITE:TXT,sel?ARA_DIM:BLACK);
    gfx->setCursor(8,y); gfx->print(sel?">":" ");
    String s=scanSsid[i]; if(s.length()>22)s=s.substring(0,22); gfx->print(s);
    char rb[16]; snprintf(rb,sizeof(rb),"%d%s",scanRssi[i],scanEnc[i]?" L":" "); int w=strlen(rb)*CHARW;
    gfx->setCursor(SCR_W-w-2,y); gfx->print(rb); y+=LINE_H+2;
  }
}

static void drawTextIn(){
  gfx->fillScreen(BLACK); headerBar(tiTitle.c_str());
  gfx->setTextSize(2); gfx->setTextColor(WHITE,BLACK); gfx->setCursor(8,LINE_H+24);
  String show; if(tiPassword){for(size_t i=0;i<tiBuf.length();i++)show+='*';} else show=tiBuf;
  gfx->print(show); gfx->print("_");
  gfx->setTextColor(DIMTXT,BLACK); gfx->setTextSize(1);
  gfx->setCursor(8,SCR_H-12); gfx->print("type + Enter to save, BK=cancel");
}

// DarkCell / Arasaka mark (used by splash + saver)
static void darkcellMark(int cx,int cy,int rad,uint16_t col){
  for(int o=0;o<2;o++){
    gfx->drawLine(cx,cy-rad+o,cx+rad-o,cy,col); gfx->drawLine(cx+rad-o,cy,cx,cy+rad-o,col);
    gfx->drawLine(cx,cy+rad-o,cx-rad+o,cy,col); gfx->drawLine(cx-rad+o,cy,cx,cy-rad+o,col);
  }
  gfx->fillRect(cx-1,cy-rad+8,2,rad*2-16,col);
  gfx->fillRect(cx-rad+8,cy-1,rad*2-16,2,col);
}
static void drawSplash(){
  gfx->fillScreen(BLACK);
  gfx->fillRect(0,0,SCR_W,3,ARA_RED); gfx->fillRect(0,SCR_H-3,SCR_W,3,ARA_RED);
  int cx=SCR_W/2, cy=SCR_H/2-14;
  darkcellMark(cx,cy,34,ARA_RED);
  gfx->setTextSize(3); gfx->setTextColor(ARA_RED,BLACK);
  const char* t="DARKCELL"; int tw=strlen(t)*18;
  gfx->setCursor((SCR_W-tw)/2,cy+44); gfx->print(t);
  gfx->setTextSize(1); gfx->setTextColor(ARA_DIM,BLACK);
  const char* s="ARASAKA  SECURITY  //  IRC LINK";
  gfx->setCursor((SCR_W-strlen(s)*6)/2,cy+72); gfx->print(s);
  // loading bar
  int bx=40,bw=SCR_W-80,by=SCR_H-20;
  gfx->drawRect(bx-1,by-1,bw+2,6,ARA_DIM);
  for(int x=0;x<bw;x++){ gfx->drawFastVLine(bx+x,by,4,ARA_RED); delay(3200/bw); }
  delay(900);   // hold the completed splash a beat
}
// Bouncing "DARKCELL" wordmark screensaver (changes color on each bounce).
static const int SV_TS=3;                 // text size
static const int SV_TW=8*6*SV_TS;         // "DARKCELL" = 8 chars * 6px * size
static const int SV_TH=8*SV_TS;
static int svx,svy,svdx=4,svdy=3,psvx,psvy;
static uint16_t svcol=ARA_RED;
static void drawSaverStatus();
static void saverEnter(){
  gfx->fillScreen(BLACK);
  analogWrite(TFT_BL,70); analogWrite(KEYBOARD_BL,0);      // dim, but DARKCELL still readable
  svx=(SCR_W-SV_TW)/2; svy=(SCR_H-SV_TH)/2; psvx=svx; psvy=svy; svcol=ARA_RED;
  drawSaverStatus();
}
static void drawSaverStatus(){
  gfx->fillRect(0,0,SCR_W,LINE_H,BLACK);
  gfx->setTextSize(2); gfx->setTextColor(unread?GRN:DIMTXT,BLACK); gfx->setCursor(2,0);
  if(unread>0) { gfx->print("UNREAD "); gfx->print(unread); }
  else gfx->print(WiFi.isConnected()?"ONLINE":"OFFLINE");
  gfx->setTextColor(otaActive?GRN:DIMTXT,BLACK);
  const char* st=otaActive?"OTA":"#HQ"; gfx->setCursor(SCR_W-strlen(st)*CHARW-2,0); gfx->print(st);
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
  gfx->setCursor(svx,svy); gfx->print("DARKCELL");
}

static void drawCalib(){
  gfx->fillScreen(BLACK);
  gfx->drawRect(0,0,SCR_W,SCR_H,GRN);              // outer edge — align this to the panel
  gfx->drawRect(3,3,SCR_W-6,SCR_H-6,ARA_RED);
  int cx=SCR_W/2, cy=SCR_H/2;
  gfx->drawFastHLine(cx-14,cy,28,GRN); gfx->drawFastVLine(cx,cy-14,28,GRN);
  gfx->setTextSize(2); gfx->setTextColor(WHITE,BLACK);
  char b[28]; snprintf(b,28,"X=%d  Y=%d",dispX,dispY);
  gfx->setCursor(cx-strlen(b)*6,cy-42); gfx->print(b);
  gfx->setTextSize(1); gfx->setTextColor(DIMTXT,BLACK);
  gfx->setCursor(cx-135,cy+30); gfx->print("turn/w,s=up-down  a,d=left-right  press/Enter=SAVE");
}
static void render(){
  switch(state){
    case ST_HOME:drawHome();break; case ST_CHAT:drawChat();break; case ST_EMAIL:drawEmail();break; case ST_MENU:drawMenu();break;
    case ST_WIFI:drawWifi();break; case ST_TEXTIN:drawTextIn();break;
    case ST_CALIB:drawCalib();break;
    case ST_SAVER: break; }   // saver drawn incrementally via saverStep()
  uiDirty=false;
}

// ───────────────────────── hardware ─────────────────────────
#define STEP(m) do{Serial.println(m);Serial.flush();}while(0)
static void hwInit(){
  STEP("[hw] power"); pinMode(PIN_POWER_ON,OUTPUT); digitalWrite(PIN_POWER_ON,HIGH);
  pinMode(BK_BTN,INPUT_PULLUP); pinMode(ENCODER_KEY,INPUT_PULLUP);
  Wire.begin(I2C_SDA,I2C_SCL);
  if(PPM.init(Wire,I2C_SDA,I2C_SCL,BQ25896_I2C_ADDRESS)){
    PPM.setSysPowerDownVoltage(3300); PPM.setInputCurrentLimit(3250); PPM.disableCurrentLimitPin();
    PPM.setChargeTargetVoltage(4208); PPM.setChargerConstantCurr(832); PPM.enableMeasure(); PPM.enableCharge();
  }
  if(io.begin(Wire,XL9555_ADDR)){
    const uint8_t en[]={EXPANDS_KB_RST,EXPANDS_KB_EN,EXPANDS_SD_EN};
    for(uint8_t p:en){io.pinMode(p,OUTPUT);io.digitalWrite(p,HIGH);delay(2);}
  }
  if(keyboard.begin(KB_I2C_ADDRESS,&Wire)){ keyboard.matrix(KB_ROWS,KB_COLS); keyboard.flush(); }
  hapticOk=haptic.begin(Wire,DRV2605_ADDR);
  if(hapticOk){ haptic.setMode(HapticMode::INTERNAL_TRIGGER); haptic.selectLibrary(1); }
  Serial.printf("[hw] haptic %s\n",hapticOk?"ok":"FAIL");
  enc=new RotaryEncoder(ENCODER_INA,ENCODER_INB,RotaryEncoder::LatchMode::FOUR3);
  attachInterrupt(digitalPinToInterrupt(ENCODER_INA),encTick,CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_INB),encTick,CHANGE);
  pinMode(KEYBOARD_BL,OUTPUT); analogWrite(KEYBOARD_BL,120);
  bus=new Arduino_HWSPI(TFT_DC,TFT_CS,TFT_SCLK,TFT_MOSI,TFT_MISO,&SPI);
  // Wipe the ENTIRE ST7796 controller RAM (full 320x480, no offsets) to erase
  // any leftover pixels from a previous firmware (e.g. old Meshtastic status bar).
  { Arduino_GFX* wipe=new Arduino_ST7796(bus,TFT_RST,0,TFT_IPS,320,480,0,0,0,0);
    wipe->begin(); wipe->fillScreen(BLACK); delete wipe; }
  gfx=new DarkGFX(bus,TFT_RST,TFT_ROTATION,TFT_IPS,TFT_WIDTH,TFT_HEIGHT,TFT_COL_OFS1,TFT_ROW_OFS1,TFT_COL_OFS2,TFT_ROW_OFS2);
  gfx->begin(); gfx->setOffsets(dispX,dispY);      // apply saved alignment
  gfx->fillScreen(BLACK); SCR_W=gfx->width(); SCR_H=gfx->height();
  pinMode(TFT_BL,OUTPUT); analogWrite(TFT_BL,200);
  Serial.printf("[hw] display %dx%d\n",SCR_W,SCR_H);
}

// ───────────────────────── input ─────────────────────────
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

static void beginTextIn(const char* title,bool pw,int purpose,const String& initial=""){
  state=ST_TEXTIN; tiTitle=title; tiPassword=pw; tiPurpose=purpose; tiBuf=initial; uiDirty=true;
}
static void startWifiScan();
static void startOta();
static void ircConnect();
static void openHomeTab(){
  switch(homeSel){
    case 0: state=ST_CHAT; unread=0; uiDirty=true; break;
    case 1: state=ST_EMAIL; emailSel=0; uiDirty=true; break;
    case 2: startWifiScan(); break;
    case 3: startOta(); break;
    case 4: state=ST_MENU; menuSel=0; uiDirty=true; break;
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
static void handleMenuInput(){
  switch(menuSel){
    case 0: startWifiScan(); break;
    case 1: beginTextIn("Set Nickname",false,2,cfgNick); break;
    case 2: cfgSaveNotify(!notifyEnabled); if(notifyEnabled)buzz(120,0x70); uiDirty=true; break;
    case 3: { int idx=0; for(int i=0;i<5;i++)if(SAVER_OPTS[i]==saverSec)idx=i; cfgSaveSaver(SAVER_OPTS[(idx+1)%5]); uiDirty=true; } break;
    case 4: state=ST_CALIB; uiDirty=true; break;
    case 5: startOta(); break;
    case 6: irc.stop(); state=ST_CHAT; uiDirty=true; break;
    case 7: state=ST_CHAT; uiDirty=true; break;
  }
}
static void handleEmailInput(){
  switch(emailSel){
    case 0: pushMsg("email",cfgEmail.length()?"Inbox support needs app password and IMAP implementation":"Set email first",false); state=ST_CHAT; uiDirty=true; break;
    case 1: pushMsg("email",cfgEmail.length()?"Compose UI is staged; SMTP send is next":"Set email first",false); state=ST_CHAT; uiDirty=true; break;
    case 2: beginTextIn("Email Address",false,3,cfgEmail); break;
    case 3: beginTextIn("Email App Pass",true,4,""); break;
    case 4: state=ST_HOME; uiDirty=true; break;
  }
}

static void pumpInput(){
  while(keyboard.available()>0){
    int ev=keyboard.getEvent(); bool pressed=ev&0x80; char c=decodeKey((uint8_t)ev,pressed);
    if(!c) continue; markActivity();
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
        if(tiPurpose==2){ if(tiBuf.length()){cfgSaveNick(tiBuf);ircNick=tiBuf;irc.stop();} state=ST_CHAT; uiDirty=true; }
        else if(tiPurpose==1){ cfgSaveWifi(pendingSsid,tiBuf); WiFi.disconnect(); WiFi.begin(cfgSsid.c_str(),cfgPass.c_str()); state=ST_CHAT; uiDirty=true; }
        else if(tiPurpose==3){ cfgSaveEmail(tiBuf); pushMsg("email","address saved",false); state=ST_EMAIL; uiDirty=true; }
        else if(tiPurpose==4){ if(tiBuf.length()){ cfgSaveEmailPass(tiBuf); pushMsg("email","app password saved",false); } state=ST_EMAIL; uiDirty=true; }
      } else if(c==KC_BKSP){ if(tiBuf.length()){tiBuf.remove(tiBuf.length()-1);uiDirty=true;} }
      else if(c>=' '&&c<127){ if(tiBuf.length()<63){tiBuf+=c;uiDirty=true;} }
    } else if(state==ST_CALIB){
      if(c=='a'&&dispX>0){dispX--;gfx->setOffsets(dispX,dispY);uiDirty=true;}
      else if(c=='d'&&dispX<120){dispX++;gfx->setOffsets(dispX,dispY);uiDirty=true;}
      else if(c=='w'&&dispY>0){dispY--;gfx->setOffsets(dispX,dispY);uiDirty=true;}
      else if(c=='s'&&dispY<120){dispY++;gfx->setOffsets(dispX,dispY);uiDirty=true;}
      else if(c==KC_ENTER){ cfgSaveDisp(); state=ST_MENU; uiDirty=true; }
    }
  }
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
  } else if(state==ST_MENU){
    if(d!=0){ menuSel=(menuSel+(d>0?1:-1)+MENU_N)%MENU_N; uiDirty=true; }
    if(eb) handleMenuInput();
    if(bb){ state=ST_HOME; uiDirty=true; }
  } else if(state==ST_WIFI){
    if(d!=0&&scanN>0){ scanSel=(scanSel+(d>0?1:-1)+scanN)%scanN; uiDirty=true; }
    if(eb&&scanN>0){ pendingSsid=scanSsid[scanSel]; beginTextIn("WiFi Password",true,1); }
    if(bb){ state=ST_HOME; uiDirty=true; }
  } else if(state==ST_TEXTIN){
    if(bb){ state=(tiPurpose==1?ST_WIFI:(tiPurpose==3||tiPurpose==4?ST_EMAIL:ST_MENU)); uiDirty=true; }
  } else if(state==ST_CALIB){
    if(d!=0){ dispY+=d; if(dispY<0)dispY=0; if(dispY>120)dispY=120; gfx->setOffsets(dispX,dispY); uiDirty=true; }
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
  if(WiFi.status()==WL_CONNECTED || !cfgSsid.length()) return;
  if(millis()-wifiTryMs<8000) return;
  int n=WiFi.scanNetworks(false,true); bool seen=false;
  for(int i=0;i<n;i++) if(WiFi.SSID(i)==cfgSsid){ seen=true; break; }
  WiFi.scanDelete();
  if(seen) wifiConnectTo(cfgSsid.c_str(),cfgPass.c_str());
}

// ───────────────────────── setup / loop ─────────────────────────
void setup(){
  Serial.begin(115200); delay(3000);
  Serial.println("\n== darksec-pager boot ==");
  cfgLoad(); ircNick=cfgNick;
  hwInit();
  drawSplash();
  pushMsg("*","darksec-pager online",false);
  state=ST_HOME; lastActivity=millis(); render();
  WiFi.mode(WIFI_STA); WiFi.persistent(false); WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm); WiFi.setAutoReconnect(true);
  wifiConnectTo(cfgSsid.c_str(),cfgPass.c_str());
  configTime(0,0,"pool.ntp.org");
}
void loop(){
  wifiEnsure();
  static bool wifiWas=false; bool up=WiFi.isConnected();
  if(up!=wifiWas){ wifiWas=up; uiDirty=true;
    if(up){ Serial.printf("[wifi] up %s\n",WiFi.localIP().toString().c_str()); pushMsg("*","wifi: "+WiFi.localIP().toString(),false); configTime(0,0,"pool.ntp.org"); } }
  if(up && state!=ST_WIFI){
    if(!irc.connected() && millis()-lastIrcAttempt>8000) ircConnect();
    if(irc.connected()){ pumpIrc(); ircKeepAlive(); }
  }
  pumpInput();
  // screensaver: enter after idle
  if(state!=ST_SAVER && state!=ST_WIFI && state!=ST_CALIB && saverSec>0 && millis()-lastActivity > (uint32_t)saverSec*1000){
    prevState=(state==ST_MENU||state==ST_TEXTIN)?ST_CHAT:state; state=ST_SAVER; saverEnter();
  }
  if(state==ST_SAVER){ static uint32_t last=0; if(millis()-last>140){ saverStep(); last=millis(); } }
  else if(uiDirty) render();
  delay(5);
}
