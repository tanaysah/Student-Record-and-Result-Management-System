#!/usr/bin/env python3
# web_wrapper.py (enhanced)
# Serves a single HTML page that either embeds demo output from the demo binary
# or lists generated reports.
#
# Usage (local): PORT=8080 python3 web_wrapper.py

import os
import html
import subprocess
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(os.environ.get("PORT", "8080"))
REPORTS_DIR = "reports"

# Prefer the web-enabled C server if compiled
BINARY_WEB = "./student_system_web"
BINARY_CONSOLE = "./student_system"
# demo args used for console-version (if it supports --demo)
DEMO_ARGS_CONSOLE = [BINARY_CONSOLE, "--demo"]
DEMO_ARGS_WEB = [BINARY_WEB, "--demo"]
TIMEOUT_SEC = 6
OUTPUT_CHAR_LIMIT = 2000  # limit output shown on page

def find_preferred_demo():
    """Return (binary_path, args_list) for demo run. Prefer student_system_web, else student_system."""
    if os.path.isfile(BINARY_WEB) and os.access(BINARY_WEB, os.X_OK):
        return (BINARY_WEB, DEMO_ARGS_WEB)
    if os.path.isfile(BINARY_CONSOLE) and os.access(BINARY_CONSOLE, os.X_OK):
        return (BINARY_CONSOLE, DEMO_ARGS_CONSOLE)
    return (None, None)

def safe_run_demo():
    """Run demo binary and return short text output (stdout or stderr)."""
    binpath, args = find_preferred_demo()
    if not binpath:
        return f"No demo binary found. Expected {BINARY_WEB} or {BINARY_CONSOLE} in repo root."
    try:
        proc = subprocess.run(args, capture_output=True, text=True, timeout=TIMEOUT_SEC)
        out = proc.stdout.strip() or proc.stderr.strip()
        if not out:
            out = "(Program produced no output.)"
        if len(out) > OUTPUT_CHAR_LIMIT:
            out = out[:OUTPUT_CHAR_LIMIT] + "\n\n...output truncated..."
        return out
    except subprocess.TimeoutExpired:
        return f"Error: program timed out after {TIMEOUT_SEC} seconds."
    except Exception as e:
        return f"Error running program: {html.escape(str(e))}"

def list_report_files():
    """Return list of report filenames in REPORTS_DIR (html and txt)."""
    try:
        if not os.path.isdir(REPORTS_DIR):
            return []
        files = sorted(f for f in os.listdir(REPORTS_DIR) if f.lower().endswith((".html", ".htm", ".txt")))
        return files
    except Exception:
        return []

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path.startswith("/reports/"):
            filename = path[len("/reports/"):]
            if ".." in filename or filename.startswith("/"):
                self.send_error(400, "Bad request")
                return
            filepath = os.path.join(REPORTS_DIR, filename)
            if os.path.isfile(filepath):
                try:
                    with open(filepath, "rb") as f:
                        data = f.read()
                    ctype = "text/plain"
                    if filename.lower().endswith(".html") or filename.lower().endswith(".htm"):
                        ctype = "text/html; charset=utf-8"
                    self.send_response(200)
                    self.send_header("Content-Type", ctype)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except Exception as e:
                    self.send_error(500, f"Error reading file: {e}")
            else:
                self.send_error(404, "Report not found")
            return

        demo_output = safe_run_demo()
        reports = list_report_files()
        safe_demo = html.escape(demo_output)

        reports_html = "<p>No reports found.</p>"
        if reports:
            lines = []
            for fn in reports:
                href = f"/reports/{html.escape(fn)}"
                lines.append(f'<li><a href="{href}" target="_blank" rel="noopener">{html.escape(fn)}</a></li>')
            reports_html = "<ul>" + "\n".join(lines) + "</ul>"

        binpath, _ = find_preferred_demo()
        usedbin = os.path.basename(binpath) if binpath else "(none)"

        html_page = f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>Student Record and Result Management System</title>
  <style>
    body {{ font-family: Arial, sans-serif; margin: 28px; background-color:#fafafa; }}
    h1 {{ font-size: 26px; color:#222; margin-bottom:4px; }}
    pre {{ background:#f5f5f5; padding:12px; border-radius:8px; white-space:pre-wrap; max-height:480px; overflow:auto; }}
    .box {{ max-width:900px; margin:auto; background:white; padding:25px; border-radius:12px; box-shadow:0 2px 8px rgba(0,0,0,0.1); }}
    footer {{ margin-top:22px; font-size:13px; color:#666; }}
  </style>
</head>
<body>
  <div class="box">
    <h1>Student Record and Result Management System</h1>
    <p><strong>Binary detected:</strong> {html.escape(usedbin)}</p>
    <hr>
    <h3>Sample Program Output (Demo Mode):</h3>
    <pre>{safe_demo}</pre>
    <h3>Generated Reports:</h3>
    {reports_html}
    <footer>
      <p><strong>Note:</strong> This page executes the compiled binary in demo mode (if available) and lists files from './{REPORTS_DIR}'.</p>
    </footer>
  </div>
</body>
</html>
"""
        data = html_page.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format, *args):
        print("%s - - [%s] %s" % (self.address_string(), self.log_date_time_string(), format%args))

def run():
    try:
        os.makedirs(REPORTS_DIR, exist_ok=True)
    except Exception as e:
        print("Warning: could not create reports dir:", e)
    server = HTTPServer(("", PORT), Handler)
    print(f"Listening on port {PORT} (serving demo + reports).")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("Shutting down server.")
    finally:
        server.server_close()

if __name__ == "__main__":
    run()
