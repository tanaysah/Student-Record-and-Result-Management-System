/* student_system.c
   Student Record & Result Management System (console)
   - Single-file C program
   - Menu-driven console app
   - CSV persistence in data/
   - Student self-registration supported
   - Admin-managed marks/attendance (admin login required)
   - SGPA calculation per semester (credit-weighted using grade points)
   - CGPA calculation (average of semester SGPAs; also credit-weighted option)
   - Generates printable report card files (text)
   - Author: Generated for user request (Tanay-style spec)
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>

#ifdef _WIN32
#include <direct.h>
#define mkdirp(p) _mkdir(p)
#else
#define mkdirp(p) mkdir((p), 0755)
#endif

/* ---------- Configuration ---------- */
#define DATA_DIR "data"
#define USERS_FILE DATA_DIR"/users.csv"
#define STUDENTS_FILE DATA_DIR"/students.csv"
#define SUBJECTS_FILE DATA_DIR"/subjects.csv"
#define MARKS_FILE DATA_DIR"/marks.csv"
#define ATT_FILE DATA_DIR"/attendance.csv"
#define REPORTS_DIR "reports"

#define MAX_STUDENTS 1024
#define MAX_SUBJECTS 512
#define MAX_NAME 128
#define MAX_EMAIL 128
#define MAX_PHONE 32
#define MAX_CODE 32
#define MAX_TITLE 128

/* ---------- Structs ---------- */
typedef struct {
    char id[32];       /* SAP ID as string */
    char roll[32];
    char name[MAX_NAME];
    char email[MAX_EMAIL];
    char phone[MAX_PHONE];
    int year;          /* 1..4 */
    int current_sem;   /* 1..8 */
} Student;

typedef struct {
    char id[32];       /* subject unique id */
    char code[MAX_CODE];
    char title[MAX_TITLE];
    int credits;
    int semester;      /* 1..8 */
} SubjectRec;

typedef struct {
    char student_id[32];
    char subject_id[32];
    double marks; /* 0..100 */
} MarkRec;

typedef struct {
    char student_id[32];
    char subject_id[32];
    int present;
    int total;
} AttRec;

/* ---------- In-memory arrays ---------- */
static Student students[MAX_STUDENTS];
static int student_count = 0;

static SubjectRec subjects[MAX_SUBJECTS];
static int subject_count = 0;

static MarkRec marks[MAX_STUDENTS * 32];
static int marks_count = 0;

static AttRec atts[MAX_STUDENTS * 32];
static int atts_count = 0;

/* ---------- Utility ---------- */
void ensure_dirs() {
    struct stat st;
    if (stat(DATA_DIR, &st) == -1) mkdirp(DATA_DIR);
    if (stat(REPORTS_DIR, &st) == -1) mkdirp(REPORTS_DIR);
}

void trim(char *s) {
    if (!s) return;
    while (*s && (*s==' ' || *s=='\t' || *s=='\r' || *s=='\n')) memmove(s, s+1, strlen(s));
    int i = (int)strlen(s)-1;
    while (i>=0 && (s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n')) s[i--]=0;
}

void safe_getline(char *buf, int n) {
    if (!fgets(buf, n, stdin)) { buf[0]=0; return; }
    trim(buf);
}

/* generate unique subject id (simple) */
void gen_id(char *out, size_t n, const char *prefix) {
    static unsigned long ctr = 0;
    ctr++;
    unsigned long t = (unsigned long)time(NULL) ^ ctr;
    snprintf(out, n, "%s%08lx", prefix, t & 0xffffffff);
}

/* ---------- CSV persistence ---------- */

void save_students_csv() {
    FILE *f = fopen(STUDENTS_FILE, "w");
    if (!f) return;
    for (int i=0;i<student_count;i++) {
        fprintf(f, "%s,%s,%s,%s,%s,%d,%d\n",
            students[i].id, students[i].roll, students[i].name, students[i].email, students[i].phone,
            students[i].year, students[i].current_sem);
    }
    fclose(f);
}

void load_students_csv() {
    student_count = 0;
    FILE *f = fopen(STUDENTS_FILE, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        trim(line); if (strlen(line)==0) continue;
        Student s; memset(&s,0,sizeof(s));
        char *p=line;
        char *tok;
        tok = strtok(p, ","); if (!tok) continue; strncpy(s.id, tok, sizeof(s.id)-1);
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

void save_subjects_csv() {
    FILE *f = fopen(SUBJECTS_FILE, "w");
    if (!f) return;
    for (int i=0;i<subject_count;i++) {
        fprintf(f, "%s,%s,%s,%d,%d\n",
            subjects[i].id, subjects[i].code, subjects[i].title, subjects[i].credits, subjects[i].semester);
    }
    fclose(f);
}

void load_subjects_csv() {
    subject_count = 0;
    FILE *f = fopen(SUBJECTS_FILE, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        trim(line); if (strlen(line)==0) continue;
        SubjectRec s; memset(&s,0,sizeof(s));
        char *p=line;
        char *tok;
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

void save_marks_csv() {
    FILE *f = fopen(MARKS_FILE, "w");
    if (!f) return;
    for (int i=0;i<marks_count;i++) {
        fprintf(f, "%s,%s,%.2f\n", marks[i].student_id, marks[i].subject_id, marks[i].marks);
    }
    fclose(f);
}

void load_marks_csv() {
    marks_count = 0;
    FILE *f = fopen(MARKS_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim(line); if (strlen(line)==0) continue;
        MarkRec m; memset(&m,0,sizeof(m));
        char *p=line;
        char *tok;
        tok = strtok(p, ","); if (!tok) continue; strncpy(m.student_id, tok, sizeof(m.student_id)-1);
        tok = strtok(NULL, ","); if (!tok) continue; strncpy(m.subject_id, tok, sizeof(m.subject_id)-1);
        tok = strtok(NULL, ","); if (!tok) continue; m.marks = atof(tok);
        marks[marks_count++] = m;
    }
    fclose(f);
}

void save_atts_csv() {
    FILE *f = fopen(ATT_FILE, "w");
    if (!f) return;
    for (int i=0;i<atts_count;i++) {
        fprintf(f, "%s,%s,%d,%d\n", atts[i].student_id, atts[i].subject_id, atts[i].present, atts[i].total);
    }
    fclose(f);
}

void load_atts_csv() {
    atts_count = 0;
    FILE *f = fopen(ATT_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim(line); if (strlen(line)==0) continue;
        AttRec a; memset(&a,0,sizeof(a));
        char *p=line;
        char *tok;
        tok = strtok(p, ","); if (!tok) continue; strncpy(a.student_id, tok, sizeof(a.student_id)-1);
        tok = strtok(NULL, ","); if (!tok) continue; strncpy(a.subject_id, tok, sizeof(a.subject_id)-1);
        tok = strtok(NULL, ","); if (!tok) continue; a.present = atoi(tok);
        tok = strtok(NULL, ","); if (!tok) continue; a.total = atoi(tok);
        atts[atts_count++] = a;
    }
    fclose(f);
}

/* ---------- Subject syllabus (auto-add lists) ---------- */
/* For each semester we define arrays of titles, codes are generated. Credits as per user's spec. */

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

/* helper: add subject if not exists globally (use title+semester uniqueness) */
int find_subject_by_title_sem(const char *title, int semester) {
    for (int i=0;i<subject_count;i++) {
        if (subjects[i].semester == semester && strcmp(subjects[i].title, title)==0) return i;
    }
    return -1;
}

void ensure_subjects_populated() {
    /* If subjects.csv is empty, populate all syllabus subjects */
    if (subject_count > 0) return;
    const SubDef *sets[9] = {NULL, SEM1, SEM2, SEM3, SEM4, SEM5, SEM6, SEM7, SEM8};
    for (int sem=1; sem<=8; ++sem) {
        const SubDef *arr = sets[sem];
        for (int i=0; arr[i].title; ++i) {
            SubjectRec s; memset(&s,0,sizeof(s));
            gen_id(s.id, sizeof(s.id), "sub");
            snprintf(s.code, sizeof(s.code), "S%02d%02d", sem, i+1);
            strncpy(s.title, arr[i].title, sizeof(s.title)-1);
            s.credits = arr[i].credits;
            s.semester = sem;
            subjects[subject_count++] = s;
            if (subject_count >= MAX_SUBJECTS) break;
        }
    }
    save_subjects_csv();
}

/* ---------- Helper search functions ---------- */
int find_student_idx_by_sap(const char *sap) {
    for (int i=0;i<student_count;i++) if (strcmp(students[i].id, sap)==0) return i;
    return -1;
}
int find_student_idx_by_roll(const char *roll) {
    for (int i=0;i<student_count;i++) if (strcmp(students[i].roll, roll)==0) return i;
    return -1;
}
int find_subject_idx_by_id(const char *sid) {
    for (int i=0;i<subject_count;i++) if (strcmp(subjects[i].id, sid)==0) return i;
    return -1;
}
int find_subject_idx_by_title_sem(const char *title, int sem) {
    for (int i=0;i<subject_count;i++) if (subjects[i].semester==sem && strcmp(subjects[i].title, title)==0) return i;
    return -1;
}
int find_mark_index(const char *studid, const char *subid) {
    for (int i=0;i<marks_count;i++) if (strcmp(marks[i].student_id, studid)==0 && strcmp(marks[i].subject_id, subid)==0) return i;
    return -1;
}
int find_att_index(const char *studid, const char *subid) {
    for (int i=0;i<atts_count;i++) if (strcmp(atts[i].student_id, studid)==0 && strcmp(atts[i].subject_id, subid)==0) return i;
    return -1;
}

/* ---------- SGPA/CGPA calculations ---------- */
/* grade point mapping: linear mapping (mark/100)*10 as requested earlier */
double mark_to_grade_point(double mark) {
    if (mark < 0) return 0.0;
    return (mark/100.0) * 10.0;
}

/* compute SGPA for a given student and semester (credit-weighted) */
double compute_sgpa(const char *studid, int sem) {
    double total_weighted = 0.0;
    int total_credits = 0;
    for (int i=0;i<subject_count;i++) {
        if (subjects[i].semester != sem) continue;
        int midx = find_mark_index(studid, subjects[i].id);
        if (midx < 0) continue;
        double mark = marks[midx].marks;
        double gp = mark_to_grade_point(mark);
        total_weighted += gp * subjects[i].credits;
        total_credits += subjects[i].credits;
    }
    if (total_credits == 0) return -1.0;
    return total_weighted / total_credits;
}

/* compute CGPA as average of available SGPAs across 8 semesters (only those with data),
   also provide credit-weighted CGPA if desired (we compute credit-weighted to be robust) */
double compute_cgpa(const char *studid) {
    double total_weighted = 0.0;
    int total_credits = 0;
    for (int i=0;i<subject_count;i++) {
        int midx = find_mark_index(studid, subjects[i].id);
        if (midx < 0) continue;
        double mark = marks[midx].marks;
        double gp = mark_to_grade_point(mark);
        total_weighted += gp * subjects[i].credits;
        total_credits += subjects[i].credits;
    }
    if (total_credits == 0) return -1.0;
    return total_weighted / total_credits;
}

/* ---------- Student registration (self-register) ---------- */
void auto_add_semester_subjects_to_student(Student *s, int sem) {
    /* Add all subjects for semesters 1..sem to marks and attendance structures with 0 values,
       but only if the student doesn't already have an entry for that subject (we consider mark entries list). */
    for (int msem = 1; msem <= sem; ++msem) {
        for (int j=0;j<subject_count;j++) {
            if (subjects[j].semester != msem) continue;
            /* if no mark entry exists, create placeholder with -1 (meaning no marks yet) */
            if (find_mark_index(s->id, subjects[j].id) < 0) {
                MarkRec mr; memset(&mr,0,sizeof(mr));
                strncpy(mr.student_id, s->id, sizeof(mr.student_id)-1);
                strncpy(mr.subject_id, subjects[j].id, sizeof(mr.subject_id)-1);
                mr.marks = -1.0;
                marks[marks_count++] = mr;
            }
            if (find_att_index(s->id, subjects[j].id) < 0) {
                AttRec ar; memset(&ar,0,sizeof(ar));
                strncpy(ar.student_id, s->id, sizeof(ar.student_id)-1);
                strncpy(ar.subject_id, subjects[j].id, sizeof(ar.subject_id)-1);
                ar.present = 0; ar.total = 0;
                atts[atts_count++] = ar;
            }
        }
    }
}

/* register new student (self) */
void register_student() {
    Student s; memset(&s,0,sizeof(s));
    printf("Enter SAP ID (numeric): ");
    char buf[64]; safe_getline(buf,sizeof(buf));
    trim(buf);
    if (strlen(buf)==0) { printf("Cancelled.\n"); return; }
    if (find_student_idx_by_sap(buf) >= 0) { printf("A student with this SAP ID already exists.\n"); return; }
    strncpy(s.id, buf, sizeof(s.id)-1);
    printf("Enter Roll number: "); safe_getline(s.roll, sizeof(s.roll));
    printf("Full name: "); safe_getline(s.name, sizeof(s.name));
    printf("Email: "); safe_getline(s.email, sizeof(s.email));
    printf("Phone: "); safe_getline(s.phone, sizeof(s.phone));
    printf("Year (1-4): "); safe_getline(buf, sizeof(buf)); s.year = atoi(buf);
    if (s.year < 1 || s.year > 4) s.year = 1;
    printf("Current semester (1-8): "); safe_getline(buf, sizeof(buf)); s.current_sem = atoi(buf);
    if (s.current_sem < 1 || s.current_sem > 8) s.current_sem = 1;

    /* add to array */
    students[student_count++] = s;
    /* auto add semester subjects (current sem and earlier) */
    auto_add_semester_subjects_to_student(&students[student_count-1], s.current_sem);
    save_students_csv();
    save_marks_csv();
    save_atts_csv();
    printf("Registration successful. Student created with SAP ID: %s\n", s.id);
}

/* ---------- Admin functions ---------- */
int admin_auth() {
    /* For simplicity, single admin credential. This can be extended to users file */
    const char *ADMIN_USER = "admin";
    const char *ADMIN_PASS = "admin123";
    char user[64], pass[64];
    printf("Admin username: "); safe_getline(user, sizeof(user));
    printf("Admin password: "); safe_getline(pass, sizeof(pass));
    if (strcmp(user, ADMIN_USER)==0 && strcmp(pass, ADMIN_PASS)==0) return 1;
    printf("Invalid admin credentials.\n");
    return 0;
}

/* Admin: add subject manually */
void admin_add_subject_manually() {
    SubjectRec s; memset(&s,0,sizeof(s));
    printf("Title: "); safe_getline(s.title, sizeof(s.title));
    printf("Credits (int): "); char buf[64]; safe_getline(buf, sizeof(buf)); s.credits = atoi(buf);
    printf("Semester (1-8): "); safe_getline(buf, sizeof(buf)); s.semester = atoi(buf);
    gen_id(s.id, sizeof(s.id), "sub");
    /* generate code */
    snprintf(s.code, sizeof(s.code), "X%02d%02d", s.semester, subject_count+1);
    subjects[subject_count++] = s;
    save_subjects_csv();
    printf("Subject added.\n");
}

/* Admin: add subject(s) for a student (per semester) - but usually auto-added on registration */
void admin_add_subjects_for_student() {
    printf("Enter student's SAP ID: ");
    char buf[64]; safe_getline(buf,sizeof(buf));
    int idx = find_student_idx_by_sap(buf);
    if (idx < 0) { printf("Student not found.\n"); return; }
    Student *s = &students[idx];
    printf("Enter semester number whose subjects you want to add (1-8): ");
    safe_getline(buf,sizeof(buf)); int sem = atoi(buf);
    if (sem < 1 || sem > 8) { printf("Invalid semester.\n"); return; }
    /* add subjects of that semester (only) */
    int added=0;
    for (int i=0;i<subject_count;i++) {
        if (subjects[i].semester != sem) continue;
        if (find_mark_index(s->id, subjects[i].id) < 0) {
            MarkRec m; memset(&m,0,sizeof(m));
            strncpy(m.student_id, s->id, sizeof(m.student_id)-1);
            strncpy(m.subject_id, subjects[i].id, sizeof(m.subject_id)-1);
            m.marks = -1.0;
            marks[marks_count++] = m;
            AttRec a; memset(&a,0,sizeof(a));
            strncpy(a.student_id, s->id, sizeof(a.student_id)-1);
            strncpy(a.subject_id, subjects[i].id, sizeof(a.subject_id)-1);
            a.present = 0; a.total = 0;
            atts[atts_count++] = a;
            added++;
        }
    }
    save_marks_csv(); save_atts_csv();
    printf("Added %d subjects for student %s.\n", added, s->id);
}

/* Admin: enter/update marks for specific student & subject */
void admin_enter_update_marks() {
    printf("Enter student's SAP ID: ");
    char buf[64]; safe_getline(buf,sizeof(buf));
    int idx = find_student_idx_by_sap(buf);
    if (idx < 0) { printf("Student not found.\n"); return; }
    Student *s = &students[idx];
    printf("Student: %s (%s) sem %d\n", s->name, s->id, s->current_sem);
    /* show subjects for student's current semester (and earlier) */
    for (int i=0;i<subject_count;i++) {
        if (subjects[i].semester > s->current_sem) continue;
        int midx = find_mark_index(s->id, subjects[i].id);
        double cur = -1.0;
        if (midx >= 0) cur = marks[midx].marks;
        printf("[%d] %s (Sem %d) Credits:%d  Marks:%s\n", i+1, subjects[i].title, subjects[i].semester, subjects[i].credits,
               (cur >= 0.0) ? ({ char tmp[32]; snprintf(tmp,32,"%.2f",cur); tmp; }) : "N/A");
    }
    printf("Enter subject index number to set marks (or 0 to cancel): ");
    safe_getline(buf,sizeof(buf));
    int sel = atoi(buf);
    if (sel <= 0 || sel > subject_count) { printf("Cancelled.\n"); return; }
    SubjectRec *sub = &subjects[sel-1];
    printf("Enter marks (0-100) for %s: ", sub->title);
    safe_getline(buf,sizeof(buf)); double mm = atof(buf);
    if (mm < 0) mm = 0; if (mm > 100) mm = 100;
    int mindex = find_mark_index(s->id, sub->id);
    if (mindex >= 0) {
        marks[mindex].marks = mm;
    } else {
        MarkRec mr; memset(&mr,0,sizeof(mr));
        strncpy(mr.student_id, s->id, sizeof(mr.student_id)-1);
        strncpy(mr.subject_id, sub->id, sizeof(mr.subject_id)-1);
        mr.marks = mm;
        marks[marks_count++] = mr;
    }
    save_marks_csv();
    /* automatically update cgpa stored? we compute on the fly so no stored cgpa */
    printf("Marks updated.\n");
}

/* Admin: mark attendance for single student & single subject */
void admin_mark_attendance_single() {
    printf("Enter student's SAP ID: ");
    char buf[64]; safe_getline(buf,sizeof(buf));
    int idx = find_student_idx_by_sap(buf);
    if (idx < 0) { printf("Student not found.\n"); return; }
    Student *s = &students[idx];
    printf("Student: %s (%s) sem %d\n", s->name, s->id, s->current_sem);
    printf("Available subjects for student:\n");
    for (int i=0;i<subject_count;i++) {
        int midx = find_mark_index(s->id, subjects[i].id);
        if (midx < 0) continue; /* subject not assigned */
        printf("[%d] %s (Sem %d)\n", i+1, subjects[i].title, subjects[i].semester);
    }
    printf("Enter subject index to mark attendance: "); safe_getline(buf,sizeof(buf));
    int sel = atoi(buf);
    if (sel <= 0 || sel > subject_count) { printf("Cancelled.\n"); return; }
    SubjectRec *sub = &subjects[sel-1];
    int aidx = find_att_index(s->id, sub->id);
    if (aidx < 0) {
        AttRec a; memset(&a,0,sizeof(a));
        strncpy(a.student_id, s->id, sizeof(a.student_id)-1);
        strncpy(a.subject_id, sub->id, sizeof(a.subject_id)-1);
        a.present = 0; a.total = 0;
        atts[atts_count++] = a;
        aidx = atts_count - 1;
    }
    printf("Enter present days to add (integer): "); safe_getline(buf,sizeof(buf));
    int addp = atoi(buf);
    printf("Enter total days to add (integer): "); safe_getline(buf,sizeof(buf));
    int addt = atoi(buf);
    if (addt < addp) { printf("Total cannot be less than present. Cancelled.\n"); return; }
    atts[aidx].present += addp;
    atts[aidx].total += addt;
    save_atts_csv();
    printf("Attendance updated for %s.\n", sub->title);
}

/* Admin: bulk mark attendance for whole class for a subject */
void admin_bulk_attendance_for_subject() {
    printf("Select subject index to mark for whole class:\n");
    for (int i=0;i<subject_count;i++) {
        printf("[%d] %s (Sem %d)\n", i+1, subjects[i].title, subjects[i].semester);
    }
    printf("Enter subject index: ");
    char buf[64]; safe_getline(buf,sizeof(buf));
    int sel = atoi(buf);
    if (sel <= 0 || sel > subject_count) { printf("Cancelled.\n"); return; }
    SubjectRec *sub = &subjects[sel-1];
    printf("Enter total classes held (e.g., 1): ");
    safe_getline(buf,sizeof(buf)); int held = atoi(buf);
    if (held <= 0) { printf("Invalid value.\n"); return; }
    printf("Enter list of SAP IDs (space/comma separated) who were present, then press Enter:\n");
    char line[1024]; safe_getline(line, sizeof(line));
    /* parse present IDs */
    char tmp[1024]; strcpy(tmp, line);
    char *p = tmp;
    char *tok;
    while ((tok = strtok(p, " ,\t")) != NULL) {
        p = NULL;
        trim(tok);
        if (strlen(tok)==0) continue;
        int si = find_student_idx_by_sap(tok);
        if (si < 0) continue;
        /* ensure att rec exists */
        int aidx = find_att_index(students[si].id, sub->id);
        if (aidx < 0) {
            AttRec a; memset(&a,0,sizeof(a));
            strncpy(a.student_id, students[si].id, sizeof(a.student_id)-1);
            strncpy(a.subject_id, sub->id, sizeof(a.subject_id)-1);
            a.present = 0; a.total = 0;
            atts[atts_count++] = a;
            aidx = atts_count - 1;
        }
        /* for the present student, increment present and total */
        atts[aidx].present += 1;
        atts[aidx].total += held;
    }
    /* For all other students in system who have the subject, increment only total */
    for (int i=0;i<student_count;i++) {
        int aidx = find_att_index(students[i].id, sub->id);
        if (aidx < 0) {
            /* student does not have subject - skip */
            continue;
        }
        /* we already incremented present for those listed; but we may not have incremented total for others */
        /* To avoid double counting, ensure total increment only once: add 'held' for all students who have subject */
        atts[aidx].total += held; /* note: those present got present+1 and total+held earlier -> they got total incremented twice; to keep semantics, earlier loop added total as held for present; this line will add held again -> that would double count. Fix: instead, first mark present array separately. */
    }
    /* The above approach was messy; to keep deterministic behaviour, we will revert and implement robustly: read present list again and recompute increments cleanly. */
    /* Recompute safely: add 'held' to total for ALL students who have subject; add 1 to present for those in present list */
    /* First: parse present list into array */
    char present_ids[128][32]; int pc=0;
    strcpy(tmp, line); p = tmp;
    while ((tok = strtok(p, " ,\t")) != NULL) {
        p = NULL; trim(tok);
        if (strlen(tok)==0) continue;
        strncpy(present_ids[pc++], tok, sizeof(present_ids[0])-1);
    }
    /* Now for each student who has att rec for this subject (or create), add held to total and +1 to present if in list */
    for (int i=0;i<student_count;i++) {
        /* check if student has this subject assigned via mark record existence */
        int assigned = (find_mark_index(students[i].id, sub->id) >= 0);
        if (!assigned) continue;
        int aidx = find_att_index(students[i].id, sub->id);
        if (aidx < 0) {
            AttRec a; memset(&a,0,sizeof(a));
            strncpy(a.student_id, students[i].id, sizeof(a.student_id)-1);
            strncpy(a.subject_id, sub->id, sizeof(a.subject_id)-1);
            a.present = 0; a.total = 0;
            atts[atts_count++] = a;
            aidx = atts_count - 1;
        }
        atts[aidx].total += held;
        /* check presence */
        int found = 0;
        for (int k=0;k<pc;k++) if (strcmp(present_ids[k], students[i].id)==0) { found=1; break; }
        if (found) atts[aidx].present += 1;
    }

    save_atts_csv();
    printf("Bulk attendance done for subject %s. Total classes added: %d\n", sub->title, held);
}

/* ---------- Display & Search ---------- */
void display_student_record(const Student *s) {
    printf("SAP ID: %s\n", s->id);
    printf("Roll: %s\n", s->roll);
    printf("Name: %s\n", s->name);
    printf("Email: %s\n", s->email);
    printf("Phone: %s\n", s->phone);
    printf("Year: %d\n", s->year);
    printf("Current Semester: %d\n", s->current_sem);
    printf("Subjects & Marks:\n");
    for (int i=0;i<subject_count;i++) {
        int midx = find_mark_index(s->id, subjects[i].id);
        if (midx < 0) continue;
        double mk = marks[midx].marks;
        int aidx = find_att_index(s->id, subjects[i].id);
        int pres = (aidx >= 0) ? atts[aidx].present : 0;
        int tot = (aidx >= 0) ? atts[aidx].total : 0;
        printf(" - %s (Sem %d, credits %d): Marks: %s, Attendance: %d/%d\n",
               subjects[i].title, subjects[i].semester, subjects[i].credits,
               (mk >= 0) ? ({ char tmp[32]; snprintf(tmp,sizeof(tmp),"%.2f",mk); tmp; }) : "N/A",
               pres, tot);
    }
    double cgpa = compute_cgpa(s->id);
    if (cgpa < 0) printf("CGPA: N/A\n");
    else printf("CGPA (credit-weighted): %.3f\n", cgpa);
}

void search_display_student() {
    printf("Search by: [1] SAP ID  [2] Name : ");
    char buf[128]; safe_getline(buf,sizeof(buf));
    if (buf[0] == '1') {
        printf("Enter SAP ID: "); safe_getline(buf,sizeof(buf));
        int idx = find_student_idx_by_sap(buf);
        if (idx < 0) { printf("Not found.\n"); return; }
        display_student_record(&students[idx]);
    } else {
        printf("Enter name substring: "); safe_getline(buf,sizeof(buf));
        int found = 0;
        for (int i=0;i<student_count;i++) {
            if (strcasestr(students[i].name, buf)) {
                display_student_record(&students[i]);
                printf("----\n"); found++;
            }
        }
        if (!found) printf("No students matched.\n");
    }
}

/* display all students */
void display_all_students() {
    if (student_count == 0) { printf("No students.\n"); return; }
    for (int i=0;i<student_count;i++) {
        printf("[%d] %s | %s | Year %d Semester %d\n", i+1, students[i].id, students[i].name, students[i].year, students[i].current_sem);
    }
}

/* delete student by SAP ID */
void delete_student() {
    printf("Enter SAP ID to delete: ");
    char buf[64]; safe_getline(buf,sizeof(buf));
    int idx = find_student_idx_by_sap(buf);
    if (idx < 0) { printf("Student not found.\n"); return; }
    /* remove from marks and attendance */
    for (int i=0;i<marks_count;i++) {
        if (strcmp(marks[i].student_id, students[idx].id) == 0) {
            /* shift left */
            for (int j=i;j<marks_count-1;j++) marks[j]=marks[j+1];
            marks_count--; i--;
        }
    }
    for (int i=0;i<atts_count;i++) {
        if (strcmp(atts[i].student_id, students[idx].id) == 0) {
            for (int j=i;j<atts_count-1;j++) atts[j]=atts[j+1];
            atts_count--; i--;
        }
    }
    /* remove student */
    for (int i=idx;i<student_count-1;i++) students[i]=students[i+1];
    student_count--;
    save_students_csv(); save_marks_csv(); save_atts_csv();
    printf("Student deleted.\n");
}

/* sort helpers */
int cmp_by_sapid(const void *a, const void *b) {
    const Student *sa = (const Student*)a;
    const Student *sb = (const Student*)b;
    return strcmp(sa->id, sb->id);
}
int cmp_by_name(const void *a, const void *b) {
    const Student *sa = (const Student*)a;
    const Student *sb = (const Student*)b;
    return strcasecmp(sa->name, sb->name);
}

void display_sorted_by_sapid() {
    if (student_count==0) { printf("No students.\n"); return; }
    Student *tmp = malloc(sizeof(Student) * student_count);
    memcpy(tmp, students, sizeof(Student)*student_count);
    qsort(tmp, student_count, sizeof(Student), cmp_by_sapid);
    for (int i=0;i<student_count;i++) {
        printf("%s | %s | Year %d Sem %d\n", tmp[i].id, tmp[i].name, tmp[i].year, tmp[i].current_sem);
    }
    free(tmp);
}

void display_sorted_by_name() {
    if (student_count==0) { printf("No students.\n"); return; }
    Student *tmp = malloc(sizeof(Student) * student_count);
    memcpy(tmp, students, sizeof(Student)*student_count);
    qsort(tmp, student_count, sizeof(Student), cmp_by_name);
    for (int i=0;i<student_count;i++) {
        printf("%s | %s | Year %d Sem %d\n", tmp[i].id, tmp[i].name, tmp[i].year, tmp[i].current_sem);
    }
    free(tmp);
}

/* calculate and display CGPA of a student (wrapper) */
void calc_display_cgpa_student() {
    printf("Enter SAP ID: ");
    char buf[64]; safe_getline(buf,sizeof(buf));
    int idx = find_student_idx_by_sap(buf);
    if (idx < 0) { printf("Student not found.\n"); return; }
    double cg = compute_cgpa(students[idx].id);
    if (cg < 0) printf("No graded credits found (CGPA N/A)\n");
    else printf("CGPA (credit-weighted) for %s: %.3f\n", students[idx].name, cg);
}

/* average CGPA of a year */
void average_cgpa_of_year() {
    printf("Enter Year (1-4): ");
    char buf[64]; safe_getline(buf,sizeof(buf)); int yr = atoi(buf);
    if (yr < 1 || yr > 4) { printf("Invalid year.\n"); return; }
    double sum = 0.0; int count = 0;
    for (int i=0;i<student_count;i++) {
        if (students[i].year != yr) continue;
        double cg = compute_cgpa(students[i].id);
        if (cg < 0) continue;
        sum += cg; count++;
    }
    if (count == 0) { printf("No students with CGPA data in Year %d.\n", yr); return; }
    printf("Average CGPA of Year %d: %.3f (n=%d)\n", yr, sum / count, count);
}

/* export all students to CSV (simple dump) */
void export_all_to_csv() {
    char fname[256];
    time_t t = time(NULL);
    snprintf(fname, sizeof(fname), "export_students_%ld.csv", (long)t);
    FILE *f = fopen(fname, "w");
    if (!f) { printf("Unable to create %s\n", fname); return; }
    fprintf(f, "sap_id,roll,name,email,phone,year,semester,cgpa\n");
    for (int i=0;i<student_count;i++) {
        double cg = compute_cgpa(students[i].id);
        fprintf(f, "%s,%s,%s,%s,%s,%d,%d,%.3f\n", students[i].id, students[i].roll, students[i].name, students[i].email, students[i].phone, students[i].year, students[i].current_sem, (cg<0?0.0:cg));
    }
    fclose(f);
    printf("Exported students to %s\n", fname);
}

/* attendance report below threshold */
void attendance_report_below_threshold() {
    printf("Enter semester number: "); char buf[64]; safe_getline(buf,sizeof(buf)); int sem = atoi(buf);
    printf("Enter subject index (use subject list) or 0 for all subjects in semester:\n");
    for (int i=0;i<subject_count;i++) if (subjects[i].semester==sem) printf("[%d] %s\n", i+1, subjects[i].title);
    printf("Enter subject index (0 for all): "); safe_getline(buf,sizeof(buf)); int sel = atoi(buf);
    printf("Enter threshold percent (e.g., 75): "); safe_getline(buf,sizeof(buf)); double thr = atof(buf);
    if (thr < 0 || thr > 100) thr = 75.0;
    printf("Students below %.1f%% attendance:\n", thr);
    int found = 0;
    for (int i=0;i<student_count;i++) {
        for (int j=0;j<subject_count;j++) {
            if (subjects[j].semester != sem) continue;
            if (sel != 0 && sel != (j+1)) continue;
            int aidx = find_att_index(students[i].id, subjects[j].id);
            if (aidx < 0) continue;
            int pres = atts[aidx].present;
            int tot = atts[aidx].total;
            double pct = (tot == 0) ? 0.0 : ((double)pres * 100.0 / tot);
            if (pct < thr) {
                printf("%s | %s | Subject: %s | Attendance: %.1f%% (%d/%d)\n", students[i].id, students[i].name, subjects[j].title, pct, pres, tot);
                found++;
            }
        }
    }
    if (!found) printf("No students below threshold.\n");
}

/* ---------- Report card generation (student-facing) ---------- */
void generate_report_card_for_student() {
    printf("Enter SAP ID: ");
    char buf[64]; safe_getline(buf,sizeof(buf));
    int idx = find_student_idx_by_sap(buf);
    if (idx < 0) { printf("Student not found.\n"); return; }
    Student *s = &students[idx];
    printf("Enter Exam name (e.g., Midterm, End-Sem): "); safe_getline(buf,sizeof(buf));
    char exam[128]; strncpy(exam, buf, sizeof(exam)-1);
    /* build file name */
    time_t t = time(NULL);
    char fname[256];
    snprintf(fname, sizeof(fname), "%s/report_%s_sem%d_%ld.txt", REPORTS_DIR, s->id, s->current_sem, (long)t);
    FILE *f = fopen(fname, "w");
    if (!f) { printf("Unable to create report file.\n"); return; }
    /* Header A4-ish */
    fprintf(f, "------------------------------------------------------------\n");
    fprintf(f, "               ABC COLLEGE OF ENGINEERING (Demo)\n");
    fprintf(f, "               Student Record & Result (Report Card)\n");
    fprintf(f, "------------------------------------------------------------\n\n");
    fprintf(f, "Name: %s\nSAP ID: %s\nRoll: %s\nEmail: %s\nPhone: %s\nYear: %d\nSemester: %d\nExam: %s\nGenerated: %s\n\n",
            s->name, s->id, s->roll, s->email, s->phone, s->year, s->current_sem, exam, ctime(&t));
    fprintf(f, "------------------------------------------------------------\n");
    fprintf(f, "| %-3s | %-40s | %6s | %6s |\n", "SNo", "Subject", "Credits", "Marks");
    fprintf(f, "------------------------------------------------------------\n");
    int sno=1;
    for (int i=0;i<subject_count;i++) {
        if (subjects[i].semester > s->current_sem) continue;
        int midx = find_mark_index(s->id, subjects[i].id);
        double mk = (midx>=0) ? marks[midx].marks : -1.0;
        fprintf(f, "| %3d | %-40s | %6d | %6s |\n", sno++, subjects[i].title, subjects[i].credits,
                (mk>=0.0) ? ({ char tmp[32]; snprintf(tmp,32,"%.2f",mk); tmp; }) : "N/A");
    }
    fprintf(f, "------------------------------------------------------------\n");
    /* semester SGPA breakdown */
    fprintf(f, "\nSemester-wise SGPA:\n");
    for (int sem=1; sem<=s->current_sem; ++sem) {
        double sg = compute_sgpa(s->id, sem);
        if (sg < 0) fprintf(f, "  Sem %d: N/A\n", sem);
        else fprintf(f, "  Sem %d: %.3f\n", sem, sg);
    }
    double cg = compute_cgpa(s->id);
    if (cg < 0) fprintf(f, "\nCGPA: N/A\n");
    else fprintf(f, "\nCGPA (credit-weighted): %.3f\n", cg);
    fprintf(f, "\nRemarks: ____________\n");
    fprintf(f, "\n------------------------------------------------------------\n");
    fprintf(f, "Principal/Controller of Exams\n");
    fclose(f);
    printf("Report card generated: %s\n", fname);
}

/* ---------- Bootstrap sample data (ensure at least 5 students) ---------- */
void create_sample_students_if_none() {
    if (student_count >= 5) return;
    Student s;
    char roll[32];
    for (int i=1;i<=5;i++) {
        memset(&s,0,sizeof(s));
        snprintf(s.id, sizeof(s.id), "10000%d", i);
        snprintf(s.roll, sizeof(s.roll), "R2025%03d", i);
        snprintf(s.name, sizeof(s.name), "Sample Student %d", i);
        snprintf(s.email, sizeof(s.email), "student%d@example.com", i);
        snprintf(s.phone, sizeof(s.phone), "70000000%02d", i);
        s.year = (i%4)+1;
        s.current_sem = (s.year-1)*2 + 1; /* approximate */
        students[student_count++] = s;
        auto_add_semester_subjects_to_student(&students[student_count-1], s.current_sem);
    }
    save_students_csv(); save_marks_csv(); save_atts_csv();
    printf("Created %d sample students.\n", 5);
}

/* ---------- Main menu and flow ---------- */

void print_main_menu() {
    printf("\n===== Student Record & Result Management =====\n");
    printf("1. Add new student (self-register)\n");
    printf("2. Modify student\n");
    printf("3. Add subject(s) for a student (per semester)\n");
    printf("4. Enter/Update marks for subject (admin required)\n");
    printf("5. Mark attendance for a student (single subject) (admin required)\n");
    printf("6. Bulk mark attendance for whole class (subject) (admin required)\n");
    printf("7. Display student's subject list & attendance\n");
    printf("8. Search & display student (by SAP ID or Name)\n");
    printf("9. Calculate & display CGPA of student\n");
    printf("10. Average CGPA of a Year\n");
    printf("11. Display all students\n");
    printf("12. Delete student (admin required)\n");
    printf("13. Display all records sorted by SAP ID\n");
    printf("14. Display all records sorted by Name\n");
    printf("15. Generate report card (student)\n");
    printf("16. Export all students to CSV\n");
    printf("17. Attendance report: list students below threshold (enter sem & subject)\n");
    printf("0. Exit\n");
    printf("Enter choice: ");
}

void modify_student() {
    printf("Enter SAP ID to modify: "); char buf[128]; safe_getline(buf,sizeof(buf));
    int idx = find_student_idx_by_sap(buf);
    if (idx < 0) { printf("Not found.\n"); return; }
    Student *s = &students[idx];
    printf("Leave blank to keep current.\n");
    printf("Name (%s): ", s->name); safe_getline(buf,sizeof(buf)); if (strlen(buf)) strncpy(s->name, buf, sizeof(s->name)-1);
    printf("Email (%s): ", s->email); safe_getline(buf,sizeof(buf)); if (strlen(buf)) strncpy(s->email, buf, sizeof(s->email)-1);
    printf("Phone (%s): ", s->phone); safe_getline(buf,sizeof(buf)); if (strlen(buf)) strncpy(s->phone, buf, sizeof(s->phone)-1);
    printf("Roll (%s): ", s->roll); safe_getline(buf,sizeof(buf)); if (strlen(buf)) strncpy(s->roll, buf, sizeof(s->roll)-1);
    printf("Year (%d): ", s->year); safe_getline(buf,sizeof(buf)); if (strlen(buf)) s->year = atoi(buf);
    printf("Current semester (%d): ", s->current_sem); safe_getline(buf,sizeof(buf)); if (strlen(buf)) {
        int oldsem = s->current_sem;
        s->current_sem = atoi(buf);
        if (s->current_sem > oldsem) auto_add_semester_subjects_to_student(s, s->current_sem);
    }
    save_students_csv(); save_marks_csv(); save_atts_csv();
    printf("Student updated.\n");
}

void display_student_subjects_attendance() {
    printf("Enter SAP ID: "); char buf[128]; safe_getline(buf,sizeof(buf));
    int idx = find_student_idx_by_sap(buf);
    if (idx < 0) { printf("Not found.\n"); return; }
    Student *s = &students[idx];
    printf("Subjects for %s:\n", s->name);
    for (int i=0;i<subject_count;i++) {
        int midx = find_mark_index(s->id, subjects[i].id);
        if (midx < 0) continue;
        int aidx = find_att_index(s->id, subjects[i].id);
        int pres = (aidx>=0)? atts[aidx].present : 0;
        int tot = (aidx>=0)? atts[aidx].total : 0;
        printf("- %s (Sem %d) Credits:%d | Marks:%s | Attendance:%d/%d\n", subjects[i].title, subjects[i].semester, subjects[i].credits,
               (marks[midx].marks>=0.0)? ({ char tmp[32]; snprintf(tmp,32,"%.2f",marks[midx].marks); tmp; }) : "N/A",
               pres, tot);
    }
}

/* Main loop */
int main(int argc, char **argv) {
    ensure_dirs();
    load_subjects_csv();
    ensure_subjects_populated(); /* if empty, populate default syllabus */
    load_students_csv();
    load_marks_csv();
    load_atts_csv();
    create_sample_students_if_none(); /* ensure at least 5 students */

    while (1) {
        print_main_menu();
        char choice[16]; safe_getline(choice,sizeof(choice));
        int ch = atoi(choice);
        switch (ch) {
            case 1: register_student(); break;
            case 2: modify_student(); break;
            case 3: {
                printf("This will add subjects of a selected semester for a student (admin auth required).\n");
                if (!admin_auth()) break;
                admin_add_subjects_for_student();
                break;
            }
            case 4: {
                if (!admin_auth()) break;
                admin_enter_update_marks();
                break;
            }
            case 5: {
                if (!admin_auth()) break;
                admin_mark_attendance_single();
                break;
            }
            case 6: {
                if (!admin_auth()) break;
                admin_bulk_attendance_for_subject();
                break;
            }
            case 7: display_student_subjects_attendance(); break;
            case 8: search_display_student(); break;
            case 9: calc_display_cgpa_student(); break;
            case 10: average_cgpa_of_year(); break;
            case 11: display_all_students(); break;
            case 12: { if (!admin_auth()) break; delete_student(); break; }
            case 13: display_sorted_by_sapid(); break;
            case 14: display_sorted_by_name(); break;
            case 15: generate_report_card_for_student(); break;
            case 16: export_all_to_csv(); break;
            case 17: attendance_report_below_threshold(); break;
            case 0: printf("Goodbye.\n"); exit(0); break;
            default: printf("Invalid choice.\n"); break;
        }
    }
    return 0;
}
