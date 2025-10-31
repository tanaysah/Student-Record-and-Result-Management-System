/* student_system_web.c
 Minimal pure-C HTTP wrapper for student_system.c
 Compile together with student_system.c:
   gcc -DBUILD_WEB student_system.c student_system_web.c -o student_system_web
 Run:
   export PORT=8080
   ./student_system_web
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
/* shared array and count */
extern Student students[];
extern int student_count;

/* API wrappers you added in student_system.c */
extern int api_find_index_by_id(int id);
extern int api_add_student(Student *s);
extern void api_generate_report(int idx, const char* college, const char* semester, const char* exam);
extern int api_calculate_update_cgpa(int idx);

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

S

/* Build small index HTML */
static char *build_index_html(void) {
    ensure_reports_dir();
    /* collect reports */
    char *reports[256]; int rc = 0;
    DIR *d = opendir("reports");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) && rc < 256) {
            if (ent->d_type == DT_REG || ent->d_type == DT_UNKNOWN) {
                size_t L = strlen(ent->d_name);
                if (L > 5 && strcasecmp(ent->d_name + L - 5, ".html") == 0) {
                    reports[rc++] = strdup(ent->d_name);
                }
            }
        }
        closedir(d);
    }

    const char *head =
        "<!doctype html><html><head><meta charset='utf-8'><title>Student Record and Result Management System</title>"
        "<style>body{font-family:Arial;background:#f6f7fb;margin:20px} .card{background:#fff;padding:18px;border-radius:8px;max-width:1000px;margin:auto;box-shadow:0 2px 6px rgba(0,0,0,0.08)}"
        "input,textarea{width:90%;padding:6px;margin:6px 0} label{display:block;margin-top:8px}</style></head><body><div class='card'>"
        "<h1>Student Record and Result Management System</h1><h2>Programming in C</h2>"
        "<p><strong>Created by:</strong> Tanay Sah (590023170) | Mahika Jaglan (590025346)</p><hr>"
        "<h3>Quick actions</h3>"
        "<p><a href='/list'>View student list</a></p>";

    const char *forms =
        "<h4>View student</h4><form method='get' action='/view'>Student ID: <input name='id' /> <button>View</button></form>"
        "<h4>Add student</h4><form method='post' action='/add'>"
        "<label>Name: <input name='name' required/></label>"
        "<label>Age: <input name='age' required/></label>"
        "<label>Dept: <input name='dept' required/></label>"
        "<label>Year: <input name='year' required/></label>"
        "<label>Number of subjects: <input name='num_subjects' required/></label>"
        "<label>Subjects (comma-separated): <input name='subjects' placeholder='C Programming,Maths' required/></label>"
        "<label>Password: <input name='password' required/></label>"
        "<button>Add</button></form>"
        "<h4>Enter marks for a student</h4>"
        "<form method='post' action='/enter-marks'>"
        "<label>Student ID: <input name='id' required/></label>"
        "<label>Marks & credits (one per line as mark,credit):<br><textarea name='marks' rows='6' placeholder='90,3\n85,4\n78,3'></textarea></label>"
        "<button>Submit marks</button></form>"
        "<h4>Generate report</h4>"
        "<form method='post' action='/generate'>"
        "<label>Student ID: <input name='id' required/></label>"
        "<label>College: <input name='college'/></label>"
        "<label>Semester: <input name='semester'/></label>"
        "<label>Exam: <input name='exam'/></label>"
        "<button>Generate</button></form>"
        "<h3>Generated reports</h3>";

    size_t bufcap = strlen(head) + strlen(forms) + 4096;
    for (int i = 0; i < rc; ++i) bufcap += strlen(reports[i]) + 64;
    bufcap += 1024;
    char *buf = malloc(bufcap);
    if (!buf) return NULL;
    strcpy(buf, head);
    strcat(buf, forms);
    if (rc == 0) strcat(buf, "<p>No reports found.</p>");
    else {
        strcat(buf, "<ul>");
        for (int i = 0; i < rc; ++i) {
            char item[1024];
            snprintf(item, sizeof(item), "<li><a href=\"/reports/%s\" target=\"_blank\">%s</a></li>", reports[i], reports[i]);
            strcat(buf, item);
            free(reports[i]);
        }
        strcat(buf, "</ul>");
    }
    strcat(buf, "<hr><p>Tip: Use the forms above. Reports will appear in the reports/ folder.</p></div></body></html>");
    return buf;
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

/* Send a generic HTML/text response */
static void send_text(int client, const char *status, const char *ctype, const char *body) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                        status, ctype, strlen(body));
    send(client, header, hlen, 0);
    send(client, body, strlen(body), 0);
}

/* Read request (headers and body) into buffer (simple) */
#define REQBUF 200000
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

/* build simple student list HTML */
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

/* build full profile text (preformatted) */
static char *build_view_text(int idx) {
    if (idx < 0 || idx >= student_count) return NULL;
    Student *s = &students[idx];
    char *buf = malloc(8192);
    if (!buf) return NULL;
    int off = snprintf(buf, 8192, "<!doctype html><html><head><meta charset='utf-8'><title>Student %d</title></head><body><pre>", s->id);
    off += snprintf(buf + off, 8192 - off, "ID: %d\nName: %s\nAge: %d\nDept: %s\nYear: %d\nSubjects: %d\n", s->id, s->name, s->age, s->dept, s->year, s->num_subjects);
    for (int i = 0; i < s->num_subjects; ++i) {
        int held = s->subjects[i].classes_held, att = s->subjects[i].classes_attended;
        double pct = (held == 0) ? 0.0 : ((double)att / held) * 100.0;
        off += snprintf(buf + off, 8192 - off, " %d) %s - Attended %d/%d (%.2f%%) | Marks: %d | Credits: %d\n",
                        i+1, s->subjects[i].name, att, held, pct, s->subjects[i].marks, s->subjects[i].credits);
    }
    off += snprintf(buf + off, 8192 - off, "\nStored CGPA: %.3f (Credits: %d)\n", s->cgpa, s->total_credits_completed);
    off += snprintf(buf + off, 8192 - off, "</pre><p><a href='/'>Back</a></p></body></html>");
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
            char *page = build_index_html();
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
        if (strncmp(path, "/view", 5) == 0) {
            /* parse query ?id= */
            char *q = strchr(path, '?');
            int id = -1;
            if (q) {
                char *qs = strdup(q+1);
                char *val = form_value(qs, "id");
                if (val) { id = atoi(val); free(val); }
                free(qs);
            }
            if (id <= 0) { send_text(client, "400 Bad Request", "text/plain", "Missing id"); close(client); return; }
            int idx = api_find_index_by_id(id);
            if (idx == -1) { send_text(client, "404 Not Found", "text/plain", "Student not found"); close(client); return; }
            char *page = build_view_text(idx);
            if (!page) send_text(client, "500 Internal Server Error", "text/plain", "Server error");
            else { send_text(client, "200 OK", "text/html; charset=utf-8", page); free(page); }
            close(client); return;
        }
    } else if (strcmp(method, "POST") == 0) {
        /* find body */
        char *body = strstr(req, "\r\n\r\n");
        if (!body) { send_text(client, "400 Bad Request", "text/plain", "No body"); close(client); return; }
        body += 4;

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
            /* fill */
            strncpy(s.name, name, sizeof(s.name)-1);
            s.age = atoi(age);
            strncpy(s.dept, dept, sizeof(s.dept)-1);
            s.year = atoi(year);
            s.num_subjects = atoi(num_sub);
            if (s.num_subjects < 1 || s.num_subjects > MAX_SUBJECTS) s.num_subjects = MAX_SUBJECTS;
            /* split subjects by comma */
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
            /* parse marks lines: mark,credit per line — apply sequentially to student's existing subjects */
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
            /* update CGPA using API */
            api_calculate_update_cgpa(idx);
            char resp[256];
            snprintf(resp, sizeof(resp), "<p>Marks updated for ID %d. <a href='/'>Back</a></p>", sid);
            send_text(client, "200 OK", "text/html; charset=utf-8", resp);
            close(client); return;
        }

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

