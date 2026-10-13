"""
Attendance Portal  —  python app.py COM5
pip install flask pyserial
Access: http://localhost:5000  or  http://YOUR_LAN_IP:5000
"""
import sys, threading, time, datetime, sqlite3
import serial
from flask import Flask, render_template_string, request

app  = Flask(__name__)
DB   = 'attendance.db'
PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
BAUD = 115200

# ── Database ────────────────────────────────────────────────────
def db():
    c = sqlite3.connect(DB); c.row_factory = sqlite3.Row; return c

def init_db():
    conn = db()
    conn.execute('''CREATE TABLE IF NOT EXISTS logs (
        id      INTEGER PRIMARY KEY AUTOINCREMENT,
        roll    TEXT NOT NULL,
        course  TEXT NOT NULL,
        date    TEXT NOT NULL,
        time    TEXT NOT NULL
    )''')
    conn.commit(); conn.close()

def log(roll, course):
    now  = datetime.datetime.now()
    d, t = now.strftime('%Y-%m-%d'), now.strftime('%H:%M:%S')
    conn = db()
    dup  = conn.execute(
        'SELECT id FROM logs WHERE roll=? AND course=? AND date=?',
        (roll, course, d)).fetchone()
    if not dup:
        conn.execute('INSERT INTO logs(roll,course,date,time) VALUES(?,?,?,?)',
                     (roll, course, d, t))
        conn.commit()
        print(f'[LOG] {roll} | {course} | {d} {t}')
    conn.close()

# ── Serial reader ────────────────────────────────────────────────
def serial_reader():
    while True:
        try:
            with serial.Serial(PORT, BAUD, timeout=1) as s:
                print(f'[Serial] {PORT} open')
                while True:
                    line = s.readline().decode('utf-8','ignore').strip()
                    if line.startswith('ATT:'):
                        p = line.split(':')
                        if len(p) >= 3: log(p[1].strip(), p[2].strip())
        except Exception as e:
            print(f'[Serial] {e} — retry in 5s'); time.sleep(5)

# ── HTML template ────────────────────────────────────────────────
PAGE = '''<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IITGN Attendance</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#f5f7fa;color:#1a1a2e}
header{background:#16213e;color:#fff;padding:20px 28px}
header h1{font-size:1.25rem;font-weight:600}
header p{font-size:.82rem;opacity:.65;margin-top:3px}
.wrap{max-width:780px;margin:28px auto;padding:0 16px}
.card{background:#fff;border-radius:12px;padding:24px;
      box-shadow:0 2px 12px rgba(0,0,0,.07);margin-bottom:24px}
.card h2{font-size:.9rem;font-weight:600;color:#16213e;
         text-transform:uppercase;letter-spacing:.05em;margin-bottom:16px}
.row{display:flex;gap:10px}
input[type=text]{flex:1;padding:11px 14px;border:1.5px solid #ddd;
                 border-radius:8px;font-size:1rem;outline:none}
input[type=text]:focus{border-color:#4361ee}
button{padding:11px 22px;background:#4361ee;color:#fff;border:none;
       border-radius:8px;font-size:1rem;cursor:pointer;font-weight:500}
button:hover{background:#3a56d4}
table{width:100%;border-collapse:collapse;font-size:.84rem}
th{background:#16213e;color:#fff;padding:10px 8px;text-align:center;font-weight:500}
td{padding:9px 8px;text-align:center;border-bottom:1px solid #f0f0f0}
td.name{text-align:left;font-weight:600}
.P{color:#22863a;font-weight:700;background:#f0fff4}
.A{color:#cb2431;background:#fff5f5}
.t{display:block;font-size:.7rem;font-weight:400;color:#22863a;margin-top:1px}
.pill{display:inline-block;padding:2px 10px;border-radius:20px;
      font-size:.75rem;font-weight:600;background:#e8eaf6;color:#3f51b5}
.summary{display:flex;gap:12px;flex-wrap:wrap;margin-top:4px}
.stat{background:#f8f9fa;border-radius:8px;padding:10px 16px;text-align:center}
.stat .n{font-size:1.6rem;font-weight:700;color:#4361ee}
.stat .l{font-size:.75rem;color:#666;margin-top:2px}
.notfound{color:#999;font-size:.9rem;margin-top:8px}
@media(max-width:540px){th,td{padding:7px 4px;font-size:.76rem}}
</style>
</head>
<body>
<header>
  <h1>IITGN Attendance Portal</h1>
  <p>Embedded Systems Department &mdash; Real-time attendance tracking</p>
</header>
<div class="wrap">

  <div class="card">
    <h2>Student Login</h2>
    <form method="POST">
      <div class="row">
        <input type="text" name="roll" placeholder="Enter 8-digit roll number"
               maxlength="12" value="{{ roll or '' }}" autocomplete="off" required>
        <button type="submit">View</button>
      </div>
    </form>
  </div>

  {% if roll %}
  <div class="card">
    <div style="display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px;margin-bottom:16px">
      <div>
        <span class="pill">{{ roll }}</span>
        <span style="font-size:.82rem;color:#666;margin-left:8px">Last 7 days</span>
      </div>
      <span style="font-size:.78rem;color:#aaa">{{ dates[0] }} → {{ dates[-1] }}</span>
    </div>

    {% if any_record %}
    <div class="summary" style="margin-bottom:20px">
      {% for course in courses %}
      {% set n = namespace(p=0) %}
      {% for d in dates %}{% if (d,course) in records %}{% set n.p = n.p+1 %}{% endif %}{% endfor %}
      <div class="stat">
        <div class="n">{{ n.p }}/7</div>
        <div class="l">{{ course }}</div>
      </div>
      {% endfor %}
    </div>
    {% endif %}

    {% if any_record %}
    <div style="overflow-x:auto">
    <table>
      <thead><tr>
        <th>Course</th>
        {% for d in dates %}<th>{{ d[5:] }}</th>{% endfor %}
        <th>Total</th>
      </tr></thead>
      <tbody>
      {% for course in courses %}
      {% set ns = namespace(total=0) %}
      <tr>
        <td class="name">{{ course }}</td>
        {% for d in dates %}
          {% set k=(d,course) %}
          {% if k in records %}
            <td class="P">P<span class="t">{{ records[k][:5] }}</span></td>
            {% set ns.total = ns.total + 1 %}
          {% else %}
            <td class="A">A</td>
          {% endif %}
        {% endfor %}
        <td><strong>{{ ns.total }}/7</strong></td>
      </tr>
      {% endfor %}
      </tbody>
    </table>
    </div>
    {% else %}
    <p class="notfound">No attendance records found for <strong>{{ roll }}</strong>.<br>
    Make sure your roll number is registered in the system.</p>
    {% endif %}
  </div>
  {% endif %}

</div>
</body>
</html>'''

# ── Routes ────────────────────────────────────────────────────────
@app.route('/', methods=['GET','POST'])
def index():
    roll = None; records = {}; dates = []; any_record = False
    courses = ['ES333','ES215']
    if request.method == 'POST':
        roll = request.form.get('roll','').strip().upper()
        if roll:
            today = datetime.date.today()
            dates = [(today - datetime.timedelta(days=i)).strftime('%Y-%m-%d')
                     for i in range(6,-1,-1)]
            conn = db()
            rows = conn.execute(
                'SELECT date,course,time FROM logs WHERE roll=? ORDER BY date',
                (roll,)).fetchall()
            conn.close()
            for r in rows:
                records[(r['date'], r['course'])] = r['time']
            any_record = len(records) > 0
    return render_template_string(PAGE,
        roll=roll, dates=dates, records=records,
        any_record=any_record, courses=courses)

@app.route('/recent')
def recent():
    conn = db()
    rows = conn.execute(
        'SELECT roll,course,date,time FROM logs ORDER BY id DESC LIMIT 30'
    ).fetchall()
    conn.close()
    lines = [f"{r['date']} {r['time']} | {r['roll']} | {r['course']}" for r in rows]
    return ('<br>'.join(lines) or 'No entries yet.') + \
           '<br><br><a href="/">Portal</a>'

if __name__ == '__main__':
    init_db()
    threading.Thread(target=serial_reader, daemon=True).start()
    print(f'Portal: http://localhost:5000')
    print(f'For LAN access (other devices): http://YOUR_IP:5000')
    print(f'Recent log: http://localhost:5000/recent')
    app.run(host='0.0.0.0', port=5000, debug=False)