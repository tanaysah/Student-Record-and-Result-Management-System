#!/usr/bin/env python3
# web_wrapper.py
# Simple HTTP wrapper to host the C student_system demo output and list generated reports.
# Usage: export PORT=8080; python3 web_wrapper.py

import os
import html
import subprocess
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(os.environ.get("PORT", "8080"))
REPORTS_DIR = "reports"
BINARY = "./student_system"
DEMO_ARGS = [BINARY, "--demo"]
TIMEOUT_SEC = 8
OUTPUT_CHAR_LIMIT = 2000  # limit output shown on page


def safe_run_demo():
    """Run the demo binary and return a short text output (stdout or stderr)."""
    if not os.path.isfile(BINARY) or not os.access(BINARY, os.X_OK):
        return f"Executable not found or not executable: {BINARY}. Make sure it was compiled."
    try:
        proc = subprocess.run(DEMO_ARGS, capture_output=True, text=True, timeout=TIMEOUT_SEC)
        out = proc.stdout.strip() or proc.stderr.strip()
        if not out:
            out = "(Program produced no output.)"
        # Limit length for safety
        if len(out) > OUTPUT_CHAR_LIMIT:
            out = out[:OUTPUT_CHAR_LIMIT] + "\n\n...output truncated..."
        return out
    except subprocess.TimeoutExpired:
        return f"Error: program timed out after {TIMEOUT_SEC} seconds."
    except Exception as e:
        return f"Error running program: {html.escape(str(e))}"


def list_report_files():
    """Return list of report filenames in REPORTS_DIR (only .html files)."""
    try:
        if not os.path.isdir(REPORTS_DIR):
            return []
        files = sorted(f for f in os.listdir(REPORTS_DIR) if f.lower().endswith(".html"))
        return files
    except Exception:
        return []


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path.startswith("/reports/"):
            # Serve a specific report file
            filename = path[len("/reports/"):]
            if ".." in filename or filename.startswith("/"):
                self.send_error(400, "Bad request")
                return
            filepath = os.path.join(REPORTS_DIR, filename)
            if os.path.isfile(filepath):
                try:
                    with open(filepath, "rb") as f:
                        data = f.read()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except Exception as e:
                    self.send_error(500, f"Error reading file: {e}")
            else:
                self.send_error(404, "Report not found")
            return

        # Otherwise, serve the main page
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

        html_page = f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>Student Record and Result Management System</title>
  <style>
    body {{ font-family: Arial, sans-serif; margin: 28px; background-color:#fafafa; }}
    h1 {{ font-size: 26px; color:#222; margin-bottom:4px; }}
    h2 {{ font-size: 18px; font-weight: normal; color:#444; margin-top:0; }}
    pre {{ background:#f5f5f5; padding:12px; border-radius:8px; white-space:pre-wrap; }}
    .box {{ max-width:900px; margin:auto; background:white; padding:25px; border-radius:12px; box-shadow:0 2px 8px rgba(0,0,0,0.1); }}
    footer {{ margin-top:22px; font-size:13px; color:#666; }}
  </style>
</head>
<body>
  <div class="box">
    <h1>Student Record and Result Management System</h1>
    <h2>Programming in C</h2>
    <p><strong>Created by:</strong> Tanay Sah (590023170) &nbsp; | &nbsp; Mahika Jaglan (590025346)</p>
    <hr>

    <h3>Sample Program Output (Demo Mode):</h3>
    <pre>{safe_demo}</pre>

    <h3>Generated Reports:</h3>
    {reports_html}

    <footer>
      <p><strong>Note:</strong> This is a demo of a C-based console program hosted via a Python wrapper for display on the web.</p>
      <p>Run <code>{html.escape(BINARY)} --demo</code> for demo mode or run interactively in a terminal for full functionality.</p>
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
