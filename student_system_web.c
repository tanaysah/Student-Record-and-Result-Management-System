/* student_system_web.c
   Web wrapper for student_system.c
   - Updated: fix enter-marks POST handling (table-based), improved student dashboard:
     groups by semester with latest semester on top, shows current-semester SGPA and marks,
     and shows stored CGPA.
   Compile:
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
extern Student students[];
extern int student_count;

extern int api_find_index_by_id(int id);
extern int api_add_student(Student *s);
extern void api_generate_report(int idx, const char* college, const char* semester, const char* exam);
extern int api_calculate_update_cgpa(int idx);
extern int api_admin_auth(const char *user, const char *pass);

/* save_data is defined in student_system.c */
extern void save_data(void);

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

/* quick grade mapping */
static int marks_to_grade_point_local(int marks) {
    if (marks >= 90) return 10;
    if (marks >= 80) return 9;
    if (marks >= 70) return 8;
    if (marks >= 60) return 7;
    if (marks >= 50) return 6;
    if (marks >= 40) return 5;
    return 0;
}

/* compute SGPA locally (used as fallback) */
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

/* helper to add an array of subjects to a student */
static void add_subjects_to_student(Student *s, const char **list, const int *credits) {
    if (!s || !list) return;
    for (int i = 0; list[i] != NULL; ++i) {
        /* skip duplicates */
        int found = 0;
        for (int k = 0; k < s->num_subjects; ++k) {
            if (strcmp(s->subjects[k].name, list[i]) == 0) { found = 1; break; }
        }
        if (found) continue;
        if (s->num_subjects >= MAX_SUBJECTS) break;
        strncpy(s->subjects[s->num_subjects].name, list[i], sizeof(s->subjects[s->num_subjects].name)-1);
        s->subjects[s->num_subjects].name[sizeof(s->subjects[s->num_subjects].name)-1] = '\0';
        s->subjects[s->num_subjects].credits = credits ? credits[i] : 0;
        s->subjects[s->num_subjects].marks = 0;
        s->subjects[s->num_subjects].classes_held = 0;
        s->subjects[s->num_subjects].classes_attended = 0;
        s->num_subjects++;
    }
}

/* semester definitions (copied to keep semester->subject mapping) */
static const char *sem1[] = {
    "Programming in C","Linux Lab","Problem Solving","Advanced Engineering Mathematics - I","Physics for Computer Engineers","Managing Self","Environmental Sustainability and Climate Change", NULL
};
static const int sem1_c[] = {5,2,2,4,5,2,2};
static const char *sem2[] = {
    "Data Structures and Algorithms","Digital Electronics","Python Programming","Advanced Engineering Mathematics - II","Environmental Sustainability and Climate Change","Time and Priority Management","Elements of AI/ML", NULL
};
static const int sem2_c[] = {5,3,5,4,2,2,3};
static const char *sem3[] = {
    "Leading Conversations","Discrete Mathematical Structures","Operating Systems","Elements of AI/ML","Database Management Systems","Design and Analysis of Algorithms", NULL
};
static const int sem3_c[] = {2,3,3,3,5,4};
static const char *sem4[] = {
    "Software Engineering","EDGE - Soft Skills","Linear Algebra","Indian Constitution","Writing with Impact","Object Oriented Programming","Data Communication and Networks","Applied Machine Learning", NULL
};
static const int sem4_c[] = {3,0,3,0,2,4,4,5};
static const char *sem5[] = {
    "Cryptography and Network Security","Formal Languages and Automata Theory","Object Oriented Analysis and Design","Exploratory-3","Start your Startup","Research Methodology in CS","Probability, Entropy, and MC Simulation","PE-2","PE-2 Lab", NULL
};
static const int sem5_c[] = {3,3,3,3,2,3,3,4,1};
static const char *sem6[] = {
    "Exploratory-4","Leadership and Teamwork","Compiler Design","Statistics and Data Analysis","PE-3","PE-3 Lab","Minor Project", NULL
};
static const int sem6_c[] = {3,2,3,3,4,1,5};
static const char *sem7[] = {
    "Exploratory-5","PE-4","PE-4 Lab","PE-5","PE-5 Lab","Capstone Project - Phase-1","Summer Internship", NULL
};
static const int sem7_c[] = {3,4,1,3,1,5,1};
static const char *sem8[] = {
    "IT Ethical Practices","Capstone Project - Phase-2", NULL
};
static const int sem8_c[] = {3,5};

/* determine semester index for a subject name by searching the semester arrays above
   returns 1..8 if found, 0 if unknown */
static int subject_semester(const char *sname) {
    const char **lists[] = { NULL, sem1, sem2, sem3, sem4, sem5, sem6, sem7, sem8 };
    for (int sem=1; sem<=8; ++sem) {
        const char **lst = lists[sem];
        if (!lst) continue;
        for (int j=0; lst[j]!=NULL; ++j) {
            if (strcmp(lst[j], sname) == 0) return sem;
        }
    }
    return 0;
}

/* Build landing page (signup includes extra fields) */
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

/* build student dashboard as HTML with attendance & marks grouped by semester (latest on top) */
static char *build_student_dashboard(int idx) {
    if (idx < 0 || idx >= student_count) return NULL;
    Student *s = &students[idx];
    char escaped_name[256]; html_escape_buf(s->name, escaped_name, sizeof(escaped_name));

    /* Organize subjects by semester */
    /* We'll create a simple buffer per semester */
    char sem_bufs[9][8192];
    for (int i=0;i<9;++i) sem_bufs[i][0]=0;
    for (int i = 0; i < s->num_subjects; ++i) {
        int sem = subject_semester(s->subjects[i].name);
        if (sem < 1 || sem > 8) sem = 0; /* unknown => group 0 (we'll show later) */
        char sname_esc[256]; html_escape_buf(s->subjects[i].name, sname_esc, sizeof(sname_esc));
        int held = s->subjects[i].classes_held;
        int att = s->subjects[i].classes_attended;
        int pct = (held == 0) ? 0 : (int)(((double)att / held) * 100.0 + 0.5);
        int marks = s->subjects[i].marks;
        double gp = (s->subjects[i].credits>0)? marks_to_grade_point_local(marks) : 0;
        char row[512];
        snprintf(row, sizeof(row), "<tr><td>%s</td><td>%d</td><td>%d</td><td>%.0f</td><td>%d%%</td></tr>", sname_esc, s->subjects[i].credits, marks, gp, pct);
        if (sem >=1 && sem <=8) {
            if (strlen(sem_bufs[sem]) + strlen(row) + 256 > sizeof(sem_bufs[sem])) continue;
            strcat(sem_bufs[sem], row);
        } else {
            if (strlen(sem_bufs[0]) + strlen(row) + 256 > sizeof(sem_bufs[0])) continue;
            strcat(sem_bufs[0], row);
        }
    }

    /* compute SGPA for current semester only */
    double sgpa_current = 0.0;
    int tot_cr = 0;
    double weighted = 0.0;
    for (int i=0;i<s->num_subjects;++i) {
        int sem = subject_semester(s->subjects[i].name);
        if (sem != s->current_semester) continue;
        int cr = s->subjects[i].credits;
        if (cr <= 0) continue;
        int mk = s->subjects[i].marks;
        int gp = marks_to_grade_point_local(mk);
        weighted += (double)gp * cr;
        tot_cr += cr;
    }
    if (tot_cr > 0) sgpa_current = weighted / (double)tot_cr;

    const char *tpl_start =
        "<!doctype html><html><head><meta charset='utf-8'><title>Dashboard</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>body{font-family:Inter,Arial;margin:18px} .card{background:#fff;padding:18px;border-radius:10px;box-shadow:0 6px 18px rgba(0,0,0,0.06);max-width:1000px;margin:auto} table{width:100%;border-collapse:collapse} table th,table td{padding:8px;border:1px solid #eee;text-align:left;font-size:14px} h3.sem{margin-top:18px}</style>"
        "</head><body><div class='card'>";

    const char *tpl_end = "<p><a href='/'>← Back to Home</a></p></div></body></html>";

    size_t cap = strlen(tpl_start) + 32768;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, tpl_start);
    char header[1024];
    char dept_esc[256]; html_escape_buf(s->dept, dept_esc, sizeof(dept_esc));
    snprintf(header, sizeof(header),
             "<h2>Welcome, %s</h2><p>ID: %d | Dept: %s | Year: %d | Current Semester: %d | Age: %d</p>"
             "<p><strong>SGPA (current sem %d):</strong> %.3f  &nbsp;&nbsp; <strong>Stored CGPA:</strong> %.3f (Credits: %d)</p>",
             escaped_name, s->id, dept_esc, s->year, s->current_semester, s->age, s->current_semester, sgpa_current, s->cgpa, s->total_credits_completed);
    strcat(buf, header);

    /* Show current semester subjects first */
    for (int sem = s->current_semester; sem >= 1; --sem) {
        char title[128];
        snprintf(title, sizeof(title), "<h3 class='sem'>Semester %d</h3>", sem);
        strcat(buf, title);
        strcat(buf, "<table><tr><th>Subject</th><th>Credits</th><th>Marks</th><th>GradePoint</th><th>Attendance</th></tr>");
        if (sem_bufs[sem][0] == '\0') {
            strcat(buf, "<tr><td colspan='5'>No subjects for this semester.</td></tr>");
        } else {
            strcat(buf, sem_bufs[sem]);
        }
        strcat(buf, "</table>");
    }
    /* Show unknown-grouped subjects (if any) */
    if (sem_bufs[0][0]) {
        strcat(buf, "<h3 class='sem'>Other / Uncategorized Subjects</h3><table><tr><th>Subject</th><th>Credits</th><th>Marks</th><th>GradePoint</th><th>Attendance</th></tr>");
        strcat(buf, sem_bufs[0]);
        strcat(buf, "</table>");
    }

    strcat(buf, tpl_end);
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
        if (strncmp(path, "/attendance", 10) == 0) {
            /* show form to mark attendance for a subject (admin-only page)
               flow:
               - GET /attendance -> semester select
               - GET /attendance?semester=N -> show subjects in that semester (radio)
               - GET /attendance?semester=N&subject=Name -> show students (filtered by current_semester == N) with checkboxes
            */
            char *q = strchr(fullpath, '?');
            int sem = 0;
            char subject_q[256] = {0};
            if (q) {
                char *qs = strdup(q+1);
                char *ssem = form_value(qs, "semester");
                char *sub = form_value(qs, "subject");
                if (ssem) { sem = atoi(ssem); free(ssem); }
                if (sub) { strncpy(subject_q, sub, sizeof(subject_q)-1); free(sub); }
                free(qs);
            }
            /* gather unique subjects for selected semester only */
            char subjects[16384]; subjects[0]=0;
            for (int i=0;i<student_count;++i) {
                if (!students[i].exists) continue;
                for (int j=0;j<students[i].num_subjects;++j) {
                    char *sname = students[i].subjects[j].name;
                    int ssem = subject_semester(sname);
                    if (sem != 0 && ssem != sem) continue;
                    if (strstr(subjects, sname)==NULL) {
                        if (strlen(subjects)+strlen(sname)+16 > sizeof(subjects)) continue;
                        strcat(subjects, sname);
                        strcat(subjects, "\n");
                    }
                }
            }
            size_t cap = 32768;
            char *buf = malloc(cap);
            if (!buf) { send_text(client, "500 Internal Server Error", "text/plain", "Server error"); close(client); return; }
            strcpy(buf, "<!doctype html><html><head><meta charset='utf-8'><title>Attendance</title></head><body><h2>Admin Attendance</h2>");
            /* Semester selection form */
            strcat(buf, "<form method='get' action='/attendance'>Select semester: <select name='semester'>");
            for (int s=1;s<=8;++s) {
                char opt[64];
                snprintf(opt, sizeof(opt), "<option value='%d'%s>Semester %d</option>", s, (s==sem) ? " selected" : "", s);
                strcat(buf, opt);
            }
            strcat(buf, "</select> <button>Choose</button></form><hr>");
            /* If semester selected but subject not, list subjects as radio buttons */
            if (sem != 0 && subject_q[0] == 0) {
                strcat(buf, "<h3>Subjects in chosen semester</h3>");
                if (strlen(subjects)==0) {
                    strcat(buf, "<p>No subjects found for this semester.</p>");
                } else {
                    strcat(buf, "<form method='get' action='/attendance'>");
                    char hidden[64];
                    snprintf(hidden, sizeof(hidden), "<input type='hidden' name='semester' value='%d'/>", sem);
                    strcat(buf, hidden);
                    strcat(buf, "<ul>");
                    char *copy = strdup(subjects);
                    char *line = strtok(copy, "\n");
                    while (line) {
                        char esc[256]; html_escape_buf(line, esc, sizeof(esc));
                        char li[512];
                        snprintf(li, sizeof(li), "<li><label><input type='radio' name='subject' value=\"%s\" required/> %s</label></li>", esc, esc);
                        strcat(buf, li);
                        line = strtok(NULL, "\n");
                    }
                    free(copy);
                    strcat(buf, "</ul><div style='margin-top:8px'><button>Open Subject</button></div></form>");
                }
            }
            /* If subject selected, show student list with checkboxes but only students whose current_semester == sem */
            if (subject_q[0]) {
                char panel[16384]; panel[0]=0;
                snprintf(panel, sizeof(panel), "<h3>Subject: %s (Semester %d)</h3><form method='post' action='/attendance'><input type='hidden' name='subject' value='%s'/><input type='hidden' name='semester' value='%d'/>Date (YYYY-MM-DD): <input name='date' value='", subject_q, sem, subject_q, sem);
                time_t t = time(NULL); struct tm tm = *localtime(&t); char datebuf[32];
                snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
                strncat(panel, datebuf, sizeof(panel)-strlen(panel)-1);
                strncat(panel, "'/> <table border='1' cellpadding='6'><tr><th>Present</th><th>ID</th><th>Name</th></tr>", sizeof(panel)-strlen(panel)-1);
                int listed = 0;
                for (int i=0;i<student_count;++i) {
                    if (!students[i].exists) continue;
                    if (students[i].current_semester != sem) continue; /* only students in selected semester */
                    int has = 0;
                    for (int j=0;j<students[i].num_subjects;++j) if (strcmp(students[i].subjects[j].name, subject_q)==0) { has = 1; break; }
                    if (!has) continue;
                    listed++;
                    char row[512];
                    snprintf(row, sizeof(row), "<tr><td><input type='checkbox' name='present' value='%d'/></td><td>%d</td><td>%s</td></tr>", students[i].id, students[i].id, students[i].name);
                    if (strlen(panel)+strlen(row)+256 > sizeof(panel)) break;
                    strcat(panel, row);
                }
                if (listed == 0) strcat(panel, "<tr><td colspan='3'>No students in this semester have this subject.</td></tr>");
                strcat(panel, "</table><div style='margin-top:8px'><button>Mark Attendance</button></div></form>");
                if (strlen(buf)+strlen(panel)+256 > cap) { cap*=2; buf=realloc(buf,cap); }
                strcat(buf, panel);
            }
            strcat(buf, "<p><a href='/'>Back</a></p></body></html>");
            send_text(client, "200 OK", "text/html; charset=utf-8", buf);
            free(buf);
            close(client); return;
        }

        if (strncmp(path, "/enter-marks", 12) == 0) {
            /* GET /enter-marks?id=<id> -> show table of student's current semester subjects with mark inputs */
            char *q = strchr(fullpath, '?');
            int id = -1;
            if (q) {
                char *qs = strdup(q+1);
                char *vid = form_value(qs, "id");
                if (vid) { id = atoi(vid); free(vid); }
                free(qs);
            }
            if (id <= 0) {
                /* render a small page explaining how to use this endpoint */
                const char *help = "<!doctype html><html><head><meta charset='utf-8'><title>Enter Marks</title></head><body><h3>Enter Marks</h3><p>Provide a student ID on the admin dashboard or use the dashboard form.</p><p><a href='/'>Back</a></p></body></html>";
                send_text(client, "200 OK", "text/html; charset=utf-8", help);
                close(client); return;
            }
            int idx = api_find_index_by_id(id);
            if (idx == -1) { send_text(client, "404 Not Found", "text/plain", "Student not found"); close(client); return; }
            Student *s = &students[idx];
            /* Build form: table of subjects that belong to current_semester */
            int cur = s->current_semester;
            size_t cap = 16384;
            char *buf = malloc(cap);
            if (!buf) { send_text(client, "500 Internal Server Error", "text/plain", "Server error"); close(client); return; }
            snprintf(buf, cap, "<!doctype html><html><head><meta charset='utf-8'><title>Enter Marks for %s</title></head><body><h2>Enter Marks for %s (ID: %d) - Semester %d</h2>", s->name, s->name, s->id, cur);
            strcat(buf, "<form method='post' action='/enter-marks'>");
            char hid[64]; snprintf(hid, sizeof(hid), "<input type='hidden' name='id' value='%d'/>", s->id); strcat(buf, hid);
            strcat(buf, "<table border='1' cellpadding='6'><tr><th>#</th><th>Subject</th><th>Credits</th><th>Marks (0-100)</th></tr>");
            int shown = 0;
            for (int i=0;i<s->num_subjects;++i) {
                int sem = subject_semester(s->subjects[i].name);
                if (sem != cur) continue;
                shown++;
                char subj_esc[256]; html_escape_buf(s->subjects[i].name, subj_esc, sizeof(subj_esc));
                char row[512];
                /* input name uses the subject array index so POST can map back */
                snprintf(row, sizeof(row), "<tr><td>%d</td><td>%s</td><td>%d</td><td><input name='mark_%d' value='%d' size='4' /></td></tr>", shown, subj_esc, s->subjects[i].credits, i, s->subjects[i].marks);
                if (strlen(buf)+strlen(row)+256 > cap) { cap*=2; buf=realloc(buf,cap); }
                strcat(buf, row);
            }
            if (shown == 0) {
                strcat(buf, "<tr><td colspan='4'>No subjects for current semester.</td></tr>");
            }
            strcat(buf, "</table><div style='margin-top:8px'><button>Submit Marks</button></div></form><p><a href='/'>Back</a></p></body></html>");
            send_text(client, "200 OK", "text/html; charset=utf-8", buf);
            free(buf);
            close(client); return;
        }
    } /* end GET */

    /* POST handlers */
    if (strcmp(method, "POST") == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if (!body) { send_text(client, "400 Bad Request", "text/plain", "No body"); close(client); return; }
        body += 4;

        /* Admin login */
        if (strncmp(path, "/admin-login", strlen("/admin-login")) == 0) {
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
            /* admin dashboard: updated flows for attendance and marks */
            const char *adm =
              "<!doctype html><html><head><meta charset='utf-8'><title>Admin Dashboard</title>"
              "<style>body{font-family:Arial;margin:18px} .card{max-width:900px;padding:18px;border-radius:10px;background:#fff;border:1px solid #eee} input,button,textarea,select{padding:8px;margin:6px 0;width:100%} button{background:#0b69ff;color:#fff;border:none;border-radius:6px}</style></head><body>"
              "<div class='card'><h2>Admin Dashboard</h2>"
              "<p>Manage marks and attendance.</p>"
              "<h3>View all students</h3><p><a href='/list'>Open students list</a></p>"
              "<h3>Enter marks for a student</h3>"
              "<p>Step 1: Enter Student ID and click <em>Load Subjects</em>. The student's current semester subjects will be shown.</p>"
              "<form method='get' action='/enter-marks' style='max-width:420px'>"
              "<input name='id' placeholder='Student ID' required />"
              "<div style='margin-top:8px'><button>Load Subjects</button></div></form>"
              "<h3>Mark attendance</h3>"
              "<p>Step 1: Select semester. Step 2: Select subject. Step 3: Mark attendance for students (only students in that semester will appear).</p>"
              "<form method='get' action='/attendance' style='max-width:420px'><select name='semester'>"
              "<option value='1'>Semester 1</option><option value='2'>Semester 2</option><option value='3'>Semester 3</option><option value='4'>Semester 4</option>"
              "<option value='5'>Semester 5</option><option value='6'>Semester 6</option><option value='7'>Semester 7</option><option value='8'>Semester 8</option>"
              "</select><div style='margin-top:8px'><button>Select Semester</button></div></form>"
              "<p><a href='/'>Back</a></p></div></body></html>";
            send_text(client, "200 OK", "text/html; charset=utf-8", adm);
            close(client); return;
        }

        /* Student sign-up */
        if (strncmp(path, "/student-signup", strlen("/student-signup")) == 0) {
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

            /* Default semester subjects arrays */
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
                if (cur==1) add_subjects_to_student(&s, sem1, sem1_c);
                else if (cur==2) add_subjects_to_student(&s, sem2, sem2_c);
                else if (cur==3) add_subjects_to_student(&s, sem3, sem3_c);
                else if (cur==4) add_subjects_to_student(&s, sem4, sem4_c);
                else if (cur==5) add_subjects_to_student(&s, sem5, sem5_c);
                else if (cur==6) add_subjects_to_student(&s, sem6, sem6_c);
                else if (cur==7) add_subjects_to_student(&s, sem7, sem7_c);
                else if (cur==8) add_subjects_to_student(&s, sem8, sem8_c);
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

        /* Enter marks (admin) - supports table-based (mark_<index>) and legacy textarea */
        if (strncmp(path, "/enter-marks", strlen("/enter-marks")) == 0) {
            /* Try to get student id first (hidden field) */
            char *id_s = form_value(body, "id");
            char *marks_txt = form_value(body, "marks"); /* legacy textarea */
            if (!id_s && !marks_txt) {
                send_text(client, "400 Bad Request", "text/plain", "Missing id/marks");
                if (id_s) free(id_s);
                if (marks_txt) free(marks_txt);
                close(client); return;
            }

            /* If id provided, prefer table-format parsing (mark_<index>=val) */
            if (id_s) {
                int sid = atoi(id_s);
                free(id_s);
                if (sid <= 0) {
                    send_text(client, "400 Bad Request", "text/plain", "Invalid student id");
                    if (marks_txt) free(marks_txt);
                    close(client); return;
                }
                int idx = api_find_index_by_id(sid);
                if (idx == -1) {
                    send_text(client, "404 Not Found", "text/plain", "Student not found");
                    if (marks_txt) free(marks_txt);
                    close(client); return;
                }
                int any_found = 0;
                /* scan for occurrences of "mark_" in the raw body and extract index and value */
                const char *p = body;
                while ((p = strstr(p, "mark_")) != NULL) {
                    p += strlen("mark_");
                    int idnum = atoi(p);
                    /* find '=' following the number */
                    const char *eq = strchr(p, '=');
                    if (!eq) break;
                    eq++;
                    char valbuf[64]; int vi=0;
                    while (*eq && *eq != '&' && vi < (int)sizeof(valbuf)-1) { valbuf[vi++]=*eq++; }
                    valbuf[vi]=0;
                    char *dec = strdup(valbuf); urldecode_inplace(dec);
                    int mk = atoi(dec);
                    free(dec);
                    if (idnum >= 0 && idnum < students[idx].num_subjects) {
                        students[idx].subjects[idnum].marks = mk;
                        any_found = 1;
                    }
                }
                /* fallback: if no mark_* fields but a legacy textarea exists, parse it */
                if (!any_found && marks_txt) {
                    char *line = strtok(marks_txt, "\n");
                    int updated = 0;
                    while (line) {
                        while (*line == ' ' || *line == '\r' || *line == '\t') line++;
                        char *sep = strstr(line, "|");
                        if (sep) {
                            *sep = 0;
                            char *subj = line;
                            char *mstr = sep + 1;
                            while (*mstr==' ') mstr++;
                            int mk = atoi(mstr);
                            if (mk < 0) mk = 0;
                            /* find subject by exact name and set marks */
                            for (int i=0;i<students[idx].num_subjects;++i) {
                                if (strcmp(students[idx].subjects[i].name, subj)==0) {
                                    students[idx].subjects[i].marks = mk;
                                    updated++;
                                    break;
                                }
                            }
                        }
                        line = strtok(NULL, "\n");
                    }
                    any_found = (updated > 0);
                }
                if (!any_found) {
                    send_text(client, "400 Bad Request", "text/plain", "No marks found in submission");
                    if (marks_txt) free(marks_txt);
                    close(client); return;
                }
                /* Recalculate CGPA via API and save */
                api_calculate_update_cgpa(idx);
                save_data();
                char resp[256];
                snprintf(resp, sizeof(resp), "<p>Marks updated for ID %d. <a href='/'>Back</a></p>", sid);
                send_text(client, "200 OK", "text/html; charset=utf-8", resp);
                if (marks_txt) free(marks_txt);
                close(client); return;
            }

            /* If we reach here and only marks_txt exists (rare), process legacy textarea */
            if (marks_txt) {
                /* try to find an id inside marks_txt? The legacy branch earlier required id as separate field.
                   Here we can't proceed without an id, so return error. */
                send_text(client, "400 Bad Request", "text/plain", "Missing student id for legacy marks format");
                free(marks_txt);
                close(client); return;
            }
        }

        /* Attendance POST (admin) - honors 'semester' hidden field and updates only students in that semester */
        if (strncmp(path, "/attendance", strlen("/attendance")) == 0) {
            char *subject = form_value(body, "subject");
            char *date = form_value(body, "date"); /* date string */
            char *semester_s = form_value(body, "semester");
            if (!subject) { send_text(client, "400 Bad Request", "text/plain", "Missing subject"); if (date) free(date); if (semester_s) free(semester_s); close(client); return; }
            int sem = 0;
            if (semester_s) { sem = atoi(semester_s); free(semester_s); }
            /* Collect present IDs */
            int present_ids[2048]; int present_count=0;
            const char *p = body;
            while ((p = strstr(p, "present=")) != NULL) {
                p += strlen("present=");
                int val = atoi(p);
                if (val > 0) { present_ids[present_count++] = val; }
                const char *amp = strchr(p, '&');
                if (!amp) break;
                p = amp + 1;
            }
            /* mark attendance: for every student who has this subject AND whose current_semester == sem (if sem>0) increment held; if in present_ids increment attended */
            int changed = 0;
            for (int i=0;i<student_count;++i) {
                if (!students[i].exists) continue;
                if (sem > 0 && students[i].current_semester != sem) continue; /* only that semester */
                for (int j=0;j<students[i].num_subjects;++j) {
                    if (strcmp(students[i].subjects[j].name, subject)==0) {
                        students[i].subjects[j].classes_held += 1;
                        int was_present = 0;
                        for (int k=0;k<present_count;++k) if (present_ids[k] == students[i].id) { was_present = 1; break; }
                        if (was_present) students[i].subjects[j].classes_attended += 1;
                        changed++;
                        break;
                    }
                }
            }
            save_data();
            /* write an attendance report file with date/time */
            ensure_reports_dir();
            time_t t = time(NULL); struct tm tm = *localtime(&t);
            char datebuf[64];
            snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
            char subjslug[256]; slugify(subject, subjslug, sizeof(subjslug));
            char fname[512]; snprintf(fname, sizeof(fname), "attendance_%s_%s.html", date ? date : datebuf, subjslug);
            char fpath[PATH_MAX]; snprintf(fpath, sizeof(fpath), "reports/%s", fname);
            FILE *f = fopen(fpath, "w");
            if (f) {
                fprintf(f, "<!doctype html><html><head><meta charset='utf-8'><title>Attendance %s</title></head><body>", subject);
                fprintf(f, "<h2>Attendance for %s on %s</h2><table border='1' cellpadding='6'><tr><th>ID</th><th>Name</th><th>Present</th></tr>", subject, date ? date : datebuf);
                for (int i=0;i<student_count;++i) {
                    if (!students[i].exists) continue;
                    if (sem > 0 && students[i].current_semester != sem) continue;
                    int has = 0, is_present = 0;
                    for (int j=0;j<students[i].num_subjects;++j) if (strcmp(students[i].subjects[j].name, subject)==0) { has=1; break; }
                    if (!has) continue;
                    for (int k=0;k<present_count;++k) if (present_ids[k] == students[i].id) { is_present = 1; break; }
                    fprintf(f, "<tr><td>%d</td><td>%s</td><td>%s</td></tr>", students[i].id, students[i].name, is_present ? "Yes" : "No");
                }
                fprintf(f, "</table><p><a href='/'>Back</a></p></body></html>");
                fclose(f);
            }
            char resp[512];
            snprintf(resp, sizeof(resp), "<p>Attendance marked for subject '%s' (processed %d students). Report: <a href='/reports/%s' target='_blank'>%s</a>. <a href='/'>Back</a></p>", subject, changed, fname, fname);
            send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            if (subject) free(subject);
            if (date) free(date);
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
