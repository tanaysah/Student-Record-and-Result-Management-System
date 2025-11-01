/* student_system_web.c
   Enhanced web wrapper for student_system.c
   - Landing page with Admin login / Student sign up / Student sign in
   - Simple admin dashboard and student dashboard
   - Inline SVG attendance chart for students
   Compile together with student_system.c:
     gcc -DBUILD_WEB student_system.c student_system_web.c -o student_system_web
   Run:
     PORT=8080 ./student_system_web
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp, strcasestr
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- Ensure structs / constants match student_system.c exactly --- */
#define MAX_NAME 100
#define MAX_SUBJECTS 8
#define MAX_SUB_NAME 50

typedef struct {
    char name[MAX_SUB_NAME];
    int classes_held;
    int classes_attended;
    int marks;
    int credits;
} Subject;

typedef struct {
    int id;
    char name[MAX_NAME];
    int age;
    char dept[MAX_NAME];
    int year;
    int num_subjects;
    Subject subjects[MAX_SUBJECTS];
    char password[50];
    int exists;
    double cgpa;
    int total_credits_completed;
} Student;

/* --- externs from student_system.c --- */
extern Student students[];
extern int student_count;

extern int api_find_index_by_id(int id);
extern int api_add_student(Student *s);
extern void api_generate_report(int idx, const char* college, const char* semester, const char* exam);
extern int api_calculate_update_cgpa(int idx);
extern int api_admin_auth(const char *user, const char *pass);

/* filesystem helper */
static void ensure_reports_dir(void) {
    struct stat st;
    if (stat("reports", &st) == -1) {
        mkdir("reports", 0755);
    }
}

/* small url-decode inplace */
static void urldecode_inplace(char *s) {
    char *d = s;
    while (*s) {
        if (*s == '+') { *d++ = ' '; s++; }
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *d++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *d++ = *s++;
        }
    }
    *d = 0;
}

/* parse application/x-www-form-urlencoded body for key (returns malloc'd string or NULL) */
static char *form_value(const char *body, const char *key) {
    size_t k = strlen(key);
    const char *p = body;
    while (*p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (!eq) break;
        size_t name_len = (amp ? (size_t)(eq - p) : (size_t)(eq - p));
        if (name_len == k && strncmp(p, key, k) == 0) {
            const char *val_start = eq + 1;
            const char *val_end = amp ? amp : (p + strlen(p));
            size_t vlen = val_end - val_start;
            char *out = malloc(vlen + 1);
            if (!out) return NULL;
            memcpy(out, val_start, vlen);
            out[vlen] = 0;
            urldecode_inplace(out);
            return out;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

/* tiny helper to escape HTML (very small subset) */
static void html_escape_buf(const char *in, char *out, size_t outcap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 6 < outcap; ++i) {
        char c = in[i];
        if (c == '&') { strcpy(out + j, "&amp;"); j += 5; }
        else if (c == '<') { strcpy(out + j, "&lt;"); j += 4; }
        else if (c == '>') { strcpy(out + j, "&gt;"); j += 4; }
        else if (c == '"') { strcpy(out + j, "&quot;"); j += 6; }
        else out[j++] = c;
    }
    out[j] = 0;
}

/* grade point helper (same logic as in student_system.c) */
static int marks_to_grade_point_local(int marks) {
    if (marks >= 90) return 10;
    if (marks >= 80) return 9;
    if (marks >= 70) return 8;
    if (marks >= 60) return 7;
    if (marks >= 50) return 6;
    if (marks >= 40) return 5;
    return 0;
}

/* compute SGPA (local copy) */
static double compute_sgpa_local(const Student *s) {
    int total_credits = 0;
    double weighted = 0.0;
    for (int i = 0; i < s->num_subjects; ++i) {
        int cr = s->subjects[i].credits;
        int mk = s->subjects[i].marks;
        if (cr <= 0) continue;
        int gp = marks_to_grade_point_local(mk);
        weighted += (double)gp * cr;
        total_credits += cr;
    }
    if (total_credits == 0) return 0.0;
    return weighted / (double)total_credits;
}

/* Build landing page with three prominent cards + nice background */
static char *build_landing_page(void) {
    ensure_reports_dir();
    const char *html_start =
        "<!doctype html><html><head><meta charset='utf-8'><title>Student System</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>"
        "body{margin:0;font-family:Inter,Arial,Helvetica,sans-serif;background:linear-gradient(135deg,#f0f6ff 0%,#ffffff 40%,#f7f2ff 100%);min-height:100vh;display:flex;align-items:center;justify-content:center}"
        ".wrap{max-width:1100px;width:95%;margin:40px auto;background:rgba(255,255,255,0.9);border-radius:12px;padding:26px;box-shadow:0 8px 28px rgba(20,20,50,0.08)}"
        "h1{margin:0;font-size:28px;color:#12263a} p.lead{color:#4b5563}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:16px;margin-top:18px}"
        ".card{background:#fff;border-radius:10px;padding:18px;border:1px solid rgba(20,20,60,0.04)}"
        ".card h3{margin:0 0 8px 0} .card p{margin:0 0 12px 0;color:#333}"
        "input,textarea,button,select{width:100%;padding:8px;border-radius:6px;border:1px solid #e6eef8;font-size:14px}"
        "button{cursor:pointer;background:linear-gradient(180deg,#2b6ef6,#215bd6);color:white;border:none;padding:10px 12px}"
        ".small{font-size:13px;color:#6b7280}"
        ".muted{color:#6b7280;font-size:13px;margin-top:8px}"
        "@media(max-width:600px){.wrap{padding:14px}}"
        "</style></head><body><div class='wrap'>"
        "<h1>Student Record & Result Management</h1>"
        "<p class='lead'>Choose an option to continue — Admin login, Student sign up, or Student sign in.</p>"
        "<div class='grid'>";

    const char *admin_card =
        "<div class='card'>"
        "<h3>Admin Login</h3>"
        "<p>Full admin control: manage students, marks, attendance, generate reports.</p>"
        "<form method='post' action='/admin-login'>"
        "<input name='username' placeholder='Admin username' required />"
        "<input name='password' placeholder='Admin password' type='password' required />"
        "<div style='margin-top:8px'><button>Login as Admin</button></div>"
        "</form>"
        "</div>";

       const char *signup_card =
        "<div class='card'>"
        "<h3>Student Sign Up</h3>"
        "<p>Create a new student account (self-registration).</p>"
        "<form method='post' action='/student-signup'>"
        "<input name='name' placeholder='Full name' required />"
        "<input name='age' placeholder='Age' required />"
        "<input name='sap_id' placeholder='SAP ID (use this as your login ID)' required />"
        "<input name='password' placeholder='Password' type='password' required />"
        "<div style='margin-top:8px'><button>Sign up</button></div>"
        "</form>"
        "<p class='muted'>Use your SAP ID to sign in after registration.</p>"
        "</div>";


    const char *signin_card =
        "<div class='card'>"
        "<h3>Student Sign In</h3>"
        "<p>Sign in to view your dashboard (attendance, marks, SGPA, CGPA).</p>"
        "<form method='get' action='/dashboard'>"
        "<input name='id' placeholder='Student ID' required />"
        "<input name='pass' placeholder='Password' type='password' required />"
        "<div style='margin-top:8px'><button>Sign in</button></div>"
        "</form>"
        "</div>";

    const char *footer = "</div><p class='small'>Demo by Tanay Sah & Mahika Jaglan — for demonstration only.</p></div></body></html>";

    size_t cap = strlen(html_start) + strlen(admin_card) + strlen(signup_card) + strlen(signin_card) + strlen(footer) + 256;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, html_start);
    strcat(buf, admin_card);
    strcat(buf, signup_card);
    strcat(buf, signin_card);
    strcat(buf, footer);
    return buf;
}

/* send text/html response */
static void send_text(int client, const char *status, const char *ctype, const char *body) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                        status, ctype, strlen(body));
    send(client, header, hlen, 0);
    send(client, body, strlen(body), 0);
}

/* Read request (headers and body) into buffer (simple) */
#define REQBUF 262144
static int read_request(int client, char *buf, int bufsz) {
    int total = 0, r;
    while ((r = recv(client, buf + total, bufsz - total - 1, 0)) > 0) {
        total += r;
        if (total > 4 && strstr(buf, "\r\n\r\n")) break;
        if (total > bufsz - 100) break;
    }
    if (r <= 0 && total == 0) return -1;
    buf[total] = 0;
    /* if Content-Length present, ensure body fully read */
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl) {
        int clv = atoi(cl + strlen("Content-Length:"));
        char *hdr_end = strstr(buf, "\r\n\r\n");
        int bodylen = hdr_end ? (int)strlen(hdr_end + 4) : 0;
        int toread = clv - bodylen;
        while (toread > 0 && (r = recv(client, buf + total, bufsz - total - 1, 0)) > 0) {
            total += r;
            toread -= r;
        }
        buf[total] = 0;
    }
    return total;
}

/* Serve a static report file from reports/ */
static void serve_report_file(int client, const char *name) {
    if (strstr(name, "..")) {
        const char *bad = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nContent-Length:11\r\n\r\nBad request";
        send(client, bad, strlen(bad), 0);
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "reports/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        const char *notf = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length:9\r\n\r\nNot found";
        send(client, notf, strlen(notf), 0);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(sz + 1);
    if (!data) { fclose(f); const char *err = "HTTP/1.1 500 Internal\r\n\r\n"; send(client, err, strlen(err), 0); return; }
    fread(data, 1, sz, f);
    data[sz] = 0;
    fclose(f);
    char header[256];
    int hlen = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n", sz);
    send(client, header, hlen, 0);
    send(client, data, sz, 0);
    free(data);
}

/* build simple student list HTML (used for admin) */
static char *build_list_html(void) {
    size_t cap = 8192;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, "<!doctype html><html><head><meta charset='utf-8'><title>Students</title></head><body><h2>Students</h2><table border='1' cellpadding='6'><tr><th>ID</th><th>Name</th><th>Year</th><th>Dept</th></tr>");
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        char row[1024];
        snprintf(row, sizeof(row), "<tr><td>%d</td><td>%s</td><td>%d</td><td>%s</td></tr>", students[i].id, students[i].name, students[i].year, students[i].dept);
        if (strlen(buf) + strlen(row) + 256 > cap) { cap *= 2; buf = realloc(buf, cap); }
        strcat(buf, row);
    }
    strcat(buf, "</table><p><a href='/'>Back</a></p></body></html>");
    return buf;
}

/* build student dashboard as HTML with an inline SVG attendance graph */
static char *build_student_dashboard(int idx) {
    if (idx < 0 || idx >= student_count) return NULL;
    Student *s = &students[idx];
    char escaped_name[256]; html_escape_buf(s->name, escaped_name, sizeof(escaped_name));
    /* build subject table & gather attendance percentages for chart */
    char subject_rows[8192];
    subject_rows[0] = 0;
    int maxbars = s->num_subjects;
    int percentages[MAX_SUBJECTS];
    for (int i = 0; i < s->num_subjects; ++i) {
        int held = s->subjects[i].classes_held;
        int att = s->subjects[i].classes_attended;
        int pct = (held == 0) ? 0 : (int)(((double)att / held) * 100.0 + 0.5);
        percentages[i] = pct;
        char row[512];
        char sname_esc[256]; html_escape_buf(s->subjects[i].name, sname_esc, sizeof(sname_esc));
        double gp = (s->subjects[i].credits>0)? marks_to_grade_point_local(s->subjects[i].marks) : 0;
        snprintf(row, sizeof(row),
                 "<tr><td>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%.0f</td><td>%d%%</td></tr>",
                 i+1, sname_esc, s->subjects[i].marks, s->subjects[i].credits, gp, pct);
        strcat(subject_rows, row);
    }
    double sgpa = compute_sgpa_local(s);
    /* Build small inline SVG bar chart */
    char svg[4096];
    int w = 480, h = 160, pad = 30;
    int barw = (maxbars>0) ? ( (w - pad*2) / maxbars - 8 ) : 20;
    if (barw < 8) barw = 8;
    char svg_start[256];
    snprintf(svg_start, sizeof(svg_start), "<svg viewBox='0 0 %d %d' width='%d' height='%d' xmlns='http://www.w3.org/2000/svg'><rect width='100%%' height='100%%' fill='transparent'/>", w, h, w, h);
    strcpy(svg, svg_start);
    /* axes */
    snprintf(svg + strlen(svg), sizeof(svg)-strlen(svg), "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#ddd'/>", pad, h-pad, w-pad, h-pad);
    for (int i = 0; i < maxbars; ++i) {
        int x = pad + i*(barw+8);
        int barh = (percentages[i] * (h - pad*2)) / 100;
        int y = (h - pad) - barh;
        snprintf(svg + strlen(svg), sizeof(svg)-strlen(svg),
                 "<rect x='%d' y='%d' width='%d' height='%d' rx='4' ry='4' fill='#3b82f6' opacity='0.85'/>",
                 x, y, barw, barh);
        /* label */
        char lbl[128]; html_escape_buf(s->subjects[i].name, lbl, sizeof(lbl));
        snprintf(svg + strlen(svg), sizeof(svg)-strlen(svg),
                 "<text x='%d' y='%d' font-size='10' fill='#111' transform='translate(0,14) rotate(0)' >%s</text>",
                 x, h-pad+12, lbl);
    }
    strcat(svg, "</svg>");

    const char *tpl_start =
        "<!doctype html><html><head><meta charset='utf-8'><title>Dashboard</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>body{font-family:Inter,Arial;margin:18px} .card{background:#fff;padding:18px;border-radius:10px;box-shadow:0 6px 18px rgba(0,0,0,0.06);max-width:1000px;margin:auto} table{width:100%;border-collapse:collapse} table th,table td{padding:8px;border:1px solid #eee;text-align:left;font-size:14px}</style>"
        "</head><body><div class='card'>";

    const char *tpl_end = "<p><a href='/'>← Back to Home</a></p></div></body></html>";

    /* estimate size */
    size_t cap = strlen(tpl_start) + 4096 + strlen(subject_rows) + strlen(svg) + 1024;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, tpl_start);
    char header[512];
    snprintf(header, sizeof(header),
             "<h2>Welcome, %s</h2><p>ID: %d | Dept: %s | Year: %d | Age: %d</p>"
             "<p><strong>SGPA (current):</strong> %.3f  &nbsp;&nbsp; <strong>Stored CGPA:</strong> %.3f (Credits: %d)</p>",
             escaped_name, s->id, s->dept, s->year, s->age, sgpa, s->cgpa, s->total_credits_completed);
    strcat(buf, header);
    strcat(buf, "<h3>Attendance (per subject)</h3>");
    strcat(buf, svg);
    strcat(buf, "<h3>Subjects & Marks</h3><table><tr><th>#</th><th>Subject</th><th>Marks</th><th>Credits</th><th>GradePoint</th><th>Attendance</th></tr>");
    strcat(buf, subject_rows);
    strcat(buf, "</table>");
    strcat(buf, tpl_end);
    return buf;
}

/* handle a client connection */
static void handle_client(int client) {
    char req[REQBUF];
    int r = read_request(client, req, sizeof(req));
    if (r <= 0) { close(client); return; }

    char method[8] = {0}, path[1024] = {0}, proto[32] = {0};
    sscanf(req, "%7s %1023s %31s", method, path, proto);

    /* route: GET /reports/<name> */
    if (strcmp(method, "GET") == 0) {
        if (strncmp(path, "/reports/", 9) == 0) {
            const char *fname = path + 9;
            while (*fname == '/') fname++;
            serve_report_file(client, fname);
            close(client); return;
        }
        if (strcmp(path, "/") == 0) {
            char *page = build_landing_page();
            if (!page) send_text(client, "500 Internal Server Error", "text/plain", "Server error");
            else { send_text(client, "200 OK", "text/html; charset=utf-8", page); free(page); }
            close(client); return;
        }
        if (strncmp(path, "/list", 5) == 0) {
            char *page = build_list_html();
            if (!page) send_text(client, "500 Internal Server Error", "text/plain", "Server error");
            else { send_text(client, "200 OK", "text/html; charset=utf-8", page); free(page); }
            close(client); return;
        }
        if (strncmp(path, "/dashboard", 10) == 0) {
            /* parse query string after ? */
            char *q = strchr(path, '?');
            int id = -1; char pass[128] = {0};
            if (q) {
                char *qs = strdup(q+1);
                char *v = form_value(qs, "id");
                char *p = form_value(qs, "pass");
                if (v) { id = atoi(v); free(v); }
                if (p) { strncpy(pass, p, sizeof(pass)-1); free(p); }
                free(qs);
            }
            if (id <= 0 || pass[0]==0) {
                send_text(client, "400 Bad Request", "text/plain", "Missing id or pass (use the sign-in form).");
                close(client); return;
            }
            int idx = api_find_index_by_id(id);
            if (idx == -1) { send_text(client, "404 Not Found", "text/plain", "Student not found"); close(client); return; }
            if (strcmp(pass, students[idx].password) != 0) { send_text(client, "401 Unauthorized", "text/plain", "Wrong password"); close(client); return; }
            char *page = build_student_dashboard(idx);
            if (!page) send_text(client, "500 Internal Server Error", "text/plain", "Server error");
            else { send_text(client, "200 OK", "text/html; charset=utf-8", page); free(page); }
            close(client); return;
        }
    } else if (strcmp(method, "POST") == 0) {
        /* find body */
        char *body = strstr(req, "\r\n\r\n");
        if (!body) { send_text(client, "400 Bad Request", "text/plain", "No body"); close(client); return; }
        body += 4;

        /* ADMIN LOGIN (returns admin dashboard page with forms for admin actions) */
        if (strncmp(path, "/admin-login", 12) == 0) {
            char *user = form_value(body, "username");
            char *pass = form_value(body, "password");
            if (!user || !pass) {
                send_text(client, "400 Bad Request", "text/plain", "Missing username or password");
                if (user) free(user); if (pass) free(pass);
                close(client); return;
            }
            int ok = api_admin_auth(user, pass);
            free(user); free(pass);
            if (!ok) { send_text(client, "401 Unauthorized", "text/plain", "Invalid admin credentials"); close(client); return; }
            /* Admin dashboard content (simple) */
            const char *adm =
              "<!doctype html><html><head><meta charset='utf-8'><title>Admin Dashboard</title>"
              "<style>body{font-family:Arial;margin:18px} .card{max-width:900px;padding:18px;border-radius:10px;background:#fff;border:1px solid #eee} input,button{padding:8px;margin:6px 0;width:100%} button{background:#0b69ff;color:#fff;border:none;border-radius:6px}</style></head><body>"
              "<div class='card'><h2>Admin Dashboard</h2>"
              "<p>Use the forms below to manage the system. (Demo: admin credentials are required on each form.)</p>"
              "<h3>View all students</h3><p><a href='/list'>Open students list</a></p>"
              "<h3>Add student (admin)</h3>"
              "<form method='post' action='/add'>"
              "<input name='name' placeholder='Full name' required />"
              "<input name='age' placeholder='Age' required />"
              "<input name='dept' placeholder='Dept' required />"
              "<input name='year' placeholder='Year' required />"
              "<input name='num_subjects' placeholder='Number of subjects' required />"
              "<input name='subjects' placeholder='Subjects comma-separated' required />"
              "<input name='password' placeholder='Password' required />"
              "<button>Add student</button></form>"
              "<h3>Enter marks (admin)</h3>"
              "<form method='post' action='/enter-marks'>"
              "<input name='id' placeholder='Student ID' required />"
              "<textarea name='marks' rows='6' placeholder='90,3\\n85,4\\n78,3'></textarea>"
              "<button>Submit marks</button></form>"
              "<h3>Generate report</h3>"
              "<form method='post' action='/generate'>"
              "<input name='id' placeholder='Student ID' required />"
              "<input name='college' placeholder='College' />"
              "<input name='semester' placeholder='Semester' />"
              "<input name='exam' placeholder='Exam' />"
              "<button>Generate</button></form>"
              "<p><a href='/'>Back</a></p></div></body></html>";
            send_text(client, "200 OK", "text/html; charset=utf-8", adm);
            close(client); return;
        }

        /* STUDENT SIGNUP - reuse existing add-file style but from form */
              if (strncmp(path, "/student-signup", 16) == 0) {
            char *name = form_value(body, "name");
            char *age = form_value(body, "age");
            char *sap = form_value(body, "sap_id");
            char *password = form_value(body, "password");
            if (!name || !age || !sap || !password) {
                send_text(client, "400 Bad Request", "text/plain", "Missing fields");
                goto signup_cleanup;
            }

            /* Validate sap numeric-ish (optional) and convert */
            int sapid = atoi(sap);
            if (sapid <= 0) {
                /* respond with friendly message */
                char resp[256];
                snprintf(resp, sizeof(resp), "<!doctype html><html><body><p>Invalid SAP ID provided. Use numeric SAP ID (e.g. 590012345).</p><p><a href='/'>Back</a></p></body></html>");
                send_text(client, "400 Bad Request", "text/html; charset=utf-8", resp);
                goto signup_cleanup;
            }

            Student s; memset(&s, 0, sizeof(s));
            s.exists = 1; s.cgpa = 0.0; s.total_credits_completed = 0;
            /* basic fields only; subjects/marks empty for now */
            safe_strncpy(s.name, name, sizeof(s.name));
            s.age = atoi(age);
            safe_strncpy(s.dept, "Not set", sizeof(s.dept)); /* optional placeholder */
            s.year = 0;
            s.num_subjects = 0;
            s.id = sapid; /* Important: use supplied SAP ID as student id */
            safe_strncpy(s.password, password, sizeof(s.password));

            /* Call API to add student; returns -2 if duplicate */
            int addres = api_add_student(&s);
            if (addres == -2) {
                char resp[256];
                snprintf(resp, sizeof(resp), "<!doctype html><html><body><p>SAP ID %d already registered. Try signing in.</p><p><a href='/'>Back</a></p></body></html>", s.id);
                send_text(client, "409 Conflict", "text/html; charset=utf-8", resp);
            } else if (addres <= 0) {
                send_text(client, "500 Internal Server Error", "text/plain", "Unable to register");
            } else {
                char resp[256];
                snprintf(resp, sizeof(resp), "<!doctype html><html><body><p>Registration successful. Your Student ID (SAP ID): <strong>%d</strong></p><p><a href='/'>Back to home</a></p></body></html>", addres);
                send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            }

        signup_cleanup:
            if (name) free(name);
            if (age) free(age);
            if (sap) free(sap);
            if (password) free(password);
            close(client); return;
        }


        /* ADD student (admin / form) - reuses earlier /add logic used previously */
        if (strncmp(path, "/add", 4) == 0) {
            char *name = form_value(body, "name");
            char *age = form_value(body, "age");
            char *dept = form_value(body, "dept");
            char *year = form_value(body, "year");
            char *num_sub = form_value(body, "num_subjects");
            char *subjects = form_value(body, "subjects");
            char *password = form_value(body, "password");
            if (!name || !age || !dept || !year || !num_sub || !subjects || !password) {
                send_text(client, "400 Bad Request", "text/plain", "Missing fields");
                goto add_cleanup;
            }
            Student s; memset(&s, 0, sizeof(s));
            s.exists = 1; s.cgpa = 0.0; s.total_credits_completed = 0;
            strncpy(s.name, name, sizeof(s.name)-1);
            s.age = atoi(age);
            strncpy(s.dept, dept, sizeof(s.dept)-1);
            s.year = atoi(year);
            s.num_subjects = atoi(num_sub);
            if (s.num_subjects < 1 || s.num_subjects > MAX_SUBJECTS) s.num_subjects = MAX_SUBJECTS;
            char *tmp = strdup(subjects);
            char *tok = strtok(tmp, ",");
            int si = 0;
            while (tok && si < s.num_subjects) {
                while (*tok == ' ') tok++;
                strncpy(s.subjects[si].name, tok, sizeof(s.subjects[si].name)-1);
                s.subjects[si].classes_held = s.subjects[si].classes_attended = s.subjects[si].marks = s.subjects[si].credits = 0;
                si++; tok = strtok(NULL, ",");
            }
            free(tmp);
            strncpy(s.password, password, sizeof(s.password)-1);
            api_add_student(&s);
            send_text(client, "200 OK", "text/html; charset=utf-8", "<p>Student added. <a href='/'>Back</a></p>");
        add_cleanup:
            if (name) free(name);
            if (age) free(age);
            if (dept) free(dept);
            if (year) free(year);
            if (num_sub) free(num_sub);
            if (subjects) free(subjects);
            if (password) free(password);
            close(client); return;
        }

        /* ENTER MARKS (admin) */
        if (strncmp(path, "/enter-marks", 12) == 0) {
            char *id_s = form_value(body, "id");
            char *marks = form_value(body, "marks");
            if (!id_s || !marks) {
                send_text(client, "400 Bad Request", "text/plain", "Missing id or marks");
                if (id_s) free(id_s);
                if (marks) free(marks);
                close(client); return;
            }
            int sid = atoi(id_s);
            free(id_s);
            int idx = api_find_index_by_id(sid);
            if (idx == -1) {
                send_text(client, "404 Not Found", "text/plain", "Student not found");
                free(marks); close(client); return;
            }
            char *line = strtok(marks, "\n");
            int i = 0;
            while (line && i < students[idx].num_subjects) {
                int mk = 0, cr = 0;
                if (sscanf(line, "%d,%d", &mk, &cr) == 2) {
                    students[idx].subjects[i].marks = mk;
                    students[idx].subjects[i].credits = cr;
                }
                i++; line = strtok(NULL, "\n");
            }
            free(marks);
            api_calculate_update_cgpa(idx);
            char resp[256];
            snprintf(resp, sizeof(resp), "<p>Marks updated for ID %d. <a href='/'>Back</a></p>", sid);
            send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            close(client); return;
        }

        /* GENERATE REPORT */
        if (strncmp(path, "/generate", 9) == 0) {
            char *id_s = form_value(body, "id");
            char *college = form_value(body, "college");
            char *semester = form_value(body, "semester");
            char *exam = form_value(body, "exam");
            if (!id_s) {
                send_text(client, "400 Bad Request", "text/plain", "Missing id");
                if (college) free(college);
                if (semester) free(semester);
                if (exam) free(exam);
                close(client); return;
            }
            int sid = atoi(id_s);
            free(id_s);
            int idx = api_find_index_by_id(sid);
            if (idx == -1) { send_text(client, "404 Not Found", "text/plain", "Student not found"); if (college) free(college); if (semester) free(semester); if (exam) free(exam); close(client); return; }
            char cbuf[256] = "Your College", sbuf[128] = "Semester -", ebuf[128] = "Exam -";
            if (college) { strncpy(cbuf, college, sizeof(cbuf)-1); free(college); }
            if (semester) { strncpy(sbuf, semester, sizeof(sbuf)-1); free(semester); }
            if (exam) { strncpy(ebuf, exam, sizeof(ebuf)-1); free(exam); }
            api_generate_report(idx, cbuf, sbuf, ebuf);
            char resp[256];
            snprintf(resp, sizeof(resp), "<p>Report generated: reports/%d_result.html <a href='/'>Back</a></p>", sid);
            send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            close(client); return;
        }

        /* unknown POST */
        send_text(client, "404 Not Found", "text/plain", "Not found");
        close(client); return;
    }

    /* fallback for other methods */
    send_text(client, "405 Method Not Allowed", "text/plain", "Method not allowed");
    close(client);
}

/* main: single-threaded iterative server */
int main(int argc, char **argv) {
    const char *portenv = getenv("PORT");
    int port = portenv ? atoi(portenv) : 8080;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(server_fd); return 1; }
    if (listen(server_fd, 10) < 0) { perror("listen"); close(server_fd); return 1; }

    ensure_reports_dir();
    fprintf(stderr, "Student system web server listening on port %d\n", port);
    fflush(stderr);

    while (1) {
        struct sockaddr_in cli; socklen_t cli_len = sizeof(cli);
        int client = accept(server_fd, (struct sockaddr*)&cli, &cli_len);
        if (client < 0) { perror("accept"); continue; }
        handle_client(client);
    }

    close(server_fd);
    return 0;
}


