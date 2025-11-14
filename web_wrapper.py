#!/usr/bin/env python3
"""
web_wrapper.py - In-memory Flask Student Record & Result Management System

- Entirely in-memory: nothing is written to disk. When process stops, all data is gone.
- Simple, robust password hashing using salted SHA256 (no passlib/bcrypt required).
- Clean browser UI (single-file template) with subjects-by-semester, students, marks, attendance,
  SGPA/CGPA calculation, admin/student login & signup, and a /health endpoint.
- Safe demo-run: will call a compiled C binary *if present and executable*, but won't fail if it's missing.
- Run:
    export PORT=8080
    python3 web_wrapper.py
  Or with Gunicorn:
    pip install flask gunicorn
    gunicorn web_wrapper:app --bind 0.0.0.0:$PORT
"""

import os
import uuid
import time
import hashlib
import subprocess
import html
from functools import wraps
from flask import (
    Flask, render_template_string, request, redirect, url_for, session, flash, jsonify
)

# --------- Configuration ----------
app = Flask(__name__)
app.secret_key = os.environ.get("SECRET_KEY", str(uuid.uuid4()))
PORT = int(os.environ.get("PORT", "8080"))
IN_MEMORY = True  # explicit: keep everything in memory

# Optional compiled demo binary (will be used only if present)
BINARY = "./student_system"  # leave as-is; safe_run_demo checks file/executable

# --------- In-memory data stores ----------
USERS = {}       # email -> {id,name,email,phone,role,password_hash}
STUDENTS = {}    # student_id -> {id,user_id,roll,program}
SUBJECTS = {}    # subject_id -> {id,code,title,credits,semester}
MARKS = {}       # (student_id,subject_id) -> float marks (0-100)
ATT = {}         # (student_id,subject_id) -> (present_days, total_days)

# --------- Utilities ----------
def uid():
    return str(uuid.uuid4())

def salt_and_hash(password: str) -> str:
    """Return salt$hexsha256(salt+password). Deterministic, simple, secure enough for ephemeral demo."""
    salt = os.urandom(8).hex()
    h = hashlib.sha256((salt + password).encode("utf-8")).hexdigest()
    return f"{salt}${h}"

def verify_hash(password: str, stored: str) -> bool:
    try:
        salt, digest = stored.split("$", 1)
        return hashlib.sha256((salt + password).encode("utf-8")).hexdigest() == digest
    except Exception:
        return False

def ensure_default_admin():
    """Create admin@local / admin123 if no admin exists."""
    if not any(u["role"] == "admin" for u in USERS.values()):
        passwd = "admin123"
        USERS["admin@local"] = {
            "id": uid(),
            "name": "Administrator",
            "email": "admin@local",
            "phone": "",
            "role": "admin",
            "password_hash": salt_and_hash(passwd)
        }

def login_required(role=None):
    def deco(f):
        @wraps(f)
        def wrapped(*args, **kwargs):
            if "user_email" not in session:
                flash("Please login first", "warning")
                return redirect(url_for("index"))
            user = USERS.get(session["user_email"])
            if not user:
                session.pop("user_email", None)
                flash("Session expired — please log in again", "warning")
                return redirect(url_for("index"))
            if role and user["role"] != role:
                flash("Access denied", "danger")
                return redirect(url_for("index"))
            return f(*args, **kwargs)
        return wrapped
    return deco

def compute_sgpa(student_id: str, semester: int):
    total_weighted = 0.0
    total_credits = 0
    for subj in SUBJECTS.values():
        if subj["semester"] != semester:
            continue
        key = (student_id, subj["id"])
        if key in MARKS:
            mark = MARKS[key]
            gp = (mark / 100.0) * 10.0
            total_weighted += gp * subj["credits"]
            total_credits += subj["credits"]
    if total_credits == 0:
        return None
    return total_weighted / total_credits

def compute_cgpa(student_id: str):
    total_weighted = 0.0
    total_credits = 0
    for subj in SUBJECTS.values():
        key = (student_id, subj["id"])
        if key in MARKS:
            mark = MARKS[key]
            gp = (mark / 100.0) * 10.0
            total_weighted += gp * subj["credits"]
            total_credits += subj["credits"]
    if total_credits == 0:
        return None
    return total_weighted / total_credits

def safe_run_demo():
    """If compiled C demo exists and is executable, run it with --demo and return escaped output (short)."""
    try:
        if os.path.isfile(BINARY) and os.access(BINARY, os.X_OK):
            proc = subprocess.run([BINARY, "--demo"], capture_output=True, text=True, timeout=6)
            out = proc.stdout.strip() or proc.stderr.strip() or "(program produced no output)"
            if len(out) > 3000:
                out = out[:3000] + "\n\n...truncated..."
            return html.escape(out)
    except Exception as e:
        return f"(demo run error: {html.escape(str(e))})"
    return "(demo binary not found; UI is in-memory-only)"

# --------- HTML template (single-file) ----------
PAGE = """<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Student Record & Result Management (In-memory)</title>
<style>
:root{--bg:#071428;--card:#0b1220;--accent:#06b6d4;--muted:#94a3b8; --glass: rgba(255,255,255,0.03)}
*{box-sizing:border-box;font-family:Inter,system-ui,Arial}
body{margin:0;background:linear-gradient(180deg,#071428 0%,#0b1220 80%);color:#e6eef6;padding:24px}
.app{max-width:1100px;margin:0 auto}
.header{display:flex;align-items:center;gap:16px;background:linear-gradient(90deg,rgba(255,255,255,0.02),rgba(255,255,255,0.01));padding:16px;border-radius:12px}
.logo{width:56px;height:56px;border-radius:10px;background:linear-gradient(135deg,var(--accent),#7c3aed);display:flex;align-items:center;justify-content:center;font-weight:700}
.title h1{margin:0;font-size:18px}
.title p{margin:4px 0 0;color:var(--muted);font-size:13px}
.container{display:flex;gap:18px;margin-top:18px}
.left{flex:2}
.right{flex:1;min-width:280px}
.card{background:linear-gradient(180deg,rgba(255,255,255,0.02),rgba(255,255,255,0.01));padding:14px;border-radius:12px;box-shadow:0 8px 28px rgba(2,6,23,0.6)}
.controls{display:flex;gap:8px}
.btn{background:transparent;border:1px solid rgba(255,255,255,0.06);padding:8px 12px;border-radius:8px;color:inherit;cursor:pointer;text-decoration:none}
.btn.primary{background:linear-gradient(90deg,var(--accent),#7c3aed);color:#041020;border:0}
.form-row{display:flex;gap:8px;margin-bottom:8px}
.input, select, textarea{background:transparent;border:1px solid rgba(255,255,255,0.06);padding:10px;border-radius:8px;color:inherit}
.pre{background:rgba(255,255,255,0.02);padding:12px;border-radius:8px;color:var(--muted);white-space:pre-wrap}
.hr{height:1px;background:linear-gradient(90deg,rgba(255,255,255,0.02),rgba(255,255,255,0.03));margin:12px 0;border-radius:4px}
.list{list-style:none;padding:0;margin:0}
.list li{display:flex;justify-content:space-between;align-items:center;padding:8px 6px;border-radius:8px}
.small{font-size:13px;color:var(--muted)}
.footer{margin-top:14px;color:var(--muted);text-align:center;font-size:13px}
@media(max-width:900px){.container{flex-direction:column}.right{min-width:unset}}
</style>
</head>
<body>
<div class="app">
  <div class="header">
    <div class="logo">SR</div>
    <div class="title">
      <h1>STUDENT RECORD && RESULT MANAGEMENT SYSTEM</h1>
      <p>Programming in C Semester — Made by Tanay Sah (590023170) & Mahika Jaglan (590025346)</p>
    </div>
    <div style="margin-left:auto" class="small">
      {% if user %}Signed in: {{ user.name }} ({{ user.role }}){% else %}Not signed in{% endif %}
    </div>
  </div>

  <div class="container" style="margin-top:18px">
    <div class="left">
      <div class="card">
        <div style="display:flex;justify-content:space-between;align-items:center">
          <div><strong>Dashboard</strong> <span class="small">— In-memory demo</span></div>
          <div class="controls">
            {% if not user %}
              <a class="btn" href="#login" onclick="document.getElementById('login-form').style.display='block'">Login</a>
              <a class="btn" href="#signup" onclick="document.getElementById('signup-form').style.display='block'">Signup</a>
            {% else %}
              <a class="btn" href="{{ url_for('logout') }}">Logout</a>
            {% endif %}
          </div>
        </div>

        <div class="hr"></div>

        <div style="display:grid;grid-template-columns:1fr 360px;gap:12px">
          <div>
            <div class="card" style="margin-bottom:12px">
              <div style="display:flex;justify-content:space-between">
                <div><strong>Sample Program Output</strong><div class="small">Demo run</div></div>
                <div><a class="btn" href="{{ url_for('run_demo') }}">Run Demo</a></div>
              </div>
              <div class="hr"></div>
              <pre class="pre">{{ demo_output }}</pre>
            </div>

            <div class="card">
              <strong>Quick Actions</strong>
              <div class="hr"></div>
              <div style="display:flex;gap:8px;flex-wrap:wrap">
                <a class="btn" href="#subjects" onclick="document.getElementById('subjects').scrollIntoView()">Subjects</a>
                <a class="btn" href="#marks" onclick="document.getElementById('marks').scrollIntoView()">Enter Marks</a>
                <a class="btn" href="#att" onclick="document.getElementById('att').scrollIntoView()">Attendance</a>
                <a class="btn" href="#students" onclick="document.getElementById('students').scrollIntoView()">Students</a>
              </div>
            </div>
          </div>

          <div>
            <div class="card" style="margin-bottom:12px">
              <strong>Summary</strong>
              <div class="hr"></div>
              <div class="small">Subjects: {{ total_subjects }} &nbsp;|&nbsp; Students: {{ total_students }}</div>
              <div class="small">Marks: {{ total_marks }} &nbsp;|&nbsp; Attendance: {{ total_att }}</div>
              <div style="margin-top:10px">
                <div class="small">Server time: {{ now }}</div>
              </div>
            </div>

            <div class="card">
              <strong>Signed-in user</strong>
              <div class="hr"></div>
              {% if user %}
                <div><strong>{{ user.name }}</strong></div>
                <div class="small">{{ user.email }}</div>
                <div class="small">{{ user.phone }}</div>
              {% else %}
                <div class="small">No user signed in</div>
              {% endif %}
            </div>
          </div>
        </div>
      </div>

      <!-- Subjects -->
      <div id="subjects" class="card" style="margin-top:12px">
        <div style="display:flex;justify-content:space-between">
          <div><strong>Subjects (by semester)</strong></div>
          <div><small class="small">Admin only: add/delete</small></div>
        </div>
        <div class="hr"></div>
        <form method="post" action="{{ url_for('add_subject') }}">
          <div class="form-row">
            <input name="code" class="input" placeholder="Code e.g. CS101" required>
            <input name="title" class="input" placeholder="Title" required>
          </div>
          <div class="form-row">
            <input name="credits" class="input" placeholder="Credits (int)" required>
            <input name="semester" class="input" placeholder="Semester (int)" required>
            <button class="btn primary" type="submit">Add Subject</button>
          </div>
        </form>

        <div class="hr"></div>
        {% if subjects_by_sem %}
          {% for sem, subs in subjects_by_sem.items() %}
            <div style="margin-bottom:10px" class="card">
              <div style="display:flex;justify-content:space-between">
                <div><strong>Semester {{ sem }}</strong></div>
                <div class="small">{{ subs|length }} subjects</div>
              </div>
              <div class="hr"></div>
              <ul class="list">
                {% for s in subs %}
                  <li>
                    <div>
                      <div><strong>{{ s.code }}</strong> — {{ s.title }}</div>
                      <div class="small">Credits: {{ s.credits }}</div>
                    </div>
                    <div>
                      <form method="post" action="{{ url_for('delete_subject', sid=s.id) }}">
                        <button class="btn" onclick="return confirm('Delete subject?');">Delete</button>
                      </form>
                    </div>
                  </li>
                {% endfor %}
              </ul>
            </div>
          {% endfor %}
        {% else %}
          <div class="small">No subjects yet.</div>
        {% endif %}
      </div>

      <!-- Marks -->
      <div id="marks" class="card" style="margin-top:12px">
        <div style="display:flex;justify-content:space-between"><div><strong>Enter / Update Marks</strong></div></div>
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
            <button class="btn primary" type="submit">Save</button>
          </div>
        </form>

        <div class="hr"></div>
        <div><strong>Existing Marks</strong></div>
        <ul class="list">
          {% if marks_list %}
            {% for m in marks_list %}
              <li><div>{{ m.student_roll }} / {{ m.student_name }} — {{ m.subject_code }}</div><div>{{ "%.2f"|format(m.marks) }}</div></li>
            {% endfor %}
          {% else %}
            <li class="small">No marks recorded yet.</li>
          {% endif %}
        </ul>
      </div>

      <!-- Attendance -->
      <div id="att" class="card" style="margin-top:12px">
        <div style="display:flex;justify-content:space-between"><div><strong>Enter / Update Attendance</strong></div></div>
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
            <input name="present" class="input" placeholder="Present days" required>
            <input name="total" class="input" placeholder="Total days" required>
            <button class="btn primary" type="submit">Save</button>
          </div>
        </form>
      </div>

      <!-- Students -->
      <div id="students" class="card" style="margin-top:12px">
        <div style="display:flex;justify-content:space-between"><div><strong>Students</strong></div></div>
        <div class="hr"></div>

        <form id="signup-form" method="post" action="{{ url_for('signup') }}" style="display:none">
          <div class="form-row">
            <input name="name" class="input" placeholder="Full name" required>
            <input name="email" class="input" placeholder="Email" required>
          </div>
          <div class="form-row">
            <input name="phone" class="input" placeholder="Phone">
            <input name="password" class="input" placeholder="Password" required>
          </div>
          <div class="form-row">
            <input name="roll" class="input" placeholder="Roll" required>
            <input name="program" class="input" placeholder="Program">
            <button class="btn primary" type="submit">Create Student</button>
          </div>
        </form>

        <div class="hr"></div>
        <ul class="list">
          {% if students %}
            {% for st in students %}
              <li>
                <div><strong>{{ st.roll }}</strong> — {{ st.program }} <div class="small">{{ st_name_map[st.user_id] }} • {{ st_email_map[st.user_id] }}</div></div>
                <div>
                  <a class="btn" href="{{ url_for('view_student', sid=st.id) }}">View</a>
                  <form method="post" action="{{ url_for('delete_student', sid=st.id) }}" style="display:inline">
                    <button class="btn" onclick="return confirm('Delete student?');">Delete</button>
                  </form>
                </div>
              </li>
            {% endfor %}
          {% else %}
            <li class="small">No students yet.</li>
          {% endif %}
        </ul>
      </div>

    </div>

    <div class="right">
      <div class="card">
        <strong>Login</strong>
        <div class="hr"></div>
        <form id="login-form" method="post" action="{{ url_for('login') }}">
          <div class="form-row"><input name="email" class="input" placeholder="Email" required></div>
          <div class="form-row"><input name="password" class="input" placeholder="Password" required></div>
          <div class="form-row"><button class="btn primary" type="submit">Login</button></div>
        </form>
      </div>

      <div style="height:12px"></div>

      <div class="card">
        <strong>Info</strong>
        <div class="hr"></div>
        <div class="small">All data is ephemeral — restart clears everything.</div>
      </div>
    </div>
  </div>

  <div class="footer"><div class="small">Server time: {{ now }}</div></div>
</div>
</body>
</html>
"""

# --------- Routes ----------
@app.route("/health")
def health():
    return jsonify({"status": "ok"}), 200

@app.route("/run_demo")
def run_demo():
    return redirect(url_for("index"))

@app.route("/", methods=["GET"])
def index():
    ensure_default_admin()
    user = USERS.get(session.get("user_email"))
    demo_output = safe_run_demo()
    # subjects grouped by semester
    subs = sorted(SUBJECTS.values(), key=lambda s: (s["semester"], s["code"]))
    subjects_by_sem = {}
    for s in subs:
        subjects_by_sem.setdefault(str(s["semester"]), []).append(s)
    studs = list(STUDENTS.values())
    # maps for display
    st_name_map = {st["user_id"]: (next((u["name"] for u in USERS.values() if u["id"] == st["user_id"]), "")) for st in studs}
    st_email_map = {st["user_id"]: (next((u["email"] for u in USERS.values() if u["id"] == st["user_id"]), "")) for st in studs}
    # marks list
    marks_list = []
    for (sid, subid), m in MARKS.items():
        st = STUDENTS.get(sid)
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
        now=time.strftime("%Y-%m-%d %H:%M:%S")
    )
    return render_template_string(PAGE, **ctx)

# Signup (create student + user)
@app.route("/signup", methods=["POST"])
def signup():
    name = (request.form.get("name") or "").strip()
    email = (request.form.get("email") or "").strip().lower()
    phone = (request.form.get("phone") or "").strip()
    password = request.form.get("password") or ""
    roll = (request.form.get("roll") or "").strip()
    program = (request.form.get("program") or "").strip()
    if not email or not password or not roll:
        flash("Email, password and roll are required", "danger")
        return redirect(url_for("index") + "#students")
    if email in USERS:
        flash("Email already registered", "warning")
        return redirect(url_for("index") + "#students")
    USERS[email] = {"id": uid(), "name": name or email.split("@")[0], "email": email, "phone": phone, "role": "student", "password_hash": salt_and_hash(password)}
    sid = uid()
    STUDENTS[sid] = {"id": sid, "user_id": USERS[email]["id"], "roll": roll, "program": program}
    session["user_email"] = email
    flash("Student created and logged in", "success")
    return redirect(url_for("index"))

# Login
@app.route("/login", methods=["POST"])
def login():
    email = (request.form.get("email") or "").strip().lower()
    password = request.form.get("password") or ""
    user = USERS.get(email)
    if not user or not verify_hash(password, user["password_hash"]):
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
    try:
        credits = int(request.form.get("credits") or 0)
        semester = int(request.form.get("semester") or 0)
    except ValueError:
        flash("Credits and semester must be integers", "danger")
        return redirect(url_for("index") + "#subjects")
    if not code or not title or credits <= 0 or semester <= 0:
        flash("Invalid subject data", "danger")
        return redirect(url_for("index") + "#subjects")
    if any(s["code"].lower() == code.lower() for s in SUBJECTS.values()):
        flash("Subject code already exists", "warning")
        return redirect(url_for("index") + "#subjects")
    sid = uid()
    SUBJECTS[sid] = {"id": sid, "code": code, "title": title, "credits": credits, "semester": semester}
    flash("Subject added", "success")
    return redirect(url_for("index") + "#subjects")

@app.route("/delete_subject/<sid>", methods=["POST"])
@login_required(role="admin")
def delete_subject(sid):
    if sid in SUBJECTS:
        # remove related marks & attendance
        for k in list(MARKS.keys()):
            if k[1] == sid: MARKS.pop(k, None)
        for k in list(ATT.keys()):
            if k[1] == sid: ATT.pop(k, None)
        SUBJECTS.pop(sid, None)
        flash("Subject deleted", "info")
    return redirect(url_for("index") + "#subjects")

# Enter/update marks
@app.route("/enter_marks", methods=["POST"])
@login_required()
def enter_marks():
    student_id = request.form.get("student_id")
    subject_id = request.form.get("subject_id")
    try:
        marks_f = float(request.form.get("marks") or "0")
    except ValueError:
        flash("Invalid marks value", "danger")
        return redirect(url_for("index") + "#marks")
    if student_id not in STUDENTS or subject_id not in SUBJECTS:
        flash("Invalid student or subject", "danger")
        return redirect(url_for("index") + "#marks")
    key = (student_id, subject_id)
    MARKS[key] = marks_f
    flash("Marks saved/updated", "success")
    return redirect(url_for("index") + "#marks")

# Enter/update attendance
@app.route("/enter_attendance", methods=["POST"])
@login_required()
def enter_attendance():
    student_id = request.form.get("student_id")
    subject_id = request.form.get("subject_id")
    try:
        pd = int(request.form.get("present") or "0")
        td = int(request.form.get("total") or "0")
    except ValueError:
        flash("Invalid attendance numbers", "danger")
        return redirect(url_for("index") + "#att")
    if pd < 0 or td <= 0 or pd > td:
        flash("Invalid attendance values", "danger")
        return redirect(url_for("index") + "#att")
    if student_id not in STUDENTS or subject_id not in SUBJECTS:
        flash("Invalid student or subject", "danger")
        return redirect(url_for("index") + "#att")
    key = (student_id, subject_id)
    ATT[key] = (pd, td)
    flash("Attendance saved", "success")
    return redirect(url_for("index") + "#att")

# View student detail
@app.route("/student/<sid>")
@login_required()
def view_student(sid):
    st = STUDENTS.get(sid)
    if not st:
        flash("Student not found", "danger")
        return redirect(url_for("index") + "#students")
    user_obj = next((u for u in USERS.values() if u["id"] == st["user_id"]), {})
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
        subjects_by_sem={},
        subjects=list(SUBJECTS.values()),
        students=list(STUDENTS.values()),
        st_name_map={},
        st_email_map={},
        marks_list=[],
        total_subjects=len(SUBJECTS),
        total_students=len(STUDENTS),
        total_marks=len(MARKS),
        total_att=len(ATT),
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
    return render_template_string(PAGE, **ctx)

# Delete student (admin)
@app.route("/delete_student/<sid>", methods=["POST"])
@login_required(role="admin")
def delete_student(sid):
    st = STUDENTS.pop(sid, None)
    if st:
        for k in list(MARKS.keys()):
            if k[0] == sid: MARKS.pop(k, None)
        for k in list(ATT.keys()):
            if k[0] == sid: ATT.pop(k, None)
        # remove associated user
        user_id = st["user_id"]
        email_to_remove = None
        for email, u in list(USERS.items()):
            if u["id"] == user_id:
                email_to_remove = email
                break
        if email_to_remove:
            USERS.pop(email_to_remove, None)
        flash("Student deleted", "info")
    return redirect(url_for("index") + "#students")

# Compatibility endpoints
@app.route("/do_login", methods=["POST"])
def do_login():
    return login()

@app.route("/do_signup", methods=["POST"])
def do_signup():
    return signup()

# Start server (bind to PORT when run directly)
if __name__ == "__main__":
    ensure_default_admin()
    app.run(host="0.0.0.0", port=PORT, debug=False)
