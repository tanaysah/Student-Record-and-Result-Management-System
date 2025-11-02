/* student_system_web.c
   Enhanced web wrapper for student_system.c
   - Landing page with Admin login / Student sign up / Student sign in
   - Student signup asks email, phone, semester
   - If signup semester = N, automatically adds subjects for sem 1..N (cumulative, deduped)
   - Admin dashboard: enter marks, mark attendance (generate report removed)
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
#define MAX_SUBJECTS 32
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
    char dept[MAX_NAME];
    int year;              // used as semester number for mapping
    int num_subjects;
    Subject subjects[MAX_SUBJECTS];
    char password[50];
    char email[100];
    char phone[32];
    int exists;
    double cgpa;
    int total_credits_completed;
} Student;

/* --- externs from student_system.c --- */
/* student array & count (must be defined in student_system.c) */
extern Student students[];
extern int student_count;

/* API wrappers (must be defined in student_system.c) */
extern int api_find_index_by_id(int id);
extern int api_add_student(Student *s);
extern int api_calculate_update_cgpa(int idx);
/* optional persistence helper in student_system.c (implement if you want persistent storage) */
extern void save_data(void);

/* filesystem helper */
static void ensure_reports_dir(void) {
    struct stat st;
    if (stat("reports", &st) == -1) {
        mkdir("reports", 0755);
    }
}

/* url-decode inplace */
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

/* minimal HTML escape helper */
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

/* grade-point helper (same logic as student_system.c) */
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

/* ---- Helpers to add subjects and to set cumulative semester subjects ---- */

/* add provided subject list to student if not already present (dedupe by name) */
static void add_subject_list_to_student(Student *s, const char *names[], const int credits[], int n) {
    for (int i = 0; i < n; ++i) {
        const char *nm = names[i];
        if (!nm || !nm[0]) continue;
        /* check existing */
        int found = 0;
        for (int j = 0; j < s->num_subjects; ++j) {
            if (strcasecmp(s->subjects[j].name, nm) == 0) { found = 1; break; }
        }
        if (found) continue;
        if (s->num_subjects >= MAX_SUBJECTS) break;
        int idx = s->num_subjects++;
        strncpy(s->subjects[idx].name, nm, sizeof(s->subjects[idx].name)-1);
        s->subjects[idx].name[sizeof(s->subjects[idx].name)-1] = '\0';
        s->subjects[idx].credits = credits[i];
        s->subjects[idx].marks = 0;
        s->subjects[idx].classes_held = 0;
        s->subjects[idx].classes_attended = 0;
    }
}

/* set (replace) subjects for a single semester (keeps only that semester) */
static void set_subjects_for_single_semester(Student *s, int sem) {
    /* clear previous */
    for (int i = 0; i < MAX_SUBJECTS; ++i) {
        s->subjects[i].name[0] = '\0';
        s->subjects[i].credits = 0;
        s->subjects[i].marks = 0;
        s->subjects[i].classes_held = 0;
        s->subjects[i].classes_attended = 0;
    }
    s->num_subjects = 0;

    if (sem <= 0) sem = 1;

    if (sem == 1) {
        const char *names[] = {
            "Programming in C",
            "Linux Lab",
            "Problem Solving",
            "Advanced Engineering Mathematics - I",
            "Physics for Computer Engineers",
            "Managing Self",
            "Environmental Sustainability and Climate Change"
        };
        const int credits[] = {5,2,2,4,5,2,2};
        add_subject_list_to_student(s, names, credits, 7);
    } else if (sem == 2) {
        const char *names[] = {
            "Data Structures and Algorithms",
            "Digital Electronics",
            "Python Programming",
            "Advanced Engineering Mathematics - II",
            "Environmental Sustainability and Climate Change",
            "Time and Priority Management",
            "Elements of AI/ML"
        };
        const int credits[] = {5,3,5,4,2,2,3};
        add_subject_list_to_student(s, names, credits, 7);
    } else if (sem == 3) {
        const char *names[] = {
            "Leading Conversations",
            "Discrete Mathematical Structures",
            "Operating Systems",
            "Elements of AIML",
            "Database Management Systems",
            "Design and Analysis of Algorithms"
        };
        const int credits[] = {2,3,3,3,5,4};
        add_subject_list_to_student(s, names, credits, 6);
    } else if (sem == 4) {
        const char *names[] = {
            "Software Engineering",
            "EDGE - SoftSkills",
            "Linear Algebra",
            "Indian Constitution",
            "Writing with Impact",
            "Object Oriented Programming",
            "Data Communication and Networks",
            "Applied Machine Learning"
        };
        const int credits[] = {3,0,3,0,2,4,4,5};
        add_subject_list_to_student(s, names, credits, 8);
    } else if (sem == 5) {
        const char *names[] = {
            "Cryptography and Network Security",
            "Formal Languages and Automata Theory",
            "Object Oriented Analysis and Design",
            "Exploratory-3",
            "Start Your Startup",
            "Research Methodology in CS",
            "Probability, Entropy, and MC Simulation",
            "PE-2",
            "PE-2 Lab"
        };
        const int credits[] = {3,3,3,3,2,3,3,4,1};
        add_subject_list_to_student(s, names, credits, 9);
    } else if (sem == 6) {
        const char *names[] = {
            "Exploratory-4",
            "Leadership and Teamwork",
            "Compiler Design",
            "Statistics and Data Analysis",
            "PE-3",
            "PE-3 Lab",
            "Minor Project"
        };
        const int credits[] = {3,2,3,3,4,1,5};
        add_subject_list_to_student(s, names, credits, 7);
    } else if (sem == 7) {
        const char *names[] = {
            "Exploratory-5",
            "PE-4",
            "PE-4 Lab",
            "PE-5",
            "PE-5 Lab",
            "Capstone Project - Phase-1",
            "Summer Internship"
        };
        const int credits[] = {3,4,1,3,1,5,1};
        add_subject_list_to_student(s, names, credits, 7);
    } else { /* sem == 8 */
        const char *names[] = {
            "IT Ethical Practices",
            "Capstone Project - Phase-2"
        };
        const int credits[] = {3,5};
        add_subject_list_to_student(s, names, credits, 2);
    }
}

/* set subjects cumulatively from sem 1 .. sem N (deduped) */
static void set_subjects_up_to_semester(Student *s, int sem) {
    if (sem < 1) sem = 1;
    /* start empty */
    for (int i = 0; i < MAX_SUBJECTS; ++i) {
        s->subjects[i].name[0] = '\0';
        s->subjects[i].credits = 0;
        s->subjects[i].marks = 0;
        s->subjects[i].classes_held = 0;
        s->subjects[i].classes_attended = 0;
    }
    s->num_subjects = 0;
    for (int k = 1; k <= sem; ++k) {
        /* reuse single-semester helper: it will call add_subject_list_to_student which dedupes */
        set_subjects_for_single_semester(s, k);
        /* Note: set_subjects_for_single_semester currently clears then adds — to use add_subject_list we must
           instead copy semester arrays here. To avoid duplication of semester arrays we create a temporary Student t
           and then add its subjects via add_subject_list_to_student. */
        /* But simpler: call small per-sem arrays by duplicating logic here instead. For clarity we call set_subjects_for_single_semester on a temp */
    }
    /* The above approach cleared subjects each iteration; to implement cumulative properly, we'll instead
       manually add subjects per semester by calling add_subject_list_to_student with the same arrays used above. */
    /* Re-implement properly below as explicit loop to avoid confusion. */
    /* Clear again and add cumulatively: */
    for (int i = 0; i < MAX_SUBJECTS; ++i) {
        s->subjects[i].name[0] = '\0';
        s->subjects[i].credits = 0;
        s->subjects[i].marks = 0;
        s->subjects[i].classes_held = 0;
        s->subjects[i].classes_attended = 0;
    }
    s->num_subjects = 0;

    for (int semidx = 1; semidx <= sem; ++semidx) {
        if (semidx == 1) {
            const char *names[] = {
                "Programming in C",
                "Linux Lab",
                "Problem Solving",
                "Advanced Engineering Mathematics - I",
                "Physics for Computer Engineers",
                "Managing Self",
                "Environmental Sustainability and Climate Change"
            };
            const int credits[] = {5,2,2,4,5,2,2};
            add_subject_list_to_student(s, names, credits, 7);
        } else if (semidx == 2) {
            const char *names[] = {
                "Data Structures and Algorithms",
                "Digital Electronics",
                "Python Programming",
                "Advanced Engineering Mathematics - II",
                "Environmental Sustainability and Climate Change",
                "Time and Priority Management",
                "Elements of AI/ML"
            };
            const int credits[] = {5,3,5,4,2,2,3};
            add_subject_list_to_student(s, names, credits, 7);
        } else if (semidx == 3) {
            const char *names[] = {
                "Leading Conversations",
                "Discrete Mathematical Structures",
                "Operating Systems",
                "Elements of AIML",
                "Database Management Systems",
                "Design and Analysis of Algorithms"
            };
            const int credits[] = {2,3,3,3,5,4};
            add_subject_list_to_student(s, names, credits, 6);
        } else if (semidx == 4) {
            const char *names[] = {
                "Software Engineering",
                "EDGE - SoftSkills",
                "Linear Algebra",
                "Indian Constitution",
                "Writing with Impact",
                "Object Oriented Programming",
                "Data Communication and Networks",
                "Applied Machine Learning"
            };
            const int credits[] = {3,0,3,0,2,4,4,5};
            add_subject_list_to_student(s, names, credits, 8);
        } else if (semidx == 5) {
            const char *names[] = {
                "Cryptography and Network Security",
                "Formal Languages and Automata Theory",
                "Object Oriented Analysis and Design",
                "Exploratory-3",
                "Start Your Startup",
                "Research Methodology in CS",
                "Probability, Entropy, and MC Simulation",
                "PE-2",
                "PE-2 Lab"
            };
            const int credits[] = {3,3,3,3,2,3,3,4,1};
            add_subject_list_to_student(s, names, credits, 9);
        } else if (semidx == 6) {
            const char *names[] = {
                "Exploratory-4",
                "Leadership and Teamwork",
                "Compiler Design",
                "Statistics and Data Analysis",
                "PE-3",
                "PE-3 Lab",
                "Minor Project"
            };
            const int credits[] = {3,2,3,3,4,1,5};
            add_subject_list_to_student(s, names, credits, 7);
        } else if (semidx == 7) {
            const char *names[] = {
                "Exploratory-5",
                "PE-4",
                "PE-4 Lab",
                "PE-5",
                "PE-5 Lab",
                "Capstone Project - Phase-1",
                "Summer Internship"
            };
            const int credits[] = {3,4,1,3,1,5,1};
            add_subject_list_to_student(s, names, credits, 7);
        } else { /* 8 */
            const char *names[] = {
                "IT Ethical Practices",
                "Capstone Project - Phase-2"
            };
            const int credits[] = {3,5};
            add_subject_list_to_student(s, names, credits, 2);
        }
    }
}

/* Build landing page with three cards (includes email/phone/semester signup) */
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
        "<p>Register — select your current semester. Subjects for all previous semesters will be added automatically.</p>"
        "<form method='post' action='/student-signup'>"
        "<input name='name' placeholder='Full Name' required />"
        "<input name='age' placeholder='Age' required />"
        "<input name='sap_id' placeholder='SAP ID' required />"
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
        "<p class='muted'>After registration, use your SAP ID to log in.</p>"
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

    size_t cap = strlen(html_start) + strlen(admin_card) + strlen(signup_card) + strlen(signin_card) + strlen(footer) + 512;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, html_start);
    strcat(buf, admin_card);
    strcat(buf, signup_card);
    strcat(buf, signin_card);
    strcat(buf, footer);
    return buf;
}

/* send text response helper */
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
    strcpy(buf, "<!doctype html><html><head><meta charset='utf-8'><title>Students</title></head><body><h2>Students</h2><table border='1' cellpadding='6'><tr><th>ID</th><th>Name</th><th>Sem</th><th>Dept</th><th>Email</th><th>Phone</th></tr>");
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        char row[2048];
        snprintf(row, sizeof(row), "<tr><td>%d</td><td>%s</td><td>%d</td><td>%s</td><td>%s</td><td>%s</td></tr>",
                 students[i].id, students[i].name, students[i].year, students[i].dept, students[i].email, students[i].phone);
        if (strlen(buf) + strlen(row) + 256 > cap) { cap *= 2; buf = realloc(buf, cap); }
        strcat(buf, row);
    }
    strcat(buf, "</table><p><a href='/'>Back</a></p></body></html>");
    return buf;
}

/* build student dashboard as HTML with an inline SVG attendance graph (shows email & phone & semester) */
static char *build_student_dashboard(int idx) {
    if (idx < 0 || idx >= student_count) return NULL;
    Student *s = &students[idx];
    char escaped_name[256]; html_escape_buf(s->name, escaped_name, sizeof(escaped_name));
    char escaped_email[256]; html_escape_buf(s->email, escaped_email, sizeof(escaped_email));
    char escaped_phone[64]; html_escape_buf(s->phone, escaped_phone, sizeof(escaped_phone));

    /* build subject table & gather attendance percentages for chart */
    char subject_rows[16384];
    subject_rows[0] = 0;
    int maxbars = s->num_subjects;
    int percentages[MAX_SUBJECTS];
    for (int i = 0; i < s->num_subjects; ++i) {
        int held = s->subjects[i].classes_held;
        int att = s->subjects[i].classes_attended;
        int pct = (held == 0) ? 0 : (int)(((double)att / held) * 100.0 + 0.5);
        percentages[i] = pct;
        char row[1024];
        char sname_esc[256]; html_escape_buf(s->subjects[i].name, sname_esc, sizeof(sname_esc));
        double gp = (s->subjects[i].credits>0)? marks_to_grade_point_local(s->subjects[i].marks) : 0;
        snprintf(row, sizeof(row),
                 "<tr><td>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%.0f</td><td>%d%%</td></tr>",
                 i+1, sname_esc, s->subjects[i].marks, s->subjects[i].credits, gp, pct);
        strncat(subject_rows, row, sizeof(subject_rows)-strlen(subject_rows)-1);
    }
    double sgpa = compute_sgpa_local(s);
    /* Build small inline SVG bar chart */
    char svg[8192];
    int w = 640, h = 220, pad = 36;
    int barw = (maxbars>0) ? ( (w - pad*2) / maxbars - 8 ) : 20;
    if (barw < 6) barw = 6;
    char svg_start[256];
    snprintf(svg_start, sizeof(svg_start), "<svg viewBox='0 0 %d %d' width='%d' height='%d' xmlns='http://www.w3.org/2000/svg'><rect width='100%%' height='100%%' fill='transparent'/>", w, h, w, h);
    strcpy(svg, svg_start);
    /* axes */
    snprintf(svg + strlen(svg), sizeof(svg)-strlen(svg), "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#eee'/>", pad, h-pad, w-pad, h-pad);
    for (int i = 0; i < maxbars; ++i) {
        int x = pad + i*(barw+8);
        int barh = (percentages[i] * (h - pad*2)) / 100;
        int y = (h - pad) - barh;
        snprintf(svg + strlen(svg), sizeof(svg)-strlen(svg),
                 "<rect x='%d' y='%d' width='%d' height='%d' rx='4' ry='4' fill='#3b82f6' opacity='0.85'/>",
                 x, y, barw, barh);
        /* label (short) */
        char lbl[96]; html_escape_buf(s->subjects[i].name, lbl, sizeof(lbl));
        if (strlen(lbl) > 20) lbl[20] = '\0';
        snprintf(svg + strlen(svg), sizeof(svg)-strlen(svg),
                 "<text x='%d' y='%d' font-size='11' fill='#111'>%s</text>",
                 x, h-pad+14, lbl);
    }
    strcat(svg, "</svg>");

    const char *tpl_start =
        "<!doctype html><html><head><meta charset='utf-8'><title>Dashboard</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>body{font-family:Inter,Arial;margin:18px} .card{background:#fff;padding:18px;border-radius:10px;box-shadow:0 6px 18px rgba(0,0,0,0.06);max-width:1100px;margin:auto} table{width:100%;border-collapse:collapse} table th,table td{padding:8px;border:1px solid #eee;text-align:left;font-size:14px}</style>"
        "</head><body><div class='card'>";

    const char *tpl_end = "<p><a href='/'>← Back to Home</a></p></div></body></html>";

    /* estimate size */
    size_t cap = strlen(tpl_start) + 8192 + strlen(subject_rows) + strlen(svg) + 2048;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    strcpy(buf, tpl_start);
    char header[1024];
    char dept_esc[256]; html_escape_buf(s->dept, dept_esc, sizeof(dept_esc));
    snprintf(header, sizeof(header),
             "<h2>Welcome, %s</h2><p>ID: %d | Dept: %s | Semester: %d | Age: %d</p>"
             "<p>Email: %s | Phone: %s</p>"
             "<p><strong>SGPA (current):</strong> %.3f  &nbsp;&nbsp; <strong>Stored CGPA:</strong> %.3f (Credits: %d)</p>",
             escaped_name, s->id, dept_esc, s->year, s->age, escaped_email, escaped_phone, sgpa, s->cgpa, s->total_credits_completed);
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

    /* GET routes */
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
                if (p) { strncpy(pass, p, sizeof(pass)-1); pass[sizeof(pass)-1] = '\0'; free(p); }
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
    }

    /* POST routes */
    if (strcmp(method, "POST") == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if (!body) { send_text(client, "400 Bad Request", "text/plain", "No body"); close(client); return; }
        body += 4;

        /* --- Admin login (demo check admin/admin) --- */
        if (strncmp(path, "/admin-login", 12) == 0) {
            char *user = form_value(body, "username");
            char *pass = form_value(body, "password");
            if (!user || !pass) {
                send_text(client, "400 Bad Request", "text/plain", "Missing username or password");
                if (user) free(user); if (pass) free(pass);
                close(client); return;
            }
            /* Demo admin check: change this if you integrate a proper api_admin_auth */
            int ok = (strcmp(user, "admin") == 0 && strcmp(pass, "admin") == 0);
            free(user); free(pass);
            if (!ok) { send_text(client, "401 Unauthorized", "text/plain", "Invalid admin credentials"); close(client); return; }

            const char *adm =
              "<!doctype html><html><head><meta charset='utf-8'><title>Admin Dashboard</title>"
              "<style>body{font-family:Arial;margin:18px} .card{max-width:900px;padding:18px;border-radius:10px;background:#fff;border:1px solid #eee} input,button,textarea{padding:8px;margin:6px 0;width:100%} button{background:#0b69ff;color:#fff;border:none;border-radius:6px}</style></head><body>"
              "<div class='card'><h2>Admin Dashboard</h2>"
              "<p>Use the sections below to manage marks and attendance.</p>"
              "<h3>View all students</h3><p><a href='/list' target='_blank'>Open students list</a></p>"
              "<h3>Enter marks for a student</h3>"
              "<p>Step 1: enter Student ID to load their subjects.</p>"
              "<form method='post' action='/enter-marks-load'>"
              "<input name='id' placeholder='Student ID' required />"
              "<div style='margin-top:8px'><button>Load Subjects</button></div>"
              "</form>"
              "<h3>Mark attendance</h3>"
              "<p>Enter the subject name (exact match) and choose date. Then mark present for each student.</p>"
              "<form method='post' action='/attendance-load'>"
              "<input name='subject' placeholder='Subject name (exact)' required />"
              "<input name='date' placeholder='Date (YYYY-MM-DD) - optional (default today)' />"
              "<div style='margin-top:8px'><button>Load Student List</button></div>"
              "</form>"
              "<p><a href='/'>Back</a></p></div></body></html>";
            send_text(client, "200 OK", "text/html; charset=utf-8", adm);
            close(client); return;
        }

        /* --- Student sign-up (auto-add semester 1..N subjects) --- */
        if (strncmp(path, "/student-signup", 16) == 0) {
            char *name = form_value(body, "name");
            char *age = form_value(body, "age");
            char *sap = form_value(body, "sap_id");
            char *email = form_value(body, "email");
            char *phone = form_value(body, "phone");
            char *semester = form_value(body, "semester");
            char *password = form_value(body, "password");
            if (!name || !age || !sap || !password || !email || !phone || !semester) {
                send_text(client, "400 Bad Request", "text/plain", "Missing fields");
                if (name) free(name); if (age) free(age); if (sap) free(sap);
                if (email) free(email); if (phone) free(phone); if (semester) free(semester);
                if (password) free(password);
                close(client); return;
            }

            int sapid = atoi(sap);
            int semnum = atoi(semester);
            if (sapid <= 0 || semnum < 1 || semnum > 8) {
                char resp[256];
                snprintf(resp, sizeof(resp),
                    "<!doctype html><html><body><p>Invalid SAP ID or semester. Use numeric SAP ID and semester 1-8.</p><p><a href='/'>Back</a></p></body></html>");
                send_text(client, "400 Bad Request", "text/html; charset=utf-8", resp);
                free(name); free(age); free(sap); free(email); free(phone); free(semester); free(password);
                close(client); return;
            }

            Student s; memset(&s, 0, sizeof(s));
            s.exists = 1;
            s.cgpa = 0.0;
            s.total_credits_completed = 0;

            /* copy simple fields safely */
            strncpy(s.name, name, sizeof(s.name) - 1); s.name[sizeof(s.name)-1] = '\0';
            s.age = atoi(age);
            strncpy(s.dept, "B.Tech CSE", sizeof(s.dept) - 1); s.dept[sizeof(s.dept)-1] = '\0';
            s.year = semnum; /* sem selected by user */
            s.id = sapid;
            strncpy(s.password, password, sizeof(s.password) - 1); s.password[sizeof(s.password)-1] = '\0';
            strncpy(s.email, email, sizeof(s.email)-1); s.email[sizeof(s.email)-1] = '\0';
            strncpy(s.phone, phone, sizeof(s.phone)-1); s.phone[sizeof(s.phone)-1] = '\0';

            /* Set subjects cumulatively up to s.year */
            set_subjects_up_to_semester(&s, s.year);

            /* Save student (api_add_student returns -2 on duplicate id) */
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
                save_data();
                char resp[512];
                snprintf(resp, sizeof(resp),
                    "<!doctype html><html><body><p>Registration successful!</p>"
                    "<p>Your Student ID (SAP ID): <strong>%d</strong></p>"
                    "<p>Subjects for semesters 1..%d have been added automatically.</p>"
                    "<p><a href='/'>Back to Home</a></p></body></html>", addres, s.year);
                send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            }

            free(name); free(age); free(sap); free(email); free(phone); free(semester); free(password);
            close(client); return;
        }

        /* --- Admin: add student (form) --- */
        if (strncmp(path, "/add", 4) == 0) {
            char *name = form_value(body, "name");
            char *age = form_value(body, "age");
            char *dept = form_value(body, "dept");
            char *year = form_value(body, "year");
            char *num_sub = form_value(body, "num_subjects");
            char *subjects = form_value(body, "subjects");
            char *password = form_value(body, "password");
            if (!name || !age || !dept || !year || !password) {
                send_text(client, "400 Bad Request", "text/plain", "Missing fields (name/age/dept/year/password required)");
                goto add_cleanup;
            }
            Student s; memset(&s, 0, sizeof(s));
            s.exists = 1; s.cgpa = 0.0; s.total_credits_completed = 0;
            strncpy(s.name, name, sizeof(s.name)-1); s.name[sizeof(s.name)-1] = '\0';
            s.age = atoi(age);
            strncpy(s.dept, dept, sizeof(s.dept)-1); s.dept[sizeof(s.dept)-1] = '\0';
            s.year = atoi(year); if (s.year < 1) s.year = 1;
            s.id = 0; /* admin can set explicit id if you extend form */
            strncpy(s.password, password, sizeof(s.password)-1); s.password[sizeof(s.password)-1] = '\0';

            /* If admin provided explicit subject list, use it; otherwise use cumulative semester mapping */
            if (subjects && strlen(subjects) > 1) {
                int ns = atoi(num_sub ? num_sub : "0");
                if (ns <= 0 || ns > MAX_SUBJECTS) ns = MAX_SUBJECTS;
                char *tmp = strdup(subjects);
                char *tok = strtok(tmp, ",");
                int si = 0;
                while (tok && si < ns) {
                    while (*tok == ' ') tok++;
                    strncpy(s.subjects[si].name, tok, sizeof(s.subjects[si].name)-1);
                    s.subjects[si].name[sizeof(s.subjects[si].name)-1] = '\0';
                    s.subjects[si].classes_held = s.subjects[si].classes_attended = s.subjects[si].marks = s.subjects[si].credits = 0;
                    si++; tok = strtok(NULL, ",");
                }
                free(tmp);
                s.num_subjects = ns;
            } else {
                /* apply cumulative semester mapping */
                set_subjects_up_to_semester(&s, s.year);
            }

            api_add_student(&s);
            save_data();
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

        /* --- Load subject-form for entering marks for a student (admin) --- */
        if (strncmp(path, "/enter-marks-load", 17) == 0) {
            char *id_s = form_value(body, "id");
            if (!id_s) { send_text(client, "400 Bad Request", "text/plain", "Missing id"); close(client); return; }
            int sid = atoi(id_s);
            free(id_s);
            int idx = api_find_index_by_id(sid);
            if (idx == -1) { send_text(client, "404 Not Found", "text/plain", "Student not found"); close(client); return; }
            Student *s = &students[idx];

            size_t cap = 4096 + s->num_subjects * 256;
            char *page = malloc(cap);
            if (!page) { send_text(client, "500 Internal Server Error", "text/plain", "OOM"); close(client); return; }
            snprintf(page, cap, "<!doctype html><html><head><meta charset='utf-8'><title>Enter Marks</title></head><body><h2>Enter marks for %s (ID %d)</h2><form method='post' action='/enter-marks-submit'>", s->name, s->id);
            for (int i = 0; i < s->num_subjects; ++i) {
                char tmp[512];
                snprintf(tmp, sizeof(tmp),
                         "<label>%s (Credits: %d):<br><input name='mark_%d' placeholder='Marks (0-100)' required /></label>"
                         "<input type='hidden' name='subidx_%d' value='%d' />",
                         s->subjects[i].name, s->subjects[i].credits, i, i, i);
                strncat(page, tmp, cap - strlen(page) - 1);
            }
            char tail[256];
            snprintf(tail, sizeof(tail), "<input type='hidden' name='id' value='%d' /><div style='margin-top:8px'><button>Submit Marks</button></div></form><p><a href='/'>Back</a></p></body></html>", s->id);
            strncat(page, tail, cap - strlen(page) - 1);
            send_text(client, "200 OK", "text/html; charset=utf-8", page);
            free(page);
            close(client); return;
        }

        /* --- Submit marks for a student (admin) --- */
        if (strncmp(path, "/enter-marks-submit", 20) == 0) {
            char *id_s = form_value(body, "id");
            if (!id_s) { send_text(client, "400 Bad Request", "text/plain", "Missing id"); close(client); return; }
            int sid = atoi(id_s); free(id_s);
            int idx = api_find_index_by_id(sid);
            if (idx == -1) { send_text(client, "404 Not Found", "text/plain", "Student not found"); close(client); return; }
            Student *s = &students[idx];

            for (int i = 0; i < s->num_subjects; ++i) {
                char key[32];
                snprintf(key, sizeof(key), "mark_%d", i);
                char *mv = form_value(body, key);
                if (mv) {
                    int mk = atoi(mv);
                    if (mk < 0) mk = 0;
                    if (mk > 100) mk = 100;
                    s->subjects[i].marks = mk;
                    free(mv);
                }
            }

            api_calculate_update_cgpa(idx);
            save_data();

            char resp[256];
            snprintf(resp, sizeof(resp), "<!doctype html><html><body><p>Marks saved for ID %d. <a href='/'>Back</a></p></body></html>", sid);
            send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            close(client); return;
        }

        /* --- Load attendance marking page for a subject (admin) --- */
        if (strncmp(path, "/attendance-load", 16) == 0) {
            char *subject = form_value(body, "subject");
            char *date = form_value(body, "date"); /* optional */
            if (!subject) { send_text(client, "400 Bad Request", "text/plain", "Missing subject"); if (date) free(date); close(client); return; }
            char datebuf[64];
            if (!date || date[0] == 0) {
                time_t t = time(NULL); struct tm tm = *localtime(&t);
                snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);
            } else {
                strncpy(datebuf, date, sizeof(datebuf)-1); datebuf[sizeof(datebuf)-1] = '\0';
            }

            size_t cap = 8192 + student_count * 512;
            char *page = malloc(cap);
            if (!page) { send_text(client, "500", "text/plain", "OOM"); free(subject); if (date) free(date); close(client); return; }
            snprintf(page, cap, "<!doctype html><html><head><meta charset='utf-8'><title>Mark Attendance</title></head><body><h2>Attendance for subject: %s on %s</h2><form method='post' action='/attendance-submit'>", subject, datebuf);

            int found = 0;
            for (int i = 0; i < student_count; ++i) {
                if (!students[i].exists) continue;
                for (int j = 0; j < students[i].num_subjects; ++j) {
                    if (strcasecmp(students[i].subjects[j].name, subject) == 0) {
                        char row[800];
                        snprintf(row, sizeof(row),
                                 "<div style='margin:8px 0'><label><input type='checkbox' name='present_%d' /> Present</label> - ID: %d | %s</div><input type='hidden' name='studentid_%d' value='%d|%d' />",
                                 students[i].id, students[i].id, students[i].name, students[i].id, students[i].id, j);
                        strncat(page, row, cap - strlen(page) - 1);
                        found = 1;
                        break;
                    }
                }
            }
            if (!found) strncat(page, "<p>No students found with that subject.</p>", cap - strlen(page) - 1);

            char tail[512];
            snprintf(tail, sizeof(tail), "<input type='hidden' name='subject' value='%s' /><input type='hidden' name='date' value='%s' /><div style='margin-top:12px'><button>Submit Attendance</button></div></form><p><a href='/'>Back</a></p></body></html>", subject, datebuf);
            strncat(page, tail, cap - strlen(page) - 1);

            send_text(client, "200 OK", "text/html; charset=utf-8", page);
            free(page);
            free(subject);
            if (date) free(date);
            close(client); return;
        }

        /* --- Submit attendance (admin) --- */
        if (strncmp(path, "/attendance-submit", 18) == 0) {
            char *subject = form_value(body, "subject");
            char *date = form_value(body, "date");
            if (!subject || !date) { send_text(client, "400", "text/plain", "Missing fields"); if (subject) free(subject); if (date) free(date); close(client); return; }

            for (int i = 0; i < student_count; ++i) {
                if (!students[i].exists) continue;
                char hidkey[48];
                snprintf(hidkey, sizeof(hidkey), "studentid_%d", students[i].id);
                char *val = form_value(body, hidkey);
                if (!val) continue;
                int sid = 0, subidx = -1;
                sscanf(val, "%d|%d", &sid, &subidx);
                free(val);
                if (sid != students[i].id || subidx < 0 || subidx >= students[i].num_subjects) continue;
                /* increment classes held */
                students[i].subjects[subidx].classes_held += 1;
                /* check present checkbox */
                char preskey[48];
                snprintf(preskey, sizeof(preskey), "present_%d", students[i].id);
                char *pres = form_value(body, preskey);
                if (pres) {
                    students[i].subjects[subidx].classes_attended += 1;
                    free(pres);
                }
            }

            save_data();

            char resp[512];
            snprintf(resp, sizeof(resp), "<!doctype html><html><body><p>Attendance recorded for subject '%s' on %s.</p><p><a href='/'>Back</a></p></body></html>", subject, date);
            send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            free(subject); free(date);
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
