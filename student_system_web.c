/* student_system_web.c
   Minimal pure-C HTTP wrapper for student_system.c
   Compile with:
     gcc student_system.c student_system_web.c -o student_system_web
   Run locally:
     export PORT=8080
     ./student_system_web
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      // for strcasecmp, strcasestr
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <dirent.h>       // for DIR, struct dirent
#include <stddef.h>

/* --- Forward declarations of functions from student_system.c --- */
/* Make sure these functions exist in your student_system.c file */
extern void load_data(void);
extern void save_data(void);
extern void generate_html_report(int idx, const char* college, const char* semester, const char* exam);
extern void add_student_custom(void *s_ptr); /* we will cast */
extern int find_index_by_id(int id);
extern void calculate_and_update_cgpa_for_student(int idx);

/* Data structures must match your student_system.c definitions.
   We'll declare the same Student and Subject structures here (copy-paste compatible). */
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

/* Helper: URL-decode (form values) */
static void urldecode_inplace(char *s) {
    char *d = s;
    while (*s) {
        if (*s == '+') { *d++ = ' '; s++; }
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *d++ = (char) strtol(hex, NULL, 16);
            s += 3;
        } else {
            *d++ = *s++;
        }
    }
    *d = 0;
}

/* Extract value for a key from x-www-form-urlencoded body (simple single-value) */
static char *form_value(const char *body, const char *key) {
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (!eq) break;
        size_t name_len = (amp ? (size_t)(eq - p) : (size_t)(eq - p));
        if (name_len == klen && strncmp(p, key, klen) == 0) {
            const char *val_start = eq + 1;
            const char *val_end = amp ? amp : (p + strlen(p));
            size_t vlen = val_end - val_start;
            char *ret = malloc(vlen + 1);
            if (!ret) return NULL;
            memcpy(ret, val_start, vlen);
            ret[vlen] = 0;
            urldecode_inplace(ret);
            return ret;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

/* Simple HTML helpers */
static void send_response(int client, const char *status, const char *content_type, const char *body) {
    char header[10240];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status, content_type, strlen(body));
    send(client, header, len, 0);
    send(client, body, strlen(body), 0);
}

/* Build small index HTML */
static char *build_index_html(const char *demo_output, char **reports, int report_count) {
    const char *template_head =
        "<!doctype html><html><head><meta charset='utf-8'><title>Student Record and Result Management System</title>"
        "<style>body{font-family:Arial;background:#f6f7fb;margin:20px} .card{background:#fff;padding:18px;border-radius:8px;max-width:1000px;margin:auto;box-shadow:0 2px 6px rgba(0,0,0,0.08)}"
        "pre{background:#f5f5f5;padding:12px;border-radius:6px;white-space:pre-wrap}</style></head><body><div class='card'>"
        "<h1>Student Record and Result Management System</h1><h2>Programming in C</h2>"
        "<p><strong>Created by:</strong> Tanay Sah (590023170) | Mahika Jaglan (590025346)</p><hr>"
        "<h3>Demo Output</h3><pre>";
    const char *template_mid = "</pre>"
        "<h3>Quick actions</h3><p><a href='/list'>View student list</a></p>"
        "<h4>View student</h4>"
        "<form method='get' action='/view'>Student ID: <input name='id' /> <button>View</button></form>"
        "<h4>Add student</h4>"
        "<form method='post' action='/add'>"
        "Name: <input name='name' required/><br>Age: <input name='age' required/><br>Dept: <input name='dept' required/><br>"
        "Year: <input name='year' required/><br>Number of subjects: <input name='num_subjects' required/><br>"
        "Subjects (comma-separated): <input name='subjects' placeholder='C Programming,Maths' style='width:70%' required/><br>"
        "Password: <input name='password' required/><br><button>Add</button></form>"
        "<h4>Enter marks for a student</h4>"
        "<form method='post' action='/enter-marks'>Student ID: <input name='id' required/><br>"
        "Marks & credits (one per line as mark,credit):<br><textarea name='marks' rows='6' cols='80' placeholder='90,3\n85,4\n78,3'></textarea><br><button>Submit marks</button></form>"
        "<h4>Generate report</h4>"
        "<form method='post' action='/generate'>Student ID: <input name='id' required/><br>"
        "College: <input name='college'/><br>Semester: <input name='semester'/><br>Exam: <input name='exam'/><br><button>Generate</button></form>"
        "<h3>Generated reports</h3>";
    const char *template_tail = "<hr><p>Tip: Use the forms above. Reports will appear in the reports/ folder.</p></div></body></html>";

    /* assemble reports HTML */
    size_t bufsize =  (strlen(template_head) + strlen(demo_output) + strlen(template_mid) + strlen(template_tail) + 1024);
    for (int i=0;i<report_count;i++) bufsize += strlen(reports[i]) + 64;
    char *buf = malloc(bufsize);
    if (!buf) return NULL;
    strcpy(buf, template_head);
    strcat(buf, demo_output);
    strcat(buf, template_mid);
    if (report_count == 0) strcat(buf, "<p>No reports found.</p>");
    else {
        strcat(buf, "<ul>");
        for (int i=0;i<report_count;i++) {
            char item[1024];
            snprintf(item, sizeof(item), "<li><a href=\"/reports/%s\" target=\"_blank\">%s</a></li>", reports[i], reports[i]);
            strcat(buf, item);
        }
        strcat(buf, "</ul>");
    }
    strcat(buf, template_tail);
    return buf;
}

/* Serve file from reports/ directory */
static void serve_report_file(int client, const char *filename) {
    if (strstr(filename, "..")) { send_response(client, "400 Bad Request", "text/plain", "Bad filename"); return; }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "reports/%s", filename);
    FILE *f = fopen(path, "rb");
    if (!f) { send_response(client, "404 Not Found", "text/plain", "Report not found"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(sz+1);
    if (!data) { fclose(f); send_response(client, "500 Internal", "text/plain", "Server memory error"); return; }
    fread(data,1,sz,f);
    data[sz]=0;
    fclose(f);
    send_response(client, "200 OK", "text/html; charset=utf-8", data);
    free(data);
}

/* Read request into buffer (simple, not streaming) */
#define REQBUF 200000
static int read_request(int client, char *buf, int bufsz) {
    int total=0;
    int r;
    /* read headers + possible body; stop when connection closed or we've read enough */
    while ((r = recv(client, buf+total, bufsz-total-1, 0)) > 0) {
        total += r;
        if (total > 4 && strstr(buf, "\r\n\r\n")) break; /* headers end */
        if (total > bufsz-100) break;
    }
    if (r <= 0 && total==0) return -1;
    buf[total]=0;
    /* if there's a Content-Length header, try to read the body as well */
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl) {
        int clv = atoi(cl + strlen("Content-Length:"));
        char *hdr_end = strstr(buf, "\r\n\r\n");
        int body_present = hdr_end ? (int)strlen(hdr_end+4) : 0;
        int to_read = clv - body_present;
        while (to_read > 0) {
            r = recv(client, buf+total, bufsz-total-1, 0);
            if (r <= 0) break;
            total += r;
            to_read -= r;
        }
        buf[total]=0;
    }
    return total;
}

/* Utility to parse query param value (e.g., /view?id=1001) - returns malloc'd string */
static char *get_query_value(const char *reqline, const char *key) {
    const char *q = strchr(reqline, '?');
    if (!q) return NULL;
    q++;
    char *pair = strdup(q);
    char *amp = strchr(pair, ' ');
    if (amp) *amp=0;
    char *val = form_value(pair, key);
    free(pair);
    return val;
}

/* Main request handler forward */
static void handle_client(int client);

/* Start listening on port from env or default 8080 */
int main(int argc, char **argv) {
    const char *portenv = getenv("PORT");
    int port = portenv ? atoi(portenv) : 8080;
    int server_fd, client_fd;
    struct sockaddr_in servaddr, cliaddr;
    socklen_t cli_len = sizeof(cliaddr);

    /* Ensure reports dir exists */
    mkdir("reports", 0755);

    /* Setup socket */
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(1);
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(1);
    }

    printf("Student system web server listening on port %d\n", port);
    fflush(stdout);

    /* Load data once at startup so functions see it (your functions may call load/save internally too) */
    load_data();

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr*)&cliaddr, &cli_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        handle_client(client_fd);
        /* handle_client closes client_fd */
    }

    close(server_fd);
    return 0;
}

/* Main request handler implementation */
static void handle_client(int client) {
    char req[REQBUF];
    int r = read_request(client, req, sizeof(req));
    if (r <= 0) { close(client); return; }

    /* parse request line */
    char method[8], path[1024], protocol[32];
    sscanf(req, "%7s %1023s %31s", method, path, protocol);

    if (strcmp(method, "GET") == 0) {
        if (strncmp(path, "/reports/", 9) == 0) {
            char *fname = path + 9;
            while (*fname == '/') fname++;
            serve_report_file(client, fname);
            close(client);
            return;
        } else if (strcmp(path, "/") == 0) {
            load_data();
            /* run demo mode to show sample output: call compiled binary if available */
            char demo_out[4096] = "(demo not available)";
            if (access("./student_system", X_OK) == 0) {
                FILE *p = popen("./student_system --demo 2>&1", "r");
                if (p) {
                    size_t got = fread(demo_out,1,sizeof(demo_out)-1,p);
                    demo_out[got]=0;
                    pclose(p);
                }
            }
            /* list reports */
            char *reports[256]; int rc=0;
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
            char *page = build_index_html(demo_out, reports, rc);
            for (int i=0;i<rc;i++) free(reports[i]);
            if (!page) send_response(client, "500 Internal Server Error", "text/plain", "Server error");
            else {
                send_response(client, "200 OK", "text/html; charset=utf-8", page);
                free(page);
            }
            close(client);
            return;
        } else if (strncmp(path, "/list", 5) == 0) {
            load_data();
            char out[8192];
            if (access("./student_system", X_OK) == 0) {
                FILE *p = popen("./student_system --list 2>&1", "r");
                if (p) {
                    size_t got = fread(out,1,sizeof(out)-1,p);
                    out[got]=0;
                    pclose(p);
                } else strcpy(out, "Error running list");
            } else strcpy(out, "Executable not found");
            send_response(client, "200 OK", "text/html; charset=utf-8", out);
            close(client);
            return;
        } else if (strncmp(path, "/view", 5) == 0) {
            char *id = get_query_value(path, "id");
            if (!id) { send_response(client, "400 Bad Request", "text/plain", "Missing id"); close(client); return; }
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "./student_system --view %s 2>&1", id);
            char out[8192];
            FILE *p = popen(cmd, "r");
            if (p) {
                size_t got = fread(out,1,sizeof(out)-1,p);
                out[got]=0;
                pclose(p);
            } else strcpy(out, "Error running view");
            send_response(client, "200 OK", "text/html; charset=utf-8", out);
            free(id);
            close(client);
            return;
        }
    } else if (strcmp(method, "POST") == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if (!body) { send_response(client, "400 Bad Request", "text/plain", "No body"); close(client); return; }
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
                send_response(client, "400 Bad Request", "text/plain", "Missing form fields");
                goto cleanup_add;
            }
            Student s;
            memset(&s, 0, sizeof(s));
            s.exists = 1;
            s.cgpa = 0.0; s.total_credits_completed = 0;
            strncpy(s.name, name, sizeof(s.name)-1);
            s.age = atoi(age);
            strncpy(s.dept, dept, sizeof(s.dept)-1);
            s.year = atoi(year);
            s.num_subjects = atoi(num_sub);
            if (s.num_subjects < 1 || s.num_subjects > MAX_SUBJECTS) s.num_subjects = MAX_SUBJECTS;
            char *subtok = strtok(subjects, ",");
            int si=0;
            while (subtok && si < s.num_subjects) {
                while (*subtok == ' ') subtok++;
                strncpy(s.subjects[si].name, subtok, sizeof(s.subjects[si].name)-1);
                s.subjects[si].classes_attended = 0;
                s.subjects[si].classes_held = 0;
                s.subjects[si].marks = 0;
                s.subjects[si].credits = 0;
                si++; subtok = strtok(NULL, ",");
            }
            strncpy(s.password, password, sizeof(s.password)-1);
            add_student_custom((void*)&s);
            send_response(client, "200 OK", "text/html; charset=utf-8", "Student added. <p><a href='/'>Back</a></p>");

        cleanup_add:
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
            char *id = form_value(body, "id");
            char *marks = form_value(body, "marks");
            if (!id || !marks) { send_response(client, "400 Bad Request", "text/plain", "Missing id or marks"); if (id) free(id); if (marks) free(marks); close(client); return; }
            int sid = atoi(id);
            int idx = find_index_by_id(sid);
            if (idx == -1) { send_response(client, "404 Not Found", "text/plain", "Student not found"); free(id); free(marks); close(client); return; }
            char tmpfn[PATH_MAX];
            snprintf(tmpfn, sizeof(tmpfn), "/tmp/marks_%d_%ld.txt", getpid(), time(NULL));
            FILE *tf = fopen(tmpfn, "w");
            if (!tf) { send_response(client, "500 Internal", "text/plain", "Cannot create temp file"); free(id); free(marks); close(client); return; }
            fprintf(tf, "%d\n", sid);
            char *line = strtok(marks, "\n");
            while (line) {
                char *clean = line;
                while (*clean && (*clean==' ' || *clean=='\r' || *clean=='\n' || *clean=='\t')) clean++;
                if (*clean) fprintf(tf, "%s\n", clean);
                line = strtok(NULL, "\n");
            }
            fclose(tf);
            char cmd[PATH_MAX + 64];
            snprintf(cmd, sizeof(cmd), "./student_system --enter-marks-file %s 2>&1", tmpfn);
            FILE *p = popen(cmd, "r");
            char out[8192]; out[0]=0;
            if (p) {
                size_t got = fread(out,1,sizeof(out)-1,p);
                out[got]=0;
                pclose(p);
            } else strcpy(out, "Error running marks update");
            unlink(tmpfn);
            send_response(client, "200 OK", "text/html; charset=utf-8", out);
            free(id); free(marks); close(client); return;
        }

        if (strncmp(path, "/generate", 9) == 0) {
            char *id = form_value(body, "id");
            char *college = form_value(body, "college");
            char *semester = form_value(body, "semester");
            char *exam = form_value(body, "exam");
            if (!id) { send_response(client, "400 Bad Request", "text/plain", "Missing id"); if (college) free(college); if (semester) free(semester); if (exam) free(exam); close(client); return; }
            int sid = atoi(id);
            int idx = find_index_by_id(sid);
            if (idx == -1) { send_response(client, "404 Not Found", "text/plain", "Student not found"); free(id); if (college) free(college); if (semester) free(semester); if (exam) free(exam); close(client); return; }
            char cbuf[256] = "Your College", sbuf[64] = "Semester -", eb[64] = "Exam -";
            if (college) { strncpy(cbuf, college, sizeof(cbuf)-1); free(college); }
            if (semester) { strncpy(sbuf, semester, sizeof(sbuf)-1); free(semester); }
            if (exam) { strncpy(eb, exam, sizeof(eb)-1); free(exam); }
            free(id);
            generate_html_report(idx, cbuf, sbuf, eb);
            char resp[256];
            snprintf(resp, sizeof(resp), "Report generated at reports/%d_result.html", sid);
            send_response(client, "200 OK", "text/html; charset=utf-8", resp);
            close(client); return;
        }

        send_response(client, "404 Not Found", "text/plain", "Not found");
        close(client);
        return;
    }

    send_response(client, "405 Method Not Allowed", "text/plain", "Method not allowed");
    close(client);
}
