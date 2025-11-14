/* student_system_mem.c
   Student Record & Result Management System (IN-MEMORY, ncurses UI)
   - All data lives only in memory (no file I/O)
   - Admin & Student login/signup
   - Subjects grouped by semester
   - Marks & Attendance entry (prevent duplicates; allow update)
   - SGPA and CGPA calculation (credit-weighted)
   - ncurses-based UI with colors, boxed panels, and simple transitions
   Author: Adapted for Tanay Sah requirements (ephemeral memory-only version)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ncurses.h>
#include <unistd.h>
#include <ctype.h>

#define MAX_USERS 256
#define MAX_STUDENTS 256
#define MAX_SUBJECTS 512
#define MAX_MARKS 4096
#define MAX_ATT 4096

#define NAME_LEN 80
#define EMAIL_LEN 80
#define PHONE_LEN 24
#define ROLE_LEN 12
#define CODE_LEN 16
#define TITLE_LEN 100
#define ID_LEN 32

/* Colors */
enum {
    C_MAIN = 1,
    C_HEADER,
    C_BOX,
    C_ACCENT,
    C_TEXT,
    C_WARN,
    C_GOOD,
    C_BG
};

/* Data structures (in-memory) */
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
    char user_id[ID_LEN]; /* links to User.id */
    char roll[32];
    char program[64];
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
    double marks; /* 0-100 */
} Mark;

typedef struct {
    char student_id[ID_LEN];
    char subject_id[ID_LEN];
    int present_days;
    int total_days;
} Attendance;

/* Global in-memory stores */
static User users[MAX_USERS];
static int users_n = 0;
static Student students[MAX_STUDENTS];
static int students_n = 0;
static Subject subjects[MAX_SUBJECTS];
static int subjects_n = 0;
static Mark marks[MAX_MARKS];
static int marks_n = 0;
static Attendance atts[MAX_ATT];
static int atts_n = 0;

/* Utility */
void gen_id(char *out, size_t n) {
    static unsigned long ctr = 0;
    ctr++;
    unsigned long t = (unsigned long)time(NULL) ^ (ctr<<8) ^ (rand() & 0xffff);
    snprintf(out, n, "id%08lx%04lx", t, (unsigned long)(rand() & 0xffff));
}

/* Simple salted hash (djb2 variant) - okay for ephemeral in-memory */
unsigned long simple_hash(const char *s, unsigned long salt) {
    unsigned long h = 5381u + salt;
    const unsigned char *p = (const unsigned char*)s;
    while (*p) {
        h = ((h << 5) + h) + *p;
        p++;
    }
    h ^= (salt << 7) | (salt >> 3);
    return h;
}

void trim_inplace(char *s) {
    if (!s) return;
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p)+1);
    int i = (int)strlen(s)-1;
    while (i>=0 && isspace((unsigned char)s[i])) s[i--] = 0;
}

/* Find helpers */
User* find_user_by_email(const char *email) {
    for (int i=0;i<users_n;i++) if (strcmp(users[i].email, email)==0) return &users[i];
    return NULL;
}
User* find_user_by_id(const char *id) {
    for (int i=0;i<users_n;i++) if (strcmp(users[i].id, id)==0) return &users[i];
    return NULL;
}
Student* find_student_by_userid(const char *uid) {
    for (int i=0;i<students_n;i++) if (strcmp(students[i].user_id, uid)==0) return &students[i];
    return NULL;
}
Student* find_student_by_roll(const char *roll) {
    for (int i=0;i<students_n;i++) if (strcmp(students[i].roll, roll)==0) return &students[i];
    return NULL;
}
Subject* find_subject_by_id(const char *id) {
    for (int i=0;i<subjects_n;i++) if (strcmp(subjects[i].id, id)==0) return &subjects[i];
    return NULL;
}
int find_mark_index(const char *student_id, const char *subject_id) {
    for (int i=0;i<marks_n;i++) if (strcmp(marks[i].student_id, student_id)==0 && strcmp(marks[i].subject_id, subject_id)==0) return i;
    return -1;
}
int find_att_index(const char *student_id, const char *subject_id) {
    for (int i=0;i<atts_n;i++) if (strcmp(atts[i].student_id, student_id)==0 && strcmp(atts[i].subject_id, subject_id)==0) return i;
    return -1;
}

/* ncurses UI helpers */
void center_text(WINDOW *w, int y, const char *s, chtype attr) {
    int wdt = getmaxx(w);
    int len = (int)strlen(s);
    int x = (wdt - len) / 2;
    wattron(w, attr);
    mvwprintw(w, y, x, "%s", s);
    wattroff(w, attr);
}

void draw_header(WINDOW *w) {
    wattron(w, COLOR_PAIR(C_HEADER));
    int wdt = getmaxx(w);
    mvwhline(w, 0, 0, ' ', wdt);
    mvwprintw(w, 0, 2, " STUDENT RECORD && RESULT MANAGEMENT SYSTEM ");
    wattroff(w, COLOR_PAIR(C_HEADER));
    wattron(w, COLOR_PAIR(C_ACCENT));
    mvwprintw(w, 1, 2, "Programming in C Semester ; Made by - Tanay Sah (590023170) - Mahika Jaglan (590025346)");
    wattroff(w, COLOR_PAIR(C_ACCENT));
}

void texture_background(WINDOW *w) {
    int h = getmaxy(w), wdt = getmaxx(w);
    for (int y=3; y<h; y++) {
        for (int x=0; x<wdt; x+=2) {
            mvwaddch(w, y, x, ACS_CKBOARD | COLOR_PAIR(C_BG));
        }
    }
}

/* Simple slide transition: slide a window from right to center */
void slide_in(WINDOW *src) {
    int scrh, scrw; getmaxyx(stdscr, scrh, scrw);
    int h = getmaxy(src), w = getmaxx(src);
    int steps = 12;
    for (int s=0; s<=steps; s++) {
        int x = scrw - (scrw-w) * s / steps - w;
        werase(stdscr);
        draw_header(stdscr);
        texture_background(stdscr);
        mvwin(src, 4, x);
        wnoutrefresh(stdscr);
        wnoutrefresh(src);
        doupdate();
        usleep(16000);
    }
}

/* Fade out / in simple */
void fade_pause() {
    usleep(120000);
}

/* Initialize some default admin */
void create_default_admin_if_none() {
    int found = 0;
    for (int i=0;i<users_n;i++) if (strcmp(users[i].role, "admin")==0) { found=1; break; }
    if (!found && users_n < MAX_USERS - 1) {
        User u; memset(&u,0,sizeof(u));
        gen_id(u.id, ID_LEN);
        strncpy(u.name, "Administrator", NAME_LEN);
        strncpy(u.email, "admin@local", EMAIL_LEN);
        strncpy(u.phone, "0000000000", PHONE_LEN);
        strncpy(u.role, "admin", ROLE_LEN);
        u.salt = (unsigned long)rand();
        u.pwd_hash = simple_hash("admin123", u.salt);
        users[users_n++] = u;
    }
}

/* Input helpers (simple, safe) */
void input_text(WINDOW *w, int y, int x, char *buf, int bufsize, const char *hint) {
    echo();
    curs_set(1);
    mvwprintw(w, y, x, "%s", hint);
    wmove(w, y, x + (int)strlen(hint));
    wgetnstr(w, buf, bufsize-1);
    noecho();
    curs_set(0);
    trim_inplace(buf);
}

/* Flow: Signup (student) */
void flow_signup() {
    WINDOW *win = newwin(14, 70, 6, (COLS-70)/2);
    wbkgd(win, COLOR_PAIR(C_BOX));
    box(win, 0, 0);
    mvwprintw(win,1,2," SIGNUP (Student) ");
    char name[NAME_LEN] = {0}, email[EMAIL_LEN] = {0}, phone[PHONE_LEN] = {0}, pwd[64] = {0}, roll[32] = {0}, program[64] = {0};
    input_text(win, 3, 4, name, NAME_LEN, "Full Name: ");
    input_text(win, 4, 4, email, EMAIL_LEN, "Email    : ");
    if (find_user_by_email(email)) {
        mvwprintw(win, 10, 4, "An account with this email already exists. Press any key.");
        wrefresh(win); wgetch(win); delwin(win); return;
    }
    input_text(win, 5, 4, phone, PHONE_LEN, "Phone    : ");
    input_text(win, 6, 4, pwd, 64, "Password : ");
    input_text(win, 7, 4, roll, 32, "Roll No  : ");
    input_text(win, 8, 4, program, 64, "Program  : ");
    /* create user and student */
    if (users_n >= MAX_USERS || students_n >= MAX_STUDENTS) {
        mvwprintw(win, 11, 4, "Limit reached - cannot create more users. Press any key.");
        wrefresh(win); wgetch(win); delwin(win); return;
    }
    User u; memset(&u,0,sizeof(u)); gen_id(u.id, ID_LEN);
    strncpy(u.name, name, NAME_LEN-1);
    strncpy(u.email, email, EMAIL_LEN-1);
    strncpy(u.phone, phone, PHONE_LEN-1);
    strncpy(u.role, "student", ROLE_LEN-1);
    u.salt = (unsigned long)rand();
    u.pwd_hash = simple_hash(pwd, u.salt);
    users[users_n++] = u;
    Student s; memset(&s,0,sizeof(s)); gen_id(s.id, ID_LEN);
    strncpy(s.user_id, u.id, ID_LEN-1);
    strncpy(s.roll, roll, 31);
    strncpy(s.program, program, 63);
    students[students_n++] = s;
    mvwprintw(win, 11, 4, "Signup successful. Press any key to continue.");
    wrefresh(win); wgetch(win); delwin(win);
}

/* Flow: Login (returns pointer to user or NULL). Role-based navigation handled by caller */
User* flow_login() {
    WINDOW *win = newwin(10, 64, 7, (COLS-64)/2);
    wbkgd(win, COLOR_PAIR(C_BOX));
    box(win,0,0);
    mvwprintw(win,1,2," LOGIN ");
    char email[EMAIL_LEN] = {0}, pwd[64] = {0};
    input_text(win, 3, 4, email, EMAIL_LEN, "Email   : ");
    input_text(win, 4, 4, pwd, 64, "Password: ");
    User *u = find_user_by_email(email);
    if (!u) {
        mvwprintw(win, 7, 4, "User not found. Press any key.");
        wrefresh(win); wgetch(win); delwin(win); return NULL;
    }
    unsigned long test = simple_hash(pwd, u->salt);
    if (test != u->pwd_hash) {
        mvwprintw(win, 7, 4, "Incorrect password. Press any key.");
        wrefresh(win); wgetch(win); delwin(win); return NULL;
    }
    mvwprintw(win, 7, 4, "Login successful. Press any key.");
    wrefresh(win); wgetch(win); delwin(win);
    return u;
}

/* Admin: Add subject */
void admin_add_subject() {
    WINDOW *win = newwin(12, 70, 6, (COLS-70)/2);
    wbkgd(win, COLOR_PAIR(C_BOX));
    box(win,0,0);
    mvwprintw(win,1,2," Add Subject ");
    char code[CODE_LEN]={0}, title[TITLE_LEN]={0};
    char credits_s[16]={0}, sem_s[16]={0};
    input_text(win, 3, 4, code, CODE_LEN, "Code (CS101): ");
    input_text(win, 4, 4, title, TITLE_LEN, "Title        : ");
    input_text(win, 5, 4, credits_s, 16, "Credits (int): ");
    input_text(win, 6, 4, sem_s, 16, "Semester (int): ");
    int credits = atoi(credits_s);
    int sem = atoi(sem_s);
    if (credits <= 0 || sem <= 0) {
        mvwprintw(win, 9, 4, "Invalid credits/semester. Press any key.");
        wrefresh(win); wgetch(win); delwin(win); return;
    }
    if (subjects_n >= MAX_SUBJECTS) {
        mvwprintw(win,9,4,"Subject limit reached. Press any key."); wrefresh(win); wgetch(win); delwin(win); return;
    }
    Subject s; memset(&s,0,sizeof(s)); gen_id(s.id, ID_LEN);
    strncpy(s.code, code, CODE_LEN-1);
    strncpy(s.title, title, TITLE_LEN-1);
    s.credits = credits;
    s.semester = sem;
    subjects[subjects_n++] = s;
    mvwprintw(win,9,4,"Subject added. Press any key.");
    wrefresh(win); wgetch(win); delwin(win);
}

/* Admin: list subjects grouped by semester (padded) */
void admin_list_subjects() {
    int maxs = 0;
    for (int i=0;i<subjects_n;i++) if (subjects[i].semester > maxs) maxs = subjects[i].semester;
    WINDOW *win = newwin(8 + maxs*3, 80, 5, (COLS-80)/2);
    if (!win) return;
    wbkgd(win, COLOR_PAIR(C_BOX));
    box(win,0,0);
    mvwprintw(win,1,2," Subjects (by Semester) ");
    int y = 3;
    for (int s=1;s<=maxs;s++) {
        wattron(win, COLOR_PAIR(C_ACCENT));
        mvwprintw(win, y++, 4, "Semester %d:", s);
        wattroff(win, COLOR_PAIR(C_ACCENT));
        for (int i=0;i<subjects_n;i++) {
            if (subjects[i].semester != s) continue;
            mvwprintw(win, y++, 6, "%s | %s (%d credits) [id:%s]", subjects[i].code, subjects[i].title, subjects[i].credits, subjects[i].id);
        }
        y++;
    }
    mvwprintw(win, y+1, 4, "Press any key.");
    wrefresh(win); wgetch(win); delwin(win);
}

/* Admin: enter/update marks */
void admin_enter_marks() {
    if (students_n == 0 || subjects_n == 0) {
        WINDOW *w = newwin(6, 60, 8, (COLS-60)/2);
        wbkgd(w, COLOR_PAIR(C_BOX));
        box(w,0,0);
        mvwprintw(w,2,2,"Need at least one student and one subject. Press any key.");
        wrefresh(w); wgetch(w); delwin(w); return;
    }
    WINDOW *win = newwin(14, 80, 5, (COLS-80)/2);
    wbkgd(win, COLOR_PAIR(C_BOX));
    box(win,0,0);
    mvwprintw(win,1,2," Enter / Update Marks ");
    char key[96]={0};
    input_text(win,3,4,key,96,"Enter student roll or email: ");
    Student *st = NULL;
    User *u = find_user_by_email(key);
    if (u) st = find_student_by_userid(u->id);
    if (!st) st = find_student_by_roll(key);
    if (!st) { mvwprintw(win,11,4,"Student not found. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
    /* list subjects (small) */
    int y=5;
    for (int i=0;i<subjects_n && i<8;i++) {
        mvwprintw(win,y++,4,"[%d] %s - %s (Sem %d, %d cr) id:%s", i+1, subjects[i].code, subjects[i].title, subjects[i].semester, subjects[i].credits, subjects[i].id);
    }
    char idxs[8]={0};
    input_text(win,y+1,4,idxs,8,"Choose subject index (from above): ");
    int idx = atoi(idxs);
    if (idx < 1 || idx > subjects_n) { mvwprintw(win,11,4,"Invalid choice. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
    Subject *sub = &subjects[idx-1];
    int midx = find_mark_index(st->id, sub->id);
    if (midx >= 0) {
        mvwprintw(win,11,4,"Existing marks: %.2f. Update? (y/n): ", marks[midx].marks);
        wrefresh(win);
        int ch = wgetch(win);
        if (ch=='y' || ch=='Y') {
            char ms[16]={0}; input_text(win,12,4,ms,16,"New marks (0-100): ");
            double m = atof(ms);
            if (m < 0 || m > 100) { mvwprintw(win,13,4,"Invalid marks. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
            marks[midx].marks = m;
            mvwprintw(win,13,4,"Marks updated. Press any key."); wrefresh(win); wgetch(win); delwin(win); return;
        } else { mvwprintw(win,13,4,"Cancelled. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
    } else {
        char ms[16]={0}; input_text(win,11,4,ms,16,"Marks (0-100): ");
        double m = atof(ms);
        if (m < 0 || m > 100) { mvwprintw(win,12,4,"Invalid marks. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
        if (marks_n >= MAX_MARKS) { mvwprintw(win,12,4,"Marks storage full. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
        Mark mk; memset(&mk,0,sizeof(mk));
        strncpy(mk.student_id, st->id, ID_LEN-1);
        strncpy(mk.subject_id, sub->id, ID_LEN-1);
        mk.marks = m;
        marks[marks_n++] = mk;
        mvwprintw(win,12,4,"Marks saved. Press any key.");
        wrefresh(win); wgetch(win); delwin(win); return;
    }
}

/* Admin: enter/update attendance */
void admin_enter_attendance() {
    if (students_n==0 || subjects_n==0) {
        WINDOW *w = newwin(6,60,8,(COLS-60)/2);
        wbkgd(w,COLOR_PAIR(C_BOX)); box(w,0,0); mvwprintw(w,2,2,"Need at least one student and one subject. Press any key.");
        wrefresh(w); wgetch(w); delwin(w); return;
    }
    WINDOW *win = newwin(14,80,5,(COLS-80)/2);
    wbkgd(win, COLOR_PAIR(C_BOX)); box(win,0,0); mvwprintw(win,1,2," Enter / Update Attendance ");
    char key[96]={0}; input_text(win,3,4,key,96,"Enter student roll or email: ");
    Student *st = NULL; User *u = find_user_by_email(key);
    if (u) st = find_student_by_userid(u->id);
    if (!st) st = find_student_by_roll(key);
    if (!st) { mvwprintw(win,11,4,"Student not found. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
    int y=5;
    for (int i=0;i<subjects_n && i<8;i++) mvwprintw(win,y++,4,"[%d] %s - %s (Sem %d) id:%s", i+1, subjects[i].code, subjects[i].title, subjects[i].semester, subjects[i].id);
    char idxs[8]={0}; input_text(win,y+1,4,idxs,8,"Choose subject index: ");
    int idx = atoi(idxs);
    if (idx < 1 || idx > subjects_n) { mvwprintw(win,11,4,"Invalid choice. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
    Subject *sub = &subjects[idx-1];
    int aidx = find_att_index(st->id, sub->id);
    if (aidx >= 0) {
        mvwprintw(win,11,4,"Existing attendance: %d/%d. Update? (y/n): ", atts[aidx].present_days, atts[aidx].total_days);
        wrefresh(win);
        int ch = wgetch(win);
        if (ch=='y' || ch=='Y') {
            char pd_s[16]={0}, td_s[16]={0}; input_text(win,12,4,pd_s,16,"Present days: "); input_text(win,13,4,td_s,16,"Total days: ");
            int pd = atoi(pd_s), td = atoi(td_s);
            if (pd < 0 || td <= 0 || pd > td) { mvwprintw(win,13,4,"Invalid values. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
            atts[aidx].present_days = pd; atts[aidx].total_days = td;
            mvwprintw(win,13,4,"Attendance updated. Press any key."); wrefresh(win); wgetch(win); delwin(win); return;
        } else { mvwprintw(win,13,4,"Cancelled. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
    } else {
        char pd_s[16]={0}, td_s[16]={0}; input_text(win,11,4,pd_s,16,"Present days: "); input_text(win,12,4,td_s,16,"Total days: ");
        int pd = atoi(pd_s), td = atoi(td_s);
        if (pd < 0 || td <= 0 || pd > td) { mvwprintw(win,13,4,"Invalid values. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
        if (atts_n >= MAX_ATT) { mvwprintw(win,13,4,"Attendance storage full. Press any key."); wrefresh(win); wgetch(win); delwin(win); return; }
        Attendance a; memset(&a,0,sizeof(a));
        strncpy(a.student_id, st->id, ID_LEN-1);
        strncpy(a.subject_id, sub->id, ID_LEN-1);
        a.present_days = pd; a.total_days = td;
        atts[atts_n++] = a;
        mvwprintw(win,13,4,"Attendance saved. Press any key."); wrefresh(win); wgetch(win); delwin(win); return;
    }
}

/* Student dashboard: show profile, subjects by semester, marks, attendance, SGPA/CGPA */
void student_dashboard(User *user) {
    if (!user) return;
    Student *st = find_student_by_userid(user->id);
    if (!st) {
        WINDOW *w = newwin(6,60,8,(COLS-60)/2); wbkgd(w,COLOR_PAIR(C_BOX)); box(w,0,0);
        mvwprintw(w,2,2,"Student profile not found. Press any key."); wrefresh(w); wgetch(w); delwin(w); return;
    }
    int maxsem = 0; for (int i=0;i<subjects_n;i++) if (subjects[i].semester > maxsem) maxsem = subjects[i].semester;
    int winh = 12 + maxsem*4; if (winh < 16) winh = 16;
    WINDOW *win = newwin(winh, COLS-8, 4, 4);
    wbkgd(win, COLOR_PAIR(C_BOX)); box(win,0,0);
    mvwprintw(win,1,2," STUDENT DASHBOARD ");
    mvwprintw(win,2,4,"Name : %s", user->name);
    mvwprintw(win,3,4,"Email: %s", user->email);
    mvwprintw(win,4,4,"Phone: %s", user->phone);
    mvwprintw(win,5,4,"Roll : %s", st->roll);
    mvwprintw(win,6,4,"Program: %s", st->program);
    int y = 8;
    double total_weighted = 0.0; int total_credits = 0;
    for (int sem=1; sem<=maxsem; sem++) {
        wattron(win, COLOR_PAIR(C_ACCENT));
        mvwprintw(win,y++,4,"-- Semester %d --", sem);
        wattroff(win, COLOR_PAIR(C_ACCENT));
        double sem_weighted = 0.0; int sem_credits = 0;
        for (int i=0;i<subjects_n;i++) {
            if (subjects[i].semester != sem) continue;
            int midx = find_mark_index(st->id, subjects[i].id);
            double markv = (midx>=0) ? marks[midx].marks : -1.0;
            int aidx = find_att_index(st->id, subjects[i].id);
            double attp = -1.0;
            if (aidx>=0 && atts[aidx].total_days > 0) attp = (double)atts[aidx].present_days * 100.0 / atts[aidx].total_days;
            char markstr[32]; if (markv<0) strcpy(markstr, "N/A"); else snprintf(markstr, sizeof(markstr), "%.2f", markv);
            char attstr[32]; if (attp<0) strcpy(attstr, "N/A"); else snprintf(attstr, sizeof(attstr), "%.1f%%", attp);
            mvwprintw(win,y++,6,"%s (%s) - %s | Credits:%d | Marks:%s | Att:%s", subjects[i].code, subjects[i].id, subjects[i].title, subjects[i].credits, markstr, attstr);
            if (markv >= 0) {
                double gp = (markv / 100.0) * 10.0;
                sem_weighted += gp * subjects[i].credits;
                sem_credits += subjects[i].credits;
            }
        }
        if (sem_credits > 0) {
            double sgpa = sem_weighted / sem_credits;
            wattron(win, COLOR_PAIR(C_GOOD));
            mvwprintw(win,y++,6,"SGPA (Sem %d): %.3f", sem, sgpa);
            wattroff(win, COLOR_PAIR(C_GOOD));
            total_weighted += sem_weighted;
            total_credits += sem_credits;
        } else {
            wattron(win, COLOR_PAIR(C_WARN));
            mvwprintw(win,y++,6,"SGPA (Sem %d): N/A (no graded subjects)", sem);
            wattroff(win, COLOR_PAIR(C_WARN));
        }
        y++;
    }
    if (total_credits > 0) {
        double cgpa = total_weighted / total_credits;
        wattron(win, COLOR_PAIR(C_MAIN));
        mvwprintw(win,y++,4,">>> Cumulative CGPA: %.3f", cgpa);
        wattroff(win, COLOR_PAIR(C_MAIN));
    } else {
        wattron(win, COLOR_PAIR(C_WARN));
        mvwprintw(win,y++,4,">>> CGPA: N/A (no graded credits yet)");
        wattroff(win, COLOR_PAIR(C_WARN));
    }
    mvwprintw(win,y+1,4,"Press any key to return.");
    wrefresh(win); wgetch(win); delwin(win);
}

/* Admin dashboard loop */
void admin_dashboard(User *admin) {
    int running = 1;
    while (running) {
        WINDOW *win = newwin(14, 70, 6, (COLS-70)/2);
        wbkgd(win, COLOR_PAIR(C_BOX)); box(win,0,0);
        mvwprintw(win,1,2," ADMIN DASHBOARD ");
        mvwprintw(win,3,4,"1) Add Subject");
        mvwprintw(win,4,4,"2) List Subjects (by semester)");
        mvwprintw(win,5,4,"3) Enter/Update Marks");
        mvwprintw(win,6,4,"4) Enter/Update Attendance");
        mvwprintw(win,7,4,"5) Logout");
        mvwprintw(win,9,4,"Choose option: ");
        wrefresh(win);
        int ch = wgetch(win);
        delwin(win);
        switch(ch) {
            case '1': admin_add_subject(); break;
            case '2': admin_list_subjects(); break;
            case '3': admin_enter_marks(); break;
            case '4': admin_enter_attendance(); break;
            case '5': running = 0; break;
            default: {
                WINDOW *w = newwin(5,50,12,(COLS-50)/2); wbkgd(w,COLOR_PAIR(C_WARN)); box(w,0,0); mvwprintw(w,2,2,"Invalid choice. Press any key."); wrefresh(w); wgetch(w); delwin(w);
            }
        }
    }
}

/* Student menu */
void student_menu(User *u) {
    int running = 1;
    while (running) {
        WINDOW *win = newwin(12, 70, 6, (COLS-70)/2);
        wbkgd(win, COLOR_PAIR(C_BOX)); box(win,0,0);
        mvwprintw(win,1,2," STUDENT MENU ");
        mvwprintw(win,3,4,"1) View Dashboard");
        mvwprintw(win,4,4,"2) View Subjects (by semester)");
        mvwprintw(win,5,4,"3) Logout");
        mvwprintw(win,8,4,"Choose option: ");
        wrefresh(win);
        int ch = wgetch(win);
        delwin(win);
        switch(ch) {
            case '1': student_dashboard(u); break;
            case '2': admin_list_subjects(); break;
            case '3': running = 0; break;
            default: { WINDOW *w = newwin(5,50,12,(COLS-50)/2); wbkgd(w,COLOR_PAIR(C_WARN)); box(w,0,0); mvwprintw(w,2,2,"Invalid choice. Press any key."); wrefresh(w); wgetch(w); delwin(w); }
        }
    }
}

/* MAIN UI: main menu with transitions */
void main_loop() {
    create_default_admin_if_none();
    int running = 1;
    while (running) {
        clear();
        draw_header(stdscr);
        texture_background(stdscr);
        int winw = 60, winh = 14;
        WINDOW *menu = newwin(winh, winw, 6, (COLS - winw)/2);
        wbkgd(menu, COLOR_PAIR(C_BOX));
        box(menu,0,0);
        mvwprintw(menu,1,2," Welcome ");
        mvwprintw(menu,3,4,"1) Login");
        mvwprintw(menu,4,4,"2) Signup (Student)");
        mvwprintw(menu,5,4,"3) Exit");
        mvwprintw(menu,8,4,"Choose: ");
        slide_in(menu);
        int ch = wgetch(menu);
        delwin(menu);
        if (ch == '1') {
            User *u = flow_login();
            if (u) {
                if (strcmp(u->role,"admin")==0) admin_dashboard(u);
                else student_menu(u);
            }
        } else if (ch == '2') {
            flow_signup();
        } else if (ch == '3') {
            running = 0;
        } else {
            WINDOW *w = newwin(5,50,12,(COLS-50)/2); wbkgd(w,COLOR_PAIR(C_WARN)); box(w,0,0); mvwprintw(w,2,2,"Invalid option. Press any key."); wrefresh(w); wgetch(w); delwin(w);
        }
    }
}

/* init ncurses and colors */
void ui_init() {
    initscr();
    start_color();
    use_default_colors();
    init_pair(C_MAIN, COLOR_WHITE, COLOR_BLUE);
    init_pair(C_HEADER, COLOR_WHITE, COLOR_BLUE);
    init_pair(C_BOX, COLOR_BLACK, COLOR_WHITE);
    init_pair(C_ACCENT, COLOR_YELLOW, -1);
    init_pair(C_TEXT, COLOR_WHITE, -1);
    init_pair(C_WARN, COLOR_RED, -1);
    init_pair(C_GOOD, COLOR_GREEN, -1);
    init_pair(C_BG, COLOR_BLACK, COLOR_BLACK);
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, FALSE);
    refresh();
}

void ui_end() {
    endwin();
}

int main(void) {
    srand((unsigned int)time(NULL));
    ui_init();
    main_loop();
    ui_end();
    printf("Exited. All data was in memory and is now lost.\n");
    return 0;
}
