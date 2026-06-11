/**
 * Yamaha MT-15 / R15 V3 — Universal Datalog + Track Mode
 * Hardware : ESP32-C3 SuperMini + L9637D + GPS (NEO-M8N)
 *
 * วงจร L9637D:
 *   Pin1 RX → GPIO21  |  Pin4 TX → GPIO20
 *   Pin3 VCC → 3.3V   |  Pin5 GND → GND
 *   Pin6 K  → Yamaha 3-pin Pin3  |  Pin7 VS → 12V
 *
 * GPS NEO-M8N:
 *   TX → GPIO3  |  RX → GPIO2  |  VCC → 3.3V  |  GND → GND
 *
 * Mode button: GPIO9 (BOOT) — กดค้าง 1 วิ toggle mode
 *
 * ค่าที่แก้ได้ผ่านเว็บ (192.168.4.1 → Settings):
 *   - WiFi SSID / Password (hotspot มือถือ)
 *   - Server URL (public IP)
 *   - S/F Lat / Lon / Radius
 *   บันทึกลง flash (Preferences) — ไม่ต้อง upload ใหม่
 *
 * Library: TinyGPSPlus, ArduinoJson
 * Partition: Default 4MB with spiffs
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <TinyGPSPlus.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ──────────────────────────────────────────────
//  HARDWARE CONFIG (ไม่ต้องแก้)
// ──────────────────────────────────────────────
#define KLINE_TX_PIN    20
#define KLINE_RX_PIN    21
#define KLINE_BAUD      10400
#define GPS_RX_PIN      3
#define GPS_TX_PIN      2
#define GPS_BAUD        9600
#define MODE_BTN_PIN    9

#define MAX_LOG_SIZE_KB 1024
#define MAX_LOG_FILES   99
#define HTTP_INTERVAL   200
#define LOG_INTERVAL_NORMAL 500
#define LOG_INTERVAL_TRACK  100

// AP (Normal mode) — คงที่
#define AP_SSID  "YamahaLog"
#define AP_PASS  "yamaha1234"

// ──────────────────────────────────────────────
//  RUNTIME CONFIG — โหลดจาก Preferences
// ──────────────────────────────────────────────
struct Config {
  // WiFi STA (Track mode)
  char staSSID[32]   = "MyPhone";
  char staPass[64]   = "phone1234";
  // Server
  char serverURL[128] = "http://x.x.x.x:5000/api/data";
  // Start/Finish
  double sfLat       = 0.0;
  double sfLon       = 0.0;
  float  sfRadius    = 15.0;
};
Config cfg;
Preferences prefs;

void loadConfig() {
  prefs.begin("ycfg", true);
  prefs.getString("staSSID",   cfg.staSSID,   sizeof(cfg.staSSID));
  prefs.getString("staPass",   cfg.staPass,   sizeof(cfg.staPass));
  prefs.getString("serverURL", cfg.serverURL, sizeof(cfg.serverURL));
  cfg.sfLat    = prefs.getDouble("sfLat",    0.0);
  cfg.sfLon    = prefs.getDouble("sfLon",    0.0);
  cfg.sfRadius = prefs.getFloat ("sfRadius", 15.0);
  prefs.end();
  Serial.println("[CFG] loaded");
  Serial.printf("  STA: %s\n", cfg.staSSID);
  Serial.printf("  URL: %s\n", cfg.serverURL);
  Serial.printf("  S/F: %.6f, %.6f  r=%.1fm\n", cfg.sfLat, cfg.sfLon, cfg.sfRadius);
}

void saveConfig() {
  prefs.begin("ycfg", false);
  prefs.putString("staSSID",   cfg.staSSID);
  prefs.putString("staPass",   cfg.staPass);
  prefs.putString("serverURL", cfg.serverURL);
  prefs.putDouble("sfLat",     cfg.sfLat);
  prefs.putDouble("sfLon",     cfg.sfLon);
  prefs.putFloat ("sfRadius",  cfg.sfRadius);
  prefs.end();
  Serial.println("[CFG] saved");
}

// ──────────────────────────────────────────────
//  PID TABLE
// ──────────────────────────────────────────────
#define PID_RPM       0x03
#define PID_SPEED     0x04
#define PID_TPS       0x1A
#define PID_ECT       0x05
#define PID_IAT       0x0F
#define PID_MAP       0x0B
#define PID_BATT      0x15
#define PID_INJ       0x18
#define PID_IGN       0x0E
#define PID_O2        0x13
#define PID_ISC       0x11
#define PID_GEAR      0x1C
#define PID_VVA       0x1D
#define PID_FUELTRIM  0x07
#define PID_LONGTRIM  0x08

// ──────────────────────────────────────────────
//  DATA STRUCTS
// ──────────────────────────────────────────────
struct BikeData {
  uint16_t rpm=0; uint8_t speed=0; uint8_t tps=0;
  int8_t ect=0; int8_t iat=0; uint8_t map_kpa=0;
  float batt=0; float inj_ms=0; int8_t ign_deg=0;
  float o2_v=0; uint8_t isc=0; uint8_t gear=0;
  bool vva=false; int8_t stFuel=0; int8_t ltFuel=0;
  uint8_t dtcCount=0; char dtcList[64]="";
  double lat=0; double lon=0; float altitude=0;
  float gpsSpeed=0; float heading=0;
  uint8_t satellites=0; float hdop=99; bool gpsValid=false;
};
BikeData bike;

struct LapData {
  uint8_t lapNum=0; uint32_t lapStart=0;
  uint32_t lastLapMs=0; uint32_t bestLapMs=0;
  bool inSFZone=false; float maxRPM=0; float maxSpeed=0;
};
LapData lap;

// ──────────────────────────────────────────────
//  GLOBALS
// ──────────────────────────────────────────────
HardwareSerial KLine(1);
HardwareSerial GPSSerial(0);
TinyGPSPlus    gps;
WebServer      server(80);

enum Mode { MODE_NORMAL, MODE_TRACK };
Mode currentMode = MODE_NORMAL;

File     logFile;
String   logFilename="";
uint32_t logStartTime=0, lastLogTime=0, logRowCount=0;
bool     loggingActive=false;
uint32_t httpOK=0, httpFail=0;

// ──────────────────────────────────────────────
//  KWP2000
// ──────────────────────────────────────────────
uint8_t calcChecksum(uint8_t* b, uint8_t len) {
  uint8_t s=0; for(uint8_t i=0;i<len;i++) s+=b[i]; return (~s)+1;
}
void klineSendByte(uint8_t b) {
  KLine.write(b); KLine.flush();
  unsigned long t=millis();
  while(!KLine.available()&&millis()-t<5);
  if(KLine.available()) KLine.read();
}
bool klineRequest(uint8_t pid, uint8_t* resp, uint8_t* rlen) {
  while(KLine.available()) KLine.read();
  uint8_t req[]={0xC2,0x10,0xF1,0x03,0x21,pid,0x00};
  req[6]=calcChecksum(req,6);
  for(uint8_t i=0;i<7;i++) klineSendByte(req[i]);
  unsigned long t=millis(); *rlen=0;
  while(millis()-t<200){
    if(KLine.available()){ resp[(*rlen)++]=KLine.read(); if(*rlen>=32) break; t=millis(); }
  }
  if(*rlen<5) return false;
  if(calcChecksum(resp,*rlen-1)!=resp[*rlen-1]) return false;
  if(resp[4]!=0x61) return false;
  return true;
}
void klineFastInit() {
  pinMode(KLINE_TX_PIN,OUTPUT);
  digitalWrite(KLINE_TX_PIN,HIGH); delay(300);
  digitalWrite(KLINE_TX_PIN,LOW);  delay(25);
  digitalWrite(KLINE_TX_PIN,HIGH); delay(25);
  KLine.begin(KLINE_BAUD,SERIAL_8N1,KLINE_RX_PIN,KLINE_TX_PIN);
  uint8_t req[]={0xC1,0x10,0xF1,0x02,0x10,0x81,0x00};
  req[6]=calcChecksum(req,6);
  for(uint8_t i=0;i<7;i++) klineSendByte(req[i]);
  delay(100); while(KLine.available()) KLine.read();
  Serial.println("[KLINE] init ok");
}
void sendTesterPresent() {
  uint8_t req[]={0xC1,0x10,0xF1,0x01,0x3E,0x00};
  req[5]=calcChecksum(req,5);
  for(uint8_t i=0;i<6;i++) klineSendByte(req[i]);
  delay(50); while(KLine.available()) KLine.read();
}
void readAllSensors() {
  uint8_t buf[32]; uint8_t len;
  #define RD(pid) klineRequest(pid,buf,&len)
  if(RD(PID_RPM)&&len>=7)      bike.rpm=(uint16_t)buf[5]<<8|buf[6];
  if(RD(PID_SPEED)&&len>=6)    bike.speed=buf[5];
  if(RD(PID_TPS)&&len>=6)      bike.tps=(uint8_t)((buf[5]*100UL)/255);
  if(RD(PID_ECT)&&len>=6)      bike.ect=(int8_t)(buf[5]-40);
  if(RD(PID_IAT)&&len>=6)      bike.iat=(int8_t)(buf[5]-40);
  if(RD(PID_MAP)&&len>=6)      bike.map_kpa=(uint8_t)(buf[5]*0.5f);
  if(RD(PID_BATT)&&len>=6)     bike.batt=buf[5]*0.1f;
  if(RD(PID_INJ)&&len>=6)      bike.inj_ms=buf[5]*0.008f;
  if(RD(PID_IGN)&&len>=6)      bike.ign_deg=(int8_t)((buf[5]-64)/2);
  if(RD(PID_O2)&&len>=6)       bike.o2_v=buf[5]*0.00488f;
  if(RD(PID_ISC)&&len>=6)      bike.isc=buf[5];
  if(RD(PID_GEAR)&&len>=6)     bike.gear=buf[5];
  if(RD(PID_VVA)&&len>=6)      bike.vva=buf[5]!=0;
  if(RD(PID_FUELTRIM)&&len>=6) bike.stFuel=(int8_t)(buf[5]-128);
  if(RD(PID_LONGTRIM)&&len>=6) bike.ltFuel=(int8_t)(buf[5]-128);
  if(currentMode==MODE_TRACK){
    if(bike.rpm>lap.maxRPM)   lap.maxRPM=bike.rpm;
    if(bike.speed>lap.maxSpeed) lap.maxSpeed=bike.speed;
  }
}

// ──────────────────────────────────────────────
//  GPS + LAP
// ──────────────────────────────────────────────
void readGPS() {
  while(GPSSerial.available()) gps.encode(GPSSerial.read());
  if(gps.location.isValid()){ bike.lat=gps.location.lat(); bike.lon=gps.location.lng(); bike.gpsValid=true; }
  if(gps.altitude.isValid())   bike.altitude=gps.altitude.meters();
  if(gps.speed.isValid())      bike.gpsSpeed=gps.speed.kmph();
  if(gps.course.isValid())     bike.heading=gps.course.deg();
  if(gps.satellites.isValid()) bike.satellites=gps.satellites.value();
  if(gps.hdop.isValid())       bike.hdop=gps.hdop.hdop();
}
double gpsDistance(double lat1,double lon1,double lat2,double lon2){
  const double R=6371000;
  double dLat=radians(lat2-lat1),dLon=radians(lon2-lon1);
  double a=sin(dLat/2)*sin(dLat/2)+cos(radians(lat1))*cos(radians(lat2))*sin(dLon/2)*sin(dLon/2);
  return R*2*atan2(sqrt(a),sqrt(1-a));
}
uint32_t currentLapMs(){ return lap.lapNum==0?0:millis()-lap.lapStart; }
void checkLapTrigger(){
  if(!bike.gpsValid||currentMode!=MODE_TRACK||cfg.sfLat==0.0) return;
  bool inZone=gpsDistance(bike.lat,bike.lon,cfg.sfLat,cfg.sfLon)<cfg.sfRadius;
  if(inZone&&!lap.inSFZone){
    uint32_t now=millis();
    if(lap.lapNum>0){
      lap.lastLapMs=now-lap.lapStart;
      if(lap.bestLapMs==0||lap.lastLapMs<lap.bestLapMs) lap.bestLapMs=lap.lastLapMs;
      Serial.printf("[LAP] #%d %.3fs best=%.3fs\n",lap.lapNum,lap.lastLapMs/1000.0f,lap.bestLapMs/1000.0f);
      lap.maxRPM=0; lap.maxSpeed=0;
    }
    lap.lapStart=now; lap.lapNum++;
  }
  lap.inSFZone=inZone;
}

// ──────────────────────────────────────────────
//  LITTLEFS
// ──────────────────────────────────────────────
void fsInit(){
  if(!LittleFS.begin(true)){Serial.println("[FS] FAILED");return;}
  size_t t=LittleFS.totalBytes(),u=LittleFS.usedBytes();
  Serial.printf("[FS] free=%dKB/%dKB\n",(t-u)/1024,t/1024);
}
int findNextIdx(){
  int mx=0; File root=LittleFS.open("/"); File f=root.openNextFile();
  while(f){ String n=f.name();
    if(n.endsWith(".csv")) mx=max(mx,n.substring(4,7).toInt());
    f=root.openNextFile(); }
  return mx+1;
}
bool logOpen(bool trackMode=false){
  int idx=findNextIdx(); if(idx>MAX_LOG_FILES) return false;
  char fname[24]; snprintf(fname,sizeof(fname),trackMode?"/trk_%03d.csv":"/log_%03d.csv",idx);
  logFilename=fname;
  logFile=LittleFS.open(logFilename,"w"); if(!logFile) return false;
  logFile.println("# RaceRender Data");
  logFile.printf("# Yamaha MT-15/R15V3  Mode:%s\n",trackMode?"TRACK":"NORMAL");
  logFile.println(
    "Time,RPM,KPH,Throttle,Gear,"
    "Latitude,Longitude,Altitude,Heading,GPS_Speed_KPH,GPS_Update,"
    "ECT_C,IAT_C,MAP_kPa,Batt_V,Inj_ms,IGN_deg,O2_V,ISC_steps,VVA,"
    "STfuel_pct,LTfuel_pct,Lap,LapTime_s,LastLap_s,BestLap_s,DTC_count"
  );
  logFile.flush();
  logStartTime=millis(); logRowCount=0; loggingActive=true;
  Serial.printf("[LOG] %s\n",fname); return true;
}
void logClose(){
  if(logFile){logFile.flush();logFile.close();}
  loggingActive=false;
  Serial.printf("[LOG] closed %s (%lu rows)\n",logFilename.c_str(),logRowCount);
}
void logWrite(){
  if(!loggingActive||!logFile) return;
  if(logFile.size()>=(size_t)MAX_LOG_SIZE_KB*1024){logClose();return;}
  char row[320];
  snprintf(row,sizeof(row),
    "%.3f,%d,%d,%d,%d,%.7f,%.7f,%.1f,%.1f,%.1f,%d,"
    "%d,%d,%d,%.1f,%.3f,%d,%.3f,%d,%d,%d,%d,"
    "%d,%.3f,%.3f,%.3f,%d",
    (millis()-logStartTime)/1000.0f,
    bike.rpm,bike.speed,bike.tps,bike.gear,
    bike.lat,bike.lon,bike.altitude,bike.heading,bike.gpsSpeed,gps.location.isUpdated()?1:0,
    bike.ect,bike.iat,bike.map_kpa,bike.batt,bike.inj_ms,bike.ign_deg,
    bike.o2_v,bike.isc,bike.vva?1:0,bike.stFuel,bike.ltFuel,
    lap.lapNum,currentLapMs()/1000.0f,lap.lastLapMs/1000.0f,lap.bestLapMs/1000.0f,
    bike.dtcCount
  );
  logFile.println(row); logFile.flush(); logRowCount++;
}

// ──────────────────────────────────────────────
//  HTTP POST
// ──────────────────────────────────────────────
void httpPost(){
  if(WiFi.status()!=WL_CONNECTED) return;
  StaticJsonDocument<512> doc;
  doc["t"]=(millis()-logStartTime)/1000.0f;
  doc["rpm"]=bike.rpm; doc["spd"]=bike.speed; doc["tps"]=bike.tps;
  doc["gear"]=bike.gear; doc["ect"]=bike.ect; doc["iat"]=bike.iat;
  doc["map"]=bike.map_kpa;
  doc["batt"]=serialized(String(bike.batt,1));
  doc["inj"]=serialized(String(bike.inj_ms,3));
  doc["ign"]=bike.ign_deg;
  doc["o2"]=serialized(String(bike.o2_v,3));
  doc["isc"]=bike.isc; doc["vva"]=bike.vva;
  doc["stf"]=bike.stFuel; doc["ltf"]=bike.ltFuel;
  doc["lat"]=serialized(String(bike.lat,7));
  doc["lon"]=serialized(String(bike.lon,7));
  doc["gspd"]=serialized(String(bike.gpsSpeed,1));
  doc["hdg"]=serialized(String(bike.heading,1));
  doc["sat"]=bike.satellites; doc["hdop"]=serialized(String(bike.hdop,1));
  doc["lap"]=lap.lapNum; doc["lpt"]=currentLapMs()/1000.0f;
  doc["llap"]=lap.lastLapMs/1000.0f; doc["blap"]=lap.bestLapMs/1000.0f;
  doc["mrpm"]=lap.maxRPM; doc["mspd"]=lap.maxSpeed;
  doc["dtc"]=bike.dtcCount; doc["mode"]="TRACK";
  char buf[512]; serializeJson(doc,buf,sizeof(buf));
  HTTPClient http; http.begin(cfg.serverURL);
  http.addHeader("Content-Type","application/json");
  http.setTimeout(2000);
  int code=http.POST(buf);
  if(code==204||code==200) httpOK++; else httpFail++;
  http.end();
}

// ──────────────────────────────────────────────
//  WIFI
// ──────────────────────────────────────────────
void startAP(){
  WiFi.disconnect(true); delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID,AP_PASS);
  Serial.printf("[WiFi] AP → http://192.168.4.1\n");
}
bool startSTA(){
  WiFi.disconnect(true); delay(100);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID,AP_PASS);   // AP ยังเปิดอยู่แม้ใน track mode
  WiFi.begin(cfg.staSSID,cfg.staPass);
  Serial.printf("[WiFi] connecting %s",cfg.staSSID);
  uint8_t tries=0;
  while(WiFi.status()!=WL_CONNECTED&&tries++<20){delay(500);Serial.print(".");}
  if(WiFi.status()==WL_CONNECTED){
    Serial.printf("\n[WiFi] STA IP: %s\n",WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("\n[WiFi] STA failed");
  return false;
}

// ──────────────────────────────────────────────
//  WEB HANDLERS
// ──────────────────────────────────────────────
void handleData(){
  char j[640];
  snprintf(j,sizeof(j),
    "{\"rpm\":%d,\"speed\":%d,\"tps\":%d,\"ect\":%d,\"iat\":%d,"
    "\"map\":%d,\"batt\":%.1f,\"inj\":%.3f,\"ign\":%d,\"o2\":%.3f,"
    "\"isc\":%d,\"gear\":%d,\"vva\":%d,\"stf\":%d,\"ltf\":%d,"
    "\"lat\":%.7f,\"lon\":%.7f,\"gspd\":%.1f,\"sat\":%d,\"hdop\":%.1f,"
    "\"dtc\":%d,\"dtcList\":\"%s\","
    "\"logging\":%s,\"file\":\"%s\",\"rows\":%lu,\"mode\":\"%s\","
    "\"sfLat\":%.6f,\"sfLon\":%.6f}",
    bike.rpm,bike.speed,bike.tps,bike.ect,bike.iat,
    bike.map_kpa,bike.batt,bike.inj_ms,bike.ign_deg,bike.o2_v,
    bike.isc,bike.gear,bike.vva?1:0,bike.stFuel,bike.ltFuel,
    bike.lat,bike.lon,bike.gpsSpeed,bike.satellites,bike.hdop,
    bike.dtcCount,bike.dtcList,
    loggingActive?"true":"false",logFilename.c_str(),logRowCount,
    currentMode==MODE_TRACK?"TRACK":"NORMAL",
    cfg.sfLat,cfg.sfLon
  );
  server.send(200,"application/json",j);
}

// GET /api/config — ดูค่าปัจจุบัน
void handleGetConfig(){
  char j[512];
  snprintf(j,sizeof(j),
    "{\"staSSID\":\"%s\",\"serverURL\":\"%s\","
    "\"sfLat\":%.6f,\"sfLon\":%.6f,\"sfRadius\":%.1f}",
    cfg.staSSID, cfg.serverURL,
    cfg.sfLat, cfg.sfLon, cfg.sfRadius
  );
  server.send(200,"application/json",j);
}

// POST /api/config — บันทึกค่าใหม่
void handlePostConfig(){
  if(!server.hasArg("plain")){server.send(400);return;}
  StaticJsonDocument<512> doc;
  DeserializationError err=deserializeJson(doc,server.arg("plain"));
  if(err){server.send(400,"text/plain","bad json");return;}

  if(doc.containsKey("staSSID"))   strlcpy(cfg.staSSID,   doc["staSSID"],   sizeof(cfg.staSSID));
  if(doc.containsKey("staPass"))   strlcpy(cfg.staPass,   doc["staPass"],   sizeof(cfg.staPass));
  if(doc.containsKey("serverURL")) strlcpy(cfg.serverURL, doc["serverURL"], sizeof(cfg.serverURL));
  if(doc.containsKey("sfLat"))     cfg.sfLat    = doc["sfLat"].as<double>();
  if(doc.containsKey("sfLon"))     cfg.sfLon    = doc["sfLon"].as<double>();
  if(doc.containsKey("sfRadius"))  cfg.sfRadius = doc["sfRadius"].as<float>();

  saveConfig();
  server.send(200,"application/json","{\"ok\":true}");
  Serial.println("[CFG] updated via web");
}

// POST /api/setlap/here — ใช้ตำแหน่ง GPS ปัจจุบันเป็น S/F
void handleSetLapHere(){
  if(!bike.gpsValid){
    server.send(400,"application/json","{\"error\":\"no GPS fix\"}");
    return;
  }
  cfg.sfLat = bike.lat;
  cfg.sfLon = bike.lon;
  saveConfig();
  char j[128];
  snprintf(j,sizeof(j),"{\"ok\":true,\"lat\":%.6f,\"lon\":%.6f}",cfg.sfLat,cfg.sfLon);
  server.send(200,"application/json",j);
  Serial.printf("[LAP] S/F set here: %.6f, %.6f\n",cfg.sfLat,cfg.sfLon);
}

void handleDownload(){
  if(!server.hasArg("f")){server.send(400);return;}
  String fn="/"+server.arg("f");
  File f=LittleFS.open(fn,"r"); if(!f){server.send(404);return;}
  server.sendHeader("Content-Disposition","attachment; filename=\""+server.arg("f")+"\"");
  server.streamFile(f,"text/csv"); f.close();
}
void handleDelete(){
  if(!server.hasArg("f")){server.send(400);return;}
  String fn="/"+server.arg("f");
  if(loggingActive&&logFilename==fn) logClose();
  LittleFS.remove(fn);
  server.sendHeader("Location","/"); server.send(303);
}
void handleLogStart(){if(!loggingActive) logOpen(false); server.sendHeader("Location","/"); server.send(303);}
void handleLogStop() {if(loggingActive) logClose();      server.sendHeader("Location","/"); server.send(303);}

void handleRoot(){
  String fileRows="";
  File root=LittleFS.open("/"); File f=root.openNextFile();
  while(f){ String n=f.name();
    if(n.endsWith(".csv")){
      size_t sz=f.size();
      fileRows+="<tr><td>"+n+"</td><td>"+String(sz/1024)+"."+String((sz%1024)*10/1024)+" KB</td>";
      fileRows+="<td><a href='/dl?f="+n+"' download class='b'>⬇</a> ";
      fileRows+="<a href='/rm?f="+n+"' class='b bd' onclick=\"return confirm('ลบ?')\">🗑</a></td></tr>";
    }
    f=root.openNextFile();
  }
  size_t ft=LittleFS.totalBytes(),fu=LittleFS.usedBytes();

  String html=R"(<!DOCTYPE html><html lang='th'>
<head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Yamaha Log</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:sans-serif;background:#0d0d0d;color:#eee;padding:12px}
h1{color:#f60;font-size:1.3em;margin-bottom:6px}
.badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:.75em;font-weight:700;margin-left:8px}
.nm{background:#2a5;color:#fff}.tm{background:#e33;color:#fff}
h2{color:#777;font-size:.82em;margin:12px 0 5px;text-transform:uppercase}
.g3{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-bottom:8px}
.c{background:#181818;border-radius:8px;padding:10px;text-align:center;border:1px solid #222}
.c .v{font-size:1.8em;font-weight:700;color:#f60;line-height:1}
.c .u{font-size:.68em;color:#666;margin-top:2px}
.c.hi .v{color:#3f6}.c.warn .v{color:#f33}.c.blue .v{color:#3af}
.st{background:#181818;border:1px solid #222;border-radius:6px;padding:8px 12px;font-size:.8em;color:#8f8;margin-bottom:8px}
.gps{background:#181818;border:1px solid #222;border-radius:6px;padding:8px 12px;font-size:.76em;color:#8af;margin-bottom:8px;line-height:1.9}
.ct{display:flex;gap:6px;margin-bottom:10px;flex-wrap:wrap}
.ct a{flex:1;min-width:80px;text-align:center;padding:9px;border-radius:7px;background:#222;color:#eee;text-decoration:none;font-size:.82em}
.ct a:active{background:#f60;color:#000}
/* ── SETTINGS PANEL ── */
.panel{background:#181818;border:1px solid #333;border-radius:10px;padding:14px;margin-bottom:10px;display:none}
.panel.open{display:block}
.panel h3{color:#f60;font-size:.9em;margin-bottom:10px}
.row{display:flex;flex-direction:column;margin-bottom:8px}
.row label{font-size:.72em;color:#888;margin-bottom:3px}
.row input{background:#111;border:1px solid #333;color:#eee;padding:7px 10px;
           border-radius:6px;font-size:.85em;outline:none}
.row input:focus{border-color:#f60}
.sfrow{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.btn-save{width:100%;padding:10px;background:#f60;color:#000;border:none;
          border-radius:7px;font-size:.9em;font-weight:700;cursor:pointer;margin-top:4px}
.btn-save:active{background:#c50}
.btn-here{width:100%;padding:8px;background:#1a3a1a;color:#3f6;border:1px solid #3f6;
          border-radius:7px;font-size:.82em;cursor:pointer;margin-bottom:8px}
.btn-here:active{background:#3f6;color:#000}
.sf-status{font-size:.72em;color:#8af;margin-top:4px;text-align:center}
.toggle{width:100%;padding:9px;background:#222;color:#eee;border:none;border-radius:7px;
        font-size:.82em;cursor:pointer;margin-bottom:6px;text-align:left}
.toggle:active{background:#333}
/* ── TABLE ── */
table{width:100%;border-collapse:collapse;font-size:.78em}
th{background:#1e1e1e;padding:7px;text-align:left;color:#888}
td{padding:7px;border-bottom:1px solid #1a1a1a}
.b{display:inline-block;padding:3px 9px;border-radius:5px;background:#f60;color:#fff;text-decoration:none;font-size:.75em}
.bd{background:#522;margin-left:4px}
.bar{background:#1e1e1e;border-radius:4px;height:5px;margin-top:5px;overflow:hidden}
.fill{height:100%;background:#f60;border-radius:4px}
.sm{font-size:.7em;color:#555;margin-top:4px}
.dtbox{background:#200;border:1px solid #633;border-radius:6px;padding:8px;font-size:.78em;color:#f77;margin-bottom:8px;display:none}
.saved{color:#3f6;font-size:.8em;text-align:center;margin-top:6px;display:none}
</style>
<script>
function f1(v){return parseFloat(v).toFixed(1)}
function f3(v){return parseFloat(v).toFixed(3)}
function sv(id,v){var e=document.getElementById(id);if(e)e.textContent=v}

// ── LIVE DATA ──
function refresh(){
  fetch('/data').then(r=>r.json()).then(d=>{
    sv('rpm',d.rpm);sv('spd',d.speed);sv('tps',d.tps);
    sv('gear',d.gear===0?'N':d.gear);sv('ign',d.ign);sv('vva',d.vva?'HIGH':'LOW');
    sv('ect',d.ect);sv('iat',d.iat);sv('map',d.map);
    sv('inj',f3(d.inj));sv('batt',f1(d.batt));sv('o2',f3(d.o2));
    sv('isc',d.isc);sv('stf',(d.stf>0?'+':'')+d.stf);sv('ltf',(d.ltf>0?'+':'')+d.ltf);
    sv('gpos',d.lat.toFixed(5)+', '+d.lon.toFixed(5));
    sv('gspd',f1(d.gspd)+' km/h');sv('gsat',d.sat+' sat');sv('ghdop','HDOP '+f1(d.hdop));
    sv('lst',d.logging?'🔴 '+d.file+' ('+d.rows+' rows)':'⏸ หยุดบันทึก');
    sv('mbadge',d.mode);
    document.getElementById('mbadge').className='badge '+(d.mode==='TRACK'?'tm':'nm');
    document.getElementById('bcard').className='c'+(d.batt<11.5?' warn':'');
    var dtb=document.getElementById('dtbox');
    if(d.dtc>0){dtb.style.display='block';sv('dtcinfo','⚠ DTC: '+d.dtcList);}
    else dtb.style.display='none';
    // อัปเดต SF status
    if(d.sfLat!==0){
      sv('sfst','📍 S/F: '+d.sfLat.toFixed(5)+', '+d.sfLon.toFixed(5));
    } else {
      sv('sfst','⚠ ยังไม่ได้ตั้ง S/F');
    }
  });
}
setInterval(refresh,500);

// ── SETTINGS ──
function toggleSettings(){
  var p=document.getElementById('settingsPanel');
  p.classList.toggle('open');
  if(p.classList.contains('open')) loadConfig();
}

function loadConfig(){
  fetch('/api/config').then(r=>r.json()).then(d=>{
    document.getElementById('cfgSSID').value=d.staSSID||'';
    document.getElementById('cfgURL').value=d.serverURL||'';
    document.getElementById('cfgLat').value=d.sfLat||'';
    document.getElementById('cfgLon').value=d.sfLon||'';
    document.getElementById('cfgRadius').value=d.sfRadius||'15';
  });
}

function saveSettings(){
  var body={
    staSSID:   document.getElementById('cfgSSID').value,
    staPass:   document.getElementById('cfgPass').value||undefined,
    serverURL: document.getElementById('cfgURL').value,
    sfLat:     parseFloat(document.getElementById('cfgLat').value)||0,
    sfLon:     parseFloat(document.getElementById('cfgLon').value)||0,
    sfRadius:  parseFloat(document.getElementById('cfgRadius').value)||15,
  };
  // ถ้า pass ว่าง ไม่ส่งไป (เก็บของเดิม)
  if(!body.staPass) delete body.staPass;
  fetch('/api/config',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify(body)
  }).then(r=>r.json()).then(d=>{
    if(d.ok){
      var s=document.getElementById('savedMsg');
      s.style.display='block';
      setTimeout(()=>s.style.display='none',2000);
    }
  });
}

function setHere(){
  fetch('/api/setlap/here',{method:'POST'}).then(r=>r.json()).then(d=>{
    if(d.ok){
      document.getElementById('cfgLat').value=d.lat.toFixed(6);
      document.getElementById('cfgLon').value=d.lon.toFixed(6);
      sv('sfst','✅ S/F ตั้งที่นี่แล้ว: '+d.lat.toFixed(5)+', '+d.lon.toFixed(5));
    } else {
      alert('ยังไม่มี GPS fix');
    }
  });
}
</script>
</head><body>
<h1>🏍 Yamaha Datalog <span class='badge nm' id='mbadge'>NORMAL</span></h1>

<h2>⚡ Engine</h2>
<div class='g3'>
  <div class='c'><div class='v' id='rpm'>-</div><div class='u'>RPM</div></div>
  <div class='c'><div class='v' id='spd'>-</div><div class='u'>km/h</div></div>
  <div class='c hi'><div class='v' id='gear'>-</div><div class='u'>Gear</div></div>
  <div class='c'><div class='v' id='tps'>-</div><div class='u'>TPS %</div></div>
  <div class='c'><div class='v' id='ign'>-</div><div class='u'>°BTDC</div></div>
  <div class='c'><div class='v' id='vva'>-</div><div class='u'>VVA</div></div>
</div>
<h2>🌡 Temps / Pressure</h2>
<div class='g3'>
  <div class='c'><div class='v' id='ect'>-</div><div class='u'>°C ECT</div></div>
  <div class='c'><div class='v' id='iat'>-</div><div class='u'>°C IAT</div></div>
  <div class='c'><div class='v' id='map'>-</div><div class='u'>kPa MAP</div></div>
</div>
<h2>⛽ Fuel / Electrical</h2>
<div class='g3'>
  <div class='c'><div class='v' id='inj'>-</div><div class='u'>ms Inject</div></div>
  <div class='c' id='bcard'><div class='v' id='batt'>-</div><div class='u'>V Batt</div></div>
  <div class='c blue'><div class='v' id='o2'>-</div><div class='u'>V O2</div></div>
  <div class='c'><div class='v' id='isc'>-</div><div class='u'>ISC</div></div>
  <div class='c'><div class='v' id='stf'>-</div><div class='u'>ST Fuel%</div></div>
  <div class='c'><div class='v' id='ltf'>-</div><div class='u'>LT Fuel%</div></div>
</div>
<h2>🛰 GPS</h2>
<div class='gps'>
  <span id='gpos'>-.-----, -.-----</span> &nbsp;|&nbsp;
  <span id='gspd'>- km/h</span> &nbsp;|&nbsp;
  <span id='gsat'>- sat</span> &nbsp;|&nbsp;
  <span id='ghdop'>HDOP -</span>
</div>
<div class='st' id='lst'>⏸ หยุดบันทึก</div>
<div class='dtbox' id='dtbox'><span id='dtcinfo'></span></div>

<div class='ct'>
  <a href='/log/start'>▶ Start</a>
  <a href='/log/stop'>⏹ Stop</a>
</div>

<!-- ── SETTINGS BUTTON ── -->
<button class='toggle' onclick='toggleSettings()'>⚙️ Settings (WiFi / Server / S/F Line)</button>

<!-- ── SETTINGS PANEL ── -->
<div class='panel' id='settingsPanel'>
  <h3>⚙️ ตั้งค่า</h3>

  <h2 style='margin-bottom:8px'>📶 WiFi Hotspot (Track mode)</h2>
  <div class='row'><label>SSID</label><input id='cfgSSID' type='text' placeholder='ชื่อ hotspot'></div>
  <div class='row'><label>Password (เว้นว่างถ้าไม่เปลี่ยน)</label><input id='cfgPass' type='password' placeholder='••••••••'></div>

  <h2 style='margin:10px 0 8px'>🌐 Server URL</h2>
  <div class='row'><label>URL</label><input id='cfgURL' type='text' placeholder='http://x.x.x.x:5005/api/data'></div>

  <h2 style='margin:10px 0 8px'>🏁 Start/Finish Line</h2>
  <button class='btn-here' onclick='setHere()'>📍 ใช้ตำแหน่ง GPS ปัจจุบันเป็น S/F</button>
  <div class='sfrow'>
    <div class='row'><label>Latitude</label><input id='cfgLat' type='number' step='0.000001' placeholder='12.682200'></div>
    <div class='row'><label>Longitude</label><input id='cfgLon' type='number' step='0.000001' placeholder='101.045600'></div>
  </div>
  <div class='row'><label>Radius (เมตร)</label><input id='cfgRadius' type='number' step='1' placeholder='15'></div>
  <div class='sf-status' id='sfst'>-</div>

  <button class='btn-save' onclick='saveSettings()'>💾 บันทึก</button>
  <div class='saved' id='savedMsg'>✅ บันทึกแล้ว</div>
</div>

<h2>📁 ไฟล์</h2>
<table><tr><th>ชื่อ</th><th>ขนาด</th><th></th></tr>
)"+fileRows+R"(</table>
<div class='sm'>พื้นที่: )"+String(fu/1024)+"KB / "+String(ft/1024)+R"(KB</div>
<div class='bar'><div class='fill' style='width:)"+String(fu*100/ft)+R"(%'></div></div>
</body></html>)";
  server.send(200,"text/html",html);
}

void webServerInit(){
  server.on("/",               handleRoot);
  server.on("/data",           handleData);
  server.on("/api/config",HTTP_GET,  handleGetConfig);
  server.on("/api/config",HTTP_POST, handlePostConfig);
  server.on("/api/setlap/here",HTTP_POST, handleSetLapHere);
  server.on("/dl",             handleDownload);
  server.on("/rm",             handleDelete);
  server.on("/log/start",      handleLogStart);
  server.on("/log/stop",       handleLogStop);
  server.begin();
}

// ──────────────────────────────────────────────
//  MODE SWITCH
// ──────────────────────────────────────────────
void switchMode(Mode newMode){
  if(loggingActive) logClose();
  currentMode=newMode;
  if(newMode==MODE_TRACK){
    Serial.println("[MODE] → TRACK");
    lap=LapData(); httpOK=0; httpFail=0;
    startSTA();
    logOpen(true);
  } else {
    Serial.println("[MODE] → NORMAL");
    startAP();
    webServerInit();
    logOpen(false);
  }
}

// ──────────────────────────────────────────────
//  SETUP
// ──────────────────────────────────────────────
void setup(){
  Serial.begin(115200); delay(500);
  Serial.println("=== Yamaha MT-15/R15V3 Universal Datalog ===");
  pinMode(MODE_BTN_PIN,INPUT_PULLUP);
  loadConfig();
  fsInit();

  // GPS NEO-M8N
  GPSSerial.begin(GPS_BAUD,SERIAL_8N1,GPS_RX_PIN,GPS_TX_PIN);
  delay(100);
  const uint8_t ubxRate10Hz[]={0xB5,0x62,0x06,0x08,0x06,0x00,0x64,0x00,0x01,0x00,0x01,0x00,0x7A,0x12};
  GPSSerial.write(ubxRate10Hz,sizeof(ubxRate10Hz)); delay(100);
  const uint8_t disableGSV[]={0xB5,0x62,0x06,0x01,0x08,0x00,0xF0,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x38};
  GPSSerial.write(disableGSV,sizeof(disableGSV)); delay(100);
  Serial.println("[GPS] NEO-M8N init 10Hz");

  // K-Line
  KLine.begin(KLINE_BAUD,SERIAL_8N1,KLINE_RX_PIN,KLINE_TX_PIN);
  delay(100); klineFastInit(); delay(200);

  switchMode(MODE_NORMAL);
}

// ──────────────────────────────────────────────
//  LOOP
// ──────────────────────────────────────────────
unsigned long lastPresent=0,lastRead=0,lastHTTP=0,lastLog=0,lastPrint=0;
unsigned long btnDown=0; bool lastBtn=HIGH;

void loop(){
  bool btn=digitalRead(MODE_BTN_PIN);
  if(btn==LOW&&lastBtn==HIGH) btnDown=millis();
  if(btn==HIGH&&lastBtn==LOW&&millis()-btnDown>1000)
    switchMode(currentMode==MODE_NORMAL?MODE_TRACK:MODE_NORMAL);
  lastBtn=btn;

  server.handleClient();
  readGPS();
  checkLapTrigger();

  unsigned long now=millis();
  if(now-lastPresent>1500){sendTesterPresent();lastPresent=now;}

  uint16_t readInt=currentMode==MODE_TRACK?100:200;
  if(now-lastRead>readInt){readAllSensors();lastRead=now;}

  uint16_t logInt=currentMode==MODE_TRACK?LOG_INTERVAL_TRACK:LOG_INTERVAL_NORMAL;
  if(loggingActive&&now-lastLog>=logInt){logWrite();lastLog=now;}

  if(currentMode==MODE_TRACK&&now-lastHTTP>=HTTP_INTERVAL){httpPost();lastHTTP=now;}

  if(now-lastPrint>2000){
    Serial.printf("[%s] RPM:%d SPD:%d GEAR:%d LAP:%d LT:%.2fs GPS:%s HTTP:%lu/%lu\n",
      currentMode==MODE_TRACK?"TRACK":"NORMAL",
      bike.rpm,bike.speed,bike.gear,
      lap.lapNum,currentLapMs()/1000.0f,
      bike.gpsValid?"OK":"no fix",httpOK,httpFail);
    lastPrint=now;
  }
}
