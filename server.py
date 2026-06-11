#!/usr/bin/env python3
"""
Yamaha Track Server — รับข้อมูลผ่าน HTTP POST จาก ESP32-C3
รันบน PC ที่มี public IP + port forward

ติดตั้ง:
  pip install -r requirements.txt

รัน:
  python server.py

Port forward ที่ router:
  External 5000 → PC port 5000 (TCP)

เปิด browser:
  http://localhost:5000        (บน PC)
  http://<public-IP>:5000      (จากนอก)
"""

import json, threading, time
from collections import deque
from datetime import datetime
from flask import Flask, render_template_string, jsonify, request
from flask_socketio import SocketIO

# ──────────────────────────────────────────────
#  CONFIG
# ──────────────────────────────────────────────
PORT = 5000   # ใช้ port เดียว — ทั้ง web dashboard และ POST /api/data
MAX_HISTORY = 300
MAX_LAPS    = 50

app = Flask(__name__)
app.config['SECRET_KEY'] = 'yamahatrack'
sio = SocketIO(app, cors_allowed_origins="*", async_mode='threading')

# ──────────────────────────────────────────────
#  STATE
# ──────────────────────────────────────────────
latest    = {}
history   = {k: deque(maxlen=MAX_HISTORY) for k in
             ['t','rpm','spd','tps','ect','iat','map','batt','inj','ign','o2','stf','ltf']}
laps      = []
last_lap  = 0
last_rx   = 0
connected = False

# ──────────────────────────────────────────────
#  PROCESS INCOMING DATA
# ──────────────────────────────────────────────
def process(d):
    global latest, last_lap, connected, last_rx
    latest  = d
    last_rx = time.time()
    connected = True

    for k in history:
        if k in d:
            history[k].append(d[k])

    if d.get('lap', 0) != last_lap and d.get('lap', 0) > 0:
        last_lap = d['lap']
        if d.get('llap', 0) > 0:
            laps.insert(0, {
                'lap':    last_lap,
                'time':   round(d.get('llap', 0), 3),
                'best':   round(d.get('blap', 0), 3),
                'maxRpm': int(d.get('mrpm', 0)),
                'maxSpd': round(d.get('mspd', 0), 1),
                'ts':     datetime.now().strftime('%H:%M:%S')
            })
            if len(laps) > MAX_LAPS:
                laps.pop()

    sio.emit('update', d)

# watchdog — ถ้าไม่ได้รับข้อมูล 5 วิ แจ้ง disconnect
def watchdog():
    global connected
    while True:
        time.sleep(2)
        if connected and time.time() - last_rx > 5:
            connected = False
            sio.emit('disconnected', {})

# ──────────────────────────────────────────────
#  ROUTES
# ──────────────────────────────────────────────

# ESP32 POST ข้อมูลมาที่นี่
@app.route('/api/data', methods=['POST'])
def api_receive():
    try:
        d = request.get_json(force=True)
        if not d:
            return 'bad json', 400
        process(d)
        return '', 204
    except Exception as e:
        return str(e), 400

@app.route('/api/latest')
def api_latest():
    return jsonify(latest)

@app.route('/api/laps')
def api_laps():
    return jsonify(laps)

@app.route('/api/history')
def api_history():
    return jsonify({k: list(v) for k, v in history.items()})

@app.route('/api/status')
def api_status():
    return jsonify({
        'connected': connected,
        'last_rx':   round(time.time()-last_rx, 1) if last_rx else None,
        'laps':      len(laps)
    })

@app.route('/')
def index():
    return render_template_string(HTML)

# ──────────────────────────────────────────────
#  HTML DASHBOARD
# ──────────────────────────────────────────────
HTML = r"""<!DOCTYPE html>
<html lang='th'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>🏍 Yamaha Track Dashboard</title>
<script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js'></script>
<script src='https://cdn.socket.io/4.7.2/socket.io.min.js'></script>
<style>
:root{
  --bg:#0a0a0a;--bg2:#131313;--bg3:#1a1a1a;
  --border:#222;--acc:#ff6600;--green:#33ff66;
  --red:#ff3333;--blue:#33aaff;--text:#eee;--muted:#555;
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:var(--bg);color:var(--text);padding:14px}
header{display:flex;align-items:center;gap:10px;margin-bottom:14px;
       border-bottom:1px solid var(--border);padding-bottom:10px;flex-wrap:wrap}
h1{font-size:1.35em;color:var(--acc);font-weight:700}
.dot{width:9px;height:9px;border-radius:50%;background:var(--red);
     box-shadow:0 0 5px var(--red);flex-shrink:0;transition:.4s}
.dot.on{background:var(--green);box-shadow:0 0 8px var(--green)}
.ctxt{font-size:.78em;color:var(--muted)}
.mbadge{padding:2px 9px;border-radius:4px;font-size:.72em;font-weight:700}
.nm{background:#2a5;color:#fff}.tm{background:#c22;color:#fff}
.layout{display:grid;grid-template-columns:1fr 1fr 320px;gap:10px;align-items:start}
@media(max-width:1050px){.layout{grid-template-columns:1fr 1fr}}
@media(max-width:660px){.layout{grid-template-columns:1fr}}
.sec{background:var(--bg2);border:1px solid var(--border);border-radius:10px;padding:13px;margin-bottom:10px}
.sec h2{font-size:.72em;color:var(--muted);text-transform:uppercase;letter-spacing:.07em;margin-bottom:9px}
.g3{display:grid;grid-template-columns:repeat(3,1fr);gap:7px}
.g2{display:grid;grid-template-columns:repeat(2,1fr);gap:7px}
.g{background:var(--bg3);border:1px solid var(--border);border-radius:8px;
   padding:10px 5px;text-align:center}
.g .v{font-size:1.85em;font-weight:700;color:var(--acc);line-height:1}
.g .u{font-size:.63em;color:var(--muted);margin-top:2px}
.g.hi .v{color:var(--green)}.g.warn .v{color:var(--red)}.g.blue .v{color:var(--blue)}
.rpm-wrap{background:var(--bg3);border-radius:4px;height:7px;margin:7px 0 3px;overflow:hidden}
.rpm-fill{height:100%;background:linear-gradient(90deg,var(--green),var(--acc),var(--red));
          border-radius:4px;transition:width .12s;width:0%}
.lap-big{text-align:center;padding:8px 0 10px}
.lap-big .lv{font-size:2.8em;font-weight:700;color:var(--green);letter-spacing:.04em;font-family:monospace}
.lap-big .lu{font-size:.75em;color:var(--muted)}
.lap-row{display:flex;justify-content:space-between;align-items:center;
         padding:6px 4px;border-bottom:1px solid var(--border);font-size:.8em}
.lap-row.best-row{background:#1a1000;border-radius:5px;padding:6px 8px;border-color:transparent}
.lap-row .lnum{color:var(--muted);width:45px}
.lap-row .ltime{font-weight:700;font-family:monospace;font-size:1.05em}
.lap-row .ldet{font-size:.75em;color:var(--muted);text-align:right}
.chart-wrap{position:relative;height:170px}
.gps-info{font-size:.74em;color:var(--blue);line-height:1.9}
.gps-info .lbl{color:var(--muted);margin-right:3px}
.dtbox{background:#1a0000;border:1px solid #500;border-radius:6px;
       padding:8px 10px;font-size:.76em;color:#f77;margin-top:8px;display:none}
.bestbox{background:var(--bg3);border:1px solid #433;border-radius:8px;
         padding:10px;text-align:center;margin-bottom:8px}
.bestbox .bv{font-size:2em;font-weight:700;color:var(--acc);font-family:monospace}
.bestbox .bu{font-size:.65em;color:var(--muted)}
.mst{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:10px}
.mst .ms{background:var(--bg3);border-radius:6px;padding:8px;text-align:center}
.mst .ms .mv{font-size:1.35em;font-weight:700;color:var(--acc)}
.mst .ms .mu{font-size:.63em;color:var(--muted)}
.laps-scroll{max-height:280px;overflow-y:auto}
</style>
</head>
<body>
<header>
  <div class='dot' id='dot'></div>
  <h1>🏍 Yamaha Track Dashboard</h1>
  <span class='mbadge nm' id='mbadge'>NORMAL</span>
  <span class='ctxt' id='ctxt'>รอการเชื่อมต่อ...</span>
</header>

<div class='layout'>

<!-- LEFT -->
<div>
  <div class='sec'>
    <h2>⚡ Engine</h2>
    <div class='g3'>
      <div class='g'><div class='v' id='rpm'>-</div><div class='u'>RPM</div></div>
      <div class='g'><div class='v' id='spd'>-</div><div class='u'>km/h</div></div>
      <div class='g hi'><div class='v' id='gear'>-</div><div class='u'>Gear</div></div>
      <div class='g'><div class='v' id='tps'>-</div><div class='u'>TPS %</div></div>
      <div class='g'><div class='v' id='ign'>-</div><div class='u'>°BTDC</div></div>
      <div class='g'><div class='v' id='vva'>-</div><div class='u'>VVA</div></div>
    </div>
    <div class='rpm-wrap'><div class='rpm-fill' id='rpmbar'></div></div>
  </div>
  <div class='sec'>
    <h2>🌡 Temps / Pressure</h2>
    <div class='g3'>
      <div class='g'><div class='v' id='ect'>-</div><div class='u'>°C ECT</div></div>
      <div class='g'><div class='v' id='iat'>-</div><div class='u'>°C IAT</div></div>
      <div class='g'><div class='v' id='map'>-</div><div class='u'>kPa MAP</div></div>
    </div>
  </div>
  <div class='sec'>
    <h2>⛽ Fuel / Electrical</h2>
    <div class='g3'>
      <div class='g'><div class='v' id='inj'>-</div><div class='u'>ms Inject</div></div>
      <div class='g' id='bcard'><div class='v' id='batt'>-</div><div class='u'>V Batt</div></div>
      <div class='g blue'><div class='v' id='o2'>-</div><div class='u'>V O2</div></div>
      <div class='g'><div class='v' id='isc'>-</div><div class='u'>ISC</div></div>
      <div class='g'><div class='v' id='stf'>-</div><div class='u'>ST Fuel%</div></div>
      <div class='g'><div class='v' id='ltf'>-</div><div class='u'>LT Fuel%</div></div>
    </div>
    <div class='dtbox' id='dtbox'><span id='dtci'></span></div>
  </div>
</div>

<!-- CENTER -->
<div>
  <div class='sec'>
    <h2>📈 RPM / Speed / TPS</h2>
    <div class='chart-wrap'><canvas id='cMain'></canvas></div>
  </div>
  <div class='sec'>
    <h2>🌡 Temps History</h2>
    <div class='chart-wrap'><canvas id='cTemp'></canvas></div>
  </div>
  <div class='sec'>
    <h2>⛽ Fuel Trim History</h2>
    <div class='chart-wrap'><canvas id='cFuel'></canvas></div>
  </div>
  <div class='sec'>
    <h2>🛰 GPS</h2>
    <div class='gps-info'>
      <div><span class='lbl'>Position:</span><span id='gpos'>-</span></div>
      <div><span class='lbl'>GPS Speed:</span><span id='gspd'>-</span>
           &emsp;<span class='lbl'>Heading:</span><span id='ghdg'>-</span></div>
      <div><span class='lbl'>Satellites:</span><span id='gsat'>-</span>
           &emsp;<span class='lbl'>HDOP:</span><span id='ghdop'>-</span></div>
    </div>
  </div>
</div>

<!-- RIGHT: Lap -->
<div>
  <div class='sec'>
    <h2>🏁 Lap Timer</h2>
    <div class='lap-big'>
      <div class='lv' id='laptime'>0:00.000</div>
      <div class='lu'>Current Lap</div>
    </div>
    <div class='g2' style='margin-bottom:8px'>
      <div class='g hi'><div class='v' id='lapnum'>0</div><div class='u'>Lap #</div></div>
      <div class='g warn'><div class='v' id='lastlap'>-</div><div class='u'>Last Lap</div></div>
    </div>
    <div class='bestbox'>
      <div class='bv' id='bestlap'>-</div>
      <div class='bu'>Best Lap 🏆</div>
    </div>
    <div class='mst'>
      <div class='ms'><div class='mv' id='mrpm'>-</div><div class='mu'>Max RPM</div></div>
      <div class='ms'><div class='mv' id='mspd'>-</div><div class='mu'>Max km/h</div></div>
    </div>
    <h2>📋 Lap History</h2>
    <div class='laps-scroll' id='laphist'></div>
  </div>
</div>

</div>

<script>
// ── CHARTS ──
const MAX_PTS = 300;
const mkChart = (id, datasets, yMax) => new Chart(
  document.getElementById(id).getContext('2d'), {
    type:'line',
    data:{labels:Array(MAX_PTS).fill(''), datasets},
    options:{
      responsive:true,maintainAspectRatio:false,animation:false,
      elements:{point:{radius:0},line:{tension:0.3,borderWidth:1.5}},
      scales:{
        x:{display:false},
        y:{max:yMax,grid:{color:'#1e1e1e'},ticks:{color:'#555',font:{size:10}}}
      },
      plugins:{legend:{labels:{color:'#666',font:{size:10},boxWidth:10}}}
    }
  }
);
const mk = (lbl,clr) => ({label:lbl,data:Array(MAX_PTS).fill(null),borderColor:clr});

const cMain = mkChart('cMain',[mk('RPM/10','#ff6600'),mk('km/h','#33aaff'),mk('TPS%','#33ff66')]);
const cTemp = mkChart('cTemp',[mk('ECT°C','#ff4444'),mk('IAT°C','#ffaa33')]);
const cFuel = mkChart('cFuel',[mk('ST Fuel%','#aa66ff'),mk('LT Fuel%','#66aaff')]);

function push(chart,...vals){
  vals.forEach((v,i)=>{
    chart.data.datasets[i].data.push(v);
    if(chart.data.datasets[i].data.length>MAX_PTS)
      chart.data.datasets[i].data.shift();
  });
  chart.update('none');
}

// ── HELPERS ──
function fmtLap(s){
  if(!s||s<=0) return '-';
  let m=Math.floor(s/60),sec=(s%60).toFixed(3).padStart(6,'0');
  return m+':'+sec;
}
function sv(id,v){var e=document.getElementById(id);if(e)e.textContent=v}
function f1(v){return parseFloat(v).toFixed(1)}
function f3(v){return parseFloat(v).toFixed(3)}

// ── SOCKET ──
const socket=io();
socket.on('update',d=>{
  document.getElementById('dot').className='dot on';
  sv('ctxt','ออนไลน์ · '+new Date().toLocaleTimeString());
  document.getElementById('mbadge').textContent=d.mode||'NORMAL';
  document.getElementById('mbadge').className='mbadge '+(d.mode==='TRACK'?'tm':'nm');

  sv('rpm',d.rpm);sv('spd',d.spd);sv('tps',d.tps);
  sv('gear',d.gear===0?'N':d.gear);sv('ign',d.ign);sv('vva',d.vva?'HIGH':'LOW');
  sv('ect',d.ect);sv('iat',d.iat);sv('map',d.map);
  sv('inj',f3(d.inj));sv('batt',f1(d.batt));sv('o2',f3(d.o2));
  sv('isc',d.isc);sv('stf',(d.stf>0?'+':'')+d.stf);sv('ltf',(d.ltf>0?'+':'')+d.ltf);
  document.getElementById('rpmbar').style.width=Math.min(d.rpm/120,100)+'%';
  document.getElementById('bcard').className='g'+(d.batt<11.5?' warn':'');

  sv('gpos',parseFloat(d.lat).toFixed(5)+', '+parseFloat(d.lon).toFixed(5));
  sv('gspd',f1(d.gspd)+' km/h');sv('ghdg',f1(d.hdg)+'°');
  sv('gsat',d.sat);sv('ghdop',f1(d.hdop));

  sv('laptime',fmtLap(d.lpt));sv('lapnum',d.lap||0);
  sv('lastlap',fmtLap(d.llap));sv('bestlap',fmtLap(d.blap));
  sv('mrpm',d.mrpm||'-');sv('mspd',d.mspd?f1(d.mspd)+' km/h':'-');

  var dtb=document.getElementById('dtbox');
  if(d.dtc>0){dtb.style.display='block';sv('dtci','⚠ DTC: '+d.dtcList);}
  else dtb.style.display='none';

  push(cMain,(d.rpm||0)/10,d.spd||0,d.tps||0);
  push(cTemp,d.ect||0,d.iat||0);
  push(cFuel,d.stf||0,d.ltf||0);
});
socket.on('disconnected',()=>{
  document.getElementById('dot').className='dot';
  sv('ctxt','ขาดการเชื่อมต่อ');
});

// ── LAP TABLE ──
function refreshLaps(){
  fetch('/api/laps').then(r=>r.json()).then(laps=>{
    if(!laps.length){
      document.getElementById('laphist').innerHTML=
        '<div style="color:#444;font-size:.78em;padding:10px">ยังไม่มี lap</div>';
      return;
    }
    const bestTime=Math.min(...laps.map(l=>l.time));
    document.getElementById('laphist').innerHTML=laps.map(l=>{
      const isBest=l.time===bestTime;
      return `<div class='lap-row${isBest?' best-row':''}'>
        <span class='lnum'>Lap ${l.lap}</span>
        <span class='ltime' style='color:${isBest?'#ff6600':'#eee'}'>${fmtLap(l.time)}</span>
        <span class='ldet'>${l.maxSpd} km/h<br>${l.maxRpm} rpm</span>
      </div>`;
    }).join('');
  });
}
setInterval(refreshLaps,2000);
refreshLaps();
</script>
</body></html>"""

# ──────────────────────────────────────────────
#  MAIN
# ──────────────────────────────────────────────
if __name__ == '__main__':
    threading.Thread(target=watchdog, daemon=True).start()
    print(f"[SERVER] http://localhost:{PORT}")
    
    print(f"[SERVER] http://<public-IP>:{PORT}  (จากนอก)")
    sio.run(app, host="0.0.0.0", port=PORT, debug=False, allow_unsafe_werkzeug=True)
