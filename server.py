#!/usr/bin/env python3
"""
Yamaha Track Server
รันบน PC/laptop ข้างสนาม
รับข้อมูล UDP จาก ESP32-C3 แล้วแสดงผลบนเว็บ

ติดตั้ง:
  pip install flask flask-socketio

รัน:
  python server.py

เปิด browser:
  http://localhost:5000
  หรือ http://<IP PC>:5000 จากมือถือที่ต่อ hotspot เดียวกัน
"""

import socket, json, threading, time
from collections import deque
from datetime import datetime
from flask import Flask, render_template_string, jsonify
from flask_socketio import SocketIO

# ──────────────────────────────────────────────
#  CONFIG
# ──────────────────────────────────────────────
UDP_PORT    = 5005
WEB_PORT    = 5000
MAX_HISTORY = 300   # จำนวน data points ใน graph (30 วิ @ 10Hz)
MAX_LAPS    = 50

app = Flask(__name__)
app.config['SECRET_KEY'] = 'yamahatrack'
sio = SocketIO(app, cors_allowed_origins="*", async_mode='threading')

# ──────────────────────────────────────────────
#  STATE
# ──────────────────────────────────────────────
latest     = {}
history    = {k: deque(maxlen=MAX_HISTORY) for k in
              ['t','rpm','spd','tps','ect','iat','map','batt','inj','ign','o2','stf','ltf']}
laps       = []   # [{lap, time, best, maxRpm, maxSpd}]
last_lap   = 0
connected  = False
last_rx    = 0

# ──────────────────────────────────────────────
#  UDP LISTENER
# ──────────────────────────────────────────────
def udp_listener():
    global latest, last_lap, connected, last_rx
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', UDP_PORT))
    sock.settimeout(1.0)
    print(f"[UDP] listening on :{UDP_PORT}")
    while True:
        try:
            data, addr = sock.recvfrom(1024)
            d = json.loads(data.decode())
            latest = d
            last_rx = time.time()
            connected = True

            # append history
            for k in history:
                if k in d:
                    history[k].append(d[k])

            # detect new lap
            if d.get('lap', 0) != last_lap and d.get('lap', 0) > 0:
                last_lap = d['lap']
                if d.get('llap', 0) > 0:
                    lap_entry = {
                        'lap':    last_lap,
                        'time':   round(d.get('llap', 0), 3),
                        'best':   round(d.get('blap', 0), 3),
                        'maxRpm': int(d.get('mrpm', 0)),
                        'maxSpd': round(d.get('mspd', 0), 1),
                        'ts':     datetime.now().strftime('%H:%M:%S')
                    }
                    laps.insert(0, lap_entry)
                    if len(laps) > MAX_LAPS:
                        laps.pop()

            # push to browser via websocket
            sio.emit('update', d)

        except socket.timeout:
            if connected and time.time() - last_rx > 3:
                connected = False
                sio.emit('disconnected', {})
        except Exception as e:
            print(f"[UDP] err: {e}")

# ──────────────────────────────────────────────
#  ROUTES
# ──────────────────────────────────────────────
@app.route('/api/latest')
def api_latest():
    return jsonify(latest)

@app.route('/api/laps')
def api_laps():
    return jsonify(laps)

@app.route('/api/history')
def api_history():
    return jsonify({k: list(v) for k, v in history.items()})

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
  --bg:#0a0a0a; --bg2:#141414; --bg3:#1c1c1c;
  --border:#252525; --accent:#ff6600; --green:#33ff66;
  --red:#ff3333; --blue:#33aaff; --text:#eeeeee; --muted:#666;
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:var(--bg);color:var(--text);
     min-height:100vh;padding:16px}
header{display:flex;align-items:center;gap:12px;margin-bottom:16px;
       border-bottom:1px solid var(--border);padding-bottom:12px}
header h1{font-size:1.4em;color:var(--accent);font-weight:700}
.dot{width:10px;height:10px;border-radius:50%;background:var(--red);
     box-shadow:0 0 6px var(--red);transition:.3s}
.dot.on{background:var(--green);box-shadow:0 0 8px var(--green)}
.conn-txt{font-size:.8em;color:var(--muted)}
.mode-badge{padding:3px 10px;border-radius:4px;font-size:.75em;font-weight:700;
            background:#e33;color:#fff;margin-left:4px}
.mode-badge.nm{background:#2a5}

/* GRID LAYOUT */
.layout{display:grid;grid-template-columns:1fr 1fr 340px;gap:12px;align-items:start}
@media(max-width:1100px){.layout{grid-template-columns:1fr 1fr}}
@media(max-width:700px){.layout{grid-template-columns:1fr}}

/* CARDS */
.section{background:var(--bg2);border:1px solid var(--border);border-radius:10px;padding:14px}
.section h2{font-size:.75em;color:var(--muted);text-transform:uppercase;
            letter-spacing:.08em;margin-bottom:10px}

/* GAUGE GRID */
.gg{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.gg2{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}
.g{background:var(--bg3);border:1px solid var(--border);border-radius:8px;
   padding:10px 6px;text-align:center}
.g .v{font-size:2em;font-weight:700;color:var(--accent);line-height:1}
.g .u{font-size:.65em;color:var(--muted);margin-top:2px}
.g.hi .v{color:var(--green)}
.g.warn .v{color:var(--red)}
.g.blue .v{color:var(--blue)}

/* RPM BAR */
.rpm-bar-wrap{margin:8px 0 4px;background:var(--bg3);border-radius:4px;height:8px;overflow:hidden}
.rpm-bar{height:100%;background:linear-gradient(90deg,var(--green),var(--accent),var(--red));
         border-radius:4px;transition:width .1s;width:0%}

/* LAP PANEL */
.lap-big{text-align:center;padding:10px 0}
.lap-big .lv{font-size:3em;font-weight:700;color:var(--green);letter-spacing:.05em}
.lap-big .lu{font-size:.8em;color:var(--muted)}
.lap-row{display:flex;justify-content:space-between;padding:6px 0;
         border-bottom:1px solid var(--border);font-size:.82em}
.lap-row .ln{color:var(--muted)}
.lap-row .lt{font-weight:700;font-family:monospace;font-size:1em}
.lap-row .lb{color:var(--accent);font-size:.8em}
.lap-row.best{background:#1a1200;border-radius:4px;padding:6px 8px}

/* CHART */
.chart-wrap{position:relative;height:180px}

/* GPS */
.gps-info{font-size:.75em;color:var(--blue);padding:4px 0;line-height:1.8}
.gps-info span{color:var(--muted);margin-right:4px}

/* DTC */
.dtc-box{background:#200;border:1px solid #633;border-radius:6px;
         padding:8px;font-size:.78em;color:#f77;margin-top:8px;display:none}

/* MINI STATS */
.mst{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-top:8px}
.mst .ms{background:var(--bg3);border-radius:6px;padding:8px;text-align:center}
.mst .ms .mv{font-size:1.4em;font-weight:700;color:var(--accent)}
.mst .ms .mu{font-size:.65em;color:var(--muted)}
</style>
</head>
<body>
<header>
  <div class='dot' id='dot'></div>
  <h1>🏍 Yamaha Track Dashboard</h1>
  <span class='mode-badge nm' id='mbadge'>NORMAL</span>
  <span class='conn-txt' id='ctxt'>ไม่ได้รับข้อมูล</span>
</header>

<div class='layout'>

<!-- LEFT: Engine + Fuel -->
<div>
  <div class='section' style='margin-bottom:10px'>
    <h2>⚡ Engine</h2>
    <div class='gg'>
      <div class='g'><div class='v' id='rpm'>-</div><div class='u'>RPM</div></div>
      <div class='g'><div class='v' id='spd'>-</div><div class='u'>km/h</div></div>
      <div class='g hi' id='gcard'><div class='v' id='gear'>-</div><div class='u'>Gear</div></div>
      <div class='g'><div class='v' id='tps'>-</div><div class='u'>TPS %</div></div>
      <div class='g'><div class='v' id='ign'>-</div><div class='u'>°BTDC</div></div>
      <div class='g'><div class='v' id='vva'>-</div><div class='u'>VVA</div></div>
    </div>
    <div class='rpm-bar-wrap'><div class='rpm-bar' id='rpmbar'></div></div>
  </div>

  <div class='section' style='margin-bottom:10px'>
    <h2>⛽ Fuel / Electrical</h2>
    <div class='gg'>
      <div class='g'><div class='v' id='inj'>-</div><div class='u'>ms Inject</div></div>
      <div class='g' id='bcard'><div class='v' id='batt'>-</div><div class='u'>V Batt</div></div>
      <div class='g blue'><div class='v' id='o2'>-</div><div class='u'>V O2</div></div>
      <div class='g'><div class='v' id='isc'>-</div><div class='u'>ISC</div></div>
      <div class='g'><div class='v' id='stf'>-</div><div class='u'>ST Fuel%</div></div>
      <div class='g'><div class='v' id='ltf'>-</div><div class='u'>LT Fuel%</div></div>
    </div>
  </div>

  <div class='section'>
    <h2>🌡 Temps / Pressure</h2>
    <div class='gg'>
      <div class='g'><div class='v' id='ect'>-</div><div class='u'>°C ECT</div></div>
      <div class='g'><div class='v' id='iat'>-</div><div class='u'>°C IAT</div></div>
      <div class='g'><div class='v' id='map'>-</div><div class='u'>kPa MAP</div></div>
    </div>
    <div class='dtc-box' id='dtcbox'><span id='dtcinfo'></span></div>
  </div>
</div>

<!-- CENTER: Charts + GPS -->
<div>
  <div class='section' style='margin-bottom:10px'>
    <h2>📈 RPM vs Speed</h2>
    <div class='chart-wrap'><canvas id='chartMain'></canvas></div>
  </div>
  <div class='section' style='margin-bottom:10px'>
    <h2>🌡 Temps History</h2>
    <div class='chart-wrap'><canvas id='chartTemp'></canvas></div>
  </div>
  <div class='section'>
    <h2>🛰 GPS</h2>
    <div class='gps-info'>
      <div><span>Position:</span><span id='gpos'>-</span></div>
      <div><span>GPS Speed:</span><span id='gspd'>-</span>
           &nbsp;&nbsp;<span>Heading:</span><span id='ghdg'>-</span></div>
      <div><span>Satellites:</span><span id='gsat'>-</span>
           &nbsp;&nbsp;<span>HDOP:</span><span id='ghdop'>-</span></div>
    </div>
  </div>
</div>

<!-- RIGHT: Lap Timer -->
<div>
  <div class='section'>
    <h2>🏁 Lap Timer</h2>
    <div class='lap-big'>
      <div class='lv' id='laptime'>0:00.000</div>
      <div class='lu'>Current Lap Time</div>
    </div>
    <div class='gg2' style='margin-bottom:10px'>
      <div class='g'><div class='v' id='lapnum'>0</div><div class='u'>Lap #</div></div>
      <div class='g warn'><div class='v' id='lastlap'>-</div><div class='u'>Last Lap</div></div>
    </div>
    <div class='g' style='margin-bottom:10px;background:#1a1200;border-color:#433'>
      <div class='v' style='color:#ff6600' id='bestlap'>-</div>
      <div class='u'>Best Lap 🏆</div>
    </div>
    <div class='mst'>
      <div class='ms'><div class='mv' id='mrpm'>-</div><div class='mu'>Max RPM</div></div>
      <div class='ms'><div class='mv' id='mspd'>-</div><div class='mu'>Max km/h</div></div>
    </div>
    <h2 style='margin-top:14px'>📋 Lap History</h2>
    <div id='laptable' style='max-height:320px;overflow-y:auto;margin-top:6px'></div>
  </div>
</div>

</div><!-- end layout -->

<script>
// ── CHART SETUP ──
const chartCfg = (labels, datasets) => ({
  type:'line',
  data:{labels,datasets},
  options:{
    responsive:true, maintainAspectRatio:false, animation:false,
    elements:{point:{radius:0},line:{tension:0.3,borderWidth:1.5}},
    scales:{
      x:{display:false},
      y:{grid:{color:'#1e1e1e'},ticks:{color:'#666',font:{size:10}}}
    },
    plugins:{legend:{labels:{color:'#888',font:{size:10},boxWidth:12}}}
  }
});

const MAX_PTS = 300;
const labels = Array(MAX_PTS).fill('');

const ctxMain = document.getElementById('chartMain').getContext('2d');
const chartMain = new Chart(ctxMain, chartCfg(labels, [
  {label:'RPM /10', data:Array(MAX_PTS).fill(null), borderColor:'#ff6600', yAxisID:'y'},
  {label:'km/h',    data:Array(MAX_PTS).fill(null), borderColor:'#33aaff', yAxisID:'y'},
  {label:'TPS %',   data:Array(MAX_PTS).fill(null), borderColor:'#33ff66', yAxisID:'y'},
]));

const ctxTemp = document.getElementById('chartTemp').getContext('2d');
const chartTemp = new Chart(ctxTemp, chartCfg(labels, [
  {label:'ECT °C', data:Array(MAX_PTS).fill(null), borderColor:'#ff4444'},
  {label:'IAT °C', data:Array(MAX_PTS).fill(null), borderColor:'#ffaa33'},
]));

function pushChart(chart, ...vals) {
  vals.forEach((v,i) => {
    chart.data.datasets[i].data.push(v);
    if (chart.data.datasets[i].data.length > MAX_PTS)
      chart.data.datasets[i].data.shift();
  });
  chart.update('none');
}

// ── FORMAT LAP TIME ──
function fmtLap(s) {
  if (!s || s <= 0) return '-';
  let m = Math.floor(s/60), sec = (s%60).toFixed(3).padStart(6,'0');
  return m+':'+sec;
}

function sv(id,v){var e=document.getElementById(id);if(e)e.textContent=v}
function f1(v){return parseFloat(v).toFixed(1)}
function f3(v){return parseFloat(v).toFixed(3)}

// ── SOCKET.IO UPDATE ──
const socket = io();

socket.on('update', d => {
  // connection indicator
  document.getElementById('dot').className = 'dot on';
  sv('ctxt', 'ออนไลน์');
  document.getElementById('mbadge').textContent = d.mode||'NORMAL';
  document.getElementById('mbadge').className = 'mode-badge'+(d.mode==='TRACK'?' ':' nm');

  // gauges
  sv('rpm', d.rpm); sv('spd', d.spd); sv('tps', d.tps);
  sv('gear', d.gear===0?'N':d.gear);
  sv('ign', d.ign); sv('vva', d.vva?'HIGH':'LOW');
  sv('ect', d.ect); sv('iat', d.iat); sv('map', d.map);
  sv('inj', f3(d.inj)); sv('batt', f1(d.batt)); sv('o2', f3(d.o2));
  sv('isc', d.isc);
  sv('stf', (d.stf>0?'+':'')+d.stf);
  sv('ltf', (d.ltf>0?'+':'')+d.ltf);

  // RPM bar
  document.getElementById('rpmbar').style.width = Math.min(d.rpm/120,100)+'%';
  // batt warning
  document.getElementById('bcard').className = 'g'+(d.batt<11.5?' warn':'');

  // GPS
  sv('gpos', d.lat.toFixed(5)+', '+d.lon.toFixed(5));
  sv('gspd', f1(d.gspd)+' km/h');
  sv('ghdg', f1(d.hdg)+'°');
  sv('gsat', d.sat||'-');
  sv('ghdop', f1(d.hdop));

  // Lap
  sv('laptime', fmtLap(d.lpt));
  sv('lapnum', d.lap||0);
  sv('lastlap', fmtLap(d.llap));
  sv('bestlap', fmtLap(d.blap));
  sv('mrpm', d.mrpm||'-');
  sv('mspd', d.mspd?f1(d.mspd)+' km/h':'-');

  // DTC
  var dtb = document.getElementById('dtcbox');
  if (d.dtc > 0) { dtb.style.display='block'; sv('dtcinfo','⚠ DTC: '+d.dtcList+' ('+d.dtc+')'); }
  else dtb.style.display='none';

  // Charts
  pushChart(chartMain, (d.rpm||0)/10, d.spd||0, d.tps||0);
  pushChart(chartTemp, d.ect||0, d.iat||0);
});

socket.on('disconnected', () => {
  document.getElementById('dot').className = 'dot';
  sv('ctxt', 'ขาดการเชื่อมต่อ');
});

// ── LAP TABLE ──
function refreshLaps() {
  fetch('/api/laps').then(r=>r.json()).then(laps => {
    let html = '';
    const best = laps.reduce((b,l) => (!b||l.time<b.time)?l:b, null);
    laps.forEach(l => {
      const isBest = best && l.lap===best.lap;
      html += `<div class='lap-row${isBest?" best":""}'>
        <span class='ln'>Lap ${l.lap}</span>
        <span class='lt' style='color:${isBest?"#ff6600":"#eee"}'>${fmtLap(l.time)}</span>
        <span class='lb'>Max: ${l.maxSpd}km/h ${l.maxRpm}rpm</span>
      </div>`;
    });
    document.getElementById('laptable').innerHTML = html || '<div style="color:#555;font-size:.8em;padding:8px">ยังไม่มี lap</div>';
  });
}
setInterval(refreshLaps, 2000);
refreshLaps();
</script>
</body></html>"""

# ──────────────────────────────────────────────
#  MAIN
# ──────────────────────────────────────────────
if __name__ == '__main__':
    import os
    t = threading.Thread(target=udp_listener, daemon=True)
    t.start()
    print(f"[WEB] http://localhost:{WEB_PORT}")
    sio.run(app, host='0.0.0.0', port=WEB_PORT, debug=False, allow_unsafe_werkzeug=True)
