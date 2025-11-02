/* student_system_web.c
   Web wrapper for student_system.c
   - Landing: Admin login / Student signup / Student signin
   - Student signup includes email, phone, semester (auto-adds semester subjects)
   - Admin: select semester -> choose subject(s) -> mark attendance
   - Admin: enter marks -> input student id -> auto-select current semester -> show semester subjects in a table and submit marks
   - Student dashboard: semester-bifurcated subjects (latest sem first), semester-wise attendance distribution, marks, SGPA, CGPA

   Build with:
     gcc -DBUILD_WEB student_system.c student_system_web.c -o student_system_web
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasestr
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

/* --- Keep structs consistent with student_system.c --- */
#define MAX_NAME 100
#define MAX_SUBJECTS 64
#define MAX_SUB_NAME 100

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
    char email[120];
    char phone[32];
    char dept[MAX_NAME];
    int year;
    int current_semester;
    int num_subjects;
    Subject subjects[MAX_SUBJECTS];
    char password[50];
    int exists;
    double cgpa;
    int total_credits_completed;
} Student;

/* --- externs from student_system.c --- */
/* globals */
extern Student students[];
extern int student_count;

/* APIs */
extern int api_find_index_by_id(int id);
extern int api_add_student(Student *s);
extern void api_generate_report(int idx, const char* college, const char* semester, const char* exam);
extern int api_calculate_update_cgpa(int idx);
extern int api_admin_auth(const char *user, const char *pass);

/* helpers (implemented in student_system.c) */
extern void save_data(void);
extern void load_data(void);

/* filesystem helper */
static void ensure_reports_dir(void) {
    struct stat st;
    if (stat("reports", &st) == -1) {
        mkdir("reports", 0755);
    }
}

/* tiny url-decode inplace */
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

/* html escape small function */
static void html_escape_buf(const char *in, char *out, size_t outcap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 7 < outcap; ++i) {
        char c = in[i];
        if (c == '&') { strcpy(out + j, "&amp;"); j += 5; }
        else if (c == '<') { strcpy(out + j, "&lt;"); j += 4; }
        else if (c == '>') { strcpy(out + j, "&gt;"); j += 4; }
        else if (c == '"') { strcpy(out + j, "&quot;"); j += 6; }
        else out[j++] = c;
    }
    out[j] = 0;
}

/* quick grade mapping (same as student_system.c) */
static int marks_to_grade_point_local(int marks) {
    if (marks >= 90) return 10;
    if (marks >= 80) return 9;
    if (marks >= 70) return 8;
    if (marks >= 60) return 7;
    if (marks >= 50) return 6;
    if (marks >= 40) return 5;
    return 0;
}

/* compute SGPA locally (per-semester or overall depending subjects passed) */
static double compute_sgpa_local_for_subjects(Subject *subs, int n) {
    int total_credits = 0;
    double weighted = 0.0;
    for (int i = 0; i < n; ++i) {
        int cr = subs[i].credits;
        int mk = subs[i].marks;
        if (cr <= 0) continue;
        int gp = marks_to_grade_point_local(mk);
        weighted += (double)gp * cr;
        total_credits += cr;
    }
    if (total_credits == 0) return 0.0;
    return weighted / (double)total_credits;
}

/* helper to slugify subject for filename */
static void slugify(const char *in, char *out, size_t outcap) {
    size_t j=0;
    for (size_t i=0; in[i] && j+1<outcap; ++i) {
        char c = in[i];
        if (isalnum((unsigned char)c)) out[j++]=tolower(c);
        else if (c==' '||c=='_'||c=='-') {
            if (j>0 && out[j-1]!='-') out[j++]='-';
        }
    }
    if (j>0 && out[j-1]=='-') j--;
    out[j]=0;
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

/* build landing page (signup includes extra fields) */
static char *build_landing_page(void) {
    ensure_reports_dir();
    const char *html_start =
        "<!doctype html><html><head><meta charset='utf-8'><title>Student System</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>"
        "body{margin:0;font-family:Inter,Arial,Helvetica,sans-serif;background:linear-gradient(135deg,#f0f6ff 0%,#ffffff 40%,#f7f2ff 100%);min-height:100vh;display:flex;align-items:center;justify-content:center}"
        ".wrap{max-width:1100px;width:95%;margin:40px auto;background:rgba(255,255,255,0.95);border-radius:12px;padding:26px;box-shadow:0 8px 28px rgba(20,20,50,0.08)}"
        "h1{margin:0;font-size:28px;color:#12263a} p.lead{color:#4b5563}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:16px;margin-top:18px}"
        ".card{background:#fff;border-radius:10px;padding:18px;border:1px solid rgba(20,20,60,0.04)}"
        ".card h3{margin:0 0 8px 0} .card p{margin:0 0 12px 0;color:#333}"
        "input,textarea,button,select{width:100%;padding:8px;border-radius:6px;border:1px solid #e6eef8;font-size:14px}"
        "button{cursor:pointer;background:linear-gradient(180deg,#2b6ef6,#215bd6);color:white;border:none;padding:10px 12px}"
        ".small{font-size:13px;color:#6b7280} .muted{color:#6b7280;font-size:13px;margin-top:8px}"
        "@media(max-width:600px){.wrap{padding:14px}}"
        "</style></head><body><div class='wrap'>"
        "<h1>Student Record & Result Management</h1>"
        "<p class='lead'>Choose an option to continue — Admin login, Student sign up, or Student sign in.</p>"
        "<div class='grid'>";

    const char *admin_card =
        "<div class='card'>"
        "<h3>Admin Login</h3>"
        "<p>Full admin control: manage students, marks, attendance.</p>"
        "<form method='post' action='/admin-login'>"
        "<input name='username' placeholder='Admin username' required />"
        "<input name='password' placeholder='Admin password' type='password' required />"
        "<div style='margin-top:8px'><button>Login as Admin</button></div>"
        "</form>"
        "</div>";

    const char *signup_card =
        "<div class='card'>"
        "<h3>Student Sign Up</h3>"
        "<p>Register (semester subjects will be added automatically).</p>"
        "<form method='post' action='/student-signup'>"
        "<input name='name' placeholder='Full Name' required />"
        "<input name='age' placeholder='Age' required />"
        "<input name='sap_id' placeholder='SAP ID (numeric)' required />"
        "<input name='email' placeholder='Email' required />"
        "<input name='phone' placeholder='Phone' required />"
        "<select name='semester' required>"
        "<option value='1'>Semester 1</option>"
        "<option value='2'>Semester 2</option>"
        "<option value='3'>Semester 3</option>"
        "<option value='4'>Semester 4</option>"
        "<option value='5'>Semester 5</option>"
        "<option value='6'>Semester 6</option>"
        "<option value='7'>Semester 7</option>"
        "<option value='8'>Semester 8</option>"
        "</select>"
        "<input name='password' placeholder='Password' type='password' required />"
        "<div style='margin-top:8px'><button>Sign Up</button></div>"
        "</form>"
        "<p class='muted'>Use your SAP ID and password to sign in after registration.</p>"
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

/* build simple student list HTML (used for admin to choose subject for attendance etc.) */
static char *build_list_html(void) {
    size_t cap = 8192;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, "<!doctype html><html><head><meta charset='utf-8'><title>Students</title></head><body><h2>Students</h2><table border='1' cellpadding='6'><tr><th>ID</th><th>Name</th><th>Year</th><th>Dept</th><th>Sem</th></tr>");
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        char row[1024];
        snprintf(row, sizeof(row), "<tr><td>%d</td><td>%s</td><td>%d</td><td>%s</td><td>%d</td></tr>", students[i].id, students[i].name, students[i].year, students[i].dept, students[i].current_semester);
        if (strlen(buf) + strlen(row) + 256 > cap) { cap *= 2; buf = realloc(buf, cap); }
        strcat(buf, row);
    }
    strcat(buf, "</table><p><a href='/'>Back</a></p></body></html>");
    return buf;
}

/* helper: collect unique subjects per semester and overall */
typedef struct {
    char name[MAX_SUB_NAME];
    int semester; /* 0 = unknown, else 1..8 */
} UniqueSub;

static int map_subject_to_semester(const char *s) {
    /* quick mapping based on the semester arrays in student_system.c
       This mapping mirrors sem_subject lists: if names change on C side, update mapping here.
       We'll implement a simple search across student data to estimate semester:
       - If a student in sem X has this subject present and current_semester>=X, we guess sem X.
       Fallback: 0 (unknown).
    */
    /* This wrapper cannot see sem_subjects directly (they are inside student_system.c),
       so we approximate by scanning students for the subject and using their subject positions.
       We'll pick most-common semester among students who have this subject.
    */
    int counts[9] = {0}; /* 0..8 */
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        for (int j = 0; j < students[i].num_subjects; ++j) {
            if (strcmp(students[i].subjects[j].name, s) == 0) {
                int sem = students[i].current_semester;
                if (sem < 1 || sem > 8) sem = 0;
                counts[sem]++;
            }
        }
    }
    int best = 0; int bestc = 0;
    for (int k=1;k<=8;++k) if (counts[k] > bestc) { bestc = counts[k]; best = k; }
    return best;
}

/* Build student dashboard as HTML with attendance & marks, grouped by semester (latest first) */
static char *build_student_dashboard(int idx) {
    if (idx < 0 || idx >= student_count) return NULL;
    Student *s = &students[idx];
    char escaped_name[256]; html_escape_buf(s->name, escaped_name, sizeof(escaped_name));
    /* Group subjects by semester using map_subject_to_semester */
    Subject *bysem[9][MAX_SUBJECTS]; /* pointers into s->subjects */
    int bysem_count[9] = {0};
    for (int i=0;i<9;++i) for (int j=0;j<MAX_SUBJECTS;++j) bysem[i][j]=NULL;
    for (int i = 0; i < s->num_subjects; ++i) {
        int sem = map_subject_to_semester(s->subjects[i].name);
        if (sem < 0 || sem > 8) sem = 0;
        bysem[sem][ bysem_count[sem]++ ] = &s->subjects[i];
    }
    /* choose order: latest semester first, then descending, then unknown (0) last */
    int order[9]; int ordc=0;
    for (int ss = s->current_semester; ss >= 1; --ss) { order[ordc++] = ss; }
    for (int ss = 8; ss >= 1; --ss) { /* ensure we include sems > current if present */ 
        int found = 0; for (int k=0;k<ordc;++k) if (order[k]==ss) { found=1; break; }
        if (!found && bysem_count[ss]>0) order[ordc++]=ss;
    }
    if (bysem_count[0] > 0) order[ordc++]=0;

    /* Build HTML */
    const char *tpl_start =
        "<!doctype html><html><head><meta charset='utf-8'><title>Dashboard</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>body{font-family:Inter,Arial;margin:18px} .card{background:#fff;padding:18px;border-radius:10px;box-shadow:0 6px 18px rgba(0,0,0,0.06);max-width:1000px;margin:auto} table{width:100%;border-collapse:collapse} table th,table td{padding:8px;border:1px solid #eee;text-align:left;font-size:14px}</style>"
        "</head><body><div class='card'>";

    const char *tpl_end = "<p><a href='/'>← Back to Home</a></p></div></body></html>";

    size_t cap = strlen(tpl_start) + 16384;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, tpl_start);
    char header[1024];
    char dept_esc[256]; html_escape_buf(s->dept, dept_esc, sizeof(dept_esc));
    double cur_sgpa = compute_sgpa_local_for_subjects(s->subjects, s->num_subjects);
    snprintf(header, sizeof(header),
             "<h2>Welcome, %s</h2><p>ID: %d | Dept: %s | Year: %d | Current Semester: %d | Age: %d</p>"
             "<p><strong>SGPA (computed):</strong> %.3f  &nbsp;&nbsp; <strong>Stored CGPA:</strong> %.3f (Credits: %d)</p>",
             escaped_name, s->id, dept_esc, s->year, s->current_semester, s->age, cur_sgpa, s->cgpa, s->total_credits_completed);
    strcat(buf, header);

    /* Per-semester sections */
    for (int oi=0; oi<ordc; ++oi) {
        int sem = order[oi];
        char sec[4096]; sec[0]=0;
        if (sem == 0) snprintf(sec, sizeof(sec), "<h3>Other / Unknown Semester Subjects</h3>");
        else snprintf(sec, sizeof(sec), "<h3>Semester %d</h3>", sem);
        strcat(buf, sec);

        /* attendance summary for this semester */
        int total_held = 0, total_att = 0;
        for (int i=0;i<bysem_count[sem];++i) {
            total_held += bysem[sem][i]->classes_held;
            total_att += bysem[sem][i]->classes_attended;
        }
        double pct = (total_held == 0) ? 0.0 : ((double)total_att / total_held) * 100.0;
        char attline[256];
        snprintf(attline, sizeof(attline), "<p>Semester attendance: %d classes held overall, %d attended (%.1f%%)</p>", total_held, total_att, pct);
        strcat(buf, attline);

        /* subject table */
        strcat(buf, "<table><tr><th>#</th><th>Subject</th><th>Marks</th><th>Credits</th><th>GradePoint</th><th>Attendance</th></tr>");
        for (int i=0;i<bysem_count[sem];++i) {
            Subject *sub = bysem[sem][i];
            int held = sub->classes_held;
            int att = sub->classes_attended;
            int pct_sub = (held==0)?0:(int)(((double)att/held)*100.0 + 0.5);
            int gp = marks_to_grade_point_local(sub->marks);
            char sname_esc[256]; html_escape_buf(sub->name, sname_esc, sizeof(sname_esc));
            char row[512];
            snprintf(row, sizeof(row), "<tr><td>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%d</td><td>%d%% (%d/%d)</td></tr>",
                     i+1, sname_esc, sub->marks, sub->credits, gp, pct_sub, att, held);
            if (strlen(buf) + strlen(row) + 256 > cap) { cap *= 2; buf = realloc(buf, cap); }
            strcat(buf, row);
        }
        strcat(buf, "</table><br/>");
    }

    strcat(buf, tpl_end);
    return buf;
}

/* build admin attendance semester selection page */
static char *build_attendance_sem_select_page(void) {
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, "<!doctype html><html><head><meta charset='utf-8'><title>Attendance - Choose Semester</title></head><body><h2>Mark Attendance - Step 1: Choose Semester</h2>");
    strcat(buf, "<form method='get' action='/attendance-subjects'>Select semester: <select name='semester'>");
    for (int i=1;i<=8;++i) {
        char opt[64]; snprintf(opt, sizeof(opt), "<option value='%d'>Semester %d</option>", i, i);
        strcat(buf, opt);
    }
    strcat(buf, "</select> <button>Next</button></form><p><a href='/'>Back</a></p></body></html>");
    return buf;
}

/* build subject checklist for a selected semester (only subjects that exist for at least one student in that semester) */
static char *build_attendance_subjects_page(int semester, const char *err) {
    size_t cap = 16384;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    snprintf(buf, cap, "<!doctype html><html><head><meta charset='utf-8'><title>Attendance - Subjects Sem %d</title></head><body><h2>Mark Attendance - Step 2: Choose Subject(s) - Semester %d</h2>", semester, semester);
    if (err && err[0]) { strncat(buf, "<p style='color:red;'>", cap - strlen(buf) -1); strncat(buf, err, cap - strlen(buf) -1); strncat(buf, "</p>", cap - strlen(buf) -1); }
    strncat(buf, "<form method='get' action='/attendance-mark'>", cap - strlen(buf) -1);
    char list_title[256]; snprintf(list_title, sizeof(list_title), "<input type='hidden' name='semester' value='%d'/>", semester);
    strncat(buf, list_title, cap - strlen(buf) -1);

    /* collect unique subjects for this semester - only include subjects that are present in at least one student whose current_semester == semester */
    char added[8192]; added[0]=0;
    int found_any = 0;
    for (int i=0;i<student_count;++i) {
        if (!students[i].exists) continue;
        if (students[i].current_semester != semester) continue;
        for (int j=0;j<students[i].num_subjects;++j) {
            const char *sname = students[i].subjects[j].name;
            if (strstr(added, sname) == NULL) {
                if (strlen(added) + strlen(sname) + 16 < sizeof(added)) {
                    strcat(added, sname);
                    strcat(added, "\n");
                }
            }
        }
    }
    if (strlen(added) == 0) {
        strncat(buf, "<p>No subjects found for that semester (no students in that semester).</p>", cap - strlen(buf) -1);
        strncat(buf, "<p><a href='/attendance'>Back</a></p></form></body></html>", cap - strlen(buf) -1);
        return buf;
    }

    char *copy = strdup(added);
    char *line = strtok(copy, "\n");
    int idx = 0;
    strncat(buf, "<ul style='list-style:none;padding-left:0;'>", cap - strlen(buf) -1);
    while (line) {
        char esc[256]; html_escape_buf(line, esc, sizeof(esc));
        char chk[512];
        snprintf(chk, sizeof(chk), "<li><label><input type='checkbox' name='subject' value=\"%s\"/> %s</label></li>", esc, esc);
        strncat(buf, chk, cap - strlen(buf) -1);
        idx++;
        line = strtok(NULL, "\n");
    }
    free(copy);
    strncat(buf, "</ul><div style='margin-top:8px'><button>Open mark page</button></div></form><p><a href='/attendance'>Back</a></p></body></html>", cap - strlen(buf) -1);
    return buf;
}

/* build attendance marking page: shows students who are in selected semester and selected subject(s) with checkboxes */
static char *build_attendance_mark_page(int semester, char **subjects, int subj_count) {
    size_t cap = 32768;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    snprintf(buf, cap, "<!doctype html><html><head><meta charset='utf-8'><title>Attendance - Mark</title></head><body><h2>Mark Attendance - Step 3: Mark Present/Absent</h2><form method='post' action='/attendance'>");
    /* hidden semester */
    char hsem[128]; snprintf(hsem, sizeof(hsem), "<input type='hidden' name='semester' value='%d'/>", semester);
    strncat(buf, hsem, cap - strlen(buf) -1);
    /* hidden subjects (multiple) */
    for (int i=0;i<subj_count;++i) {
        char esc[256]; html_escape_buf(subjects[i], esc, sizeof(esc));
        char hs[512]; snprintf(hs, sizeof(hs), "<input type='hidden' name='subject' value=\"%s\"/>", esc);
        strncat(buf, hs, cap - strlen(buf) -1);
    }

    /* date */
    time_t t = time(NULL); struct tm tm = *localtime(&t); char datebuf[32];
    snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
    char dateinp[256]; snprintf(dateinp, sizeof(dateinp), "Date (YYYY-MM-DD): <input name='date' value='%s'/>", datebuf);
    strncat(buf, dateinp, cap - strlen(buf) -1);

    /* table header */
    strncat(buf, "<table border='1' cellpadding='6'><tr><th>Student ID</th><th>Name</th>", cap - strlen(buf) -1);
    for (int i=0;i<subj_count;++i) {
        char esc[256]; html_escape_buf(subjects[i], esc, sizeof(esc));
        char th[256]; snprintf(th, sizeof(th), "<th>%s (Present)</th>", esc);
        strncat(buf, th, cap - strlen(buf) -1);
    }
    strncat(buf, "</tr>", cap - strlen(buf) -1);

    int rows = 0;
    for (int i=0;i<student_count;++i) {
        if (!students[i].exists) continue;
        if (students[i].current_semester != semester) continue;
        /* check student has all selected subjects (or at least one) */
        int has_any = 0;
        for (int si=0; si<subj_count; ++si) {
            for (int j=0;j<students[i].num_subjects;++j) {
                if (strcmp(students[i].subjects[j].name, subjects[si])==0) { has_any = 1; break; }
            }
            if (has_any) break;
        }
        if (!has_any) continue;
        /* build row */
        char row[2048]; char cells[1024]; cells[0]=0;
        for (int si=0; si<subj_count; ++si) {
            char cb[256]; snprintf(cb, sizeof(cb), "<td><input type='checkbox' name='present_%d' value='%d'/></td>", si, students[i].id);
            strncat(cells, cb, sizeof(cells)-strlen(cells)-1);
        }
        snprintf(row, sizeof(row), "<tr><td>%d</td><td>%s</td>%s</tr>", students[i].id, students[i].name, cells);
        if (strlen(buf) + strlen(row) + 256 > cap) { cap *= 2; buf = realloc(buf, cap); }
        strcat(buf, row);
        rows++;
    }
    if (rows == 0) {
        strncat(buf, "<tr><td colspan='10'>No students found for the selected semester/subjects.</td></tr>", cap - strlen(buf) -1);
    }
    strncat(buf, "</table><div style='margin-top:8px'><button>Mark Attendance</button></div></form><p><a href='/attendance'>Back</a></p></body></html>", cap - strlen(buf) -1);
    return buf;
}

/* Build admin marks entry: first page ask for student id (or choose from list) */
static char *build_marks_enter_id_page(const char *msg) {
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, "<!doctype html><html><head><meta charset='utf-8'><title>Enter Marks - Student</title></head><body><h2>Enter Marks - Step 1: Enter Student ID</h2>");
    if (msg && msg[0]) { strncat(buf, "<p style='color:red;'>", cap - strlen(buf) -1); strncat(buf, msg, cap - strlen(buf) -1); strncat(buf, "</p>", cap - strlen(buf) -1); }
    strcat(buf, "<form method='get' action='/enter-marks-student'>Student ID: <input name='id' required/> <button>Open</button></form>");
    strcat(buf, "<h3>Or choose from list</h3><ul>");
    for (int i=0;i<student_count;++i) {
        if (!students[i].exists) continue;
        char li[256]; snprintf(li, sizeof(li), "<li><a href='/enter-marks-student?id=%d'>%d - %s (sem %d)</a></li>", students[i].id, students[i].id, students[i].name, students[i].current_semester);
        strncat(buf, li, cap - strlen(buf) -1);
    }
    strcat(buf, "</ul><p><a href='/'>Back</a></p></body></html>");
    return buf;
}

/* Build marks entry page for a student: auto-selects current semester and shows only subjects from that semester */
static char *build_marks_table_page_for_student(int sid, const char *msg) {
    int idx = api_find_index_by_id(sid);
    if (idx == -1) return NULL;
    Student *s = &students[idx];
    size_t cap = 32768;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    snprintf(buf, cap, "<!doctype html><html><head><meta charset='utf-8'><title>Enter Marks for %d</title></head><body><h2>Enter Marks - %s (ID %d)</h2>", s->current_semester, s->name, s->id);
    if (msg && msg[0]) { strncat(buf, "<p style='color:red;'>", cap - strlen(buf) -1); strncat(buf, msg, cap - strlen(buf) -1); strncat(buf, "</p>", cap - strlen(buf) -1); }
    /* Build list of subjects that belong to student's current semester (approx by checking students in system) */
    /* We'll include subjects that student has and that we can reasonably say belong to current semester (by matching their presence) */
    /* For simplicity: include any subject that the student has whose name appears among subjects for other students in same current_semester OR simply where its semester inferred equals current_semester */
    char subj_names[8192]; subj_names[0]=0;
    for (int j=0;j<s->num_subjects;++j) {
        /* include only if this subject appears as part of student's set and student's current_semester matches or it's one of the later semester subjects */
        /* We'll simply pick subjects where a majority of students who have that subject are in same sem — but to keep it simple, include if s->current_semester matches student's own current_semester (always true) */
        /* So show only subjects which the student has AND (we approximate semester match by checking if many students with this subject are in current_semester) */
        int sem_count = 0, other_count=0;
        for (int i=0;i<student_count;++i) {
            if (!students[i].exists) continue;
            for (int k=0;k<students[i].num_subjects;++k) {
                if (strcmp(students[i].subjects[k].name, s->subjects[j].name)==0) {
                    if (students[i].current_semester == s->current_semester) sem_count++;
                    other_count++;
                    break;
                }
            }
        }
        /* include if sem_count >= other_count/2 (i.e. common in this semester), or if other_count==0 */
        if (other_count == 0 || sem_count * 2 >= other_count) {
            if (strlen(subj_names) + strlen(s->subjects[j].name) + 8 < sizeof(subj_names)) {
                strcat(subj_names, s->subjects[j].name);
                strcat(subj_names, "\n");
            }
        }
    }

    if (strlen(subj_names) == 0) {
        strncat(buf, "<p>No subjects found for the student's current semester. Showing all subjects instead.</p>", cap - strlen(buf) -1);
        for (int j=0;j<s->num_subjects;++j) {
            strncat(subj_names, s->subjects[j].name, sizeof(subj_names)-strlen(subj_names)-1);
            strncat(subj_names, "\n", sizeof(subj_names)-strlen(subj_names)-1);
        }
    }

    /* form */
    strncat(buf, "<form method='post' action='/enter-marks'><input type='hidden' name='id' value='", cap - strlen(buf) -1);
    char idbuf[64]; snprintf(idbuf, sizeof(idbuf), "%d", s->id);
    strncat(buf, idbuf, cap - strlen(buf) -1);
    strncat(buf, "'/>", cap - strlen(buf) -1);

    strncat(buf, "<table border='1' cellpadding='6'><tr><th>Subject</th><th>Marks (0-100)</th></tr>", cap - strlen(buf) -1);
    char *c = strdup(subj_names);
    char *line = strtok(c, "\n");
    while (line) {
        /* find current marks for subject */
        int mk = 0;
        for (int j=0;j<s->num_subjects;++j) if (strcmp(s->subjects[j].name, line)==0) { mk = s->subjects[j].marks; break; }
        char esc[256]; html_escape_buf(line, esc, sizeof(esc));
        char row[512]; snprintf(row, sizeof(row), "<tr><td>%s</td><td><input name='m_%s' value='%d' /></td></tr>", esc, esc, mk);
        if (strlen(buf) + strlen(row) + 256 > cap) { cap *= 2; buf = realloc(buf, cap); }
        strcat(buf, row);
        line = strtok(NULL, "\n");
    }
    free(c);
    strncat(buf, "</table><div style='margin-top:8px'><button>Submit Marks</button></div></form><p><a href='/admin'>Back</a></p></body></html>", cap - strlen(buf) -1);
    return buf;
}

/* handle a client connection */
static void handle_client(int client) {
    char req[REQBUF];
    int r = read_request(client, req, sizeof(req));
    if (r <= 0) { close(client); return; }

    char method[8] = {0}, fullpath[1024] = {0}, proto[32] = {0};
    sscanf(req, "%7s %1023s %31s", method, fullpath, proto);

    /* separate path and query */
    char path[1024]; strcpy(path, fullpath);
    char *qmark = strchr(path, '?');
    if (qmark) *qmark = 0;

    /* GET handlers */
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

        /* dashboard query: id and pass */
        if (strncmp(path, "/dashboard", 10) == 0) {
            /* parse query string after ? */
            char *q = strchr(fullpath, '?');
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

        /* Attendance: Step 1 - choose semester */
        if (strncmp(path, "/attendance", 10) == 0) {
            /* if query contains semester -> redirect to subject selection */
            char *q = strchr(fullpath, '?');
            if (!q) {
                char *page = build_attendance_sem_select_page();
                send_text(client, "200 OK", "text/html; charset=utf-8", page);
                free(page); close(client); return;
            } else {
                /* forward to attendance subject selection handler path /attendance-subjects */
                /* To keep REST simple, we provide a separate route /attendance-subjects */
                send_text(client, "302 Found", "text/plain", "Redirecting"); close(client); return;
            }
        }

        if (strncmp(path, "/attendance-subjects", 19) == 0) {
            /* parse query ?semester= */
            char *q = strchr(fullpath, '?');
            int semester = 0;
            if (q) {
                char *qs = strdup(q+1);
                char *sem = form_value(qs, "semester");
                if (sem) { semester = atoi(sem); free(sem); }
                free(qs);
            }
            if (semester < 1 || semester > 8) {
                char *page = build_attendance_sem_select_page();
                send_text(client, "200 OK", "text/html; charset=utf-8", page);
                free(page); close(client); return;
            }
            char *page = build_attendance_subjects_page(semester, NULL);
            send_text(client, "200 OK", "text/html; charset=utf-8", page);
            free(page); close(client); return;
        }

        if (strncmp(path, "/attendance-mark", 15) == 0) {
            /* parse query: semester and subject repeated */
            char *q = strchr(fullpath, '?');
            int semester = 0;
            char *subjects[64]; int subj_count=0;
            if (q) {
                char *qs = strdup(q+1);
                char *sem = form_value(qs, "semester");
                if (sem) { semester = atoi(sem); free(sem); }
                /* collect all 'subject' occurrences by scanning qs */
                const char *p = qs;
                while ((p = strstr(p, "subject=")) != NULL) {
                    p += strlen("subject=");
                    /* read until & or end and unescape */
                    const char *amp = strchr(p, '&');
                    size_t len = amp ? (size_t)(amp - p) : strlen(p);
                    char *val = malloc(len+1);
                    memcpy(val, p, len); val[len]=0; urldecode_inplace(val);
                    if (subj_count < 64) subjects[subj_count++] = val;
                    if (!amp) break;
                    p = amp + 1;
                }
                free(qs);
            }
            if (semester < 1 || subj_count==0) {
                /* redirect to semester select */
                char *page = build_attendance_sem_select_page();
                send_text(client, "200 OK", "text/html; charset=utf-8", page);
                free(page); close(client); return;
            }
            char *page = build_attendance_mark_page(semester, subjects, subj_count);
            for (int i=0;i<subj_count;++i) free(subjects[i]);
            send_text(client, "200 OK", "text/html; charset=utf-8", page);
            free(page); close(client); return;
        }

        /* marks entry: Step 1 page to input student id */
        if (strncmp(path, "/enter-marks", 11) == 0 && strstr(fullpath, "student") == NULL) {
            /* show ID entry page */
            char *page = build_marks_enter_id_page(NULL);
            send_text(client, "200 OK", "text/html; charset=utf-8", page);
            free(page); close(client); return;
        }

        /* marks entry: show student marks table when id provided as query (route /enter-marks-student?id=) */
        if (strncmp(path, "/enter-marks-student", 20) == 0) {
            char *q = strchr(fullpath, '?');
            int sid = 0;
            if (q) {
                char *qs = strdup(q+1);
                char *v = form_value(qs, "id");
                if (v) { sid = atoi(v); free(v); }
                free(qs);
            }
            if (sid <= 0) {
                char *page = build_marks_enter_id_page("Please provide a valid student ID.");
                send_text(client, "200 OK", "text/html; charset=utf-8", page);
                free(page); close(client); return;
            }
            char *page = build_marks_table_page_for_student(sid, NULL);
            if (!page) send_text(client, "404 Not Found", "text/plain", "Student not found");
            else { send_text(client, "200 OK", "text/html; charset=utf-8", page); free(page); }
            close(client); return;
        }

    } /* end GET */

    /* POST handlers */
    if (strcmp(method, "POST") == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if (!body) { send_text(client, "400 Bad Request", "text/plain", "No body"); close(client); return; }
        body += 4;

        /* Admin login */
        if (strncmp(path, "/admin-login", 12) == 0) {
            char *user = form_value(body, "username");
            char *pass = form_value(body, "password");
            if (!user || !pass) {
                send_text(client, "400 Bad Request", "text/plain", "Missing username or password");
                if (user) free(user); if (pass) free(pass);
                close(client); return;
            }
            int ok = api_admin_auth(user, pass); /* uses student_system.c auth */
            free(user); free(pass);
            if (!ok) { send_text(client, "401 Unauthorized", "text/plain", "Invalid admin credentials"); close(client); return; }
            /* admin dashboard with new flows */
            const char *adm =
              "<!doctype html><html><head><meta charset='utf-8'><title>Admin Dashboard</title>"
              "<style>body{font-family:Arial;margin:18px} .card{max-width:900px;padding:18px;border-radius:10px;background:#fff;border:1px solid #eee} input,button,textarea,select{padding:8px;margin:6px 0;width:100%} button{background:#0b69ff;color:#fff;border:none;border-radius:6px}</style></head><body>"
              "<div class='card'><h2>Admin Dashboard</h2>"
              "<p>Manage marks and attendance.</p>"
              "<h3>View all students</h3><p><a href='/list'>Open students list</a></p>"
              "<h3>Enter marks for a student</h3>"
              "<p><a href='/enter-marks'>Enter marks (open by student ID)</a></p>"
              "<h3>Mark attendance</h3>"
              "<p><a href='/attendance'>Start attendance flow (select semester → subject → mark)</a></p>"
              "<p><a href='/'>Back</a></p></div></body></html>";
            send_text(client, "200 OK", "text/html; charset=utf-8", adm);
            close(client); return;
        }

        /* Student sign-up */
        if (strncmp(path, "/student-signup", 16) == 0) {
            char *name = form_value(body, "name");
            char *age = form_value(body, "age");
            char *sap = form_value(body, "sap_id");
            char *password = form_value(body, "password");
            char *email = form_value(body, "email");
            char *phone = form_value(body, "phone");
            char *semester = form_value(body, "semester");
            if (!name || !age || !sap || !password || !email || !phone || !semester) {
                send_text(client, "400 Bad Request", "text/plain", "Missing fields");
                goto signup_cleanup;
            }
            int sapid = atoi(sap);
            int sem = atoi(semester);
            if (sapid <= 0 || sem < 1 || sem > 8) {
                char resp[256];
                snprintf(resp, sizeof(resp),
                    "<!doctype html><html><body><p>Invalid SAP ID or semester provided.</p><p><a href='/'>Back</a></p></body></html>");
                send_text(client, "400 Bad Request", "text/html; charset=utf-8", resp);
                goto signup_cleanup;
            }
            Student s; memset(&s, 0, sizeof(s));
            s.exists = 1; s.cgpa = 0.0; s.total_credits_completed = 0;
            s.id = sapid;
            strncpy(s.name, name, sizeof(s.name)-1); s.name[sizeof(s.name)-1]=0;
            s.age = atoi(age);
            strncpy(s.email, email, sizeof(s.email)-1); s.email[sizeof(s.email)-1]=0;
            strncpy(s.phone, phone, sizeof(s.phone)-1); s.phone[sizeof(s.phone)-1]=0;
            strncpy(s.dept, "B.Tech CSE", sizeof(s.dept)-1); s.dept[sizeof(s.dept)-1]=0;
            s.year = 1;
            s.current_semester = sem;
            s.num_subjects = 0;
            strncpy(s.password, password, sizeof(s.password)-1); s.password[sizeof(s.password)-1]=0;

            /* Default semester subjects arrays are same as student_system.c; we'll add simple lists here */
            const char *sem1[] = {
                "Programming in C","Linux Lab","Problem Solving","Advanced Engineering Mathematics - I","Physics for Computer Engineers","Managing Self","Environmental Sustainability and Climate Change", NULL
            };
            const int sem1_c[] = {5,2,2,4,5,2,2};
            const char *sem2[] = {
                "Data Structures and Algorithms","Digital Electronics","Python Programming","Advanced Engineering Mathematics - II","Environmental Sustainability and Climate Change","Time and Priority Management","Elements of AI/ML", NULL
            };
            const int sem2_c[] = {5,3,5,4,2,2,3};
            const char *sem3[] = {
                "Leading Conversations","Discrete Mathematical Structures","Operating Systems","Elements of AI/ML","Database Management Systems","Design and Analysis of Algorithms", NULL
            };
            const int sem3_c[] = {2,3,3,3,5,4};
            const char *sem4[] = {
                "Software Engineering","EDGE - Soft Skills","Linear Algebra","Indian Constitution","Writing with Impact","Object Oriented Programming","Data Communication and Networks","Applied Machine Learning", NULL
            };
            const int sem4_c[] = {3,0,3,0,2,4,4,5};
            const char *sem5[] = {
                "Cryptography and Network Security","Formal Languages and Automata Theory","Object Oriented Analysis and Design","Exploratory-3","Start your Startup","Research Methodology in CS","Probability, Entropy, and MC Simulation","PE-2","PE-2 Lab", NULL
            };
            const int sem5_c[] = {3,3,3,3,2,3,3,4,1};
            const char *sem6[] = {
                "Exploratory-4","Leadership and Teamwork","Compiler Design","Statistics and Data Analysis","PE-3","PE-3 Lab","Minor Project", NULL
            };
            const int sem6_c[] = {3,2,3,3,4,1,5};
            const char *sem7[] = {
                "Exploratory-5","PE-4","PE-4 Lab","PE-5","PE-5 Lab","Capstone Project - Phase-1","Summer Internship", NULL
            };
            const int sem7_c[] = {3,4,1,3,1,5,1};
            const char *sem8[] = {
                "IT Ethical Practices","Capstone Project - Phase-2", NULL
            };
            const int sem8_c[] = {3,5};

            for (int cur=1; cur<=sem; ++cur) {
                if (cur==1) {
                    for (int i=0; sem1[i]!=NULL; ++i) {
                        if (s.num_subjects < MAX_SUBJECTS) {
                            strncpy(s.subjects[s.num_subjects].name, sem1[i], MAX_SUB_NAME-1); s.subjects[s.num_subjects].name[MAX_SUB_NAME-1]=0;
                            s.subjects[s.num_subjects].credits = sem1_c[i];
                            s.subjects[s.num_subjects].marks = 0;
                            s.subjects[s.num_subjects].classes_held = 0;
                            s.subjects[s.num_subjects].classes_attended = 0;
                            s.num_subjects++;
                        }
                    }
                } else if (cur==2) {
                    for (int i=0; sem2[i]!=NULL; ++i) {
                        if (s.num_subjects < MAX_SUBJECTS) {
                            strncpy(s.subjects[s.num_subjects].name, sem2[i], MAX_SUB_NAME-1); s.subjects[s.num_subjects].name[MAX_SUB_NAME-1]=0;
                            s.subjects[s.num_subjects].credits = sem2_c[i];
                            s.subjects[s.num_subjects].marks = 0;
                            s.subjects[s.num_subjects].classes_held = 0;
                            s.subjects[s.num_subjects].classes_attended = 0;
                            s.num_subjects++;
                        }
                    }
                } else if (cur==3) {
                    for (int i=0; sem3[i]!=NULL; ++i) {
                        if (s.num_subjects < MAX_SUBJECTS) {
                            strncpy(s.subjects[s.num_subjects].name, sem3[i], MAX_SUB_NAME-1); s.subjects[s.num_subjects].name[MAX_SUB_NAME-1]=0;
                            s.subjects[s.num_subjects].credits = sem3_c[i];
                            s.subjects[s.num_subjects].marks = 0;
                            s.subjects[s.num_subjects].classes_held = 0;
                            s.subjects[s.num_subjects].classes_attended = 0;
                            s.num_subjects++;
                        }
                    }
                } else if (cur==4) {
                    for (int i=0; sem4[i]!=NULL; ++i) {
                        if (s.num_subjects < MAX_SUBJECTS) {
                            strncpy(s.subjects[s.num_subjects].name, sem4[i], MAX_SUB_NAME-1); s.subjects[s.num_subjects].name[MAX_SUB_NAME-1]=0;
                            s.subjects[s.num_subjects].credits = sem4_c[i];
                            s.subjects[s.num_subjects].marks = 0;
                            s.subjects[s.num_subjects].classes_held = 0;
                            s.subjects[s.num_subjects].classes_attended = 0;
                            s.num_subjects++;
                        }
                    }
                } else if (cur==5) {
                    for (int i=0; sem5[i]!=NULL; ++i) {
                        if (s.num_subjects < MAX_SUBJECTS) {
                            strncpy(s.subjects[s.num_subjects].name, sem5[i], MAX_SUB_NAME-1); s.subjects[s.num_subjects].name[MAX_SUB_NAME-1]=0;
                            s.subjects[s.num_subjects].credits = sem5_c[i];
                            s.subjects[s.num_subjects].marks = 0;
                            s.subjects[s.num_subjects].classes_held = 0;
                            s.subjects[s.num_subjects].classes_attended = 0;
                            s.num_subjects++;
                        }
                    }
                } else if (cur==6) {
                    for (int i=0; sem6[i]!=NULL; ++i) {
                        if (s.num_subjects < MAX_SUBJECTS) {
                            strncpy(s.subjects[s.num_subjects].name, sem6[i], MAX_SUB_NAME-1); s.subjects[s.num_subjects].name[MAX_SUB_NAME-1]=0;
                            s.subjects[s.num_subjects].credits = sem6_c[i];
                            s.subjects[s.num_subjects].marks = 0;
                            s.subjects[s.num_subjects].classes_held = 0;
                            s.subjects[s.num_subjects].classes_attended = 0;
                            s.num_subjects++;
                        }
                    }
                } else if (cur==7) {
                    for (int i=0; sem7[i]!=NULL; ++i) {
                        if (s.num_subjects < MAX_SUBJECTS) {
                            strncpy(s.subjects[s.num_subjects].name, sem7[i], MAX_SUB_NAME-1); s.subjects[s.num_subjects].name[MAX_SUB_NAME-1]=0;
                            s.subjects[s.num_subjects].credits = sem7_c[i];
                            s.subjects[s.num_subjects].marks = 0;
                            s.subjects[s.num_subjects].classes_held = 0;
                            s.subjects[s.num_subjects].classes_attended = 0;
                            s.num_subjects++;
                        }
                    }
                } else if (cur==8) {
                    for (int i=0; sem8[i]!=NULL; ++i) {
                        if (s.num_subjects < MAX_SUBJECTS) {
                            strncpy(s.subjects[s.num_subjects].name, sem8[i], MAX_SUB_NAME-1); s.subjects[s.num_subjects].name[MAX_SUB_NAME-1]=0;
                            s.subjects[s.num_subjects].credits = sem8_c[i];
                            s.subjects[s.num_subjects].marks = 0;
                            s.subjects[s.num_subjects].classes_held = 0;
                            s.subjects[s.num_subjects].classes_attended = 0;
                            s.num_subjects++;
                        }
                    }
                }
            }

            /* Save via API */
            int addres = api_add_student(&s);
            if (addres == -2) {
                char resp[256];
                snprintf(resp, sizeof(resp),
                    "<!doctype html><html><body><p>SAP ID %d already registered. Try signing in.</p><p><a href='/'>Back</a></p></body></html>",
                    s.id);
                send_text(client, "409 Conflict", "text/html; charset=utf-8", resp);
            } else if (addres <= 0) {
                send_text(client, "500 Internal Server Error", "text/plain", "Unable to register");
            } else {
                char resp[512];
                snprintf(resp, sizeof(resp),
                    "<!doctype html><html><body><p>Registration successful!</p>"
                    "<p>Your Student ID (SAP ID): <strong>%d</strong></p>"
                    "<p>Default subjects for semester %d and earlier have been added automatically.</p>"
                    "<p><a href='/'>Back to Home</a></p></body></html>", addres, sem);
                send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            }

        signup_cleanup:
            if (name) free(name);
            if (age) free(age);
            if (sap) free(sap);
            if (password) free(password);
            if (email) free(email);
            if (phone) free(phone);
            if (semester) free(semester);
            close(client); return;
        }

        /* Enter marks (admin) - POST endpoint /enter-marks */
        if (strncmp(path, "/enter-marks", 12) == 0) {
            /* body contains fields id and many m_<subject>=<marks> entries */
            char *id_s = form_value(body, "id");
            if (!id_s) {
                send_text(client, "400 Bad Request", "text/plain", "Missing id");
                close(client); return;
            }
            int sid = atoi(id_s); free(id_s);
            int idx = api_find_index_by_id(sid);
            if (idx == -1) {
                send_text(client, "404 Not Found", "text/plain", "Student not found");
                close(client); return;
            }
            Student *s = &students[idx];
            /* naive parser: iterate over all "m_" occurrences and set marks */
            const char *p = body;
            int updated = 0;
            while ((p = strstr(p, "m_")) != NULL) {
                /* p points at m_<subject>=value ... */
                p += 2;
                /* read subject name up to '=' */
                const char *eq = strchr(p, '=');
                if (!eq) break;
                size_t sname_len = (size_t)(eq - p);
                char *sname_enc = malloc(sname_len+1);
                memcpy(sname_enc, p, sname_len); sname_enc[sname_len]=0;
                urldecode_inplace(sname_enc);
                /* read value */
                const char *val_start = eq + 1;
                /* value up to & or end */
                const char *amp = strchr(val_start, '&');
                size_t vlen = amp ? (size_t)(amp - val_start) : strlen(val_start);
                char *venc = malloc(vlen+1);
                memcpy(venc, val_start, vlen); venc[vlen]=0;
                urldecode_inplace(venc);
                int mk = atoi(venc);
                /* apply: find subject by exact name and set marks */
                for (int i=0;i<s->num_subjects;++i) {
                    if (strcmp(s->subjects[i].name, sname_enc)==0) {
                        if (mk < 0) mk = 0; if (mk > 100) mk = 100;
                        s->subjects[i].marks = mk;
                        updated++;
                        break;
                    }
                }
                free(sname_enc); free(venc);
                if (!amp) break;
                p = amp + 1;
            }
            /* recalc CGPA via API */
            api_calculate_update_cgpa(idx);
            save_data();
            char resp[256];
            snprintf(resp, sizeof(resp), "<p>Marks updated for ID %d (%d subjects updated). <a href='/admin'>Back</a></p>", sid, updated);
            send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            close(client); return;
        }

        /* Attendance POST (admin) - POST to /attendance (from build_attendance_mark_page) */
        if (strncmp(path, "/attendance", 10) == 0) {
            /* parse semester and subject hidden fields + date + present_N fields */
            char *sem_s = form_value(body, "semester");
            if (!sem_s) { send_text(client, "400 Bad Request", "text/plain", "Missing semester"); close(client); return; }
            int semester = atoi(sem_s); free(sem_s);
            /* collect subject occurrences from hidden fields 'subject' - there may be multiple */
            char *subjects[64]; int subj_count=0;
            {
                const char *p = body;
                while ((p = strstr(p, "subject=")) != NULL) {
                    p += strlen("subject=");
                    const char *amp = strchr(p, '&');
                    size_t len = amp ? (size_t)(amp - p) : strlen(p);
                    char *val = malloc(len+1);
                    memcpy(val, p, len); val[len]=0; urldecode_inplace(val);
                    if (subj_count < 64) subjects[subj_count++] = val;
                    if (!amp) break;
                    p = amp + 1;
                }
            }
            /* collect present_X parameters: present_0, present_1 ... For simplicity, we detect all "present_" occurrences followed by value (student id) */
            int present_ids[4096]; int present_count = 0;
            const char *p = body;
            while ((p = strstr(p, "present_")) != NULL) {
                /* skip until '=' */
                const char *eq = strchr(p, '=');
                if (!eq) break;
                const char *val_start = eq + 1;
                const char *amp = strchr(val_start, '&');
                size_t len = amp ? (size_t)(amp - val_start) : strlen(val_start);
                char *v = malloc(len+1);
                memcpy(v, val_start, len); v[len]=0; urldecode_inplace(v);
                int vid = atoi(v);
                if (vid>0 && present_count < (int)(sizeof(present_ids)/sizeof(int))) present_ids[present_count++] = vid;
                free(v);
                if (!amp) break;
                p = amp + 1;
            }
            /* apply attendance marking: For every student in that semester who has subject(s) selected, increment classes_held for those subjects, and if present, increment classes_attended */
            int processed = 0;
            for (int i=0;i<student_count;++i) {
                if (!students[i].exists) continue;
                if (students[i].current_semester != semester) continue;
                for (int sj=0; sj<subj_count; ++sj) {
                    for (int k=0;k<students[i].num_subjects;++k) {
                        if (strcmp(students[i].subjects[k].name, subjects[sj])==0) {
                            students[i].subjects[k].classes_held += 1;
                            /* check present */
                            int was_present = 0;
                            for (int pi=0; pi<present_count; ++pi) if (present_ids[pi] == students[i].id) { was_present = 1; break; }
                            if (was_present) students[i].subjects[k].classes_attended += 1;
                            processed++;
                        }
                    }
                }
            }
            save_data();
            /* write a small attendance report file */
            ensure_reports_dir();
            time_t t = time(NULL); struct tm tm = *localtime(&t);
            char datebuf[64];
            snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
            char fname[256];
            /* slugify first subject only to create filename */
            char subslug[128]; slugify(subjects[0] ? subjects[0] : "attendance", subslug, sizeof(subslug));
            snprintf(fname, sizeof(fname), "attendance_%d_%s_%s.html", semester, datebuf, subslug);
            char fpath[PATH_MAX]; snprintf(fpath, sizeof(fpath), "reports/%s", fname);
            FILE *f = fopen(fpath, "w");
            if (f) {
                fprintf(f, "<!doctype html><html><head><meta charset='utf-8'><title>Attendance</title></head><body>");
                fprintf(f, "<h2>Attendance - Semester %d - %s</h2><table border='1' cellpadding='6'><tr><th>ID</th><th>Name</th>", semester, datebuf);
                for (int sj=0; sj<subj_count; ++sj) fprintf(f, "<th>%s</th>", subjects[sj]);
                fprintf(f, "</tr>");
                for (int i=0;i<student_count;++i) {
                    if (!students[i].exists) continue;
                    if (students[i].current_semester != semester) continue;
                    /* see if student has at least one of the subjects */
                    int has_any = 0; for (int sj=0; sj<subj_count; ++sj) {
                        for (int k=0;k<students[i].num_subjects;++k) if (strcmp(students[i].subjects[k].name, subjects[sj])==0) { has_any=1; break; }
                        if (has_any) break;
                    }
                    if (!has_any) continue;
                    fprintf(f, "<tr><td>%d</td><td>%s</td>", students[i].id, students[i].name);
                    for (int sj=0; sj<subj_count; ++sj) {
                        int is_present = 0;
                        for (int pi=0; pi<present_count; ++pi) if (present_ids[pi]==students[i].id) { is_present=1; break; }
                        fprintf(f, "<td>%s</td>", is_present ? "Yes" : "No");
                    }
                    fprintf(f, "</tr>");
                }
                fprintf(f, "</table></body></html>");
                fclose(f);
            }

            for (int i=0;i<subj_count;++i) free(subjects[i]);
            char resp[512];
            snprintf(resp, sizeof(resp), "<p>Attendance marked (processed %d items). Report: <a href='/reports/%s'>%s</a>. <a href='/admin'>Back</a></p>", processed, fname, fname);
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
