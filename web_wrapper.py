import os
import subprocess
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(os.environ.get("PORT", "8080"))

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            # Run your compiled C program and capture its output (first few lines only)
            result = subprocess.run(["./student_system"], capture_output=True, text=True, timeout=3)
            output = result.stdout[:500] or "(No output)"
        except Exception as e:
            output = f"Error running program: {e}"

        html = f"""
        <html>
        <head><title>Tanay's Student Management System</title></head>
        <body style="font-family:sans-serif; margin:40px;">
            <h2>Tanay's Student Management System (C Project)</h2>
            <p>This is my 1st semester C programming project built entirely in C language and hosted on Render.</p>
            <h3>Sample Program Output:</h3>
            <pre style="background:#f5f5f5; padding:15px; border-radius:8px;">{output}</pre>
            <p>-- Created by Tanay Sah</p>
        </body>
        </html>
        """
        self.send_response(200)
        self.send_header("Content-type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(html.encode("utf-8"))

if __name__ == "__main__":
    server = HTTPServer(("", PORT), Handler)
    print(f"Listening on port {PORT}")
    server.serve_forever()
