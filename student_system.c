/* student_system.c
   Student Record & Result Management System (console)
   Features:
   - Admin login / Student login / Signup
   - Persistent CSV storage in data/
   - Subjects stored with semester; display bifurcated by semester
   - Prevent duplicate marks / attendance entries (offer update)
   - SGPA per semester and CGPA calculation (credit-weighted)
   - Student dashboard shows contact info (email, phone)
   - Console dashboard style with header and credits
   - Minimal password hashing (salted custom hash) -- replace with real crypto for production
   Author: adapted to user's requirements (Tanay Sah requested)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#define mkdirp(path) _mkdir(path)
#else
#include <unistd.h>
#define mkdirp(path) mkdir(path, 0755)
#endif

/* ---------- Configuration ---------- */
#define DATA_DIR "data"
#define USERS_FILE DATA_DIR"/users.csv"
#define STUDENTS_FILE DATA_DIR"/students.csv"
#define SUBJECTS_FILE DATA_DIR"/subjects.csv"
#define MARKS_FILE DATA_DIR"/marks.csv"
#define ATT_FILE DATA_DIR"/attendance.csv"

#define MAX_LINE 1024
#define ID_LEN 40
#define NAME_LEN 100
#define EMAIL_LEN 100
#define PHONE_LEN 25
#define ROLE_LEN 10
#define CODE_LEN 20
#define TITLE_LEN 120

/* ANSI colors for console polish */
#define COL_RESET "\x1b[0m"
#define COL_HEADER "\x1b[97;44m"   /* white on blue */
#define COL_BOX "\x1b[1;36m"
#define COL_WARN "\x1b[1;33m"
#define COL_ERR  "\x1b[1;31m"
#define COL_OK   "\x1b[1;32m"
#define COL_ACCENT "\x1b[1;35m"

/* ---------- Structs ---------- */
typedef struct {
    char id[ID_LEN];
    char name[NAME_LEN];
    char email[EMAIL_LEN];
    char phone[PHONE_LEN];
    char role[ROLE_LEN]; /* "admin" or "student" */
    unsigned long pwd_hash;
    unsigned long salt;
} User;

typedef struct {
    char id[ID_LEN];
    char user_id[ID_LEN]; /* link to User.id */
    char roll[30];
    char program[80];
} Student;

typedef struct {
    char id[ID_LEN];
    char code[CODE_LEN];
    char title[TITLE_LEN];
    int credits;
    int semester;
} Subject;

typedef struct {
    char student_id[ID_LEN];
    char subject_id[ID_LEN];
    double marks; /* percentage 0-100 */
} Mark;

typedef struct {
    char student_id[ID_LEN];
    char subject_id[ID_LEN];
    int present_days;
    int total_days;
} Attendance;

/* ---------- Utility functions ---------- */

/* generate simple unique id */
void gen_id(char *out, size_t len) {
    static unsigned long ctr = 0;
    ctr++;
    unsigned long t = (unsigned long)time(NULL) ^ ctr;
    snprintf(out, len, "id%08lx%04lx", t, (unsigned long)(rand() & 0xffff));
}

/* simple salted hash (NOT cryptographically secure) */
unsigned long simple_hash(const char *s, unsigned long salt) {
    unsigned long h = 5381 + salt;
    const unsigned char *p = (const unsigned char*)s;
    while (*p) {
        h = ((h << 5) + h) + *p; /* djb2 */
        p++;
    }
    /* mix with salt */
    h ^= (salt<<13) | (salt>>7);
    return h;
}

/* create data dir if absent */
void ensure_data_dir() {
    struct stat st;
    if (stat(DATA_DIR, &st) == -1) {
        mkdirp(DATA_DIR);
    }
}

/* safe fgets trim newline */
void sfgets(char *buf, int n) {
    if (fgets(buf, n, stdin)) {
        size_t l = strlen(buf);
        if (l && buf[l-1]=='\n') buf[l-1]=0;
    }
}

/* trim */
void trim(char *s) {
    char *p = s;
    while (*p && (*p==' ' || *p=='\t' || *p=='\r' || *p=='\n')) p++;
    if (p!=s) memmove(s,p,strlen(p)+1);
    int i = strlen(s)-1;
    while (i>=0 && (s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n')) { s[i]=0; i--; }
}

/* ---------- CSV load/save helpers ---------- */

/* Append a line to file (create if not exists) */
void append_line(const char *file, const char *line) {
    ensure_data_dir();
    FILE *f = fopen(file, "a");
    if (!f) { fprintf(stderr, COL_ERR "Error: cannot open %s for append\n" COL_RESET, file); return; }
    fprintf(f, "%s\n", line);
    fclose(f);
}

/* Overwrite file with text */
void write_all(const char *file, const char *text) {
    ensure_data_dir();
    FILE *f = fopen(file, "w");
    if (!f) { fprintf(stderr, COL_ERR "Error: cannot open %s for write\n" COL_RESET, file); return; }
    fputs(text, f);
    fclose(f);
}

/* ---------- Load functions ---------- */

/* Load all users into array, return count */
int load_users(User **out) {
    ensure_data_dir();
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) { *out = NULL; return 0; }
    char line[MAX_LINE];
    User *arr = NULL; int n=0;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (strlen(line)==0) continue;
        char id[ID_LEN], name[NAME_LEN], email[EMAIL_LEN], phone[PHONE_LEN], role[ROLE_LEN];
        unsigned long h, salt;
        int r = sscanf(line, "%39[^,],%99[^,],%99[^,],%24[^,],%9[^,],%lu,%lu", id,name,email,phone,role,&h,&salt);
        if (r>=7) {
            arr = realloc(arr, sizeof(User)*(n+1));
            strncpy(arr[n].id,id,ID_LEN);
            strncpy(arr[n].name,name,NAME_LEN);
            strncpy(arr[n].email,email,EMAIL_LEN);
            strncpy(arr[n].phone,phone,PHONE_LEN);
            strncpy(arr[n].role,role,ROLE_LEN);
            arr[n].pwd_hash = h;
            arr[n].salt = salt;
            n++;
        }
    }
    fclose(f);
    *out = arr;
    return n;
}

/* save users array */
void save_users(User *arr, int n) {
    ensure_data_dir();
    FILE *f = fopen(USERS_FILE,"w");
    if (!f) { fprintf(stderr, COL_ERR "Error: cannot write users\n" COL_RESET); return; }
    for (int i=0;i<n;i++) {
        fprintf(f, "%s,%s,%s,%s,%s,%lu,%lu\n",
            arr[i].id, arr[i].name, arr[i].email, arr[i].phone, arr[i].role, arr[i].pwd_hash, arr[i].salt);
    }
    fclose(f);
}

/* Load students */
int load_students(Student **out) {
    FILE *f = fopen(STUDENTS_FILE,"r");
    if (!f) { *out=NULL; return 0; }
    char line[MAX_LINE]; Student *arr=NULL; int n=0;
    while (fgets(line,sizeof(line),f)) {
        trim(line);
        if (strlen(line)==0) continue;
        char id[ID_LEN], user_id[ID_LEN], roll[30], program[80];
        int r = sscanf(line, "%39[^,],%39[^,],%29[^,],%79[^,]", id, user_id, roll, program);
        if (r>=4) {
            arr = realloc(arr, sizeof(Student)*(n+1));
            strncpy(arr[n].id,id,ID_LEN);
            strncpy(arr[n].user_id,user_id,ID_LEN);
            strncpy(arr[n].roll,roll,30);
            strncpy(arr[n].program,program,80);
            n++;
        }
    }
    fclose(f);
    *out = arr; return n;
}

void save_students(Student *arr, int n) {
    FILE *f = fopen(STUDENTS_FILE,"w"); if (!f) { fprintf(stderr, COL_ERR "Error write students\n" COL_RESET); return; }
    for (int i=0;i<n;i++) {
        fprintf(f, "%s,%s,%s,%s\n", arr[i].id, arr[i].user_id, arr[i].roll, arr[i].program);
    }
    fclose(f);
}

/* Load subjects */
int load_subjects(Subject **out) {
    FILE *f = fopen(SUBJECTS_FILE,"r");
    if (!f) { *out=NULL; return 0; }
    char line[MAX_LINE]; Subject *arr=NULL; int n=0;
    while (fgets(line,sizeof(line),f)) {
        trim(line);
        if (strlen(line)==0) continue;
        char id[ID_LEN], code[CODE_LEN], title[TITLE_LEN]; int credits, sem;
        int r = sscanf(line,"%39[^,],%19[^,],%119[^,],%d,%d", id, code, title, &credits, &sem);
        if (r>=5) {
            arr = realloc(arr, sizeof(Subject)*(n+1));
            strncpy(arr[n].id,id,ID_LEN);
            strncpy(arr[n].code,code,CODE_LEN);
            strncpy(arr[n].title,title,TITLE_LEN);
            arr[n].credits = credits;
            arr[n].semester = sem;
            n++;
        }
    }
    fclose(f);
    *out = arr; return n;
}

void save_subjects(Subject *arr, int n) {
    FILE *f = fopen(SUBJECTS_FILE,"w"); if (!f) { fprintf(stderr, COL_ERR "Error write subjects\n" COL_RESET); return; }
    for (int i=0;i<n;i++) {
        fprintf(f,"%s,%s,%s,%d,%d\n", arr[i].id, arr[i].code, arr[i].title, arr[i].credits, arr[i].semester);
    }
    fclose(f);
}

/* Load marks */
int load_marks(Mark **out) {
    FILE *f = fopen(MARKS_FILE,"r");
    if (!f) { *out=NULL; return 0; }
    char line[MAX_LINE]; Mark *arr=NULL; int n=0;
    while (fgets(line,sizeof(line),f)) {
        trim(line); if (strlen(line)==0) continue;
        char sid[ID_LEN], subid[ID_LEN]; double marks;
        int r = sscanf(line,"%39[^,],%39[^,],%lf", sid, subid, &marks);
        if (r>=3) {
            arr = realloc(arr, sizeof(Mark)*(n+1));
            strncpy(arr[n].student_id,sid,ID_LEN);
            strncpy(arr[n].subject_id,subid,ID_LEN);
            arr[n].marks = marks;
            n++;
        }
    }
    fclose(f); *out = arr; return n;
}
void save_marks(Mark *arr, int n) {
    FILE *f = fopen(MARKS_FILE,"w"); if (!f) { fprintf(stderr, COL_ERR "Error write marks\n" COL_RESET); return; }
    for (int i=0;i<n;i++) {
        fprintf(f,"%s,%s,%.2f\n", arr[i].student_id, arr[i].subject_id, arr[i].marks);
    }
    fclose(f);
}

/* Load attendance */
int load_att(Attendance **out) {
    FILE *f = fopen(ATT_FILE,"r");
    if (!f) { *out=NULL; return 0; }
    char line[MAX_LINE]; Attendance *arr=NULL; int n=0;
    while (fgets(line,sizeof(line),f)) {
        trim(line); if (strlen(line)==0) continue;
        char sid[ID_LEN], subid[ID_LEN]; int pd, td;
        int r = sscanf(line,"%39[^,],%39[^,],%d,%d", sid, subid, &pd, &td);
        if (r>=4) {
            arr = realloc(arr, sizeof(Attendance)*(n+1));
            strncpy(arr[n].student_id,sid,ID_LEN);
            strncpy(arr[n].subject_id,subid,ID_LEN);
            arr[n].present_days = pd;
            arr[n].total_days = td;
            n++;
        }
    }
    fclose(f); *out = arr; return n;
}
void save_att(Attendance *arr, int n) {
    FILE *f = fopen(ATT_FILE,"w"); if (!f) { fprintf(stderr, COL_ERR "Error write attendance\n" COL_RESET); return; }
    for (int i=0;i<n;i++) {
        fprintf(f,"%s,%s,%d,%d\n", arr[i].student_id, arr[i].subject_id, arr[i].present_days, arr[i].total_days);
    }
    fclose(f);
}

/* ---------- Searching helpers ---------- */

User *find_user_by_email(User *users, int nu, const char *email) {
    for (int i=0;i<nu;i++) if (strcmp(users[i].email, email)==0) return &users[i];
    return NULL;
}
User *find_user_by_id(User *users, int nu, const char *id) {
    for (int i=0;i<nu;i++) if (strcmp(users[i].id, id)==0) return &users[i];
    return NULL;
}
Student *find_student_by_userid(Student *stu, int ns, const char *uid) {
    for (int i=0;i<ns;i++) if (strcmp(stu[i].user_id, uid)==0) return &stu[i];
    return NULL;
}
Student *find_student_by_id(Student *stu, int ns, const char *sid) {
    for (int i=0;i<ns;i++) if (strcmp(stu[i].id, sid)==0) return &stu[i];
    return NULL;
}
Subject *find_subject_by_code(Subject *sub, int ns, const char *code) {
    for (int i=0;i<ns;i++) if (strcmp(sub[i].code, code)==0) return &sub[i];
    return NULL;
}
Subject *find_subject_by_id(Subject *sub, int ns, const char *id) {
    for (int i=0;i<ns;i++) if (strcmp(sub[i].id, id)==0) return &sub[i];
    return NULL;
}

/* find mark index for student+subject or -1 */
int find_mark_index(Mark *marks, int nm, const char *student_id, const char *subject_id) {
    for (int i=0;i<nm;i++) if (strcmp(marks[i].student_id, student_id)==0 && strcmp(marks[i].subject_id, subject_id)==0) return i;
    return -1;
}

/* find attendance index */
int find_att_index(Attendance *arr, int n, const char *sid, const char *subid) {
    for (int i=0;i<n;i++) if (strcmp(arr[i].student_id, sid)==0 && strcmp(arr[i].subject_id, subid)==0) return i;
    return -1;
}

/* ---------- Console UI helpers ---------- */
void print_header() {
    printf("%s", COL_HEADER);
    printf(" %-72s "," STUDENT RECORD && RESULT MANAGEMENT SYSTEM ");
    printf("%s\n", COL_RESET);
    printf(" %sProgramming in C Semester ; Made by - Tanay Sah (590023170) - Mahika Jaglan (590025346)%s\n\n", COL_ACCENT, COL_RESET);
}

void print_boxed(const char *title) {
    printf("%s+----------------------------------------------------------------------+%s\n", COL_BOX, COL_RESET);
    printf("%s| %-68s |%s\n", COL_BOX, title, COL_RESET);
    printf("%s+----------------------------------------------------------------------+%s\n", COL_BOX, COL_RESET);
}

/* wait for Enter */
void wait_enter() {
    printf("\nPress Enter to continue...");
    getchar();
}

/* ---------- Core features ---------- */

/* Signup (student) */
void signup_flow() {
    User *users= NULL; int nu = load_users(&users);
    Student *students = NULL; int ns = load_students(&students);

    char name[NAME_LEN], email[EMAIL_LEN], phone[PHONE_LEN], pwd[128], roll[30], program[80];

    print_boxed("SIGN UP (Student)");
    printf("Name: "); sfgets(name, sizeof(name)); trim(name);
    printf("Email: "); sfgets(email, sizeof(email)); trim(email);
    if (find_user_by_email(users, nu, email) != NULL) {
        printf(COL_WARN "An account with this email already exists.\n" COL_RESET);
        free(users); free(students); return;
    }
    printf("Phone: "); sfgets(phone, sizeof(phone)); trim(phone);
    printf("Choose a password: "); sfgets(pwd, sizeof(pwd)); trim(pwd);
    printf("Roll number: "); sfgets(roll, sizeof(roll)); trim(roll);
    printf("Program (e.g., B.E. Software): "); sfgets(program, sizeof(program)); trim(program);

    User u; memset(&u,0,sizeof(u));
    gen_id(u.id, ID_LEN);
    strncpy(u.name, name, NAME_LEN);
    strncpy(u.email, email, EMAIL_LEN);
    strncpy(u.phone, phone, PHONE_LEN);
    strncpy(u.role, "student", ROLE_LEN);
    u.salt = (unsigned long)rand();
    u.pwd_hash = simple_hash(pwd, u.salt);

    users = realloc(users, sizeof(User)*(nu+1));
    users[nu] = u; nu++;
    save_users(users, nu);

    Student s; memset(&s,0,sizeof(s));
    gen_id(s.id, ID_LEN);
    strncpy(s.user_id, u.id, ID_LEN);
    strncpy(s.roll, roll, 30);
    strncpy(s.program, program, 80);

    students = realloc(students, sizeof(Student)*(ns+1));
    students[ns] = s; ns++;
    save_students(students, ns);

    printf(COL_OK "Signup successful. You can now login.\n" COL_RESET);
    free(users); free(students);
}

/* Simple login: returns pointer to logged-in user (caller must reload arrays) */
User *login_flow(User **users_ptr, int nu) {
    char email[EMAIL_LEN], pwd[128];
    print_boxed("LOGIN");
    printf("Email: "); sfgets(email, sizeof(email)); trim(email);
    printf("Password: "); sfgets(pwd, sizeof(pwd)); trim(pwd);
    User *u = find_user_by_email(*users_ptr, nu, email);
    if (!u) {
        printf(COL_ERR "No user found with this email.\n" COL_RESET);
        return NULL;
    }
    unsigned long test = simple_hash(pwd, u->salt);
    if (test != u->pwd_hash) {
        printf(COL_ERR "Incorrect password.\n" COL_RESET);
        return NULL;
    }
    printf(COL_OK "Login successful. Welcome %s.\n" COL_RESET, u->name);
    return u;
}

/* Admin: add subject */
void admin_add_subject() {
    Subject *arr = NULL; int n = load_subjects(&arr);
    char code[CODE_LEN], title[TITLE_LEN]; int credits, sem;
    print_boxed("Add Subject");
    printf("Subject code (e.g., CS101): "); sfgets(code,sizeof(code)); trim(code);
    if (find_subject_by_code(arr,n,code) != NULL) { printf(COL_WARN "Subject code exists already.\n" COL_RESET); free(arr); return; }
    printf("Title: "); sfgets(title,sizeof(title)); trim(title);
    printf("Credits (integer): "); scanf("%d%*c",&credits);
    printf("Semester (integer): "); scanf("%d%*c",&sem);
    Subject s; memset(&s,0,sizeof(s)); gen_id(s.id, ID_LEN);
    strncpy(s.code,code,CODE_LEN); strncpy(s.title,title,TITLE_LEN);
    s.credits = credits; s.semester = sem;
    arr = realloc(arr, sizeof(Subject)*(n+1)); arr[n]=s; n++;
    save_subjects(arr,n);
    printf(COL_OK "Subject added.\n" COL_RESET);
    free(arr);
}

/* Admin: list subjects sorted by semester (bifurcated) */
void admin_list_subjects() {
    Subject *arr=NULL; int n = load_subjects(&arr);
    if (n==0) { printf(COL_WARN "No subjects defined.\n" COL_RESET); free(arr); return; }
    /* find max semester */
    int maxs = 0;
    for (int i=0;i<n;i++) if (arr[i].semester>maxs) maxs=arr[i].semester;
    printf(COL_BOX "Subjects by Semester\n" COL_RESET);
    for (int s=1;s<=maxs;s++) {
        printf("%s\n Semester %d:\n", COL_ACCENT, s);
        for (int i=0;i<n;i++) if (arr[i].semester==s) {
            printf("  [%s] %s  - %s  (Credits:%d)\n", arr[i].id, arr[i].code, arr[i].title, arr[i].credits);
        }
        printf("%s\n", COL_RESET);
    }
    free(arr);
}

/* Admin: enter/update marks for a student+subject */
void admin_enter_marks() {
    User *users=NULL; int nu=load_users(&users);
    Student *students=NULL; int ns=load_students(&students);
    Subject *subjects=NULL; int nsub=load_subjects(&subjects);
    Mark *marks=NULL; int nm=load_marks(&marks);

    /* choose student */
    printf("Enter student roll or user email: ");
    char key[120]; sfgets(key,sizeof(key)); trim(key);
    Student *st = NULL;
    User *target_user = find_user_by_email(users, nu, key);
    if (target_user) st = find_student_by_userid(students, ns, target_user->id);
    if (!st) {
        /* search by roll */
        for (int i=0;i<ns;i++) if (strcmp(students[i].roll, key)==0) { st=&students[i]; break; }
    }
    if (!st) { printf(COL_ERR "Student not found.\n" COL_RESET); free(users); free(students); free(subjects); free(marks); return; }

    /* list subjects */
    printf("Subjects available:\n");
    for (int i=0;i<nsub;i++) {
        printf(" [%d] %s - %s (Sem %d, Credits %d)\n", i+1, subjects[i].code, subjects[i].title, subjects[i].semester, subjects[i].credits);
    }
    printf("Choose subject number: "); int idx; scanf("%d%*c",&idx);
    if (idx < 1 || idx > nsub) { printf(COL_ERR "Invalid subject choice.\n" COL_RESET); free(users); free(students); free(subjects); free(marks); return; }
    Subject *sub = &subjects[idx-1];

    /* check existing */
    int mindex = find_mark_index(marks, nm, st->id, sub->id);
    if (mindex >= 0) {
        printf(COL_WARN "Marks for this student & subject already exist: %.2f\n" COL_RESET, marks[mindex].marks);
        printf("Do you want to (u)pdate or (c)ancel? [u/c]: ");
        char c[4]; sfgets(c, sizeof(c)); trim(c);
        if (c[0]=='u' || c[0]=='U') {
            printf("Enter new marks (0-100): "); double newm; scanf("%lf%*c",&newm);
            marks[mindex].marks = newm;
            save_marks(marks, nm);
            printf(COL_OK "Marks updated.\n" COL_RESET);
        } else {
            printf("Cancelled.\n");
        }
    } else {
        printf("Enter marks (0-100): "); double m; scanf("%lf%*c",&m);
        /* append */
        marks = realloc(marks, sizeof(Mark)*(nm+1));
        strncpy(marks[nm].student_id, st->id, ID_LEN);
        strncpy(marks[nm].subject_id, sub->id, ID_LEN);
        marks[nm].marks = m; nm++;
        save_marks(marks, nm);
        printf(COL_OK "Marks saved.\n" COL_RESET);
    }

    free(users); free(students); free(subjects); free(marks);
}

/* Admin: enter/update attendance */
void admin_enter_attendance() {
    User *users=NULL; int nu=load_users(&users);
    Student *students=NULL; int ns=load_students(&students);
    Subject *subjects=NULL; int nsub=load_subjects(&subjects);
    Attendance *arr=NULL; int na=load_att(&arr);

    printf("Enter student email or roll: ");
    char key[120]; sfgets(key,sizeof(key)); trim(key);
    Student *st=NULL; User *u = find_user_by_email(users, nu, key);
    if (u) st = find_student_by_userid(students, ns, u->id);
    if (!st) {
        for (int i=0;i<ns;i++) if (strcmp(students[i].roll, key)==0) { st=&students[i]; break; }
    }
    if (!st) { printf(COL_ERR "Student not found.\n" COL_RESET); free(users); free(students); free(subjects); free(arr); return; }
    printf("Subjects:\n"); for (int i=0;i<nsub;i++) printf(" [%d] %s - %s\n", i+1, subjects[i].code, subjects[i].title);
    printf("Choose subject number: "); int idx; scanf("%d%*c",&idx);
    if (idx<1 || idx>nsub) { printf(COL_ERR "Invalid.\n" COL_RESET); free(users); free(students); free(subjects); free(arr); return; }
    Subject *sub = &subjects[idx-1];

    int aidx = find_att_index(arr, na, st->id, sub->id);
    if (aidx>=0) {
        printf(COL_WARN "Existing attendance: %d/%d\n" COL_RESET, arr[aidx].present_days, arr[aidx].total_days);
        printf("Update or cancel? (u/c): "); char c[4]; sfgets(c,sizeof(c)); trim(c);
        if (c[0]=='u' || c[0]=='U') {
            printf("Enter present days: "); int pd, td; scanf("%d%*c",&pd);
            printf("Enter total days: "); scanf("%d%*c",&td);
            arr[aidx].present_days = pd; arr[aidx].total_days = td;
            save_att(arr, na);
            printf(COL_OK "Attendance updated.\n" COL_RESET);
        } else printf("Cancelled.\n");
    } else {
        int pd, td;
        printf("Enter present days: "); scanf("%d%*c",&pd);
        printf("Enter total days: "); scanf("%d%*c",&td);
        arr = realloc(arr, sizeof(Attendance)*(na+1));
        strncpy(arr[na].student_id, st->id, ID_LEN);
        strncpy(arr[na].subject_id, sub->id, ID_LEN);
        arr[na].present_days = pd; arr[na].total_days = td; na++;
        save_att(arr, na);
        printf(COL_OK "Attendance saved.\n" COL_RESET);
    }

    free(users); free(students); free(subjects); free(arr);
}

/* Student view: display dashboard with subjects grouped by semester and compute SGPA/CGPA */
void student_dashboard(User *user) {
    if (!user) return;
    Student *students=NULL; int ns = load_students(&students);
    Student *stu = find_student_by_userid(students, ns, user->id);
    if (!stu) { printf(COL_ERR "Student profile not found.\n" COL_RESET); free(students); return; }

    Subject *subs=NULL; int nsub = load_subjects(&subs);
    Mark *marks=NULL; int nmarks = load_marks(&marks);
    Attendance *att=NULL; int natt = load_att(&att);

    /* header */
    print_boxed("STUDENT DASHBOARD");
    printf("Name: %s\n", user->name);
    printf("Email: %s\n", user->email);
    printf("Phone: %s\n", user->phone);
    printf("Roll: %s\n", stu->roll);
    printf("Program: %s\n\n", stu->program);

    /* Group subjects by semester */
    int maxsem = 0;
    for (int i=0;i<nsub;i++) if (subs[i].semester > maxsem) maxsem = subs[i].semester;
    double total_weighted_gradepoints = 0.0;
    int total_credits = 0;
    for (int sem=1; sem<=maxsem; sem++) {
        int sem_credits = 0;
        double sem_weighted_gp = 0.0;
        printf("%s---- Semester %d ----%s\n", COL_ACCENT, sem, COL_RESET);
        for (int i=0;i<nsub;i++) {
            if (subs[i].semester != sem) continue;
            /* find mark */
            int midx = find_mark_index(marks, nmarks, stu->id, subs[i].id);
            double markval = (midx>=0)? marks[midx].marks : -1.0;
            int aidx = find_att_index(att, natt, stu->id, subs[i].id);
            double attperc = -1.0;
            if (aidx>=0 && att[aidx].total_days>0) attperc = (double)att[aidx].present_days*100.0/att[aidx].total_days;
            printf(" %s (%s) - %s  Credits:%d  Marks:%s  Attendance:%s\n",
                subs[i].code, subs[i].id, subs[i].title, subs[i].credits,
                (markval>=0)?({ char tmp[32]; snprintf(tmp, sizeof(tmp),"%.2f", markval); tmp; }) : "N/A",
                (attperc>=0)?({ char tmp2[32]; snprintf(tmp2,sizeof(tmp2),"%.1f%%", attperc); tmp2; }) : "N/A");
            /* accumulate for SGPA */
            if (markval >= 0) {
                double gp = (markval/100.0)*10.0; /* linear conversion to 10-point scale */
                sem_weighted_gp += gp * subs[i].credits;
                sem_credits += subs[i].credits;
            }
        }
        /* compute SGPA for sem if credits>0 */
        if (sem_credits > 0) {
            double sgpa = sem_weighted_gp / sem_credits;
            printf("   %sSGPA (Semester %d): %.3f%s\n", COL_OK, sem, sgpa, COL_RESET);
            total_weighted_gradepoints += sem_weighted_gp;
            total_credits += sem_credits;
        } else {
            printf("   %sSGPA (Semester %d): N/A (no graded subjects)%s\n", COL_WARN, sem, COL_RESET);
        }
        printf("\n");
    }

    /* compute CGPA if any credits */
    if (total_credits > 0) {
        double cgpa = total_weighted_gradepoints / total_credits;
        printf("%s>>> Cumulative CGPA: %.3f (Weighted by credits)%s\n", COL_BOX, cgpa, COL_RESET);
    } else {
        printf("%s>>> CGPA: N/A (no graded credits yet)%s\n", COL_WARN, COL_RESET);
    }

    free(students); free(subs); free(marks); free(att);
}

/* Admin dashboard loop */
void admin_menu() {
    while (1) {
        print_boxed("ADMIN DASHBOARD");
        printf("[1] Add Subject\n[2] List Subjects (by semester)\n[3] Enter/Update Marks\n[4] Enter/Update Attendance\n[5] Back to Main\nChoose: ");
        int c; scanf("%d%*c",&c);
        if (c==1) admin_add_subject();
        else if (c==2) admin_list_subjects();
        else if (c==3) admin_enter_marks();
        else if (c==4) admin_enter_attendance();
        else if (c==5) break;
        else printf(COL_WARN "Invalid choice\n" COL_RESET);
        wait_enter();
    }
}

/* Student menu */
void student_menu(User *user) {
    while (1) {
        print_boxed("STUDENT MENU");
        printf("[1] View Dashboard\n[2] View Subjects (by semester)\n[3] Logout\nChoose: ");
        int c; scanf("%d%*c",&c);
        if (c==1) { student_dashboard(user); }
        else if (c==2) {
            Subject *arr=NULL; int n=load_subjects(&arr);
            if (n==0) printf(COL_WARN "No subjects.\n" COL_RESET);
            else {
                int maxs=0; for (int i=0;i<n;i++) if (arr[i].semester>maxs) maxs=arr[i].semester;
                for (int s=1;s<=maxs;s++) {
                    printf("%sSemester %d:%s\n", COL_ACCENT, s, COL_RESET);
                    for (int i=0;i<n;i++) if (arr[i].semester==s) printf("  %s - %s (Credits %d)\n", arr[i].code, arr[i].title, arr[i].credits);
                }
            }
            free(arr);
        }
        else if (c==3) break;
        else printf(COL_WARN "Invalid choice\n" COL_RESET);
        wait_enter();
    }
}

/* ---------- Bootstrap: ensure at least one admin exists ---------- */
void ensure_admin_exists() {
    User *users=NULL; int nu = load_users(&users);
    int found = 0;
    for (int i=0;i<nu;i++) if (strcmp(users[i].role,"admin")==0) { found=1; break; }
    if (!found) {
        printf(COL_WARN "No admin account found. Creating default admin (admin@school.edu / admin123)\n" COL_RESET);
        User u; memset(&u,0,sizeof(u));
        gen_id(u.id, ID_LEN);
        strncpy(u.name, "Administrator", NAME_LEN);
        strncpy(u.email, "admin@school.edu", EMAIL_LEN);
        strncpy(u.phone, "0000000000", PHONE_LEN);
        strncpy(u.role, "admin", ROLE_LEN);
        strncpy(u.name, "Administrator", NAME_LEN);
        u.salt = (unsigned long)rand();
        u.pwd_hash = simple_hash("admin123", u.salt);
        users = realloc(users, sizeof(User)*(nu+1));
        users[nu] = u; nu++;
        save_users(users, nu);
        printf(COL_OK "Default admin created. Please login and change password.\n" COL_RESET);
    }
    free(users);
}

/* ---------- Main menu ---------- */
int main_menu() {
    ensure_data_dir();
    srand((unsigned int)time(NULL));
    ensure_admin_exists();
    while (1) {
        system(NULL); /* no-op sometimes needed for windows */
        print_header();
        print_boxed("MAIN MENU");
        printf("[1] Login\n[2] Signup (Student)\n[3] Exit\nChoose: ");
        int choice; scanf("%d%*c",&choice);
        if (choice == 1) {
            User *users=NULL; int nu = load_users(&users);
            User *logged = login_flow(&users, nu);
            if (logged) {
                if (strcmp(logged->role,"admin")==0) {
                    admin_menu();
                } else {
                    student_menu(logged);
                }
            }
            free(users);
        } else if (choice == 2) {
            signup_flow();
        } else if (choice == 3) {
            printf("Goodbye!\n"); return 0;
        } else {
            printf(COL_WARN "Invalid choice\n" COL_RESET);
        }
    }
    return 0;
}
