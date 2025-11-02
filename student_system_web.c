/* student_system.c
   Student Record, Attendance, Grades & Printable Result Report
   - Interactive console program (admin + student)
   - Non-interactive CLI modes for web hosting / automated use
   - API wrappers for linking with a separate web-server binary

   Edits:
    - semester mapping helper `subject_semester`
    - deterministic random defaults for marks & attendance via populate_random_results()
    - calculate_and_update_cgpa_for_student now recomputes CGPA over all subjects (correct and idempotent)
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #define MKDIR(d) _mkdir(d)
  #define isatty _isatty
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #define MKDIR(d) mkdir(d, 0755)
#endif

#define DATA_FILE "students.dat"
#define REPORTS_DIR "reports"
#define MAX_STUDENTS 2000
#define MAX_NAME 100
#define MAX_SUBJECTS 64   /* increased to store many semesters worth of subjects */
#define MAX_SUB_NAME 100
#define ADMIN_USER "admin"
#define ADMIN_PASS "admin"

/* ---------- types ---------- */
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
    int current_semester;        /* new: current semester (1..8) */
    int num_subjects;
    Subject subjects[MAX_SUBJECTS];
    char password[50];
    int exists;
    double cgpa;
    int total_credits_completed;
} Student;

/* Global in-memory storage (non-static so web wrapper can link) */
Student students[MAX_STUDENTS];
int student_count = 0;

/* forward declarations to avoid implicit warnings */
void save_data(void);
void load_data(void);
int find_index_by_id(int id);
int api_find_index_by_id(int id);
int api_add_student(Student *s);
void api_generate_report(int idx, const char* college, const char* semester, const char* exam);
int api_calculate_update_cgpa(int idx);
int api_admin_auth(const char *user, const char *pass);
void generate_html_report(int idx, const char* college, const char* semester, const char* exam);
double calculate_sgpa_for_student(Student *s);
void calculate_and_update_cgpa_for_student(int idx);

/* New helpers */
static int subject_semester(const char *sname);
static void populate_random_results(Student *s);

/* ---------- Utility ---------- */
void safe_strncpy(char *dst, const char *src, size_t n) {
    if (!dst) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, n-1);
    dst[n-1] = '\0';
}

static void clear_stdin(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
}

static void pause_console(void) {
    printf("\nPress Enter to continue...");
    getchar();
}

static void to_lower_str(char *s) {
    for (; *s; ++s) *s = (char)tolower((unsigned char)*s);
}

/* ---------- Semester subjects data ---------- */
/* For each semester we define an array of subject names and credits.
   The lists are taken (and normalized) from the user's input.
*/
static const char *sem_subjects[][20] = {
    { NULL },
    {
        "Programming in C",
        "Linux Lab",
        "Problem Solving",
        "Advanced Engineering Mathematics - I",
        "Physics for Computer Engineers",
        "Managing Self",
        "Environmental Sustainability and Climate Change",
        NULL
    },
    {
        "Data Structures and Algorithms",
        "Digital Electronics",
        "Python Programming",
        "Advanced Engineering Mathematics - II",
        "Environmental Sustainability and Climate Change",
        "Time and Priority Management",
        "Elements of AI/ML",
        NULL
    },
    {
        "Leading Conversations",
        "Discrete Mathematical Structures",
        "Operating Systems",
        "Elements of AI/ML",
        "Database Management Systems",
        "Design and Analysis of Algorithms",
        NULL
    },
    {
        "Software Engineering",
        "EDGE - Soft Skills",
        "Linear Algebra",
        "Indian Constitution",
        "Writing with Impact",
        "Object Oriented Programming",
        "Data Communication and Networks",
        "Applied Machine Learning",
        NULL
    },
    {
        "Cryptography and Network Security",
        "Formal Languages and Automata Theory",
        "Object Oriented Analysis and Design",
        "Exploratory-3",
        "Start your Startup",
        "Research Methodology in CS",
        "Probability, Entropy, and MC Simulation",
        "PE-2",
        "PE-2 Lab",
        NULL
    },
    {
        "Exploratory-4",
        "Leadership and Teamwork",
        "Compiler Design",
        "Statistics and Data Analysis",
        "PE-3",
        "PE-3 Lab",
        "Minor Project",
        NULL
    },
    {
        "Exploratory-5",
        "PE-4",
        "PE-4 Lab",
        "PE-5",
        "PE-5 Lab",
        "Capstone Project - Phase-1",
        "Summer Internship",
        NULL
    },
    {
        "IT Ethical Practices",
        "Capstone Project - Phase-2",
        NULL
    }
};

/* Corresponding credits */
static const int sem_credits[][20] = {
    { 0 },
    { 5, 2, 2, 4, 5, 2, 2, 0 },
    { 5, 3, 5, 4, 2, 2, 3, 0 },
    { 2, 3, 3, 3, 5, 4, 0 },
    { 3, 0, 3, 0, 2, 4, 4, 5, 0 },
    { 3, 3, 3, 3, 2, 3, 3, 4, 1, 0 },
    { 3, 2, 3, 3, 4, 1, 5, 0 },
    { 3, 4, 1, 3, 1, 5, 1, 0 },
    { 3, 5, 0 }
};

/* helper to append semester subjects 1..sem into student subject list (avoids duplicates) */
static void add_semesters_to_student(Student *s, int sem) {
    if (!s) return;
    for (int cur = 1; cur <= sem && cur <= 8; ++cur) {
        const char **list = sem_subjects[cur];
        if (!list) continue;
        for (int j = 0; list[j] != NULL; ++j) {
            /* check if subject already present (case-sensitive compare on exact name) */
            int found = 0;
            for (int k = 0; k < s->num_subjects; ++k) {
                if (strcmp(s->subjects[k].name, list[j]) == 0) { found = 1; break; }
            }
            if (found) continue;
            if (s->num_subjects >= MAX_SUBJECTS) break;
            safe_strncpy(s->subjects[s->num_subjects].name, list[j], sizeof(s->subjects[s->num_subjects].name));
            /* lookup credit if available */
            int credit = 0;
            if (cur >= 1 && cur <= 8) {
                credit = sem_credits[cur][j];
            }
            s->subjects[s->num_subjects].credits = credit;
            s->subjects[s->num_subjects].marks = 0;
            s->subjects[s->num_subjects].classes_held = 0;
            s->subjects[s->num_subjects].classes_attended = 0;
            s->num_subjects++;
        }
    }
}

/* ---------- File operations ---------- */
void load_data(void) {
    FILE *fp = fopen(DATA_FILE, "rb");
    if (!fp) {
        student_count = 0;
        for (int i = 0; i < MAX_STUDENTS; ++i) students[i].exists = 0;
        return;
    }
    if (fread(&student_count, sizeof(student_count), 1, fp) != 1) {
        fclose(fp);
        student_count = 0;
        for (int i = 0; i < MAX_STUDENTS; ++i) students[i].exists = 0;
        return;
    }
    if (student_count < 0 || student_count > MAX_STUDENTS) student_count = 0;
    if (fread(students, sizeof(Student), student_count, fp) < (size_t)student_count) {
        /* tolerate truncated file */
    }
    fclose(fp);
}

void save_data(void) {
    FILE *fp = fopen(DATA_FILE, "wb");
    if (!fp) {
        perror("Unable to save data");
        return;
    }
    if (fwrite(&student_count, sizeof(student_count), 1, fp) != 1) {
        perror("Unable to write header");
        fclose(fp);
        return;
    }
    if (student_count > 0) {
        if (fwrite(students, sizeof(Student), student_count, fp) != (size_t)student_count) {
            perror("Unable to write records");
        }
    }
    fclose(fp);
}

/* ---------- Helpers ---------- */

int find_index_by_id(int id) {
    for (int i = 0; i < student_count; ++i) {
        if (students[i].exists && students[i].id == id) return i;
    }
    return -1;
}

int next_free_spot(void) {
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) return i;
    }
    if (student_count < MAX_STUDENTS) {
        students[student_count].exists = 0;
        return student_count++;
    }
    return -1;
}

int generate_unique_id(void) {
    int id = 1001;
    while (find_index_by_id(id) != -1) id++;
    return id;
}

int find_duplicate_by_details(const char *name, const char *dept, int year) {
    char name_low[MAX_NAME], dept_low[MAX_NAME];
    safe_strncpy(name_low, name, sizeof(name_low));
    safe_strncpy(dept_low, dept, sizeof(dept_low));
    to_lower_str(name_low); to_lower_str(dept_low);
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        char other_name[MAX_NAME], other_dept[MAX_NAME];
        safe_strncpy(other_name, students[i].name, sizeof(other_name));
        safe_strncpy(other_dept, students[i].dept, sizeof(other_dept));
        to_lower_str(other_name); to_lower_str(other_dept);
        if (strcmp(name_low, other_name) == 0 && strcmp(dept_low, other_dept) == 0 && students[i].year == year) return i;
    }
    return -1;
}

/* ---------- SGPA/CGPA ---------- */
static int marks_to_grade_point(int marks) {
    if (marks >= 90) return 10;
    if (marks >= 80) return 9;
    if (marks >= 70) return 8;
    if (marks >= 60) return 7;
    if (marks >= 50) return 6;
    if (marks >= 40) return 5;
    return 0;
}

double calculate_sgpa_for_student(Student *s) {
    int total_credits = 0;
    double weighted_sum = 0.0;
    for (int i = 0; i < s->num_subjects; ++i) {
        int cr = s->subjects[i].credits;
        int mk = s->subjects[i].marks;
        if (cr <= 0) continue;
        int gp = marks_to_grade_point(mk);
        weighted_sum += (double)gp * cr;
        total_credits += cr;
    }
    if (total_credits == 0) return 0.0;
    return weighted_sum / (double)total_credits;
}

/* New: recompute CGPA over ALL subjects (idempotent & correct) */
void calculate_and_update_cgpa_for_student(int idx) {
    if (idx < 0 || idx >= student_count) return;
    Student *s = &students[idx];
    int total_credits = 0;
    double total_weighted = 0.0;
    for (int i = 0; i < s->num_subjects; ++i) {
        int cr = s->subjects[i].credits;
        int mk = s->subjects[i].marks;
        if (cr <= 0) continue;
        int gp = marks_to_grade_point(mk);
        total_weighted += (double)gp * cr;
        total_credits += cr;
    }
    if (total_credits > 0) {
        s->cgpa = total_weighted / (double)total_credits;
        s->total_credits_completed = total_credits;
    } else {
        s->cgpa = 0.0;
        s->total_credits_completed = 0;
    }
    save_data();
    /* Print a line for console logs */
    double sgpa = calculate_sgpa_for_student(s);
    printf("Recomputed for student %d (%s): SGPA(all-subjects-view): %.3f, CGPA: %.3f (Credits: %d)\n", s->id, s->name, sgpa, s->cgpa, s->total_credits_completed);
}

/* ---------- Reports (HTML) ---------- */

static void html_escape(FILE *f, const char *s) {
    for (; *s; ++s) {
        if (*s == '&') fputs("&amp;", f);
        else if (*s == '<') fputs("&lt;", f);
        else if (*s == '>') fputs("&gt;", f);
        else if (*s == '"') fputs("&quot;", f);
        else fputc(*s, f);
    }
}

void generate_html_report(int idx, const char* college, const char* semester, const char* exam) {
    if (idx < 0 || idx >= student_count) return;
    Student *s = &students[idx];
    MKDIR(REPORTS_DIR);
    char path[512];
    snprintf(path, sizeof(path), "%s/%d_result.html", REPORTS_DIR, s->id);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("Unable to create report file");
        return;
    }
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char datebuf[64];
    snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);

    fprintf(f, "<!doctype html>\n<html>\n<head>\n<meta charset='utf-8'>\n");
    fprintf(f, "<title>Result - %d - %s</title>\n", s->id, s->name);
    fprintf(f, "<style>@page{size:A4;margin:20mm} body{font-family:Arial;font-size:12px} .table{width:100%%;border-collapse:collapse} .table th,.table td{border:1px solid #333;padding:6px;text-align:left}</style>\n");
    fprintf(f, "</head>\n<body>\n");
    fprintf(f, "<h2>");
    html_escape(f, college ? college : "Your College");
    fprintf(f, "</h2>\n");
    fprintf(f, "<p><strong>Student:</strong> "); html_escape(f, s->name); fprintf(f, "<br>\n");
    fprintf(f, "<strong>ID:</strong> %d<br>\n", s->id);
    fprintf(f, "<strong>Dept:</strong> "); html_escape(f, s->dept); fprintf(f, "<br>\n");
    fprintf(f, "<strong>Year:</strong> %d<br>\n", s->year);
    fprintf(f, "<strong>Semester:</strong> "); html_escape(f, semester ? semester : "Semester -"); fprintf(f, "<br>\n");
    fprintf(f, "<strong>Exam:</strong> "); html_escape(f, exam ? exam : "Exam -"); fprintf(f, "<br>\n");
    fprintf(f, "<strong>Date:</strong> %s</p>\n", datebuf);

    fprintf(f, "<table class='table'><tr><th>#</th><th>Subject</th><th>Marks</th><th>Credits</th><th>Grade Point</th></tr>\n");
    for (int i = 0; i < s->num_subjects; ++i) {
        int gp = marks_to_grade_point(s->subjects[i].marks);
        fprintf(f, "<tr><td>%d</td><td>", i+1);
        html_escape(f, s->subjects[i].name);
        fprintf(f, "</td><td>%d</td><td>%d</td><td>%d</td></tr>\n", s->subjects[i].marks, s->subjects[i].credits, gp);
    }
    double sgpa = calculate_sgpa_for_student(s);
    fprintf(f, "</table>\n<p><strong>SGPA (all subjects view):</strong> %.3f<br>\n<strong>CGPA:</strong> %.3f<br>\n<strong>Total Credits Counted:</strong> %d</p>\n", sgpa, s->cgpa, s->total_credits_completed);
    fprintf(f, "</body>\n</html>\n");
    fclose(f);
}

/* ---------- CRUD / Menus (interactive) ---------- */

void print_student_short(Student *s) {
    printf("ID: %d | Name: %s | Year: %d | Dept: %s | Sem: %d\n", s->id, s->name, s->year, s->dept, s->current_semester);
}

void print_student_full(Student *s) {
    printf("------------- Student Profile -------------\n");
    printf("ID        : %d\nName      : %s\nAge       : %d\nEmail     : %s\nPhone     : %s\nDepartment: %s\nYear      : %d\nSemester  : %d\nSubjects  : %d\n",
           s->id, s->name, s->age, s->email, s->phone, s->dept, s->year, s->current_semester, s->num_subjects);
    for (int i = 0; i < s->num_subjects; ++i) {
        Subject *sub = &s->subjects[i];
        int held = sub->classes_held, att = sub->classes_attended;
        double pct = (held == 0) ? 0.0 : ((double)att / held) * 100.0;
        printf("  %d) %s - Attended %d / %d (%.2f%%) | Marks: %d | Credits: %d\n",
               i+1, sub->name, att, held, pct, sub->marks, sub->credits);
    }
    double sgpa = calculate_sgpa_for_student(s);
    printf("Current semester SGPA: %.3f\nStored CGPA: %.3f (Credits: %d)\n", sgpa, s->cgpa, s->total_credits_completed);
    printf("-------------------------------------------\n");
}

/* add student: accepts fully-initialized Student (id may be 0 => auto-gen) */
void add_student_custom(Student *s) {
    if (!s) return;
    /* If an explicit non-zero id is provided, reject if duplicate */
    if (s->id != 0) {
        if (find_index_by_id(s->id) != -1) {
            printf("Student with ID %d already exists. Aborting add.\n", s->id);
            return;
        }
    }
    int idx = next_free_spot();
    if (idx == -1) { printf("Maximum students reached.\n"); return; }
    if (s->id == 0) s->id = generate_unique_id();

    /* Populate sensible default marks/attendance if none present (and student has subjects) */
    populate_random_results(s);

    students[idx] = *s;
    save_data();
    printf("Student added successfully. ID: %d\n", s->id);
}

/* interactive add student (admin) - now asks email, phone, semester */
void add_student(void) {
    Student s;
    memset(&s, 0, sizeof(s));
    s.exists = 1; s.cgpa = 0.0; s.total_credits_completed = 0;

    printf("Enter student ID (integer) or 0 to auto-generate: ");
    while (scanf("%d", &s.id) != 1) { clear_stdin(); printf("Invalid. Enter student ID (integer) or 0 to auto-generate: "); }
    clear_stdin();
    if (s.id == 0) s.id = generate_unique_id();
    else if (find_index_by_id(s.id) != -1) { printf("Student with ID %d already exists.\n", s.id); return; }

    printf("Enter full name: ");
    fgets(s.name, sizeof(s.name), stdin); s.name[strcspn(s.name, "\n")] = 0;

    printf("Enter age: ");
    while (scanf("%d", &s.age) != 1) { clear_stdin(); printf("Invalid. Enter age: "); }
    clear_stdin();

    printf("Enter email: "); fgets(s.email, sizeof(s.email), stdin); s.email[strcspn(s.email, "\n")] = 0;
    printf("Enter phone: "); fgets(s.phone, sizeof(s.phone), stdin); s.phone[strcspn(s.phone, "\n")] = 0;

    printf("Enter department: "); fgets(s.dept, sizeof(s.dept), stdin); s.dept[strcspn(s.dept, "\n")] = 0;
    printf("Enter year (e.g., 1,2,3,4): ");
    while (scanf("%d", &s.year) != 1) { clear_stdin(); printf("Invalid. Enter year: "); }
    clear_stdin();

    printf("Enter current semester (1..8): ");
    while (scanf("%d", &s.current_semester) != 1 || s.current_semester < 1 || s.current_semester > 8) {
        clear_stdin(); printf("Invalid. Enter semester (1..8): ");
    }
    clear_stdin();

    if (find_duplicate_by_details(s.name, s.dept, s.year) != -1) { printf("Duplicate found. Registration cancelled.\n"); return; }

    s.num_subjects = 0;
    add_semesters_to_student(&s, s.current_semester);

    printf("Set password for this student (no spaces): ");
    fgets(s.password, sizeof(s.password), stdin); s.password[strcspn(s.password, "\n")] = 0;

    add_student_custom(&s);
}

/* student self-register (CLI) - simplified: asks id or choose auto, name, age, email, phone, semester */
void student_self_register(void) {
    Student s;
    memset(&s, 0, sizeof(s));
    s.exists = 1; s.cgpa = 0.0; s.total_credits_completed = 0;

    printf("Student Self-Registration\nEnter student ID (integer) or 0 to auto-generate: ");
    while (scanf("%d", &s.id) != 1) { clear_stdin(); printf("Invalid. Enter student ID (integer) or 0 to auto-generate: "); }
    clear_stdin();
    if (s.id == 0) s.id = generate_unique_id();
    else if (find_index_by_id(s.id) != -1) { printf("ID exists. Use login instead.\n"); return; }

    printf("Enter full name: "); fgets(s.name, sizeof(s.name), stdin); s.name[strcspn(s.name, "\n")] = 0;
    printf("Enter age: "); while (scanf("%d", &s.age) != 1) { clear_stdin(); printf("Invalid. Enter age: "); } clear_stdin();

    printf("Enter email: "); fgets(s.email, sizeof(s.email), stdin); s.email[strcspn(s.email, "\n")] = 0;
    printf("Enter phone: "); fgets(s.phone, sizeof(s.phone), stdin); s.phone[strcspn(s.phone, "\n")] = 0;

    printf("Enter department: "); fgets(s.dept, sizeof(s.dept), stdin); s.dept[strcspn(s.dept, "\n")] = 0;
    printf("Enter year (e.g., 1,2,3,4): "); while (scanf("%d", &s.year) != 1) { clear_stdin(); printf("Invalid. Enter year: "); } clear_stdin();

    printf("Enter current semester (1..8): ");
    while (scanf("%d", &s.current_semester) != 1 || s.current_semester < 1 || s.current_semester > 8) {
        clear_stdin(); printf("Invalid. Enter semester (1..8): ");
    }
    clear_stdin();

    if (find_duplicate_by_details(s.name, s.dept, s.year) != -1) { printf("Duplicate. Registration cancelled.\n"); return; }

    s.num_subjects = 0;
    add_semesters_to_student(&s, s.current_semester);

    printf("Set password for this student (no spaces): ");
    fgets(s.password, sizeof(s.password), stdin); s.password[strcspn(s.password, "\n")] = 0;
    add_student_custom(&s);
    printf("Registration complete. Use your ID and password to login.\n");
}

void edit_student(void) {
    int id;
    printf("Enter student ID to edit: ");
    while (scanf("%d", &id) != 1) { clear_stdin(); printf("Invalid. Enter student ID: "); }
    clear_stdin();
    int idx = find_index_by_id(id);
    if (idx == -1) { printf("Student not found.\n"); return; }
    Student *s = &students[idx];
    print_student_full(s);
    printf("What to edit?\n1) Name\n2) Age\n3) Email\n4) Phone\n5) Dept\n6) Year\n7) Semester (rebuild subjects)\n8) Subjects (rename)\n9) Password\n10) Cancel\nChoose: ");
    int ch;
    while (scanf("%d", &ch) != 1) { clear_stdin(); printf("Invalid. Choose: "); }
    clear_stdin();
    switch (ch) {
        case 1: printf("New name: "); fgets(s->name, sizeof(s->name), stdin); s->name[strcspn(s->name, "\n")] = 0; break;
        case 2: printf("New age: "); while (scanf("%d", &s->age) != 1) { clear_stdin(); printf("Invalid. Enter age: "); } clear_stdin(); break;
        case 3: printf("New email: "); fgets(s->email, sizeof(s->email), stdin); s->email[strcspn(s->email, "\n")] = 0; break;
        case 4: printf("New phone: "); fgets(s->phone, sizeof(s->phone), stdin); s->phone[strcspn(s->phone, "\n")] = 0; break;
        case 5: printf("New dept: "); fgets(s->dept, sizeof(s->dept), stdin); s->dept[strcspn(s->dept, "\n")] = 0; break;
        case 6: printf("New year: "); while (scanf("%d", &s->year) != 1) { clear_stdin(); printf("Invalid. Enter year: "); } clear_stdin(); break;
        case 7: {
            printf("Enter new current semester (1..8): ");
            int sem; while (scanf("%d", &sem) != 1 || sem < 1 || sem > 8) { clear_stdin(); printf("Invalid. Enter semester (1..8): "); }
            clear_stdin();
            s->current_semester = sem;
            /* rebuild subject list from semesters 1..sem (preserve marks/attendance if names match) */
            Subject backup[MAX_SUBJECTS];
            int bk = s->num_subjects;
            for (int i = 0; i < bk; ++i) backup[i] = s->subjects[i];
            /* reset */
            for (int i = 0; i < MAX_SUBJECTS; ++i) { s->subjects[i].name[0] = '\0'; s->subjects[i].credits = s->subjects[i].marks = s->subjects[i].classes_attended = s->subjects[i].classes_held = 0; }
            s->num_subjects = 0;
            add_semesters_to_student(s, sem);
            /* try to restore marks/attendance where subject names match */
            for (int i = 0; i < s->num_subjects; ++i) {
                for (int j = 0; j < bk; ++j) {
                    if (strcmp(s->subjects[i].name, backup[j].name) == 0) {
                        s->subjects[i].marks = backup[j].marks;
                        s->subjects[i].credits = backup[j].credits;
                        s->subjects[i].classes_attended = backup[j].classes_attended;
                        s->subjects[i].classes_held = backup[j].classes_held;
                        break;
                    }
                }
            }
            break;
        }
        case 8: {
            printf("Subjects:\n"); for (int i = 0; i < s->num_subjects; ++i) printf("%d) %s\n", i+1, s->subjects[i].name);
            printf("Enter subject number to rename: ");
            int sn; while (scanf("%d", &sn) != 1 || sn < 1 || sn > s->num_subjects) { clear_stdin(); printf("Invalid. Enter subject number: "); }
            clear_stdin(); printf("New subject name: "); fgets(s->subjects[sn-1].name, sizeof(s->subjects[sn-1].name), stdin); s->subjects[sn-1].name[strcspn(s->subjects[sn-1].name, "\n")] = 0;
            break;
        }
        case 9: printf("New password: "); fgets(s->password, sizeof(s->password), stdin); s->password[strcspn(s->password, "\n")] = 0; break;
        case 10: printf("Edit cancelled.\n"); return;
        default: printf("Invalid option.\n"); return;
    }
    save_data(); printf("Student updated.\n");
}

void delete_student(void) {
    int id; printf("Enter student ID to delete: ");
    while (scanf("%d", &id) != 1) { clear_stdin(); printf("Invalid. Enter student ID: "); }
    clear_stdin();
    int idx = find_index_by_id(id);
    if (idx == -1) { printf("Student not found.\n"); return; }
    print_student_short(&students[idx]);
    printf("Confirm delete (y/n): ");
    char c = getchar(); clear_stdin();
    if (c == 'y' || c == 'Y') { students[idx].exists = 0; save_data(); printf("Deleted.\n"); }
    else printf("Cancelled.\n");
}

void search_student(void) {
    printf("Search by:\n1) ID\n2) Name substring\nChoose: ");
    int ch; while (scanf("%d", &ch) != 1) { clear_stdin(); printf("Invalid. Choose: "); }
    clear_stdin();
    if (ch == 1) {
        int id; printf("Enter ID: "); while (scanf("%d", &id) != 1) { clear_stdin(); printf("Invalid. Enter ID: "); } clear_stdin();
        int idx = find_index_by_id(id); if (idx == -1) printf("Not found.\n"); else print_student_full(&students[idx]);
    } else if (ch == 2) {
        char q[128]; printf("Enter substring: "); fgets(q, sizeof(q), stdin); q[strcspn(q, "\n")] = 0;
        char ql[128]; safe_strncpy(ql, q, sizeof(ql)); to_lower_str(ql);
        int found = 0;
        for (int i = 0; i < student_count; ++i) {
            if (!students[i].exists) continue;
            char lname[128]; safe_strncpy(lname, students[i].name, sizeof(lname)); to_lower_str(lname);
            if (strstr(lname, ql)) { print_student_short(&students[i]); found = 1; }
        }
        if (!found) printf("No matches.\n");
    } else printf("Invalid option.\n");
}

/* Attendance functions (admin-run) - day-by-day attendance list & marking is simulated by per-class marking */
void mark_attendance_for_class(void) {
    printf("Enter exact subject name to mark (case-sensitive): ");
    char sname[MAX_SUB_NAME]; fgets(sname, sizeof(sname), stdin); sname[strcspn(sname, "\n")] = 0;
    int any = 0;
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        Student *st = &students[i];
        for (int j = 0; j < st->num_subjects; ++j) {
            if (strcmp(st->subjects[j].name, sname) == 0) {
                any = 1;
                printf("Student ID %d | %s : Present? (y/n) : ", st->id, st->name);
                char c = getchar(); clear_stdin();
                st->subjects[j].classes_held += 1;
                if (c == 'y' || c == 'Y') st->subjects[j].classes_attended += 1;
                break;
            }
        }
    }
    if (!any) printf("No students have subject '%s'.\n", sname);
    else { save_data(); printf("Attendance recorded.\n"); }
}

/* Mark attendance with a list and tickboxes is a web UI feature; here we keep CLI single-student and per-class methods */
void mark_attendance_single_student(void) {
    int id; printf("Enter student ID: "); while (scanf("%d", &id) != 1) { clear_stdin(); printf("Invalid. Enter student ID: "); } clear_stdin();
    int idx = find_index_by_id(id); if (idx == -1) { printf("Not found.\n"); return; }
    Student *s = &students[idx];
    for (int i = 0; i < s->num_subjects; ++i) printf("%d) %s (Attended %d / Held %d)\n", i+1, s->subjects[i].name, s->subjects[i].classes_attended, s->subjects[i].classes_held);
    printf("Choose subject number: "); int sn; while (scanf("%d", &sn) != 1 || sn < 1 || sn > s->num_subjects) { clear_stdin(); printf("Invalid. Choose subject number: "); } clear_stdin();
    int idxs = sn - 1; s->subjects[idxs].classes_held += 1;
    printf("Present? (y/n): "); char c = getchar(); clear_stdin();
    if (c == 'y' || c == 'Y') s->subjects[idxs].classes_attended += 1;
    save_data(); printf("Attendance updated.\n");
}

void increment_classes_held_only(void) {
    printf("Enter exact subject name to increment classes held: ");
    char sname[MAX_SUB_NAME]; fgets(sname, sizeof(sname), stdin); sname[strcspn(sname, "\n")] = 0;
    int any = 0;
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        Student *st = &students[i];
        for (int j = 0; j < st->num_subjects; ++j) {
            if (strcmp(st->subjects[j].name, sname) == 0) { st->subjects[j].classes_held += 1; any = 1; }
        }
    }
    if (!any) printf("No students have subject '%s'.\n", sname);
    else { save_data(); printf("Classes held incremented.\n"); }
}

void attendance_report_subject(void) {
    printf("Enter exact subject name for report: ");
    char sname[MAX_SUB_NAME]; fgets(sname, sizeof(sname), stdin); sname[strcspn(sname, "\n")] = 0;
    int found = 0; printf("Attendance report for '%s'\nID | Name | Attended | Held | %%\n", sname);
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        Student *st = &students[i];
        for (int j = 0; j < st->num_subjects; ++j) {
            if (strcmp(st->subjects[j].name, sname) == 0) {
                int att = st->subjects[j].classes_attended; int held = st->subjects[j].classes_held;
                double pct = (held==0)?0.0:((double)att/held)*100.0;
                printf("%d | %s | %d | %d | %.2f%%\n", st->id, st->name, att, held, pct); found = 1;
            }
        }
    }
    if (!found) printf("No records for '%s'.\n", sname);
}

/* Student view functions */
void student_view_profile(int idx) {
    if (idx < 0 || idx >= student_count) return;
    print_student_full(&students[idx]);
}

void student_view_sgpa_and_cgpa(int idx) {
    if (idx < 0 || idx >= student_count) return;
    double sgpa = calculate_sgpa_for_student(&students[idx]);
    printf("Student: %s (ID %d)\nSGPA (current semester): %.3f\nCGPA (stored): %.3f (Credits: %d)\n",
           students[idx].name, students[idx].id, sgpa, students[idx].cgpa, students[idx].total_credits_completed);
    char path[512]; snprintf(path, sizeof(path), "%s/%d_result.html", REPORTS_DIR, students[idx].id);
    FILE *f = fopen(path, "r"); if (f) { fclose(f); printf("Printable result: %s\n", path); } else printf("No printable result yet.\n");
}

/* Interactive admin marks handler — does NOT auto-generate printable report per your request */
void admin_enter_marks_and_update_cgpa(void) {
    int id;
    printf("Enter student ID to enter marks for: ");
    while (scanf("%d", &id) != 1) { clear_stdin(); printf("Invalid. Enter student ID: "); }
    clear_stdin();
    int idx = find_index_by_id(id);
    if (idx == -1) { printf("Student not found.\n"); return; }
    Student *s = &students[idx];
    if (s->num_subjects <= 0) { printf("Student has no subjects defined.\n"); return; }

    printf("Entering marks for %s (ID: %d). Enter marks for each subject.\n", s->name, s->id);
    for (int i = 0; i < s->num_subjects; ++i) {
        int mk = -1;
        printf("Subject %d) %s (credits %d)\n", i+1, s->subjects[i].name, s->subjects[i].credits);
        printf("  Enter marks (0-100): ");
        while (scanf("%d", &mk) != 1 || mk < 0 || mk > 100) { clear_stdin(); printf("Invalid. Enter marks (0-100): "); }
        clear_stdin();
        s->subjects[i].marks = mk;
    }

    /* Recalculate CGPA and save */
    calculate_and_update_cgpa_for_student(idx);
    save_data();
    printf("Marks entered and CGPA updated for ID %d.\n", id);
}

/* Menus & auth (admin menu updated: removed "generate report" option) */
int admin_menu(void) {
    while (1) {
        printf("\n=== ADMIN MENU ===\n1) Add student\n2) Edit student\n3) Delete student\n4) List students\n5) Search student\n6) Mark attendance (class)\n7) Mark attendance (single student)\n8) Increment classes held only\n9) Attendance report (subject)\n10) Enter marks & update CGPA for a student\n11) Logout\nChoose: ");
        int ch; while (scanf("%d", &ch) != 1) { clear_stdin(); printf("Invalid. Choose: "); } clear_stdin();
        switch (ch) {
            case 1: add_student(); break;
            case 2: edit_student(); break;
            case 3: delete_student(); break;
            case 4: { printf("List of students:\n"); for (int i=0;i<student_count;i++) if (students[i].exists) print_student_short(&students[i]); break; }
            case 5: search_student(); break;
            case 6: mark_attendance_for_class(); break;
            case 7: mark_attendance_single_student(); break;
            case 8: increment_classes_held_only(); break;
            case 9: attendance_report_subject(); break;
            case 10: admin_enter_marks_and_update_cgpa(); break;
            case 11: return 0;
            default: printf("Invalid option.\n"); break;
        }
        pause_console();
    }
    return 0;
}

int student_menu(int student_idx) {
    if (student_idx < 0 || student_idx >= student_count) return -1;
    while (1) {
        printf("\n=== STUDENT MENU ===\n1) View profile & attendance\n2) View SGPA & CGPA\n3) Download/See printable report path\n4) Change password\n5) Logout\nChoose: ");
        int ch; while (scanf("%d", &ch) != 1) { clear_stdin(); printf("Invalid. Choose: "); } clear_stdin();
        Student *s = &students[student_idx];
        switch (ch) {
            case 1: student_view_profile(student_idx); break;
            case 2: student_view_sgpa_and_cgpa(student_idx); break;
            case 3: {
                char path[512]; snprintf(path, sizeof(path), "%s/%d_result.html", REPORTS_DIR, s->id);
                FILE *f = fopen(path, "r"); if (f) { fclose(f); printf("Printable result: %s\n", path); } else printf("No printable result.\n");
                break;
            }
            case 4: {
                char oldp[50], newp[50];
                printf("Enter current password: "); fgets(oldp, sizeof(oldp), stdin); oldp[strcspn(oldp, "\n")] = 0;
                if (strcmp(oldp, s->password) != 0) { printf("Wrong password.\n"); }
                else { printf("Enter new password: "); fgets(newp, sizeof(newp), stdin); newp[strcspn(newp, "\n")] = 0; safe_strncpy(s->password, newp, sizeof(s->password)); save_data(); printf("Password changed.\n"); }
                break;
            }
            case 5: return 0;
            default: printf("Invalid option.\n"); break;
        }
        pause_console();
    }
    return 0;
}

void admin_login(void) {
    char user[50], pass[50];
    printf("Admin Username: "); fgets(user, sizeof(user), stdin); user[strcspn(user, "\n")] = 0;
    printf("Admin Password: "); fgets(pass, sizeof(pass), stdin); pass[strcspn(pass, "\n")] = 0;
    if (strcmp(user, ADMIN_USER) == 0 && strcmp(pass, ADMIN_PASS) == 0) { printf("Admin authenticated.\n"); admin_menu(); }
    else printf("Invalid admin credentials.\n");
}

void student_login(void) {
    int id; printf("Enter student ID: "); while (scanf("%d", &id) != 1) { clear_stdin(); printf("Invalid. Enter student ID: "); } clear_stdin();
    int idx = find_index_by_id(id); if (idx == -1) { printf("Student ID not found.\n"); return; }
    char pass[50]; printf("Enter password: "); fgets(pass, sizeof(pass), stdin); pass[strcspn(pass, "\n")] = 0;
    if (strcmp(pass, students[idx].password) == 0) { printf("Welcome, %s!\n", students[idx].name); student_menu(idx); }
    else printf("Wrong password.\n");
}

void main_menu(void) {
    while (1) {
        printf("\n=== STUDENT MANAGEMENT SYSTEM ===\n1) Admin login\n2) Student login\n3) Exit\n4) Student self-register\nChoose: ");
        int ch; while (scanf("%d", &ch) != 1) { clear_stdin(); printf("Invalid. Choose: "); } clear_stdin();
        switch (ch) {
            case 1: admin_login(); break;
            case 2: student_login(); break;
            case 3: printf("Exiting... Goodbye.\n"); return;
            case 4: student_self_register(); break;
            default: printf("Invalid option.\n"); break;
        }
    }
}

/* ---------- Non-interactive CLI helpers (file formats) ---------- */

/* add-file format (single line):
   id|name|age|email|phone|dept|year|semester
*/
int cli_add_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Unable to open add-file: %s\n", path); return 1; }
    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); fprintf(stderr, "Empty add-file\n"); return 1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    /* parse fields */
    char *parts[10]; int pi = 0;
    char *tok = strtok(line, "|");
    while (tok && pi < 10) { parts[pi++] = tok; tok = strtok(NULL, "|"); }
    if (pi < 8) { fprintf(stderr, "Invalid add-file format (need id|name|age|email|phone|dept|year|semester)\n"); return 1; }
    Student s; memset(&s, 0, sizeof(s)); s.exists = 1; s.cgpa = 0.0; s.total_credits_completed = 0;
    s.id = atoi(parts[0]);
    safe_strncpy(s.name, parts[1], sizeof(s.name));
    s.age = atoi(parts[2]);
    safe_strncpy(s.email, parts[3], sizeof(s.email));
    safe_strncpy(s.phone, parts[4], sizeof(s.phone));
    safe_strncpy(s.dept, parts[5], sizeof(s.dept));
    s.year = atoi(parts[6]);
    s.current_semester = atoi(parts[7]);
    if (s.current_semester < 1 || s.current_semester > 8) s.current_semester = 1;
    s.num_subjects = 0;
    add_semesters_to_student(&s, s.current_semester);
    safe_strncpy(s.password, "changeme", sizeof(s.password));

    /* populate defaults */
    populate_random_results(&s);

    if (s.id == 0) s.id = generate_unique_id();
    add_student_custom(&s);
    printf("Added student ID %d\n", s.id);
    return 0;
}

/* marks file:
   first line: id
   subsequent lines: mark,subject-name (subject name must exactly match)
*/
int cli_enter_marks_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Unable to open marks file %s\n", path); return 1; }
    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); fprintf(stderr, "Invalid marks file\n"); return 1; }
    int id = atoi(line); int idx = find_index_by_id(id);
    if (idx == -1) { fclose(f); fprintf(stderr, "Student not found\n"); return 1; }
    Student *s = &students[idx];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        int mk = 0; char subj[MAX_SUB_NAME];
        if (sscanf(line, "%d|%99[^\n]", &mk, subj) == 2) {
            /* find subject */
            for (int i = 0; i < s->num_subjects; ++i) {
                if (strcmp(s->subjects[i].name, subj) == 0) {
                    s->subjects[i].marks = mk;
                    break;
                }
            }
        }
    }
    fclose(f);
    calculate_and_update_cgpa_for_student(idx);
    printf("Marks updated for ID %d\n", id);
    return 0;
}

/* CLI view/list */
int cli_list(void) {
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        printf("%d|%s|%d|%s|sem:%d\n", students[i].id, students[i].name, students[i].year, students[i].dept, students[i].current_semester);
    }
    return 0;
}

int cli_view(int id) {
    int idx = find_index_by_id(id);
    if (idx == -1) { fprintf(stderr, "Not found\n"); return 1; }
    print_student_full(&students[idx]);
    return 0;
}

/* generate-report arg format: "<id>|<college>|<semester>|<exam>" */
int cli_generate_report_arg(const char *arg) {
    if (!arg) { fprintf(stderr, "Missing arg\n"); return 1; }
    char buf[1024]; safe_strncpy(buf, arg, sizeof(buf));
    char *p = strtok(buf, "|"); if (!p) return 1;
    int id = atoi(p);
    char college[256] = "Your College", semester[128] = "Semester -", exam[128] = "Exam -";
    if ((p = strtok(NULL, "|")) != NULL) safe_strncpy(college, p, sizeof(college));
    if ((p = strtok(NULL, "|")) != NULL) safe_strncpy(semester, p, sizeof(semester));
    if ((p = strtok(NULL, "|")) != NULL) safe_strncpy(exam, p, sizeof(exam));
    int idx = find_index_by_id(id);
    if (idx == -1) { fprintf(stderr, "Student not found\n"); return 1; }
    generate_html_report(idx, college, semester, exam);
    printf("Report written: %s/%d_result.html\n", REPORTS_DIR, id);
    return 0;
}

/* ---------- API wrappers for web server linking ---------- */

int api_find_index_by_id(int id) { return find_index_by_id(id); }
int api_add_student(Student *s) {
    if (!s) return -1;
    /* if caller provided an explicit id, check duplicate */
    if (s->id != 0) {
        if (find_index_by_id(s->id) != -1) return -2;
    }
    if (s->id == 0) s->id = generate_unique_id();

    /* populate defaults if needed */
    populate_random_results(s);

    add_student_custom(s);
    return s->id;
}
void api_generate_report(int idx, const char* college, const char* semester, const char* exam) { generate_html_report(idx, college, semester, exam); }
int api_calculate_update_cgpa(int idx) { calculate_and_update_cgpa_for_student(idx); return 0; }

/* ---------- new: API admin auth (used by web wrapper) ---------- */
int api_admin_auth(const char *user, const char *pass) {
    if (!user || !pass) return 0;
    if (strcmp(user, ADMIN_USER) == 0 && strcmp(pass, ADMIN_PASS) == 0) return 1;
    return 0;
}

/* ---------- main (interactive) ---------- */

#ifndef BUILD_WEB
int main(int argc, char **argv) {
    /* support demo and CLI modes when run directly (useful for testing) */
    if (argc > 1) {
        load_data();
        if (strcmp(argv[1], "--demo") == 0) {
            printf("Demo Mode: Student Management System\n");
            /* Demo: create two sample students if none exist */
            if (student_count == 0 || find_index_by_id(1001) == -1) {
                Student s; memset(&s,0,sizeof(s)); s.exists=1; s.id=1001; safe_strncpy(s.name,"Tanay Sah",sizeof(s.name)); s.age=18; safe_strncpy(s.dept,"B.Tech CSE",sizeof(s.dept)); s.year=1; s.current_semester=1; s.num_subjects=0; add_semesters_to_student(&s,1); safe_strncpy(s.password,"pass",sizeof(s.password)); populate_random_results(&s); add_student_custom(&s);
            }
            if (find_index_by_id(1002) == -1) {
                Student s; memset(&s,0,sizeof(s)); s.exists=1; s.id=1002; safe_strncpy(s.name,"Riya Sharma",sizeof(s.name)); s.age=18; safe_strncpy(s.dept,"B.Tech CSE",sizeof(s.dept)); s.year=1; s.current_semester=1; s.num_subjects=0; add_semesters_to_student(&s,1); safe_strncpy(s.password,"pass",sizeof(s.password)); populate_random_results(&s); add_student_custom(&s);
            }
            printf("Sample students created (1001,1002). Use interactive menu to explore.\n");
            return 0;
        } else if (strcmp(argv[1], "--list") == 0) {
            load_data(); return cli_list();
        } else if (strcmp(argv[1], "--view") == 0 && argc > 2) {
            load_data(); return cli_view(atoi(argv[2]));
        } else if (strcmp(argv[1], "--generate-report") == 0 && argc > 2) {
            load_data(); return cli_generate_report_arg(argv[2]);
        } else if (strcmp(argv[1], "--add-file") == 0 && argc > 2) {
            load_data(); return cli_add_from_file(argv[2]);
        } else if (strcmp(argv[1], "--enter-marks-file") == 0 && argc > 2) {
            load_data(); return cli_enter_marks_file(argv[2]);
        }
    }

    /* Don't launch interactive menu if no TTY (useful for hosting) */
    if (!isatty(fileno(stdin))) {
        fprintf(stderr, "Non-interactive environment detected. Use --demo for sample output.\n");
        return 1;
    }

    MKDIR(REPORTS_DIR);
    for (int i = 0; i < MAX_STUDENTS; ++i) students[i].exists = 0;
    load_data();
    printf("Welcome to Student Record & Attendance Management System\n(Note: Default admin -> username: %s | password: %s)\n", ADMIN_USER, ADMIN_PASS);
    main_menu();
    return 0;
}
#endif /* BUILD_WEB */

/* ---------- Implementations of new helpers ---------- */

/* Map subject name to semester index (1..8); returns 0 if unknown */
static int subject_semester(const char *sname) {
    if (!sname) return 0;
    for (int sem = 1; sem <= 8; ++sem) {
        const char **list = sem_subjects[sem];
        if (!list) continue;
        for (int j = 0; list[j] != NULL; ++j) {
            if (strcmp(list[j], sname) == 0) return sem;
        }
    }
    return 0;
}

/* populate_random_results:
   - deterministic via seed based on student id (so repeated runs for same id yield same defaults)
   - only fills marks/attendance if they are zero (so manual edits are preserved)
*/
static void populate_random_results(Student *s) {
    if (!s) return;
    /* seed using student's id for determinism */
    unsigned int seed = (unsigned int)(s->id ? (unsigned)s->id ^ 0x9e3779b9u : (unsigned)time(NULL));
    /* simple helper */
    auto_rand:
    ;
    /* local lambda-like helpers using rand_r */
    unsigned int rseed = seed;

    int any_subject = 0;
    for (int i = 0; i < s->num_subjects; ++i) {
        any_subject = 1;
        int sem = subject_semester(s->subjects[i].name);
        /* default classes_held between 10 and 30 if zero */
        if (s->subjects[i].classes_held == 0) {
            int x = (int)(rand_r(&rseed) % 21) + 10; /* 10..30 */
            s->subjects[i].classes_held = x;
        }
        /* if classes_attended == 0, make it between 60% and 95% of held (older sems higher), otherwise preserve */
        if (s->subjects[i].classes_attended == 0) {
            double minpct = 0.6, maxpct = 0.95;
            if (sem > 0 && s->current_semester > 0 && sem < s->current_semester) {
                /* older semesters: higher pass/attendance */
                minpct = 0.75; maxpct = 0.98;
            } else if (sem == s->current_semester) {
                /* current semester: realistic but slightly lower */
                minpct = 0.55; maxpct = 0.92;
            }
            int held = s->subjects[i].classes_held;
            int pct = (int)(minpct*100) + (rand_r(&rseed) % (int)((maxpct-minpct)*100 + 1));
            int attended = (int)((pct/100.0) * held + 0.5);
            if (attended > held) attended = held;
            s->subjects[i].classes_attended = attended;
        }
        /* marks: if zero, assign based on semester relation */
        if (s->subjects[i].marks == 0) {
            int mk;
            if (sem > 0 && s->current_semester > 0 && sem < s->current_semester) {
                /* past semesters: likely passed => marks 55..95 */
                mk = 55 + (rand_r(&rseed) % 41); /* 55..95 */
            } else if (sem == s->current_semester) {
                /* current semester: slightly wider spread 40..92 */
                mk = 45 + (rand_r(&rseed) % 48); /* 45..92 */
            } else {
                /* unknown grouping */
                mk = 40 + (rand_r(&rseed) % 51); /* 40..90 */
            }
            if (mk < 0) mk = 0; if (mk > 100) mk = 100;
            s->subjects[i].marks = mk;
        }
    }

    /* compute CGPA now (a full recompute over all subjects) */
    if (any_subject) {
        /* compute totals */
        int total_credits = 0;
        double total_weighted = 0.0;
        for (int i = 0; i < s->num_subjects; ++i) {
            int cr = s->subjects[i].credits;
            int mk = s->subjects[i].marks;
            if (cr <= 0) continue;
            int gp = marks_to_grade_point(mk);
            total_weighted += (double)gp * cr;
            total_credits += cr;
        }
        if (total_credits > 0) {
            s->cgpa = total_weighted / (double)total_credits;
            s->total_credits_completed = total_credits;
        } else {
            s->cgpa = 0.0;
            s->total_credits_completed = 0;
        }
    }
    /* update seed out-of-band: not necessary (deterministic by ID) */
    return;
}
