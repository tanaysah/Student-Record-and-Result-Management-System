#!/usr/bin/env python3
# web_wrapper.py
# In-memory Flask web UI for Student Record & Result Management System
# - All data is kept in memory (no files written). Exiting the process clears everything.
# - Browser UI with CSS textures, colors, padding and smooth transitions.
# - Admin & Student signup/login, subjects by semester, marks/attendance (update, no duplicates),
#   SGPA & CGPA calculation (credit-weighted).
# - Health route and binds to PORT when run directly or under gunicorn.
#
# Dependencies: flask, optional passlib for stronger hashing.
# Put in requirements.txt: flask, passlib (optional).
#
# Run locally:
#   export PORT=8080
#   python3 web_wrapper.py
# Or with Gunicorn on Render:
#   gunicorn web_wrapper:app --bind 0.0.0.0:$PORT

import os, uuid, math, html, time
from functools import wraps
from flask import Flask, render_template_string, request, redirect, url_for, session, flash, jsonify

# optional passlib
try:
    from passlib.hash import bcrypt
    HAS_PASSLIB = True
except Exception:
    HAS_PASSLIB = False

app = Flask(__name__)
app.secret_key = os.environ.get("SECRET_KEY", str(uuid.uuid4()))
PORT = int(os.environ.get("PORT", "8080"))
IN_MEMORY = True  # keep everything ephemeral

# ---------- In-memory stores ----------
USERS = {}      # email -> {id,name,email,phone,role,password_hash}
STUDENTS = {}   # user_id -> {id,user_id,roll,program}
SUBJECTS = {}   # subject_id -> {id,code,title,credits,semester}
MARKS = {}      # (student_id,subject_id) -> marks (float)
ATT = {}        # (student_id,subject_id) -> (present_days,total_days)

# ---------- Utilities ----------
def uid():
    return str(uuid.uuid4())

def hash_password(pwd):
    if HAS_PASSLIB:
        return bcrypt.hash(pwd)
    else:
        import hashlib, os
        salt = os.urandom(8).hex()
        return salt + "$" + hashlib.sha256((salt + pwd).encode()).hexdigest()

def verify_password(pwd, stored):
    if HAS_PASSLIB and isinstance(stored, str) and stored.startswith("$2"):
        try:
            return bcrypt.verify(pwd, stored)
        except Exception:
            return False
    else:
        try:
            salt, digest = stored.split("$", 1)
            import hashlib
            return hashlib.sha256((salt + pwd).encode()).hexdigest() == digest
        except Exception:
            return False

def ensure_default_admin():
    if not any(u["role"] == "admin" for u in USERS.values()):
        h = hash_password("admin123")
        u = {"id": uid(), "name": "Administrator", "email": "admin@local", "phone": "", "role": "admin", "password_hash": h}
        USERS[u["email"]] = u

def login_required(role=None):
    def deco(f):
        @wraps(f)
        def wrapped(*a, **kw):
            if "user_email" not in session:
                flash("Please login to continue", "warning")
                return redirect(url_for("index"))
            u = USERS.get(session["user_email"])
            if not u:
                session.pop("user_email", None)
                flash("Session expired, please login again", "warning")
                return redirect(url_for("index"))
            if role and u["role"] != role:
                flash("Access denied", "danger")
                return redirect(url_for("index"))
            return f(*a, **kw)
        return wrapped
    return deco

def compute_sgpa(student_id, semester):
    # gp = (marks/100)*10, weighted by credits
    total_weighted = 0.0
    total_credits = 0
    for s in SUBJECTS.values():
        if s["semester"] != semester:
            continue
        key = (student_id, s["id"])
        if key in MARKS:
            marks = MARKS[key]
            gp = (marks / 100.0) * 10.0
            total_weighted += gp * s["credits"]
            total_credits += s["credits"]
    if total_credits == 0:
        return None
    return total_weighted / total_credits

def compute_cgpa(student_id):
    total_weighted = 0.0
    total_credits = 0
    for s in SUBJECTS.values():
        key = (student_id, s["id"])
        if key in MARKS:
            marks = MARKS[key]
            gp = (marks / 100.0) * 10.0
            total_weighted += gp * s["credits"]
            total_credits += s["credits"]
    if total_credits == 0:
        return None
    return total_weighted / total_credits

# ---------- Demo helper (keeps original behavior if compiled C exists) ----------
BINARY = "./student_system"
def safe_run_demo():
    # try to run compiled binary in demo mode if it exists, else return a friendly message
    try:
        if os.path.isfile(BINARY) and os.access(BINARY, os.X_OK):
            import subprocess
            p = subprocess.run([BINARY, "--demo"], capture_output=True, text=True, timeout=6)
            out = p.stdout.strip() or p.stderr.strip() or "(program produced no output)"
            if len(out) > 3000:
                out = out[:3000] + "\n\n...truncated..."
            return html.escape(out)
    except Exception as e:
        return f"(error running demo: {html.escape(str(e))})"
    return "(demo binary not found; this is an in-memory web UI demo)"

# ---------- Templates (single-file) ----------
BASE_HTML = """
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Student Record & Result Management</title>
<style>
:root{
  --bg:#0f1724; --card:#0b1220; --accent:#06b6d4; --muted:#94a3b8; --panel:#071023;
  --glass: rgba(255,255,255,0.03);
}
*{box-sizing:border-box}
body{margin:0;font-family:Inter,Segoe UI,Roboto,Arial;background:linear-gradient(180deg,#071428 0%, #0b1220 60%); color:#e6eef6; -webkit-font-smoothing:antialiased;}
.app{max-width:1100px;margin:34px auto;padding:22px;}
.header{display:flex;align-items:center;gap:18px;padding:18px;border-radius:14px;background:linear-gradient(90deg, rgba(255,255,255,0.03), rgba(255,255,255,0.02));box-shadow:0 6px 24px rgba(2,6,23,0.6)}
.logo{width:64px;height:64px;border-radius:12px;background:linear-gradient(135deg,var(--accent),#7c3aed);display:flex;align-items:center;justify-content:center;font-weight:700}
.title h1{margin:0;font-size:20px}
.title p{margin:2px 0 0;color:var(--muted);font-size:13px}
.container{display:flex;gap:18px;margin-top:18px;align-items:flex-start}
.left{flex:2}
.right{flex:1;min-width:280px}
.card{background:linear-gradient(180deg, rgba(255,255,255,0.03), rgba(255,255,255,0.01));border-radius:12px;padding:16px;box-shadow:0 8px 28px rgba(2,6,23,0.5)}
.controls{display:flex;gap:8px;flex-wrap:wrap}
.btn{background:transparent;border:1px solid rgba(255,255,255,0.06);padding:8px 12px;border-radius:8px;color:inherit;cursor:pointer;transition:all .18s}
.btn:hover{transform:translateY(-3px);box-shadow:0 6px 18px rgba(2,6,23,0.5)}
.btn.primary{background:linear-gradient(90deg,var(--accent),#7c3aed);color:#071428;border:0}
.small{font-size:13px;padding:6px 10px}
.pre{background:linear-gradient(180deg,rgba(255,255,255,0.03),rgba(255,255,255,0.02));padding:12px;border-radius:8px;color:var(--muted);white-space:pre-wrap}
.list{margin:0;padding:0;list-style:none}
.list li{padding:8px 6px;border-radius:8px;display:flex;justify-content:space-between;align-items:center}
.hr{height:1px;background:linear-gradient(90deg,rgba(255,255,255,0.02),rgba(255,255,255,0.03));margin:12px 0;border-radius:2px}
.form-row{display:flex;gap:8px;margin-bottom:8px}
.input, textarea, select{background:transparent;border:1px solid rgba(255,255,255,0.06);padding:10px;border-radius:8px;color:inherit;min-width:0}
textarea{min-height:80px}
.notice{padding:8px;border-radius:8px;background:linear-gradient(90deg, rgba(255,255,255,0.02), rgba(255,255,255,0.01));color:var(--muted)}
.meta{font-size:12px;color:var(--muted)}
.fade-enter{opacity:0;transform:translateY(6px)}
.fade-enter-active{opacity:1;transform:none;transition:all .28s ease}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px}
.small-muted{font-size:12px;color:var(--muted)}
.topbar{display:flex;justify-content:space-between;gap:12px;align-items:center;margin-bottom:12px}
.badge{padding:6px 8px;border-radius:999px;background:rgba(255,255,255,0.03);font-size:13px}
.footer{margin-top:18px;color:var(--muted);font-size:13px;text-align:center}
@media(max-width:880px){.container{flex-direction:column}.right{min-width:unset}}
</style>
<script>
function show(id){
  document.querySelectorAll('.panel').forEach(p=>p.style.display='none');
  const el = document.getElementById(id);
  if(el){ el.style.display='block'; el.classList.add('fade-enter'); setTimeout(()=>el.classList.remove('fade-enter'),10); }
}
function confirmAction(msg, href){ if(confirm(msg)){ window.location=href; } }
</script>
</head>
<body>
<div class="app">
  <div class="header">
    <div class="logo">SR</div>
    <div class="title">
      <h1>STUDENT RECORD && RESULT MANAGEMENT SYSTEM</h1>
      <p>Programming in C Semester &nbsp;|&nbsp; Made by - Tanay Sah (590023170) - Mahika Jaglan (590025346)</p>
    </div>
    <div style="margin-left:auto" class="meta">
      {% if user %}
        <div class="badge">Signed in: {{ user.name }} ({{ user.role }})</div>
      {% else %}
        <div class="badge">Not signed in</div>
      {% endif %}
    </div>
  </div>

  <div class="container">
    <div class="left">
      <!-- MAIN PANELS -->
      <div id="panel-home" class="card panel" style="display:block">
        <div class="topbar">
          <div><strong>Dashboard</strong> <span class="small-muted">— In-memory demo UI</span></div>
          <div class="controls">
            {% if not user %}
              <a class="btn" href="{{ url_for('index') }}?show=login">Login</a>
              <a class="btn" href="{{ url_for('index') }}?show=signup">Signup</a>
            {% else %}
              <a class="btn" href="{{ url_for('logout') }}">Logout</a>
            {% endif %}
          </div>
        </div>

        <div class="grid">
          <div class="card">
            <div style="display:flex;justify-content:space-between;align-items:center">
              <div><strong>Sample Program Output</strong><div class="small-muted">demo run</div></div>
              <div><a class="btn small" href="{{ url_for('run_demo') }}">Run Demo</a></div>
            </div>
            <div class="hr"></div>
            <pre class="pre">{{ demo_output }}</pre>
          </div>

          <div class="card">
            <strong>Quick Actions</strong>
            <div class="hr"></div>
            <div style="display:flex;flex-direction:column;gap:8px">
              <a class="btn" href="#" onclick="show('panel-subjects')">View Subjects</a>
              <a class="btn" href="#" onclick="show('panel-marks')">Enter Marks</a>
              <a class="btn" href="#" onclick="show('panel-att')">Enter Attendance</a>
              <a class="btn" href="#" onclick="show('panel-students')">Students</a>
            </div>
          </div>
        </div>

        <div class="hr"></div>

        <div style="display:flex;gap:12px">
          <div style="flex:1" class="notice">
            <strong>Ephemeral mode:</strong> All data lives in memory — when this process stops, data is lost.
          </div>
          <div style="width:220px;text-align:right">
            <div class="small-muted">Server time</div>
            <div>{{ now }}</div>
          </div>
        </div>
      </div>

      <!-- Subjects Panel -->
      <div id="panel-subjects" class="panel card" style="display:none">
        <div style="display:flex;justify-content:space-between;align-items:center">
          <div><strong>Subjects (by semester)</strong></div>
          <div><a class="btn" href="#" onclick="show('panel-home')">Back</a></div>
        </div>
        <div class="hr"></div>
        <div style="margin-bottom:12px">
          <form method="post" action="{{ url_for('add_subject') }}">
            <div class="form-row">
              <input class="input" name="code" placeholder="Code e.g. CS101" required>
              <input class="input" name="title" placeholder="Title" required>
            </div>
            <div class="form-row">
              <input class="input" name="credits" placeholder="Credits (int)" required>
              <input class="input" name="semester" placeholder="Semester (int)" required>
              <button class="btn primary small" type="submit">Add Subject</button>
            </div>
          </form>
        </div>

        {% if subjects_by_sem %}
          {% for sem, subs in subjects_by_sem.items() %}
            <div style="margin-bottom:12px" class="card">
              <div style="display:flex;justify-content:space-between;align-items:center">
                <div><strong>Semester {{ sem }}</strong></div>
                <div class="small-muted">{{ subs|length }} subjects</div>
              </div>
              <div class="hr"></div>
              <ul class="list">
                {% for s in subs %}
                  <li>
                    <div>
                      <div><strong>{{ s.code }}</strong> — {{ s.title }}</div>
                      <div class="small-muted">Credits: {{ s.credits }}</div>
                    </div>
                    <div>
                      <form method="post" action="{{ url_for('delete_subject', sid=s.id) }}" style="display:inline">
                        <button class="btn small" onclick="return confirm('Delete subject?');">Delete</button>
                      </form>
                    </div>
                  </li>
                {% endfor %}
              </ul>
            </div>
          {% endfor %}
        {% else %}
          <div class="notice">No subjects yet. Add one above.</div>
        {% endif %}
      </div>

      <!-- Marks Panel -->
      <div id="panel-marks" class="panel card" style="display:none">
        <div style="display:flex;justify-content:space-between;align-items:center">
          <div><strong>Enter / Update Marks</strong></div>
          <div><a class="btn" href="#" onclick="show('panel-home')">Back</a></div>
        </div>
        <div class="hr"></div>

        <form method="post" action="{{ url_for('enter_marks') }}">
          <div class="form-row">
            <select name="student_id" class="input" required>
              <option value="">Choose student</option>
              {% for st in students %}
                <option value="{{ st.id }}">{{ st.roll }} — {{ st_name_map[st.user_id] }}</option>
              {% endfor %}
            </select>
            <select name="subject_id" class="input" required>
              <option value="">Choose subject</option>
              {% for s in subjects %}
                <option value="{{ s.id }}">{{ s.code }} — {{ s.title }}</option>
              {% endfor %}
            </select>
          </div>
          <div class="form-row">
            <input name="marks" class="input" placeholder="Marks (0-100)" required>
            <button class="btn primary small" type="submit">Save</button>
          </div>
        </form>

        <div class="hr"></div>
        <div><strong>Existing Marks</strong></div>
        <ul class="list">
          {% if marks_list %}
            {% for m in marks_list %}
              <li>
                <div>{{ m.student_roll }} / {{ m.student_name }} — {{ m.subject_code }} ({{ m.subject_title }})</div>
                <div>{{ "%.2f"|format(m.marks) }}</div>
              </li>
            {% endfor %}
          {% else %}
            <li class="small-muted">No marks recorded yet.</li>
          {% endif %}
        </ul>
      </div>

      <!-- Attendance Panel -->
      <div id="panel-att" class="panel card" style="display:none">
        <div style="display:flex;justify-content:space-between;align-items:center">
          <div><strong>Enter / Update Attendance</strong></div>
          <div><a class="btn" href="#" onclick="show('panel-home')">Back</a></div>
        </div>
        <div class="hr"></div>

        <form method="post" action="{{ url_for('enter_attendance') }}">
          <div class="form-row">
            <select name="student_id" class="input" required>
              <option value="">Choose student</option>
              {% for st in students %}
                <option value="{{ st.id }}">{{ st.roll }} — {{ st_name_map[st.user_id] }}</option>
              {% endfor %}
            </select>
            <select name="subject_id" class="input" required>
              <option value="">Choose subject</option>
              {% for s in subjects %}
                <option value="{{ s.id }}">{{ s.code }} — {{ s.title }}</option>
              {% endfor %}
            </select>
          </div>
          <div class="form-row">
            <input name="present" class="input" placeholder="Present days (int)" required>
            <input name="total" class="input" placeholder="Total days (int)" required>
            <button class="btn primary small" type="submit">Save</button>
          </div>
        </form>
      </div>

      <!-- Students Panel -->
      <div id="panel-students" class="panel card" style="display:none">
        <div style="display:flex;justify-content:space-between;align-items:center">
          <div><strong>Students</strong></div>
          <div><a class="btn" href="#" onclick="show('panel-home')">Back</a></div>
        </div>
        <div class="hr"></div>
        <div style="margin-bottom:12px">
          <form method="post" action="{{ url_for('signup') }}">
            <div class="form-row">
              <input class="input" name="name" placeholder="Full name" required>
              <input class="input" name="email" placeholder="Email" required>
            </div>
            <div class="form-row">
              <input class="input" name="phone" placeholder="Phone">
              <input class="input" name="password" placeholder="Password" required>
            </div>
            <div class="form-row">
              <input class="input" name="roll" placeholder="Roll" required>
              <input class="input" name="program" placeholder="Program (e.g., B.E.)">
              <button class="btn primary small" type="submit">Create Student</button>
            </div>
          </form>
        </div>

        <div>
          <ul class="list">
            {% if students %}
              {% for st in students %}
                <li>
                  <div>
                    <strong>{{ st.roll }}</strong> — {{ st.program }} <div class="small-muted">{{ st_name_map[st.user_id] }} • {{ st_email_map[st.user_id] }}</div>
                  </div>
                  <div>
                    <a class="btn small" href="{{ url_for('view_student', sid=st.id) }}">View</a>
                    <form method="post" action="{{ url_for('delete_student', sid=st.id) }}" style="display:inline">
                      <button class="btn small" onclick="return confirm('Delete student?');">Delete</button>
                    </form>
                  </div>
                </li>
              {% endfor %}
            {% else %}
              <li class="small-muted">No students yet.</li>
            {% endif %}
          </ul>
        </div>
      </div>

      <!-- Student Detail -->
      <div id="panel-student-detail" class="panel card" style="display:none">
        {% if detail %}
          <div style="display:flex;justify-content:space-between;align-items:center">
            <div><strong>Student: {{ detail.name }} ({{ detail.roll }})</strong></div>
            <div><a class="btn" href="#" onclick="show('panel-students')">Back</a></div>
          </div>
          <div class="hr"></div>
          <div style="display:flex;gap:12px;flex-wrap:wrap">
            <div class="card" style="flex:1">
              <div class="small-muted">Contact</div>
              <div>{{ detail.email }} • {{ detail.phone }}</div>
            </div>
            <div class="card" style="flex:1">
              <div class="small-muted">Program</div>
              <div>{{ detail.program }}</div>
            </div>
            <div class="card" style="flex:1">
              <div class="small-muted">CGPA</div>
              <div>{{ detail.cgpa if detail.cgpa is not none else 'N/A' }}</div>
            </div>
          </div>

          <div class="hr"></div>
          {% for sem, subs in detail.semesters.items() %}
            <div class="card" style="margin-bottom:8px">
              <div style="display:flex;justify-content:space-between"><strong>Semester {{ sem }}</strong><div class="small-muted">SGPA: {{ detail.sgpa.get(sem,'N/A') }}</div></div>
              <div class="hr"></div>
              <ul class="list">
                {% for sub in subs %}
                  <li>
                    <div>{{ sub.code }} — {{ sub.title }} <div class="small-muted">Credits: {{ sub.credits }}</div></div>
                    <div>
                      <div class="small-muted">Marks: {{ sub.marks if sub.marks is not none else 'N/A' }}</div>
                      <div class="small-muted">Att: {{ sub.att if sub.att is not none else 'N/A' }}</div>
                    </div>
                  </li>
                {% endfor %}
              </ul>
            </div>
          {% endfor %}
        {% endif %}
      </div>

    </div>

    <div class="right">
      <div class="card">
        <strong>Signed-in user</strong>
        <div class="hr"></div>
        {% if user %}
          <div><strong>{{ user.name }}</strong></div>
          <div class="small-muted">{{ user.email }}</div>
          <div class="small-muted">{{ user.phone }}</div>
          <div style="margin-top:8px"><a class="btn" href="{{ url_for('logout') }}">Logout</a></div>
        {% else %}
          <div class="small-muted">No user signed in</div>
          <div style="margin-top:8px">
            <a class="btn" href="{{ url_for('index') }}?show=login">Login</a>
            <a class="btn" href="{{ url_for('index') }}?show=signup">Signup</a>
          </div>
        {% endif %}
      </div>

      <div style="height:12px"></div>

      <div class="card">
        <strong>Summary</strong>
        <div class="hr"></div>
        <div class="small-muted">Subjects: {{ total_subjects }} | Students: {{ total_students }}</div>
        <div class="small-muted">Marks recorded: {{ total_marks }} | Attendance records: {{ total_att }}</div>
      </div>

      <div style="height:12px"></div>

      <div class="card">
        <strong>Notes</strong>
        <div class="hr"></div>
        <div class="meta">All edits are ephemeral — restart clears data.</div>
      </div>
    </div>
  </div>

  <div class="footer">
    <div class="meta">Server: In-memory demo • Time: {{ now }}</div>
  </div>
</div>

<script>
  // show panel based on query param
  (function(){
    const params = new URLSearchParams(window.location.search);
    const show = params.get('show');
    if(show) showPanel(show);
    function showPanel(s){
      const mapping = {login:'panel-students', signup:'panel-students', subjects:'panel-subjects', marks:'panel-marks', att:'panel-att', students:'panel-students'};
      const id = mapping[s] || s;
      if(document.getElementById(id)) show(id);
    }
    if(location.hash){
      const target = location.hash.replace('#','');
      if(target && document.getElementById(target)) show(target);
    }
  })();
</script>
</body>
</html>
"""

# ---------- Routes ----------
@app.route("/health")
def health():
    return jsonify({"status": "ok"}), 200

@app.route("/run_demo")
def run_demo():
    return redirect(url_for("index"))

@app.route("/", methods=["GET"])
def index():
    ensure_default_admin()
    # build view context
    user = USERS.get(session.get("user_email"))
    demo_output = safe_run_demo()
    # prepare subjects grouped by semester
    subs = sorted(SUBJECTS.values(), key=lambda s: (s["semester"], s["code"]))
    subjects_by_sem = {}
    for s in subs:
        subjects_by_sem.setdefault(str(s["semester"]), []).append(s)
    # students list
    studs = list(STUDENTS.values())
    st_name_map = {sid: USERS.get(USERS.get(session.get("user_email"), {}).get("email"), {}).get("name") for sid in []}  # placeholder
    # real maps
    st_name_map = {st["user_id"]: USERS.get(next((u for u in USERS if False), ""), {}).get("name", "") for st in studs}  # dummy, will be replaced properly
    # build proper maps:
    st_name_map = {}
    st_email_map = {}
    for st in studs:
        # find user by id
        user_obj = next((u for u in USERS.values() if u["id"] == st["user_id"]), None)
        st_name_map[st["user_id"]] = user_obj["name"] if user_obj else ""
        st_email_map[st["user_id"]] = user_obj["email"] if user_obj else ""
    # marks list for display
    marks_list = []
    for (sid, subid), m in MARKS.items():
        st = next((s for s in studs if s["id"] == sid), None)
        sub = SUBJECTS.get(subid)
        user_obj = next((u for u in USERS.values() if u["id"] == (st["user_id"] if st else None)), None)
        if st and sub:
            marks_list.append({
                "student_roll": st["roll"],
                "student_name": user_obj["name"] if user_obj else "",
                "subject_code": sub["code"],
                "subject_title": sub["title"],
                "marks": m
            })
    ctx = dict(
        user=user,
        demo_output=demo_output,
        subjects_by_sem=subjects_by_sem,
        subjects=subs,
        students=studs,
        st_name_map=st_name_map,
        st_email_map=st_email_map,
        marks_list=marks_list,
        total_subjects=len(SUBJECTS),
        total_students=len(STUDENTS),
        total_marks=len(MARKS),
        total_att=len(ATT),
        now=time.strftime("%Y-%m-%d %H:%M:%S"),
        detail=None
    )
    return render_template_string(BASE_HTML, **ctx)

# Signup (create student + user)
@app.route("/signup", methods=["POST"])
def signup():
    name = request.form.get("name") or request.form.get("email")
    email = request.form.get("email") or ""
    phone = request.form.get("phone") or ""
    password = request.form.get("password") or ""
    roll = request.form.get("roll") or request.form.get("roll")
    program = request.form.get("program") or ""
    if not email or not password or not roll:
        flash("Email, password and roll are required", "danger")
        return redirect(url_for("index", _anchor="panel-students"))
    if email in USERS:
        flash("Email already registered", "warning")
        return redirect(url_for("index", _anchor="panel-students"))
    h = hash_password(password)
    uobj = {"id": uid(), "name": name, "email": email, "phone": phone, "role": "student", "password_hash": h}
    USERS[email] = uobj
    sobj = {"id": uid(), "user_id": uobj["id"], "roll": roll, "program": program}
    STUDENTS[sobj["id"]] = sobj
    session["user_email"] = email
    flash("Student created and logged in", "success")
    return redirect(url_for("index"))

# Login (simple)
@app.route("/login", methods=["POST"])
def login():
    email = request.form.get("email") or ""
    password = request.form.get("password") or ""
    user = USERS.get(email)
    if not user or not verify_password(password, user["password_hash"]):
        flash("Invalid credentials", "danger")
        return redirect(url_for("index"))
    session["user_email"] = email
    flash("Logged in", "success")
    return redirect(url_for("index"))

@app.route("/logout")
def logout():
    session.pop("user_email", None)
    flash("Logged out", "info")
    return redirect(url_for("index"))

# Add subject (admin only)
@app.route("/add_subject", methods=["POST"])
@login_required(role="admin")
def add_subject():
    code = (request.form.get("code") or "").strip()
    title = (request.form.get("title") or "").strip()
    credits = int(request.form.get("credits") or 0)
    semester = int(request.form.get("semester") or 0)
    if not code or not title or credits <= 0 or semester <= 0:
        flash("Invalid subject data", "danger")
        return redirect(url_for("index", _anchor="panel-subjects"))
    # ensure uniqueness by code
    if any(s["code"].lower() == code.lower() for s in SUBJECTS.values()):
        flash("Subject code already exists", "warning")
        return redirect(url_for("index", _anchor="panel-subjects"))
    sid = uid()
    SUBJECTS[sid] = {"id": sid, "code": code, "title": title, "credits": credits, "semester": semester}
    flash("Subject added", "success")
    return redirect(url_for("index", _anchor="panel-subjects"))

@app.route("/delete_subject/<sid>", methods=["POST"])
@login_required(role="admin")
def delete_subject(sid):
    if sid in SUBJECTS:
        # remove marks/att for that subject
        to_rm = [k for k in list(MARKS.keys()) if k[1] == sid]
        for k in to_rm: MARKS.pop(k, None)
        to_rm = [k for k in list(ATT.keys()) if k[1] == sid]
        for k in to_rm: ATT.pop(k, None)
        SUBJECTS.pop(sid, None)
        flash("Subject deleted", "info")
    return redirect(url_for("index", _anchor="panel-subjects"))

# Enter or update marks
@app.route("/enter_marks", methods=["POST"])
@login_required()
def enter_marks():
    student_id = request.form.get("student_id")
    subject_id = request.form.get("subject_id")
    marks_val = request.form.get("marks")
    try:
        marks_f = float(marks_val)
    except Exception:
        flash("Invalid marks value", "danger")
        return redirect(url_for("index", _anchor="panel-marks"))
    if not student_id or not subject_id or subject_id not in SUBJECTS or student_id not in STUDENTS:
        flash("Invalid student or subject", "danger")
        return redirect(url_for("index", _anchor="panel-marks"))
    key = (student_id, subject_id)
    if key in MARKS:
        MARKS[key] = marks_f
        flash("Marks updated", "success")
    else:
        MARKS[key] = marks_f
        flash("Marks saved", "success")
    return redirect(url_for("index", _anchor="panel-marks"))

# Enter or update attendance
@app.route("/enter_attendance", methods=["POST"])
@login_required()
def enter_attendance():
    student_id = request.form.get("student_id")
    subject_id = request.form.get("subject_id")
    present = request.form.get("present")
    total = request.form.get("total")
    try:
        pd = int(present); td = int(total)
    except Exception:
        flash("Invalid attendance numbers", "danger")
        return redirect(url_for("index", _anchor="panel-att"))
    if pd < 0 or td <= 0 or pd > td:
        flash("Invalid attendance values", "danger")
        return redirect(url_for("index", _anchor="panel-att"))
    if student_id not in STUDENTS or subject_id not in SUBJECTS:
        flash("Invalid student or subject", "danger")
        return redirect(url_for("index", _anchor="panel-att"))
    key = (student_id, subject_id)
    ATT[key] = (pd, td)
    flash("Attendance saved", "success")
    return redirect(url_for("index", _anchor="panel-att"))

# View student detail (dashboard-like)
@app.route("/student/<sid>")
@login_required()
def view_student(sid):
    st = STUDENTS.get(sid)
    if not st:
        flash("Student not found", "danger")
        return redirect(url_for("index", _anchor="panel-students"))
    user_obj = next((u for u in USERS.values() if u["id"] == st["user_id"]), {})
    # build sem-wise subjects
    sems = {}
    sgpa_map = {}
    for s in SUBJECTS.values():
        sems.setdefault(s["semester"], []).append({
            "id": s["id"], "code": s["code"], "title": s["title"], "credits": s["credits"],
            "marks": MARKS.get((sid, s["id"])),
            "att": None if (sid, s["id"]) not in ATT else f"{ATT[(sid, s['id'])][0]}/{ATT[(sid, s['id'])][1]}"
        })
    for sem in sems:
        sg = compute_sgpa(sid, sem)
        sgpa_map[sem] = ("{:.3f}".format(sg) if sg is not None else "N/A")
    cg = compute_cgpa(sid)
    ctx = dict(
        user=USERS.get(session.get("user_email")),
        demo_output=safe_run_demo(),
        subjects_by_sem={}, subjects=list(SUBJECTS.values()),
        students=list(STUDENTS.values()), st_name_map={},
        st_email_map={},
        marks_list=[],
        total_subjects=len(SUBJECTS), total_students=len(STUDENTS),
        total_marks=len(MARKS), total_att=len(ATT),
        now=time.strftime("%Y-%m-%d %H:%M:%S"),
        detail={
            "name": user_obj.get("name", ""),
            "email": user_obj.get("email", ""),
            "phone": user_obj.get("phone", ""),
            "roll": st["roll"],
            "program": st.get("program", ""),
            "semesters": {str(k): v for k, v in sems.items()},
            "sgpa": sgpa_map,
            "cgpa": ("{:.3f}".format(cg) if cg is not None else None)
        }
    )
    return render_template_string(BASE_HTML, **ctx)

# Delete student (admin)
@app.route("/delete_student/<sid>", methods=["POST"])
@login_required(role="admin")
def delete_student(sid):
    st = STUDENTS.pop(sid, None)
    if st:
        # remove related marks and attendance
        for k in list(MARKS.keys()):
            if k[0] == sid: MARKS.pop(k, None)
        for k in list(ATT.keys()):
            if k[0] == sid: ATT.pop(k, None)
        # optionally remove user (or keep)
        user_id = st["user_id"]
        email_to_remove = None
        for email, u in list(USERS.items()):
            if u["id"] == user_id:
                email_to_remove = email
                break
        if email_to_remove: USERS.pop(email_to_remove, None)
        flash("Student deleted", "info")
    return redirect(url_for("index", _anchor="panel-students"))

# Compatibility: allow form-based login/signup via named endpoints
@app.route("/do_login", methods=["POST"])
def do_login():
    return login()

@app.route("/do_signup", methods=["POST"])
def do_signup():
    return signup()

# Simple route to run demo (redirect home which runs demo)
@app.route("/demo")
def demo():
    return redirect(url_for("index"))

# Start the app (bind to PORT if run directly)
if __name__ == "__main__":
    ensure_default_admin()
    app.run(host="0.0.0.0", port=PORT, debug=False)
