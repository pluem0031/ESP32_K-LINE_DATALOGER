# Yamaha Track Datalog System

## ไฟล์ในโปรเจกต์
- `yamaha_datalog.ino` — ESP32-C3 firmware
- `server.py` — PC track server
- `requirements.txt` — Python dependencies

## วิธีใช้งาน

### ESP32-C3 Setup
1. Arduino IDE → Tools → Partition Scheme → **Default 4MB with spiffs**
2. ติดตั้ง library:
   - TinyGPSPlus (Mikal Hart)
   - ArduinoJson (Benoit Blanchon)
3. แก้ไขในโค้ด:
   - `STA_SSID` / `STA_PASS` → ชื่อ hotspot มือถือ
   - `SERVER_IP` → IP ของ PC (ดูจาก ipconfig)
   - `SF_LAT` / `SF_LON` → GPS coordinate เส้น Start/Finish

### GPS Module (Neo-6M)
- VCC → 3.3V
- GND → GND
- TX  → GPIO3

### PC Server
```bash
pip install -r requirements.txt
python server.py
```
เปิด browser → http://localhost:5000

### การใช้งานสนาม
1. เชื่อม ESP32 กับรถ
2. เปิด hotspot มือถือ
3. รัน `server.py` บน PC
4. กดปุ่ม BOOT (GPIO9) บน ESP32 → เข้า **TRACK MODE** 🔴
5. เปิด browser บน PC/มือถือ → http://<IP PC>:5000
6. กดปุ่มอีกครั้ง → กลับ **NORMAL MODE**

## CSV Format (Universal)
ไฟล์ `log_xxx.csv` = Normal mode  
ไฟล์ `trk_xxx.csv` = Track mode

compatible กับ:
- ✅ RaceRender (Time ใน seconds + # RaceRender Data)
- ✅ Excel / Google Sheets
- ✅ Python pandas
- ✅ MoTeC / AiM (column names + units)
