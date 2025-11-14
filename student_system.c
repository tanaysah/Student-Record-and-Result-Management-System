/*
  student_system.c
  Student Record & Result Management System
  - Single-file C program
  - Menu-driven console app
  - CSV persistence in data/
  - Student self-registration supported
  - Admin-managed marks/attendance
  - SGPA calculation per semester (credit-weighted)
  - CGPA calculation (credit-weighted)
  - Generates printable report card files (text)
  - Portable: works on Linux and Windows (MinGW)
  Author: Rewritten per user request
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdirp(p) _mkdir(p)
#else
#define mkdirp(p) mkdir((p), 0755)
#endif

/* ---------- Config & Limits ---------- */
#define DATA_DIR "data"
#define REPORTS_DIR "reports"
#define STUDENTS_FILE DATA_DIR"/students.csv"
#define SUBJECTS_FILE DATA_DIR"/subjects.csv"
#define MARKS_FILE DATA_DIR"/marks.csv"
#define ATT_FILE DATA_DIR"/attendance.csv"

#define MAX_STUDENTS 2048
#define MAX_SUBJECTS 512
#define MAX_MARKS (MAX_STUDENTS * 48)
#define MAX_ATTS  (MAX_STUDENTS * 48)

#define MAX_NAME 128
#define MAX_EMAIL 128
#define MAX_PHONE 32
#define MAX_TITLE 160
#define MAX_CODE 32

/* ---------- Types ---------- */
typedef struct {
    char sap[32];        /* SAP ID (we store as string) */
    char roll[32];
    char name[MAX_NAME];
    char email[MAX_EMAIL];
    char phone[MAX_PHONE];
    int year;            /* 1..4 */
    int current_sem;     /* 1..8 */
} Student;

typedef struct {
    char id[32];         /* subject unique id */
    char code[MAX_CODE];
    char title[MAX_TITLE];
    int credits;
    int semester;        /* 1..8 */
} SubjectRec;

typedef struct {
    char sap[32];
    char subid[32];
    double marks;        /* -1 means not graded yet */
} MarkRec;

typedef struct {
    char sap[32];
    char subid[32];
    int present;
    int total;
} AttRec;

/* ---------- In-memory storage ---------- */
static Student students[MAX_STUDENTS];
static int student_count = 0;

static SubjectRec subjects[MAX_SUBJECTS];
static int subject_count = 0;

static MarkRec marks[MAX_MARKS];
static int marks_count = 0;

static AttRec atts[MAX_ATTS];
static int atts_count = 0;

/* ---------- Utility helpers ---------- */
void ensure_dirs(void) {
    struct stat st;
    if (stat(DATA_DIR, &st) == -1) mkdirp(DATA_DIR);
    if (stat(REPORTS_DIR, &st) == -1) mkdirp(REPORTS_DIR);
}

/* trim in-place */
void trim(char *s) {
    if (!s) return;
    /* left */
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    /* right */
    int i = (int)strlen(s) - 1;
    while (i >= 0 && isspace((unsigned char)s[i])) s[i--] = '\0';
}

/* safe getline */
void safe_getline(char *buf, size_t n) {
    if (!fgets(buf, (int)n, stdin)) { buf[0] = '\0'; return; }
    trim(buf);
}

/* case-insensitive substring (portable) */
char *strcasestr_compat(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (*needle == '\0') return (char *)haystack;
    size_t nl = strlen(needle);
    for (; *haystack; ++haystack) {
        size_t i;
        for (i = 0; i < nl; ++i) {
            char a = haystack[i];
            char b = needle[i];
            if (!a || !b) break;
            if (tolower((unsigned char)a) != tolower((unsigned char)b)) break;
        }
        if (i == nl) return (char *)haystack;
    }
    return NULL;
}

/* generate simple unique id */
void gen_id(char *out, size_t n, const char *pref) {
    static unsigned long ctr = 0;
    ctr++;
    unsigned long t = (unsigned long)time(NULL) ^ ctr;
    snprintf(out, n, "%s%08lx", pref ? pref : "id", (unsigned long)(t & 0xffffffff));
}

/* ---------- CSV load/save ---------- */
void save_students_csv(void) {
    FILE *f = fopen(STUDENTS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < student_count; ++i) {
        fprintf(f, "%s,%s,%s,%s,%s,%d,%d\n",
                students[i].sap, students[i].roll, students[i].name,
                students[i].email, students[i].phone, students[i].year, students[i].current_sem);
    }
    fclose(f);
}

void load_students_csv(void) {
    student_count = 0;
    FILE *f = fopen(STUDENTS_FILE, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        trim(line); if (line[0] == '\0') continue;
        char *p = line;
        char *tok;
        Student s; memset(&s, 0, sizeof(s));
        tok = strtok(p, ","); if (!tok) continue; strncpy(s.sap, tok, sizeof(s.sap)-1);
        tok = strtok(NULL, ","); if (!tok) continue; strncpy(s.roll, tok, sizeof(s.roll)-1);
        tok = strtok(NULL, ","); if (!tok) continue; strncpy(s.name, tok, sizeof(s.name)-1);
        tok = strtok(NULL, ","); if (!tok) continue; strncpy(s.email, tok, sizeof(s.email)-1);
        tok = strtok(NULL, ","); if (!tok) continue; strncpy(s.phone, tok, sizeof(s.phone)-1);
        tok = strtok(NULL, ","); if (!tok) continue; s.year = atoi(tok);
        tok = strtok(NULL, ","); if (!tok) continue; s.current_sem = atoi(tok);
        students[student_count++] = s;
        if (student_count >= MAX_STUDENTS) break;
    }
    fclose(f);
}

void save_subjects_csv(void) {
    FILE *f = fopen(SUBJECTS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < subject_count; ++i) {
        fprintf(f, "%s,%s,%s,%d,%d\n",
                subjects[i].id, subjects[i].code, subjects[i].title,
                subjects[i].credits, subjects[i].semester);
    }
    fclose(f);
}

void load_subjects_csv(void) {
    subject_count = 0;
    FILE *f = fopen(SUBJECTS_FILE, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        trim(line); if (line[0] == '\0') continue;
        char *p = line;
        char *tok;
        SubjectRec s; memset(&s,0,sizeof(s));
        tok = strtok(p, ","); if (!tok) continue; strncpy(s.id, tok, sizeof(s.id)-1);
        tok = strtok(NULL, ","); if (!tok) continue; strncpy(s.code, tok, sizeof(s.code)-1);
        tok = strtok(NULL, ","); if (!tok) continue; strncpy(s.title, tok, sizeof(s.title)-1);
        tok = strtok(NULL, ","); if (!tok) continue; s.credits = atoi(tok);
        tok = strtok(NULL, ","); if (!tok) continue; s.semester = atoi(tok);
        subjects[subject_count++] = s;
        if (subject_count >= MAX_SUBJECTS) break;
    }
    fclose(f);
}

void save_marks_csv(void) {
    FILE *f = fopen(MARKS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < marks_count; ++i) {
        fprintf(f, "%s,%s,%.2f\n", marks[i].sap, marks[i].subid, marks[i].marks);
    }
    fclose(f);
}

void load_marks_csv(void) {
    marks_count = 0;
    FILE *f = fopen(MARKS_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim(line); if (line[0] == '\0') continue;
        char *p = line; char *tok;
        MarkRec m; memset(&m,0,sizeof(m));
        tok = strtok(p, ","); if (!tok) continue; strncpy(m.sap, tok, sizeof(m.sap)-1);
        tok = strtok(NULL, ","); if (!tok) continue; strncpy(m.subid, tok, sizeof(m.subid)-1);
        tok = strtok(NULL, ","); if (!tok) continue; m.marks = atof(tok);
        marks[marks_count++] = m;
        if (marks_count >= MAX_MARKS) break;
    }
    fclose(f);
}

void save_atts_csv(void) {
    FILE *f = fopen(ATT_FILE, "w");
    if (!f) return;
    for (int i = 0; i < atts_count; ++i) {
        fprintf(f, "%s,%s,%d,%d\n", atts[i].sap, atts[i].subid, atts[i].present, atts[i].total);
    }
    fclose(f);
}

void load_atts_csv(void) {
    atts_count = 0;
    FILE *f = fopen(ATT_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim(line); if (line[0] == '\0') continue;
        char *p = line; char *tok;
        AttRec a; memset(&a,0,sizeof(a));
        tok = strtok(p, ","); if (!tok) continue; strncpy(a.sap, tok, sizeof(a.sap)-1);
        tok = strtok(NULL, ","); if (!tok) continue; strncpy(a.subid, tok, sizeof(a.subid)-1);
        tok = strtok(NULL, ","); if (!tok) continue; a.present = atoi(tok);
        tok = strtok(NULL, ","); if (!tok) continue; a.total = atoi(tok);
        atts[atts_count++] = a;
        if (atts_count >= MAX_ATTS) break;
    }
    fclose(f);
}

/* ---------- Default syllabus (per-semester subject lists & credits) ---------- */
typedef struct { const char *title; int credits; } SubDef;

static const SubDef SEM1[] = {
    {"Programming in C",5}, {"Linux Lab",2}, {"Problem Solving",2},
    {"Advanced Engineering Mathematics - I",4}, {"Physics for Computer Engineers",5},
    {"Managing Self",2}, {"Environmental Sustainability and Climate Change",2}, {NULL,0}
};

static const SubDef SEM2[] = {
    {"Data Structures and Algorithms",5}, {"Digital Electronics",3}, {"Python Programming",5},
    {"Advanced Engineering Mathematics - II",4}, {"Environmental Sustainability and Climate Change",2},
    {"Time and Priority Management",2}, {"Elements of AI/ML",3}, {NULL,0}
};

static const SubDef SEM3[] = {
    {"Leading Conversations",2}, {"Discrete Mathematical Structures",3}, {"Operating Systems",3},
    {"Elements of AI/ML",3}, {"Database Management Systems",5}, {"Design and Analysis of Algorithms",4}, {NULL,0}
};

static const SubDef SEM4[] = {
    {"Software Engineering",3}, {"EDGE - Soft Skills",0}, {"Linear Algebra",3}, {"Indian Constitution",0},
    {"Writing with Impact",2}, {"Object Oriented Programming",4}, {"Data Communication and Networks",4},
    {"Applied Machine Learning",5}, {NULL,0}
};

static const SubDef SEM5[] = {
    {"Cryptography and Network Security",3}, {"Formal Languages and Automata Theory",3},
    {"Object Oriented Analysis and Design",3}, {"Exploratory-3",3}, {"Start your Startup",2},
    {"Research Methodology in CS",3}, {"Probability, Entropy, and MC Simulation",3},
    {"PE-2",4}, {"PE-2 Lab",1}, {NULL,0}
};

static const SubDef SEM6[] = {
    {"Exploratory-4",3}, {"Leadership and Teamwork",2}, {"Compiler Design",3},
    {"Statistics and Data Analysis",3}, {"PE-3",4}, {"PE-3 Lab",1}, {"Minor Project",5}, {NULL,0}
};

static const SubDef SEM7[] = {
    {"Exploratory-5",3}, {"PE-4",4}, {"PE-4 Lab",1}, {"PE-5",3}, {"PE-5 Lab",1},
    {"Capstone Project - Phase-1",5}, {"Summer Internship",1}, {NULL,0}
};

static const SubDef SEM8[] = {
    {"IT Ethical Practices",3}, {"Capstone Project - Phase-2",5}, {NULL,0}
};

void populate_default_subjects_if_empty(void) {
    if (subject_count > 0) return;
    const SubDef *sets[9] = { NULL, SEM1, SEM2, SEM3, SEM4, SEM5, SEM6, SEM7, SEM8 };
    for (int sem = 1; sem <= 8; ++sem) {
        const SubDef *arr = sets[sem];
        for (int i = 0; arr[i].title != NULL; ++i) {
            SubjectRec s; memset(&s,0,sizeof(s));
            gen_id(s.id, sizeof(s.id), "sub");
            snprintf(s.code, sizeof(s.code), "S%02d%02d", sem, i+1);
            strncpy(s.title, arr[i].title, sizeof(s.title)-1);
            s.credits = arr[i].credits;
            s.semester = sem;
            if (subject_count < MAX_SUBJECTS) subjects[subject_count++] = s;
        }
    }
    save_subjects_csv();
}

/* ---------- Index and search helpers ---------- */
int student_index_by_sap(const char *sap) {
    for (int i=0;i<student_count;i++) if (strcmp(students[i].sap, sap) == 0) return i;
    return -1;
}

int subject_index_by_id(const char *id) {
    for (int i=0;i<subject_count;i++) if (strcmp(subjects[i].id, id) == 0) return i;
    return -1;
}

int mark_index(const char *sap, const char *subid) {
    for (int i=0;i<marks_count;i++) if (strcmp(marks[i].sap, sap)==0 && strcmp(marks[i].subid, subid)==0) return i;
    return -1;
}

int att_index(const char *sap, const char *subid) {
    for (int i=0;i<atts_count;i++) if (strcmp(atts[i].sap, sap)==0 && strcmp(atts[i].subid, subid)==0) return i;
    return -1;
}

/* ---------- SGPA/CGPA ---------- */
/* grade point formula: linear conversion mark/100 * 10 */
double mark_to_gp(double mark) {
    if (mark < 0.0) return 0.0;
    return (mark / 100.0) * 10.0;
}

double compute_sgpa_for_sem(const char *sap, int sem) {
    double weighted = 0.0;
    int credits = 0;
    for (int i=0;i<subject_count;i++) {
        if (subjects[i].semester != sem) continue;
        int mi = mark_index(sap, subjects[i].id);
        if (mi < 0) continue;
        if (marks[mi].marks < 0.0) continue;
        double gp = mark_to_gp(marks[mi].marks);
        weighted += gp * subjects[i].credits;
        credits += subjects[i].credits;
    }
    if (credits == 0) return -1.0;
    return weighted / credits;
}

double compute_cgpa_credit_weighted(const char *sap) {
    double weighted = 0.0;
    int total_credits = 0;
    for (int i=0;i<subject_count;i++) {
        int mi = mark_index(sap, subjects[i].id);
        if (mi < 0) continue;
        if (marks[mi].marks < 0.0) continue;
        double gp = mark_to_gp(marks[mi].marks);
        weighted += gp * subjects[i].credits;
        total_credits += subjects[i].credits;
    }
    if (total_credits == 0) return -1.0;
    return weighted / total_credits;
}

/* ---------- Student registration & subject assignment ---------- */
void add_marks_placeholder_for_student(const char *sap, int sem_limit) {
    /* ensure every subject in semester 1..sem_limit has a mark record (-1) and att record (0/0) */
    for (int i=0;i<subject_count;i++) {
        if (subjects[i].semester > sem_limit) continue;
        if (mark_index(sap, subjects[i].id) < 0) {
            if (marks_count < MAX_MARKS) {
                MarkRec m; memset(&m,0,sizeof(m));
                strncpy(m.sap, sap, sizeof(m.sap)-1);
                strncpy(m.subid, subjects[i].id, sizeof(m.subid)-1);
                m.marks = -1.0;
                marks[marks_count++] = m;
            }
        }
        if (att_index(sap, subjects[i].id) < 0) {
            if (atts_count < MAX_ATTS) {
                AttRec a; memset(&a,0,sizeof(a));
                strncpy(a.sap, sap, sizeof(a.sap)-1);
                strncpy(a.subid, subjects[i].id, sizeof(a.subid)-1);
                a.present = 0; a.total = 0;
                atts[atts_count++] = a;
            }
        }
    }
}

void register_student_self(void) {
    if (student_count >= MAX_STUDENTS) { printf("Student capacity reached.\n"); return; }
    char buf[256];
    Student s; memset(&s,0,sizeof(s));
    printf("Enter SAP ID (numeric): ");
    safe_getline(buf, sizeof(buf)); if (strlen(buf)==0) { printf("Cancelled.\n"); return; }
    if (student_index_by_sap(buf) >= 0) { printf("SAP ID already registered.\n"); return; }
    strncpy(s.sap, buf, sizeof(s.sap)-1);
    printf("Enter Roll: "); safe_getline(s.roll, sizeof(s.roll));
    printf("Full name: "); safe_getline(s.name, sizeof(s.name));
    printf("Email: "); safe_getline(s.email, sizeof(s.email));
    printf("Phone: "); safe_getline(s.phone, sizeof(s.phone));
    printf("Year (1-4): "); safe_getline(buf, sizeof(buf)); s.year = atoi(buf); if (s.year<1||s.year>4) s.year=1;
    printf("Current Semester (1-8): "); safe_getline(buf, sizeof(buf)); s.current_sem = atoi(buf); if (s.current_sem <1||s.current_sem>8) s.current_sem=1;
    students[student_count++] = s;
    add_marks_placeholder_for_student(s.sap, s.current_sem);
    save_students_csv(); save_marks_csv(); save_atts_csv();
    printf("Registration complete. SAP: %s\n", s.sap);
}

/* ---------- Admin operations ---------- */
int admin_auth(void) {
    /* simple builtin admin user for single-file program */
    const char *U = "admin";
    const char *P = "admin123";
    char user[64], pass[64];
    printf("Admin username: "); safe_getline(user, sizeof(user));
    printf("Admin password: "); safe_getline(pass, sizeof(pass));
    if (strcmp(user, U)==0 && strcmp(pass, P)==0) return 1;
    printf("Invalid admin credentials.\n"); return 0;
}

/* add a new subject to global master */
void admin_add_subject(void) {
    if (subject_count >= MAX_SUBJECTS) { printf("Subject capacity reached.\n"); return; }
    SubjectRec s; memset(&s,0,sizeof(s));
    char buf[256];
    printf("Subject title: "); safe_getline(s.title, sizeof(s.title));
    printf("Credits (int): "); safe_getline(buf, sizeof(buf)); s.credits = atoi(buf);
    printf("Semester (1-8): "); safe_getline(buf, sizeof(buf)); s.semester = atoi(buf);
    gen_id(s.id, sizeof(s.id), "sub");
    snprintf(s.code, sizeof(s.code), "X%02d%02d", s.semester, subject_count+1);
    subjects[subject_count++] = s;
    save_subjects_csv();
    printf("Subject added.\n");
}

/* add subjects of semester for a student (admin option) */
void admin_add_subjects_for_student(void) {
    char buf[256];
    printf("Enter SAP ID: "); safe_getline(buf, sizeof(buf));
    int si = student_index_by_sap(buf);
    if (si < 0) { printf("Student not found.\n"); return; }
    printf("Enter semester to add: "); safe_getline(buf, sizeof(buf)); int sem = atoi(buf);
    if (sem < 1 || sem > 8) { printf("Invalid semester.\n"); return; }
    add_marks_placeholder_for_student(students[si].sap, sem);
    save_marks_csv(); save_atts_csv();
    printf("Subjects for semester %d added (placeholders).\n", sem);
}

/* Enter/update marks */
void admin_enter_update_marks(void) {
    char buf[256];
    printf("Enter SAP ID: "); safe_getline(buf, sizeof(buf));
    int si = student_index_by_sap(buf);
    if (si < 0) { printf("Student not found.\n"); return; }
    Student *st = &students[si];
    printf("Entering marks for %s (%s) current sem %d\n", st->name, st->sap, st->current_sem);
    /* list subjects up to student's semester */
    for (int i=0;i<subject_count;i++) {
        if (subjects[i].semester > st->current_sem) continue;
        int m = mark_index(st->sap, subjects[i].id);
        double cur = (m >= 0) ? marks[m].marks : -1.0;
        char curstr[32]; if (cur >= 0.0) snprintf(curstr, sizeof(curstr), "%.2f", cur); else strcpy(curstr, "N/A");
        printf("[%d] %s (Sem %d) Credits:%d | Marks: %s\n", i+1, subjects[i].title, subjects[i].semester, subjects[i].credits, curstr);
    }
    printf("Enter subject index (number) to update marks (0 to cancel): ");
    safe_getline(buf, sizeof(buf)); int idx = atoi(buf);
    if (idx <= 0 || idx > subject_count) { printf("Cancelled.\n"); return; }
    SubjectRec *sub = &subjects[idx-1];
    if (sub->semester > st->current_sem) { printf("Student not assigned this future semester subject.\n"); return; }
    printf("Enter marks (0-100): "); safe_getline(buf, sizeof(buf)); double mm = atof(buf);
    if (mm < 0) mm = 0; if (mm > 100) mm = 100;
    int mi = mark_index(st->sap, sub->id);
    if (mi >= 0) {
        marks[mi].marks = mm;
    } else {
        if (marks_count < MAX_MARKS) {
            MarkRec m; memset(&m,0,sizeof(m));
            strncpy(m.sap, st->sap, sizeof(m.sap)-1);
            strncpy(m.subid, sub->id, sizeof(m.subid)-1);
            m.marks = mm;
            marks[marks_count++] = m;
        } else { printf("Marks storage full.\n"); return; }
    }
    save_marks_csv();
    printf("Marks saved.\n");
}

/* mark attendance for a single student & subject */
void admin_mark_attendance_single(void) {
    char buf[256];
    printf("Enter SAP ID: "); safe_getline(buf, sizeof(buf));
    int si = student_index_by_sap(buf);
    if (si < 0) { printf("Student not found.\n"); return; }
    Student *st = &students[si];
    printf("Subjects assigned to this student:\n");
    int count = 0;
    for (int i=0;i<subject_count;i++) {
        int mi = mark_index(st->sap, subjects[i].id);
        if (mi >= 0) {
            printf("[%d] %s (Sem %d)\n", i+1, subjects[i].title, subjects[i].semester);
            ++count;
        }
    }
    if (count == 0) { printf("No subjects assigned.\n"); return; }
    printf("Enter subject index to mark attendance: "); safe_getline(buf, sizeof(buf)); int idx = atoi(buf);
    if (idx <= 0 || idx > subject_count) { printf("Cancelled.\n"); return; }
    SubjectRec *sub = &subjects[idx-1];
    int aidx = att_index(st->sap, sub->id);
    if (aidx < 0) {
        if (atts_count >= MAX_ATTS) { printf("Attendance storage full.\n"); return; }
        AttRec a; memset(&a,0,sizeof(a));
        strncpy(a.sap, st->sap, sizeof(a.sap)-1);
        strncpy(a.subid, sub->id, sizeof(a.subid)-1);
        a.present = 0; a.total = 0;
        atts[atts_count++] = a;
        aidx = atts_count - 1;
    }
    printf("Enter number of classes held to add (e.g., 1): "); safe_getline(buf, sizeof(buf)); int held = atoi(buf);
    if (held <= 0) { printf("Invalid.\n"); return; }
    printf("Was the student present? (y/n): "); safe_getline(buf, sizeof(buf));
    int present_flag = (buf[0]=='y' || buf[0]=='Y') ? 1 : 0;
    atts[aidx].total += held;
    if (present_flag) atts[aidx].present += held;
    save_atts_csv();
    printf("Attendance updated.\n");
}

/* bulk attendance for a subject: add `held` to total for all students assigned the subject;
   for SAP IDs listed as present, add `present_increment` (typically equals held) */
void admin_bulk_attendance_for_subject(void) {
    char buf[2048];
    printf("List of subjects:\n");
    for (int i=0;i<subject_count;i++) printf("[%d] %s (Sem %d)\n", i+1, subjects[i].title, subjects[i].semester);
    printf("Enter subject index: "); safe_getline(buf, sizeof(buf)); int idx = atoi(buf);
    if (idx <= 0 || idx > subject_count) { printf("Cancelled.\n"); return; }
    SubjectRec *sub = &subjects[idx-1];
    printf("Enter classes held to add (e.g., 1): "); safe_getline(buf, sizeof(buf)); int held = atoi(buf);
    if (held <= 0) { printf("Invalid held value.\n"); return; }
    printf("Enter SAP IDs of present students separated by space or comma, then Enter (or blank for none):\n");
    safe_getline(buf, sizeof(buf));
    /* parse present list */
    char present_list[256][32]; int pcount = 0;
    char tmp[2048]; strncpy(tmp, buf, sizeof(tmp)-1);
    char *tok = strtok(tmp, " ,\t");
    while (tok && pcount < 256) {
        trim(tok);
        if (strlen(tok) > 0) strncpy(present_list[pcount++], tok, 31);
        tok = strtok(NULL, " ,\t");
    }
    /* For every student who has mark entry for this subject, increment total by held, and present by held if in present_list */
    for (int i=0;i<student_count;i++) {
        int mi = mark_index(students[i].sap, sub->id);
        if (mi < 0) continue; /* student not assigned that subject */
        int aidx = att_index(students[i].sap, sub->id);
        if (aidx < 0) {
            if (atts_count >= MAX_ATTS) continue;
            AttRec a; memset(&a,0,sizeof(a));
            strncpy(a.sap, students[i].sap, sizeof(a.sap)-1);
            strncpy(a.subid, sub->id, sizeof(a.subid)-1);
            a.present = 0; a.total = 0;
            atts[atts_count++] = a;
            aidx = atts_count - 1;
        }
        atts[aidx].total += held;
        /* check presence */
        int found = 0;
        for (int k=0;k<pcount;k++) if (strcmp(students[i].sap, present_list[k]) == 0) { found = 1; break; }
        if (found) atts[aidx].present += held;
    }
    save_atts_csv();
    printf("Bulk attendance updated for subject %s.\n", sub->title);
}

/* ---------- Display, search, modify, delete ---------- */
void display_student_record(const Student *s) {
    printf("--------------------------------------------------\n");
    printf("SAP ID: %s\n", s->sap);
    printf("Roll: %s\n", s->roll);
    printf("Name: %s\n", s->name);
    printf("Email: %s\n", s->email);
    printf("Phone: %s\n", s->phone);
    printf("Year: %d\n", s->year);
    printf("Current Semester: %d\n", s->current_sem);
    printf("Subjects (up to current semester) and details:\n");
    for (int i=0;i<subject_count;i++) {
        if (subjects[i].semester > s->current_sem) continue;
        int mi = mark_index(s->sap, subjects[i].id);
        double mk = (mi >=0) ? marks[mi].marks : -1.0;
        int ai = att_index(s->sap, subjects[i].id);
        int pres = (ai>=0) ? atts[ai].present : 0;
        int tot = (ai>=0) ? atts[ai].total : 0;
        char mkstr[32];
        if (mk >= 0.0) snprintf(mkstr, sizeof(mkstr), "%.2f", mk);
        else strcpy(mkstr, "N/A");
        printf(" - %s (Sem %d, Cr:%d) Marks: %s | Attendance: %d/%d\n",
               subjects[i].title, subjects[i].semester, subjects[i].credits, mkstr, pres, tot);
    }
    double cg = compute_cgpa_credit_weighted(s->sap);
    if (cg < 0.0) printf("CGPA: N/A\n"); else printf("CGPA (credit-weighted): %.3f\n", cg);
    printf("--------------------------------------------------\n");
}

void search_and_display_student(void) {
    char buf[256];
    printf("Search by: [1] SAP ID  [2] Name substring\nChoice: "); safe_getline(buf, sizeof(buf));
    if (buf[0] == '1') {
        printf("Enter SAP ID: "); safe_getline(buf, sizeof(buf));
        int idx = student_index_by_sap(buf);
        if (idx < 0) { printf("Not found.\n"); return; }
        display_student_record(&students[idx]);
    } else {
        printf("Enter name substring: "); safe_getline(buf, sizeof(buf));
        int found = 0;
        for (int i=0;i<student_count;i++) {
            if (strcasestr_compat(students[i].name, buf)) {
                display_student_record(&students[i]);
                found++;
            }
        }
        if (!found) printf("No matches.\n");
    }
}

void display_all_students(void) {
    if (student_count == 0) { printf("No students.\n"); return; }
    for (int i=0;i<student_count;i++) {
        printf("[%d] %s | %s | Year %d | Sem %d\n", i+1, students[i].sap, students[i].name, students[i].year, students[i].current_sem);
    }
}

/* modify student */
void modify_student(void) {
    char buf[256];
    printf("Enter SAP ID to modify: "); safe_getline(buf, sizeof(buf));
    int si = student_index_by_sap(buf);
    if (si < 0) { printf("Student not found.\n"); return; }
    Student *s = &students[si];
    printf("Leave blank to keep current value.\n");
    printf("Name (%s): ", s->name); safe_getline(buf, sizeof(buf)); if (strlen(buf)) strncpy(s->name, buf, sizeof(s->name)-1);
    printf("Email (%s): ", s->email); safe_getline(buf, sizeof(buf)); if (strlen(buf)) strncpy(s->email, buf, sizeof(s->email)-1);
    printf("Phone (%s): ", s->phone); safe_getline(buf, sizeof(buf)); if (strlen(buf)) strncpy(s->phone, buf, sizeof(s->phone)-1);
    printf("Roll (%s): ", s->roll); safe_getline(buf, sizeof(buf)); if (strlen(buf)) strncpy(s->roll, buf, sizeof(s->roll)-1);
    printf("Year (%d): ", s->year); safe_getline(buf, sizeof(buf)); if (strlen(buf)) s->year = atoi(buf);
    printf("Current Semester (%d): ", s->current_sem); safe_getline(buf, sizeof(buf));
    if (strlen(buf)) {
        int oldsem = s->current_sem;
        s->current_sem = atoi(buf);
        if (s->current_sem > oldsem) add_marks_placeholder_for_student(s->sap, s->current_sem);
    }
    save_students_csv(); save_marks_csv(); save_atts_csv();
    printf("Student modified.\n");
}

/* delete student */
void delete_student(void) {
    char buf[256];
    printf("Enter SAP ID to delete: "); safe_getline(buf, sizeof(buf));
    int si = student_index_by_sap(buf);
    if (si < 0) { printf("Student not found.\n"); return; }
    /* remove marks */
    for (int i = 0; i < marks_count; ) {
        if (strcmp(marks[i].sap, students[si].sap) == 0) {
            /* shift left */
            for (int j=i;j<marks_count-1;j++) marks[j]=marks[j+1];
            marks_count--;
        } else ++i;
    }
    /* remove atts */
    for (int i = 0; i < atts_count; ) {
        if (strcmp(atts[i].sap, students[si].sap) == 0) {
            for (int j=i;j<atts_count-1;j++) atts[j]=atts[j+1];
            atts_count--;
        } else ++i;
    }
    /* remove student */
    for (int i = si; i < student_count-1; ++i) students[i] = students[i+1];
    student_count--;
    save_students_csv(); save_marks_csv(); save_atts_csv();
    printf("Student deleted.\n");
}

/* sorts and displays */
int cmp_sap(const void *a, const void *b) {
    const Student *sa = a; const Student *sb = b;
    return strcmp(sa->sap, sb->sap);
}
int cmp_name(const void *a, const void *b) {
    const Student *sa = a; const Student *sb = b;
    return strcasecmp(sa->name, sb->name);
}

void display_sorted_by_sapid(void) {
    if (student_count == 0) { printf("No students.\n"); return; }
    Student *tmp = malloc(sizeof(Student) * student_count);
    if (!tmp) return;
    memcpy(tmp, students, sizeof(Student) * student_count);
    qsort(tmp, student_count, sizeof(Student), cmp_sap);
    for (int i=0;i<student_count;i++) printf("%s | %s | Year %d | Sem %d\n", tmp[i].sap, tmp[i].name, tmp[i].year, tmp[i].current_sem);
    free(tmp);
}

void display_sorted_by_name(void) {
    if (student_count == 0) { printf("No students.\n"); return; }
    Student *tmp = malloc(sizeof(Student) * student_count);
    if (!tmp) return;
    memcpy(tmp, students, sizeof(Student) * student_count);
    qsort(tmp, student_count, sizeof(Student), cmp_name);
    for (int i=0;i<student_count;i++) printf("%s | %s | Year %d | Sem %d\n", tmp[i].sap, tmp[i].name, tmp[i].year, tmp[i].current_sem);
    free(tmp);
}

/* compute & display CGPA for student */
void calculate_display_cgpa(void) {
    char buf[256];
    printf("Enter SAP ID: "); safe_getline(buf, sizeof(buf));
    int si = student_index_by_sap(buf);
    if (si < 0) { printf("Student not found.\n"); return; }
    double cg = compute_cgpa_credit_weighted(students[si].sap);
    if (cg < 0.0) printf("CGPA: N/A (no graded credits)\n");
    else printf("CGPA (credit-weighted): %.3f\n", cg);
}

/* average CGPA of a year */
void average_cgpa_of_year(void) {
    char buf[64];
    printf("Enter Year (1-4): "); safe_getline(buf, sizeof(buf)); int yr = atoi(buf);
    if (yr < 1 || yr > 4) { printf("Invalid year.\n"); return; }
    double sum = 0.0; int count = 0;
    for (int i=0;i<student_count;i++) {
        if (students[i].year != yr) continue;
        double cg = compute_cgpa_credit_weighted(students[i].sap);
        if (cg < 0.0) continue;
        sum += cg; ++count;
    }
    if (count == 0) printf("No CGPA data for Year %d.\n", yr);
    else printf("Average CGPA for Year %d: %.3f (n=%d)\n", yr, sum / count, count);
}

/* export all students CSV (timestamped) */
void export_all_students_to_csv(void) {
    time_t t = time(NULL);
    char fname[256];
    snprintf(fname, sizeof(fname), "export_students_%ld.csv", (long)t);
    FILE *f = fopen(fname, "w");
    if (!f) { printf("Failed to create export file.\n"); return; }
    fprintf(f, "sap,roll,name,email,phone,year,current_sem,cgpa\n");
    for (int i=0;i<student_count;i++) {
        double cg = compute_cgpa_credit_weighted(students[i].sap);
        if (cg < 0.0) cg = 0.0;
        fprintf(f, "%s,%s,%s,%s,%s,%d,%d,%.3f\n",
                students[i].sap, students[i].roll, students[i].name, students[i].email, students[i].phone, students[i].year, students[i].current_sem, cg);
    }
    fclose(f); printf("Exported to %s\n", fname);
}

/* attendance report: list students below threshold for given semester & subject (or all subjects) */
void attendance_report_below_threshold(void) {
    char buf[256];
    printf("Enter semester number (1-8): "); safe_getline(buf, sizeof(buf)); int sem = atoi(buf);
    if (sem < 1 || sem > 8) { printf("Invalid semester.\n"); return; }
    printf("Subjects in semester %d:\n", sem);
    int listed = 0;
    for (int i=0;i<subject_count;i++) if (subjects[i].semester == sem) printf("[%d] %s\n", i+1, subjects[i].title), listed++;
    if (listed == 0) { printf("No subjects in this semester.\n"); return; }
    printf("Enter subject index (0 for all subjects in semester): "); safe_getline(buf, sizeof(buf)); int sel = atoi(buf);
    printf("Enter threshold percent (e.g., 75): "); safe_getline(buf, sizeof(buf)); double thr = atof(buf);
    if (thr < 0.0 || thr > 100.0) thr = 75.0;
    int found = 0;
    for (int i=0;i<student_count;i++) {
        for (int j=0;j<subject_count;j++) {
            if (subjects[j].semester != sem) continue;
            if (sel != 0 && sel != (j+1)) continue;
            int aidx = att_index(students[i].sap, subjects[j].id);
            if (aidx < 0) continue;
            int pres = atts[aidx].present; int tot = atts[aidx].total;
            double pct = (tot == 0) ? 0.0 : ((double)pres * 100.0 / tot);
            if (pct < thr) {
                printf("%s | %s | Subject: %s | Attendance: %.1f%% (%d/%d)\n",
                       students[i].sap, students[i].name, subjects[j].title, pct, pres, tot);
                found++;
            }
        }
    }
    if (!found) printf("No students below threshold.\n");
}

/* ---------- Report card generation ---------- */
void generate_report_card(void) {
    char buf[256];
    printf("Enter SAP ID: "); safe_getline(buf, sizeof(buf));
    int si = student_index_by_sap(buf);
    if (si < 0) { printf("Student not found.\n"); return; }
    Student *s = &students[si];
    printf("Enter Exam name (e.g., Midterm, End-Sem): "); safe_getline(buf, sizeof(buf));
    char exam[128]; strncpy(exam, buf, sizeof(exam)-1);
    time_t t = time(NULL);
    char fname[512];
    snprintf(fname, sizeof(fname), REPORTS_DIR"/report_%s_sem%d_%ld.txt", s->sap, s->current_sem, (long)t);
    FILE *f = fopen(fname, "w");
    if (!f) { printf("Failed to create report file.\n"); return; }
    fprintf(f, "------------------------------------------------------------\n");
    fprintf(f, "           XYZ INSTITUTE OF TECHNOLOGY (Demo College)\n");
    fprintf(f, "           Student Report Card\n");
    fprintf(f, "------------------------------------------------------------\n\n");
    fprintf(f, "Name: %s\nSAP ID: %s\nRoll: %s\nEmail: %s\nPhone: %s\nYear: %d\nSemester: %d\nExam: %s\nGenerated: %s\n",
            s->name, s->sap, s->roll, s->email, s->phone, s->year, s->current_sem, exam, ctime(&t));
    fprintf(f, "------------------------------------------------------------\n");
    fprintf(f, "| %-3s | %-40s | %6s | %6s |\n", "No", "Subject", "Credits", "Marks");
    fprintf(f, "------------------------------------------------------------\n");
    int sno = 1;
    for (int i=0;i<subject_count;i++) {
        if (subjects[i].semester > s->current_sem) continue;
        int mi = mark_index(s->sap, subjects[i].id);
        double mk = (mi >= 0) ? marks[mi].marks : -1.0;
        char mkstr[32]; if (mk >= 0.0) snprintf(mkstr, sizeof(mkstr), "%.2f", mk); else strcpy(mkstr, "N/A");
        fprintf(f, "| %3d | %-40s | %6d | %6s |\n", sno++, subjects[i].title, subjects[i].credits, mkstr);
    }
    fprintf(f, "------------------------------------------------------------\n\n");
    fprintf(f, "Semester-wise SGPA:\n");
    for (int sem = 1; sem <= s->current_sem; ++sem) {
        double sg = compute_sgpa_for_sem(s->sap, sem);
        if (sg < 0.0) fprintf(f, "  Sem %d: N/A\n", sem);
        else fprintf(f, "  Sem %d: %.3f\n", sem, sg);
    }
    double cg = compute_cgpa_credit_weighted(s->sap);
    if (cg < 0.0) fprintf(f, "\nCGPA: N/A\n");
    else fprintf(f, "\nCGPA (credit-weighted): %.3f\n", cg);
    fprintf(f, "\nRemarks: ___________________________\n\n");
    fprintf(f, "------------------------------------------------------------\n");
    fprintf(f, "Principal / Controller of Examinations\n");
    fclose(f);
    printf("Report card generated: %s\n", fname);
}

/* ---------- Sample students if <5 ---------- */
void create_sample_students_if_needed(void) {
    if (student_count >= 5) return;
    for (int i = 1; i <= 5 && student_count < MAX_STUDENTS; ++i) {
        Student s; memset(&s,0,sizeof(s));
        snprintf(s.sap, sizeof(s.sap), "10000%d", i);
        snprintf(s.roll, sizeof(s.roll), "R2025%03d", i);
        snprintf(s.name, sizeof(s.name), "Sample Student %d", i);
        snprintf(s.email, sizeof(s.email), "student%d@example.com", i);
        snprintf(s.phone, sizeof(s.phone), "70000000%02d", i);
        s.year = (i % 4) + 1;
        s.current_sem = ((s.year - 1) * 2) + 1;
        students[student_count++] = s;
        add_marks_placeholder_for_student(s.sap, s.current_sem);
    }
    save_students_csv(); save_marks_csv(); save_atts_csv();
}

/* ---------- Main menu ---------- */
void print_menu(void) {
    printf("\n===== Student Record & Result Management =====\n");
    printf("1. Add new student (self-register)\n");
    printf("2. Modify student\n");
    printf("3. Add subject(s) for a student (per semester)\n");
    printf("4. Enter/Update marks for subject (admin)\n");
    printf("5. Mark attendance for a student (single subject) (admin)\n");
    printf("6. Bulk mark attendance for whole class (subject) (admin)\n");
    printf("7. Display student's subject list & attendance\n");
    printf("8. Search & display student (by SAP ID or Name)\n");
    printf("9. Calculate & display CGPA of student\n");
    printf("10. Average CGPA of a Year\n");
    printf("11. Display all students\n");
    printf("12. Delete student (admin)\n");
    printf("13. Display all records sorted by SAP ID\n");
    printf("14. Display all records sorted by Name\n");
    printf("15. Generate report card (student)\n");
    printf("16. Export all students to CSV\n");
    printf("17. Attendance report: list students below threshold (enter sem & subject)\n");
    printf("0. Exit\n");
    printf("Enter choice: ");
}

int main(int argc, char **argv) {
    ensure_dirs();
    load_subjects_csv();
    populate_default_subjects_if_empty();
    load_students_csv();
    load_marks_csv();
    load_atts_csv();
    create_sample_students_if_needed();

    while (1) {
        print_menu();
        char choice[64]; safe_getline(choice, sizeof(choice));
        int ch = atoi(choice);
        switch (ch) {
            case 1: register_student_self(); break;
            case 2: modify_student(); break;
            case 3:
                if (!admin_auth()) break;
                admin_add_subjects_for_student();
                break;
            case 4:
                if (!admin_auth()) break;
                admin_enter_update_marks();
                break;
            case 5:
                if (!admin_auth()) break;
                admin_mark_attendance_single();
                
                break;
            case 6:
                if (!admin_auth()) break;
                admin_bulk_attendance_for_subject();
                break;
            case 7: {
                char buf[128];
                printf("Enter SAP ID: "); safe_getline(buf, sizeof(buf));
                int si = student_index_by_sap(buf);
                if (si < 0) printf("Student not found.\n"); else display_student_record(&students[si]);
                break;
            }
            case 8: search_and_display_student(); break;
            case 9: calculate_display_cgpa(); break;
            case 10: average_cgpa_of_year(); break;
            case 11: display_all_students(); break;
            case 12:
                if (!admin_auth()) break;
                delete_student();
                break;
            case 13: display_sorted_by_sapid(); break;
            case 14: display_sorted_by_name(); break;
            case 15: generate_report_card(); break;
            case 16: export_all_students_to_csv(); break;
            case 17: attendance_report_below_threshold(); break;
            case 0: printf("Goodbye.\n"); return 0;
            default: printf("Invalid choice.\n"); break;
        }
    }
    return 0;
}
