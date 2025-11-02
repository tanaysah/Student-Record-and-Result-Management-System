/* student_system_web.c
   Web wrapper for student_system.c
   - Landing: Admin login / Student signup / Student signin
   - Student signup includes email, phone, semester (auto-adds semester subjects)
   - Admin: enter marks by subject-name, mark attendance via checkboxes per subject & date
   - Student dashboard: attendance chart, subjects, SGPA, CGPA
   Compile with:
     gcc -DBUILD_WEB student_system.c student_system_web.c -o student_system_web

   UPDATED: student dashboard now groups subjects by semester (1..N) and shows
   semester-wise attendance distribution. For students beyond semester 1, if
   subject marks and attendance are empty (zero), the dashboard displays
   deterministic "default" random-looking marks and attendance computed from
   student ID + subject name so you don't need to manually populate past data.
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

/* compute SGPA locally */
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

/* helper to add an array of subjects to a student (replaces the lambda used earlier) */
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

/* --- Semester arrays (duplicate of student_system.c lists to allow grouping in dashboard) --- */
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

/* helper: deterministic hash to produce repeatable 'random' values per student+subject */
static unsigned int deterministic_hash(const char *s, unsigned int seed) {
    unsigned int h = seed ^ 2166136261u;
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 16777619u;
    }
    /* further scramble */
    h ^= (h >> 16);
    h *= 0x85ebca6b;
    h ^= (h >> 13);
    h *= 0xc2b2ae35;
    h ^= (h >> 16);
    return h;
}

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

/* build student dashboard as HTML with attendance & marks (UPDATED)
   - groups subjects by semester
   - displays deterministic default marks/attendance for earlier semesters when data is empty
   - shows semester-wise attendance distribution (bar chart)
*/
static char *build_student_dashboard(int idx) {
    if (idx < 0 || idx >= student_count) return NULL;
    Student *s = &students[idx];
    char escaped_name[256]; html_escape_buf(s->name, escaped_name, sizeof(escaped_name));

    /* Prepare semester buckets 1..8 */
    char sem_tables[9][8192];
    int sem_caps[9];
    for (int i=0;i<9;++i) { sem_tables[i][0]=0; sem_caps[i]=8192; }

    /* We will also compute semester-wise average attendance */
    int sem_subject_counts[9] = {0};
    int sem_attended_sum[9] = {0};
    int sem_held_sum[9] = {0};

    /* helper local buffers */
    char sname_esc[256];

    for (int i = 0; i < s->num_subjects; ++i) {
        const char *subjname = s->subjects[i].name;
        int subj_sem = subject_semester(subjname); /* 1..8 or 0 if unknown */
        if (subj_sem <= 0) subj_sem = 0; /* put unknown into 0 bucket (we will show later) */

        /* determine display marks and attendance: if stored values are zero AND
           subject belongs to a semester earlier than or equal to current_semester - 1,
           produce deterministic defaults so dashboard shows meaningful data */
        int disp_marks = s->subjects[i].marks;
        int held = s->subjects[i].classes_held;
        int att = s->subjects[i].classes_attended;

        if (s->current_semester > 1 && subj_sem > 0 && subj_sem < s->current_semester) {
            /* for past semesters: if there's no real data, create deterministic defaults */
            if (disp_marks == 0 && held == 0) {
                unsigned int h = deterministic_hash(subjname, (unsigned int)s->id);
                disp_marks = 45 + (h % 46); /* 45..90-ish */
                held = 20 + (h % 21);      /* 20..40 classes held */
                att = (held * (50 + ((h >> 8) % 51))) / 100; /* attendance 50%-100% */
            }
        }

        /* prepare escaped subject name */
        html_escape_buf(subjname, sname_esc, sizeof(sname_esc));

        /* grade point for display */
        int gp = (s->subjects[i].credits>0)? marks_to_grade_point_local(disp_marks) : 0;

        /* build a row */
        char row[512];
        snprintf(row, sizeof(row), "<tr><td>%s</td><td>%d</td><td>%d</td><td>%.0f</td><td>%d%%</td></tr>", sname_esc, disp_marks, s->subjects[i].credits, (double)gp, (held==0?0:(int)(((double)att/held)*100.0 + 0.5)));

        int semidx = subj_sem;
        if (semidx < 0 || semidx > 8) semidx = 0;
        if (strlen(sem_tables[semidx]) + strlen(row) + 256 > (size_t)sem_caps[semidx]) {
            sem_caps[semidx] *= 2; /* grow but stack buffers are fixed; in practice subjects small */
        }
        strncat(sem_tables[semidx], row, sizeof(sem_tables[semidx]) - strlen(sem_tables[semidx]) - 1);

        /* accumulate for semester attendance averages */
        if (held > 0) {
            sem_subject_counts[semidx]++;
            sem_attended_sum[semidx] += att;
            sem_held_sum[semidx] += held;
        }
    }

    /* Build semester-wise attendance SVG (one bar per semester 1..current_semester) */
    int max_sem = s->current_semester;
    if (max_sem < 1) max_sem = 1;
    if (max_sem > 8) max_sem = 8;
    int w = 640, h = 180, pad = 28;
    char svg[4096]; svg[0]=0;
    char svg_start[256];
    snprintf(svg_start, sizeof(svg_start), "<svg viewBox='0 0 %d %d' width='%d' height='%d' xmlns='http://www.w3.org/2000/svg'><rect width='100%%' height='100%%' fill='transparent'/>", w, h, w, h);
    strcpy(svg, svg_start);
    snprintf(svg + strlen(svg), sizeof(svg)-strlen(svg), "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#eee'/>", pad, h-pad, w-pad, h-pad);
    int barw = (max_sem>0) ? ((w - pad*2) / max_sem - 8) : 20;
    if (barw < 8) barw = 8;
    for (int sem = 1; sem <= max_sem; ++sem) {
        int x = pad + (sem-1)*(barw+8);
        int pct = 0;
        if (sem_held_sum[sem] > 0) {
            pct = (int)((double)sem_attended_sum[sem] / (double)sem_held_sum[sem] * 100.0 + 0.5);
        } else {
            /* If no data, create a deterministic default for the semester based on student id + sem */
            unsigned int h = deterministic_hash("SEM", (unsigned int)(s->id ^ sem));
            pct = 55 + (h % 41); /* 55..95 */
        }
        if (pct > 100) pct = 100;
        int barh = (pct * (h - pad*2)) / 100;
        int y = (h - pad) - barh;
        snprintf(svg + strlen(svg), sizeof(svg)-strlen(svg), "<rect x='%d' y='%d' width='%d' height='%d' rx='6' ry='6' fill='#3b82f6' opacity='0.85'/>", x, y, barw, barh);
        snprintf(svg + strlen(svg), sizeof(svg)-strlen(svg), "<text x='%d' y='%d' font-size='11' fill='#111'>Sem %d</text>", x, h-pad+14, sem);
        snprintf(svg + strlen(svg), sizeof(svg)-strlen(svg), "<text x='%d' y='%d' font-size='11' fill='#111'>%d%%</text>", x, y-4, pct);
    }
    strcat(svg, "</svg>");

    const char *tpl_start =
        "<!doctype html><html><head><meta charset='utf-8'><title>Dashboard</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>body{font-family:Inter,Arial;margin:18px} .card{background:#fff;padding:18px;border-radius:10px;box-shadow:0 6px 18px rgba(0,0,0,0.06);max-width:1000px;margin:auto} table{width:100%;border-collapse:collapse} table th,table td{padding:8px;border:1px solid #eee;text-align:left;font-size:14px}</style>"
        "</head><body><div class='card'>";

    const char *tpl_end = "<p><a href='/'>← Back to Home</a></p></div></body></html>";

    size_t cap = strlen(tpl_start) + 8192 + 8192 + strlen(svg) + 2048;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, tpl_start);
    char header[512];
    char dept_esc[256]; html_escape_buf(s->dept, dept_esc, sizeof(dept_esc));
    double sgpa = compute_sgpa_local(s);
    snprintf(header, sizeof(header),
             "<h2>Welcome, %s</h2><p>ID: %d | Dept: %s | Year: %d | Semester: %d | Age: %d</p>"
             "<p><strong>SGPA (current):</strong> %.3f  &nbsp;&nbsp; <strong>Stored CGPA:</strong> %.3f (Credits: %d)</p>",
             escaped_name, s->id, dept_esc, s->year, s->current_semester, s->age, sgpa, s->cgpa, s->total_credits_completed);
    strcat(buf, header);
    strcat(buf, "<h3>Semester-wise Attendance</h3>");
    strcat(buf, svg);

    /* For each semester, render a table if it has subjects (or if sem <= current_semester show even empty) */
    for (int sem=1; sem<=s->current_semester && sem<=8; ++sem) {
        char semtitle[128]; snprintf(semtitle, sizeof(semtitle), "<h3>Semester %d</h3>", sem);
        strcat(buf, semtitle);
        strcat(buf, "<table><tr><th>Subject</th><th>Marks</th><th>Credits</th><th>GradePoint</th><th>Attendance</th></tr>");
        if (strlen(sem_tables[sem]) == 0) {
            strcat(buf, "<tr><td colspan='5'>No subjects recorded for this semester.</td></tr>");
        } else {
            strcat(buf, sem_tables[sem]);
        }
        strcat(buf, "</table>");
    }

    /* Unknown/other subjects (seminar/project extras) */
    if (strlen(sem_tables[0]) > 0) {
        strcat(buf, "<h3>Other Subjects</h3><table><tr><th>Subject</th><th>Marks</th><th>Credits</th><th>GradePoint</th><th>Attendance</th></tr>");
        strcat(buf, sem_tables[0]);
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
            /* show form to mark attendance for a subject (admin-only page) */
            /* Expects query ?subject=... optionally */
            char *q = strchr(fullpath, '?');
            char subject_q[256] = {0};
            if (q) {
                char *qs = strdup(q+1);
                char *sub = form_value(qs, "subject");
                if (sub) { strncpy(subject_q, sub, sizeof(subject_q)-1); free(sub); }
                free(qs);
            }
            /* build form: choose subject (unique subjects across students), then list students who have it */
            /* gather unique subjects */
            char subjects[8192]; subjects[0]=0;
            for (int i=0;i<student_count;++i) {
                if (!students[i].exists) continue;
                for (int j=0;j<students[i].num_subjects;++j) {
                    char *sname = students[i].subjects[j].name;
                    if (strstr(subjects, sname)==NULL) {
                        if (strlen(subjects)+strlen(sname)+16 > sizeof(subjects)) continue;
                        strcat(subjects, sname);
                        strcat(subjects, "\n");
                    }
                }
            }
            /* build HTML */
            size_t cap = 16384;
            char *buf = malloc(cap);
            if (!buf) { send_text(client, "500 Internal Server Error", "text/plain", "Server error"); close(client); return; }
            strcpy(buf, "<!doctype html><html><head><meta charset='utf-8'><title>Attendance</title></head><body><h2>Admin Attendance</h2>");
            strcat(buf, "<form method='get' action='/attendance'>Choose subject: <select name='subject'>");
            /* add options */
            char *subjects_copy = strdup(subjects);
            char *line = strtok(subjects_copy, "\n");
            while (line) {
                char esc[256]; html_escape_buf(line, esc, sizeof(esc));
                char option[512];
                snprintf(option, sizeof(option), "<option value=\"%s\"%s>%s</option>", esc, (strcmp(esc, subject_q)==0) ? " selected" : "", esc);
                if (strlen(buf)+strlen(option)+256 > cap) { cap*=2; buf=realloc(buf,cap); }
                strcat(buf, option);
                line = strtok(NULL, "\n");
            }
            free(subjects_copy);
            strcat(buf, "</select><button>Open</button></form>");
            if (subject_q[0]) {
                /* show list of students with checkboxes for this subject */
                char panel[8192]; panel[0]=0;
                snprintf(panel, sizeof(panel), "<h3>Subject: %s</h3><form method='post' action='/attendance'><input type='hidden' name='subject' value='%s'/>Date (YYYY-MM-DD): <input name='date' value='", subject_q, subject_q);
                time_t t = time(NULL); struct tm tm = *localtime(&t); char datebuf[32];
                snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
                strncat(panel, datebuf, sizeof(panel)-strlen(panel)-1);
                strncat(panel, "'/> <table border='1' cellpadding='6'><tr><th>Present</th><th>ID</th><th>Name</th></tr>", sizeof(panel)-strlen(panel)-1);
                for (int i=0;i<student_count;++i) {
                    if (!students[i].exists) continue;
                    int has = 0;
                    for (int j=0;j<students[i].num_subjects;++j) if (strcmp(students[i].subjects[j].name, subject_q)==0) { has = 1; break; }
                    if (!has) continue;
                    char row[512];
                    snprintf(row, sizeof(row), "<tr><td><input type='checkbox' name='present' value='%d'/></td><td>%d</td><td>%s</td></tr>", students[i].id, students[i].id, students[i].name);
                    if (strlen(panel)+strlen(row)+256 > sizeof(panel)) break;
                    strcat(panel, row);
                }
                strcat(panel, "</table><div style='margin-top:8px'><button>Mark Attendance</button></div></form>");
                if (strlen(buf)+strlen(panel)+256 > cap) { cap*=2; buf=realloc(buf,cap); }
                strcat(buf, panel);
            }
            strcat(buf, "<p><a href='/'>Back</a></p></body></html>");
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
            /* admin dashboard: removed add student & generate report per request */
            const char *adm =
              "<!doctype html><html><head><meta charset='utf-8'><title>Admin Dashboard</title>"
              "<style>body{font-family:Arial;margin:18px} .card{max-width:900px;padding:18px;border-radius:10px;background:#fff;border:1px solid #eee} input,button,textarea,select{padding:8px;margin:6px 0;width:100%} button{background:#0b69ff;color:#fff;border:none;border-radius:6px}</style></head><body>"
              "<div class='card'><h2>Admin Dashboard</h2>"
              "<p>Manage marks and attendance.</p>"
              "<h3>View all students</h3><p><a href='/list'>Open students list</a></p>"
              "<h3>Enter marks for a student (by subject name)</h3>"
              "<form method='post' action='/enter-marks'>"
              "<input name='id' placeholder='Student ID' required />"
              "<textarea name='marks' rows='8' placeholder='Format: Subject Name|marks\\nOne subject per line'></textarea>"
              "<div style='margin-top:8px'><button>Submit marks</button></div></form>"
              "<h3>Mark attendance for a subject</h3>"
              "<form method='get' action='/attendance'><input name='subject' placeholder='Subject name (exact)'/> <button>Open</button></form>"
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

        /* Enter marks (admin) */
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
            /* parse lines */
            char *line = strtok(marks, "\n");
            int updated = 0;
            while (line) {
                /* trim */
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
            free(marks);
            api_calculate_update_cgpa(idx);
            char resp[256];
            snprintf(resp, sizeof(resp), "<p>Marks updated for ID %d (%d subjects updated). <a href='/'>Back</a></p>", sid, updated);
            send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            close(client); return;
        }

        /* Attendance POST (admin) */
        if (strncmp(path, "/attendance", 10) == 0) {
            char *subject = form_value(body, "subject");
            char *date = form_value(body, "date"); /* date string */
            /* multiple 'present' fields with student IDs */
            /* find all occurrences of "present=" by scanning body manually */
            if (!subject) { send_text(client, "400 Bad Request", "text/plain", "Missing subject"); if (date) free(date); close(client); return; }
            /* Collect present IDs */
            int present_ids[2048]; int present_count=0;
            /* naive parser: look for "present=" substrings */
            const char *p = body;
            while ((p = strstr(p, "present=")) != NULL) {
                p += strlen("present=");
                /* read number until & or end */
                int val = atoi(p);
                if (val > 0) { present_ids[present_count++] = val; }
                /* advance */
                const char *amp = strchr(p, '&');
                if (!amp) break;
                p = amp + 1;
            }
            /* mark attendance: for every student who has this subject, increment held; if in present_ids increment attended */
            int changed = 0;
            for (int i=0;i<student_count;++i) {
                if (!students[i].exists) continue;
                for (int j=0;j<students[i].num_subjects;++j) {
                    if (strcmp(students[i].subjects[j].name, subject)==0) {
                        students[i].subjects[j].classes_held += 1;
                        /* check present */
                        int was_present = 0;
                        for (int k=0;k<present_count;++k) if (present_ids[k] == students[i].id) { was_present = 1; break; }
                        if (was_present) students[i].subjects[j].classes_attended += 1;
                        changed++;
                        break;
                    }
                }
            }
            save_data(); /* call student_system.c save via global data file (students[] shared) */
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

