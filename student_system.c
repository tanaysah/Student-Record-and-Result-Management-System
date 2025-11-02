/* student_system.c 
   Student Record, Attendance, Grades & Printable Result Report
   - Interactive console program (admin + student)
   - Non-interactive CLI modes for web hosting / automated use
   - API wrappers for linking with a separate web-server binary

   Build:
     Console: gcc student_system.c -o student_system
     Web: gcc -DBUILD_WEB student_system.c student_system_web.c -o student_system_web

   Notes:
     - Data persisted to students_v2.dat (binary). If an older students.dat exists,
       the loader will attempt a basic migration to the new format.
     - Reports written to reports/<id>_result.html
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #define MKDIR(d) _mkdir(d)
  #define isatty _isatty
#else
  #include <sys/types.h>
  #include <unistd.h>
  #define MKDIR(d) mkdir(d, 0755)
#endif

#define DATA_FILE_OLD "students.dat"
#define DATA_FILE "students_v2.dat"
#define REPORTS_DIR "reports"
#define MAX_STUDENTS 2000
#define MAX_NAME 100
/* Increased to hold cumulative subjects across semesters */
#define MAX_SUBJECTS 64
#define MAX_SUB_NAME 100
#define ADMIN_USER "admin"
#define ADMIN_PASS "admin"

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
    int current_semester;
    int num_subjects;
    Subject subjects[MAX_SUBJECTS];
    char password[64];
    char email[128];
    char phone[32];
    int exists;
    double cgpa;
    int total_credits_completed;
} Student;

/* For backward migration: old student layout (before we added email/phone/current_semester)
   matches the layout used previously in older versions of this repository.
*/
typedef struct {
    int id;
    char name[MAX_NAME];
    int age;
    char dept[MAX_NAME];
    int year;
    int num_subjects;
    Subject subjects[8];
    char password[50];
    int exists;
    double cgpa;
    int total_credits_completed;
} OldStudent;

/* Global in-memory storage (non-static so web wrapper can link) */
Student students[MAX_STUDENTS];
int student_count = 0;

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

/* ---------- Semester subject catalogs ---------- */
/* Each semester is represented by an array of strings and corresponding credits.
   These reflect the subject lists the user provided.
*/

static const char *sem_subjects[][16] = {
    /* index 0 unused - semesters start at 1 */
    { NULL },
    /* Semester 1 */
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
    /* Semester 2 */
    {
        "Data Structures and Algorithms",
        "Digital Electronics",
        "Python Programming",
        "Advanced Engineering Mathematics - II",
        "Environmental Sustainability and Climate Change",
        "Time and Priority Management",
        "Elements of AI & ML",
        NULL
    },
    /* Semester 3 */
    {
        "Leading Conversations",
        "Discrete Mathematical Structures",
        "Operating Systems",
        "Elements of AIML",
        "Database Management Systems",
        "Design and Analysis of Algorithms",
        NULL
    },
    /* Semester 4 */
    {
        "Software Engineering",
        "EDGE-SoftSkills",
        "Linear Algebra",
        "Indian Constitution",
        "Writing with Impact",
        "Object Oriented Programming",
        "Data Communication and Networks",
        "Applied Machine Learning",
        NULL
    },
    /* Semester 5 */
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
    /* Semester 6 */
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
    /* Semester 7 */
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
    /* Semester 8 */
    {
        "IT Ethical Practices",
        "Capstone Project - Phase-2",
        NULL
    }
};

/* Credits per semester subjects (parallel arrays) */
static const int sem_credits[][16] = {
    { 0 },
    /* sem1 */
    {5,2,2,4,5,2,2, 0},
    /* sem2 */
    {5,3,5,4,2,2,3, 0},
    /* sem3 */
    {2,3,3,3,5,4, 0},
    /* sem4 */
    {3,0,3,0,2,4,4,5, 0},
    /* sem5 */
    {3,3,3,3,2,3,3,4,1, 0},
    /* sem6 */
    {3,2,3,3,4,1,5, 0},
    /* sem7 */
    {3,4,1,3,1,5,1, 0},
    /* sem8 */
    {3,5, 0}
};

/* Helper to get subject count for a semester */
static int sem_subject_count(int sem) {
    if (sem < 1 || sem > 8) return 0;
    int c = 0;
    while (sem_subjects[sem][c] != NULL && c < 16) c++;
    return c;
}

/* Append subjects for semesters 1..sem to student s (no duplicates).
   Returns number of subjects appended (or -1 on error).
*/
int populate_subjects_for_semesters(Student *s, int sem) {
    if (!s || sem < 1) return -1;
    if (sem > 8) sem = 8;
    int added = 0;
    for (int ss = 1; ss <= sem; ++ss) {
        int cnt = sem_subject_count(ss);
        for (int j = 0; j < cnt; ++j) {
            const char *sname = sem_subjects[ss][j];
            int credit = sem_credits[ss][j];
            /* check existing */
            int exists = 0;
            for (int k = 0; k < s->num_subjects; ++k) {
                if (strcmp(s->subjects[k].name, sname) == 0) { exists = 1; break; }
            }
            if (exists) continue;
            if (s->num_subjects >= MAX_SUBJECTS) {
                /* cannot add more */
                continue;
            }
            safe_strncpy(s->subjects[s->num_subjects].name, sname, sizeof(s->subjects[s->num_subjects].name));
            s->subjects[s->num_subjects].credits = credit;
            s->subjects[s->num_subjects].marks = 0;
            s->subjects[s->num_subjects].classes_held = 0;
            s->subjects[s->num_subjects].classes_attended = 0;
            s->num_subjects++;
            added++;
        }
    }
    return added;
}

/* ---------- File operations (with migration support) ---------- */

void load_data(void) {
    /* If v2 file exists, load it */
    FILE *fp = fopen(DATA_FILE, "rb");
    if (fp) {
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
        return;
    }

    /* Else attempt migration from old file (students.dat) if present */
    FILE *fold = fopen(DATA_FILE_OLD, "rb");
    if (!fold) {
        /* nothing to load */
        student_count = 0;
        for (int i = 0; i < MAX_STUDENTS; ++i) students[i].exists = 0;
        return;
    }
    /* Read old header count and try to map old structs */
    int old_count = 0;
    if (fread(&old_count, sizeof(old_count), 1, fold) != 1) {
        fclose(fold);
        student_count = 0;
        for (int i = 0; i < MAX_STUDENTS; ++i) students[i].exists = 0;
        return;
    }
    if (old_count < 0 || old_count > MAX_STUDENTS) old_count = 0;
    OldStudent *oldbuf = calloc(old_count, sizeof(OldStudent));
    if (!oldbuf) { fclose(fold); student_count = 0; for (int i = 0; i < MAX_STUDENTS; ++i) students[i].exists = 0; return; }
    if (fread(oldbuf, sizeof(OldStudent), old_count, fold) < (size_t)old_count) {
        /* truncated maybe - we'll only use what we read */
    }
    fclose(fold);

    /* Map old to new */
    student_count = 0;
    for (int i = 0; i < old_count && student_count < MAX_STUDENTS; ++i) {
        if (!oldbuf[i].exists) continue;
        Student ns;
        memset(&ns, 0, sizeof(ns));
        ns.id = oldbuf[i].id;
        safe_strncpy(ns.name, oldbuf[i].name, sizeof(ns.name));
        ns.age = oldbuf[i].age;
        safe_strncpy(ns.dept, oldbuf[i].dept, sizeof(ns.dept));
        ns.year = oldbuf[i].year;
        ns.current_semester = 1;
        ns.num_subjects = oldbuf[i].num_subjects;
        if (ns.num_subjects > MAX_SUBJECTS) ns.num_subjects = MAX_SUBJECTS;
        for (int j = 0; j < ns.num_subjects; ++j) {
            safe_strncpy(ns.subjects[j].name, oldbuf[i].subjects[j].name, sizeof(ns.subjects[j].name));
            ns.subjects[j].marks = oldbuf[i].subjects[j].marks;
            ns.subjects[j].credits = oldbuf[i].subjects[j].credits;
            ns.subjects[j].classes_held = oldbuf[i].subjects[j].classes_held;
            ns.subjects[j].classes_attended = oldbuf[i].subjects[j].classes_attended;
        }
        safe_strncpy(ns.password, oldbuf[i].password, sizeof(ns.password));
        ns.exists = oldbuf[i].exists;
        ns.cgpa = oldbuf[i].cgpa;
        ns.total_credits_completed = oldbuf[i].total_credits_completed;
        /* email/phone empty */
        safe_strncpy(ns.email, "", sizeof(ns.email));
        safe_strncpy(ns.phone, "", sizeof(ns.phone));
        students[student_count++] = ns;
    }
    free(oldbuf);
    /* Save to new file for future loads */
    save_data();
    return;
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

void calculate_and_update_cgpa_for_student(int idx) {
    if (idx < 0 || idx >= student_count) return;
    Student *s = &students[idx];
    int sem_credits = 0; double sem_weighted = 0.0;
    for (int i = 0; i < s->num_subjects; ++i) {
        int cr = s->subjects[i].credits;
        int mk = s->subjects[i].marks;
        if (cr <= 0) continue;
        int gp = marks_to_grade_point(mk);
        sem_weighted += (double)gp * cr;
        sem_credits += cr;
    }
    double sgpa = (sem_credits > 0) ? sem_weighted / sem_credits : 0.0;
    int old_credits = s->total_credits_completed;
    double old_cgpa = s->cgpa;
    if (old_credits + sem_credits > 0) {
        double new_cgpa = ((old_cgpa * old_credits) + (sgpa * sem_credits)) / (double)(old_credits + sem_credits);
        s->cgpa = new_cgpa;
        s->total_credits_completed = old_credits + sem_credits;
    } else {
        s->cgpa = sgpa;
        s->total_credits_completed = sem_credits;
    }
    save_data();
    printf("SGPA for student %d (%s): %.3f\n", s->id, s->name, sgpa);
    printf("Updated CGPA: %.3f (Total credits: %d)\n", s->cgpa, s->total_credits_completed);
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
    fprintf(f, "<strong>Current Semester:</strong> %d<br>\n", s->current_semester);
    if (s->email[0]) { fprintf(f, "<strong>Email:</strong> "); html_escape(f, s->email); fprintf(f, "<br>\n"); }
    if (s->phone[0]) { fprintf(f, "<strong>Phone:</strong> "); html_escape(f, s->phone); fprintf(f, "<br>\n"); }
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
    fprintf(f, "</table>\n<p><strong>SGPA:</strong> %.3f<br>\n<strong>CGPA:</strong> %.3f<br>\n<strong>Total Credits Counted:</strong> %d</p>\n", sgpa, s->cgpa, s->total_credits_completed);
    fprintf(f, "</body>\n</html>\n");
    fclose(f);
}

/* ---------- CRUD / Menus (interactive) ---------- */

void print_student_short(Student *s) {
    printf("ID: %d | Name: %s | Year: %d | Dept: %s\n", s->id, s->name, s->year, s->dept);
}

void print_student_full(Student *s) {
    printf("------------- Student Profile -------------\n");
    printf("ID      : %d\nName    : %s\nAge     : %d\nDepartment: %s\nYear    : %d\nCurrent Semester: %d\nEmail   : %s\nPhone   : %s\nSubjects: %d\n",
           s->id, s->name, s->age, s->dept, s->year, s->current_semester, s->email, s->phone, s->num_subjects);
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

/* Adds a student record using provided Student struct (caller fills fields).
   Checks duplicates based on provided id (if non-zero).
*/
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
    students[idx] = *s;
    save_data();
    printf("Student added successfully. ID: %d\n", s->id);
}

/* Interactive add student (manual) */
void add_student(void) {
    Student s;
    memset(&s, 0, sizeof(s));
    s.exists = 1; s.cgpa = 0.0; s.total_credits_completed = 0;
    for (int i = 0; i < MAX_SUBJECTS; ++i) {
        s.subjects[i].classes_held = s.subjects[i].classes_attended = s.subjects[i].marks = s.subjects[i].credits = 0;
        s.subjects[i].name[0] = '\0';
    }

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
    printf("Enter department: "); fgets(s.dept, sizeof(s.dept), stdin); s.dept[strcspn(s.dept, "\n")] = 0;
    printf("Enter year (e.g., 1,2,3,4): ");
    while (scanf("%d", &s.year) != 1) { clear_stdin(); printf("Invalid. Enter year: "); }
    clear_stdin();

    printf("Enter email (optional): "); fgets(s.email, sizeof(s.email), stdin); s.email[strcspn(s.email, "\n")] = 0;
    printf("Enter phone (optional): "); fgets(s.phone, sizeof(s.phone), stdin); s.phone[strcspn(s.phone, "\n")] = 0;
    printf("Enter current semester (1-8), which will auto-add subjects up to this semester: ");
    while (scanf("%d", &s.current_semester) != 1 || s.current_semester < 1 || s.current_semester > 8) { clear_stdin(); printf("Invalid. Enter semester 1-8: "); }
    clear_stdin();

    if (find_duplicate_by_details(s.name, s.dept, s.year) != -1) { printf("Duplicate found. Registration cancelled.\n"); return; }

    s.num_subjects = 0;
    populate_subjects_for_semesters(&s, s.current_semester);

    printf("Set password for this student (no spaces): ");
    fgets(s.password, sizeof(s.password), stdin); s.password[strcspn(s.password, "\n")] = 0;
    add_student_custom(&s);
}

void student_self_register(void) {
    Student s;
    memset(&s, 0, sizeof(s));
    s.exists = 1; s.cgpa = 0.0; s.total_credits_completed = 0;
    for (int i = 0; i < MAX_SUBJECTS; ++i) s.subjects[i].name[0] = '\0';

    printf("Student Self-Registration\nEnter student ID (integer) or 0 to auto-generate: ");
    while (scanf("%d", &s.id) != 1) { clear_stdin(); printf("Invalid. Enter student ID (integer) or 0 to auto-generate: "); }
    clear_stdin();
    if (s.id == 0) s.id = generate_unique_id();
    else if (find_index_by_id(s.id) != -1) { printf("ID exists. Use login instead.\n"); return; }

    printf("Enter full name: "); fgets(s.name, sizeof(s.name), stdin); s.name[strcspn(s.name, "\n")] = 0;
    printf("Enter age: "); while (scanf("%d", &s.age) != 1) { clear_stdin(); printf("Invalid. Enter age: "); } clear_stdin();
    printf("Enter department: "); fgets(s.dept, sizeof(s.dept), stdin); s.dept[strcspn(s.dept, "\n")] = 0;
    printf("Enter year (e.g., 1,2,3,4): "); while (scanf("%d", &s.year) != 1) { clear_stdin(); printf("Invalid. Enter year: "); } clear_stdin();
    printf("Enter email (optional): "); fgets(s.email, sizeof(s.email), stdin); s.email[strcspn(s.email, "\n")] = 0;
    printf("Enter phone (optional): "); fgets(s.phone, sizeof(s.phone), stdin); s.phone[strcspn(s.phone, "\n")] = 0;
    printf("Enter current semester (1-8), which will auto-add subjects up to this semester: ");
    while (scanf("%d", &s.current_semester) != 1 || s.current_semester < 1 || s.current_semester > 8) { clear_stdin(); printf("Invalid. Enter semester 1-8: "); }
    clear_stdin();

    if (find_duplicate_by_details(s.name, s.dept, s.year) != -1) { printf("Duplicate. Registration cancelled.\n"); return; }

    s.num_subjects = 0;
    populate_subjects_for_semesters(&s, s.current_semester);

    printf("Set password for this student (no spaces): ");
    fgets(s.password, sizeof(s.password), stdin); s.password[strcspn(s.password, "\n")] = 0;
    add_student_custom(&s);
    printf("Registration complete. Use your ID and password to login.\n");
}

/* Remaining interactive functions (edit/delete/search/attendance) are preserved and lightly updated
   to account for increased subject array size and new student fields.
*/

void edit_student(void) {
    int id;
    printf("Enter student ID to edit: ");
    while (scanf("%d", &id) != 1) { clear_stdin(); printf("Invalid. Enter student ID: "); }
    clear_stdin();
    int idx = find_index_by_id(id);
    if (idx == -1) { printf("Student not found.\n"); return; }
    Student *s = &students[idx];
    print_student_full(s);
    printf("What to edit?\n1) Name\n2) Age\n3) Dept\n4) Year\n5) Current semester & auto-add subjects\n6) Subjects\n7) Password\n8) Email\n9) Phone\n10) Cancel\nChoose: ");
    int ch;
    while (scanf("%d", &ch) != 1) { clear_stdin(); printf("Invalid. Choose: "); }
    clear_stdin();
    switch (ch) {
        case 1: printf("New name: "); fgets(s->name, sizeof(s->name), stdin); s->name[strcspn(s->name, "\n")] = 0; break;
        case 2: printf("New age: "); while (scanf("%d", &s->age) != 1) { clear_stdin(); printf("Invalid. Enter age: "); } clear_stdin(); break;
        case 3: printf("New dept: "); fgets(s->dept, sizeof(s->dept), stdin); s->dept[strcspn(s->dept, "\n")] = 0; break;
        case 4: printf("New year: "); while (scanf("%d", &s->year) != 1) { clear_stdin(); printf("Invalid. Enter year: "); } clear_stdin(); break;
        case 5: {
            printf("Set new current semester (1-8): ");
            int sem; while (scanf("%d", &sem) != 1 || sem < 1 || sem > 8) { clear_stdin(); printf("Invalid. Enter semester 1-8: "); } clear_stdin();
            s->current_semester = sem;
            populate_subjects_for_semesters(s, sem);
            break;
        }
        case 6: {
            printf("Subjects:\n"); for (int i = 0; i < s->num_subjects; ++i) printf("%d) %s\n", i+1, s->subjects[i].name);
            printf("Enter subject number to rename: ");
            int sn; while (scanf("%d", &sn) != 1 || sn < 1 || sn > s->num_subjects) { clear_stdin(); printf("Invalid. Enter subject number: "); }
            clear_stdin(); printf("New subject name: "); fgets(s->subjects[sn-1].name, sizeof(s->subjects[sn-1].name), stdin); s->subjects[sn-1].name[strcspn(s->subjects[sn-1].name, "\n")] = 0;
            break;
        }
        case 7: printf("New password: "); fgets(s->password, sizeof(s->password), stdin); s->password[strcspn(s->password, "\n")] = 0; break;
        case 8: printf("New email: "); fgets(s->email, sizeof(s->email), stdin); s->email[strcspn(s->email, "\n")] = 0; break;
        case 9: printf("New phone: "); fgets(s->phone, sizeof(s->phone), stdin); s->phone[strcspn(s->phone, "\n")] = 0; break;
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

/* ---------- Attendance functions (APIs) ---------- */

/* Mark attendance for a given student and subject index (present=1 or 0). */
int api_mark_attendance_for_student_subject(int student_id, int subject_index, int present) {
    int idx = find_index_by_id(student_id);
    if (idx == -1) return -1;
    Student *s = &students[idx];
    if (subject_index < 0 || subject_index >= s->num_subjects) return -2;
    s->subjects[subject_index].classes_held += 1;
    if (present) s->subjects[subject_index].classes_attended += 1;
    save_data();
    return 0;
}

/* Mark attendance for a subject name across all students for a given date.
   present_ids is an array of student IDs who are present; count is number of present IDs.
   If date_str is non-NULL it will be used to write a log entry in "attendance_<date>.csv"
   Format of CSV: id,subject_name,present(1/0)
*/
int api_mark_attendance_for_subject_on_date(const char *subject_name, const int *present_ids, int present_count, const char *date_str) {
    if (!subject_name) return -1;
    int any = 0;
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        Student *s = &students[i];
        for (int j = 0; j < s->num_subjects; ++j) {
            if (strcmp(s->subjects[j].name, subject_name) == 0) {
                any = 1;
                int present = 0;
                for (int k = 0; k < present_count; ++k) if (present_ids[k] == s->id) { present = 1; break; }
                s->subjects[j].classes_held += 1;
                if (present) s->subjects[j].classes_attended += 1;
            }
        }
    }
    if (!any) return -2;

    /* optional logging */
    if (date_str) {
        MKDIR("attendance");
        char path[512];
        snprintf(path, sizeof(path), "attendance/attendance_%s.csv", date_str);
        FILE *f = fopen(path, "a");
        if (f) {
            for (int i = 0; i < student_count; ++i) {
                if (!students[i].exists) continue;
                Student *s = &students[i];
                for (int j = 0; j < s->num_subjects; ++j) {
                    if (strcmp(s->subjects[j].name, subject_name) == 0) {
                        int present = 0;
                        for (int k = 0; k < present_count; ++k) if (present_ids[k] == s->id) { present = 1; break; }
                        fprintf(f, "%d,%s,%d\n", s->id, subject_name, present);
                    }
                }
            }
            fclose(f);
        }
    }

    save_data();
    return 0;
}

/* ---------- Student view functions ---------- */

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

/* ---------- Interactive admin marks handler ---------- */

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
        printf("Subject %d) %s\n", i+1, s->subjects[i].name);
        printf("  Enter marks (0-100): ");
        while (scanf("%d", &mk) != 1 || mk < 0 || mk > 100) { clear_stdin(); printf("Invalid. Enter marks (0-100): "); }
        clear_stdin();
        s->subjects[i].marks = mk;
        /* keep credits as existing */
    }

    /* Recalculate CGPA and save */
    calculate_and_update_cgpa_for_student(idx);

    /* Optionally generate report immediately */
    MKDIR(REPORTS_DIR);
    generate_html_report(idx, NULL, NULL, NULL);

    save_data();
    printf("Marks entered and CGPA updated for ID %d. Report generated (if possible).\n", id);
}

/* ---------- main menus & auth ---------- */

int admin_menu(void) {
    while (1) {
        printf("\n=== ADMIN MENU ===\n1) Add student\n2) Edit student\n3) Delete student\n4) List students\n5) Search student\n6) Mark attendance (class)\n7) Mark attendance (single student)\n8) Increment classes held only\n9) Attendance report (subject)\n10) Enter marks & update CGPA for a student (generate report)\n11) Logout\nChoose: ");
        int ch; while (scanf("%d", &ch) != 1) { clear_stdin(); printf("Invalid. Choose: "); } clear_stdin();
        switch (ch) {
            case 1: add_student(); break;
            case 2: edit_student(); break;
            case 3: delete_student(); break;
            case 4: { printf("List of students:\n"); for (int i=0;i<student_count;i++) if (students[i].exists) print_student_short(&students[i]); break; }
            case 5: search_student(); break;
            case 6: {
                /* interactive class attendance by subject name and check present y/n */
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
                break;
            }
            case 7: {
                int id; printf("Enter student ID: "); while (scanf("%d", &id) != 1) { clear_stdin(); printf("Invalid. Enter student ID: "); } clear_stdin();
                int idx = find_index_by_id(id); if (idx == -1) { printf("Not found.\n"); break; }
                Student *s = &students[idx];
                for (int i = 0; i < s->num_subjects; ++i) printf("%d) %s (Attended %d / Held %d)\n", i+1, s->subjects[i].name, s->subjects[i].classes_attended, s->subjects[i].classes_held);
                printf("Choose subject number: "); int sn; while (scanf("%d", &sn) != 1 || sn < 1 || sn > s->num_subjects) { clear_stdin(); printf("Invalid. Choose subject number: "); } clear_stdin();
                int idxs = sn - 1; s->subjects[idxs].classes_held += 1;
                printf("Present? (y/n): "); char c = getchar(); clear_stdin();
                if (c == 'y' || c == 'Y') s->subjects[idxs].classes_attended += 1;
                save_data(); printf("Attendance updated.\n");
                break;
            }
            case 8: {
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
                break;
            }
            case 9: {
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
                break;
            }
            case 10: {
                admin_enter_marks_and_update_cgpa();
                break;
            }
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
                char oldp[64], newp[64];
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
    char user[64], pass[64];
    printf("Admin Username: "); fgets(user, sizeof(user), stdin); user[strcspn(user, "\n")] = 0;
    printf("Admin Password: "); fgets(pass, sizeof(pass), stdin); pass[strcspn(pass, "\n")] = 0;
    if (strcmp(user, ADMIN_USER) == 0 && strcmp(pass, ADMIN_PASS) == 0) { printf("Admin authenticated.\n"); admin_menu(); }
    else printf("Invalid admin credentials.\n");
}

void student_login(void) {
    int id; printf("Enter student ID: "); while (scanf("%d", &id) != 1) { clear_stdin(); printf("Invalid. Enter student ID: "); } clear_stdin();
    int idx = find_index_by_id(id); if (idx == -1) { printf("Student ID not found.\n"); return; }
    char pass[64]; printf("Enter password: "); fgets(pass, sizeof(pass), stdin); pass[strcspn(pass, "\n")] = 0;
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
   name|age|dept|year|num_subjects|subject1,subject2,...|password
   NOTE: this older CLI format does not include email/phone/current_semester.
*/
int cli_add_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Unable to open add-file: %s\n", path); return 1; }
    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); fprintf(stderr, "Empty add-file\n"); return 1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    char *parts[8]; int pi = 0;
    char *tok = strtok(line, "|");
    while (tok && pi < 8) { parts[pi++] = tok; tok = strtok(NULL, "|"); }
    if (pi < 7) { fprintf(stderr, "Invalid add-file format\n"); return 1; }
    Student s; memset(&s, 0, sizeof(s)); s.exists = 1; s.cgpa = 0.0; s.total_credits_completed = 0;
    safe_strncpy(s.name, parts[0], sizeof(s.name));
    s.age = atoi(parts[1]); safe_strncpy(s.dept, parts[2], sizeof(s.dept)); s.year = atoi(parts[3]);
    s.num_subjects = atoi(parts[4]); if (s.num_subjects < 1 || s.num_subjects > MAX_SUBJECTS) s.num_subjects = MAX_SUBJECTS;
    char *subtok = strtok(parts[5], ",");
    int si = 0;
    while (subtok && si < s.num_subjects) { while (*subtok == ' ') subtok++; safe_strncpy(s.subjects[si].name, subtok, sizeof(s.subjects[si].name)); si++; subtok = strtok(NULL, ","); }
    safe_strncpy(s.password, parts[6], sizeof(s.password));
    s.id = generate_unique_id();
    add_student_custom(&s);
    printf("Added student ID %d\n", s.id);
    return 0;
}

/* marks file:
   first line: id
   subsequent lines: mark,credit
*/
int cli_enter_marks_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Unable to open marks file %s\n", path); return 1; }
    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); fprintf(stderr, "Invalid marks file\n"); return 1; }
    int id = atoi(line); int idx = find_index_by_id(id);
    if (idx == -1) { fclose(f); fprintf(stderr, "Student not found\n"); return 1; }
    Student *s = &students[idx];
    int i = 0;
    while (fgets(line, sizeof(line), f) && i < s->num_subjects) {
        int mk=0, cr=0;
        if (sscanf(line, "%d,%d", &mk, &cr) == 2) {
            s->subjects[i].marks = mk; s->subjects[i].credits = cr;
        }
        i++;
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
        printf("%d|%s|%d|%s\n", students[i].id, students[i].name, students[i].year, students[i].dept);
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
    /* If current_semester present, populate that student's subjects up to that semester */
    if (s->current_semester <= 0) s->current_semester = 1;
    populate_subjects_for_semesters(s, s->current_semester);
    add_student_custom(s); /* add_student_custom also checks again and saves */
    return s->id;
}

void api_generate_report(int idx, const char* college, const char* semester, const char* exam) { generate_html_report(idx, college, semester, exam); }

int api_calculate_update_cgpa(int idx) { calculate_and_update_cgpa_for_student(idx); return 0; }

/* ---------- new: admin auth API ---------- */
int api_admin_auth(const char *user, const char *pass) {
    if (!user || !pass) return 0;
    if (strcmp(user, ADMIN_USER) == 0 && strcmp(pass, ADMIN_PASS) == 0) return 1;
    return 0;
}

/* ---------- new: attendance API wrappers exposed to web layer ---------- */
/* mark attendance for subject on date (present_ids is array of present student IDs) */
int api_mark_attendance_for_subject_on_date_wrapper(const char *subject_name, const int *present_ids, int present_count, const char *date_str) {
    return api_mark_attendance_for_subject_on_date(subject_name, present_ids, present_count, date_str);
}

/* mark attendance for a single student subject index */
int api_mark_attendance_for_student_subject_wrapper(int student_id, int subject_index, int present) {
    return api_mark_attendance_for_student_subject(student_id, subject_index, present);
}

/* ---------- main (interactive) ---------- */

#ifndef BUILD_WEB
int main(int argc, char **argv) {
    /* support demo and CLI modes when run directly (useful for testing) */
    if (argc > 1) {
        load_data();
        if (strcmp(argv[1], "--demo") == 0) {
            printf("Demo Mode: Student Management System\n1) Add Student: ID=1001, Name=Tanay Sah, Year=1, Dept=CS\n2) Add Student: ID=1002, Name=Riya Sharma, Year=1, Dept=CS\n");
            return 0;
        } else if (strcmp(argv[1], "--list") == 0) {
            cli_list(); return 0;
        } else if (strcmp(argv[1], "--view") == 0 && argc > 2) {
            int id = atoi(argv[2]); cli_view(id); return 0;
        } else if (strcmp(argv[1], "--generate-report") == 0 && argc > 2) {
            cli_generate_report_arg(argv[2]); return 0;
        } else if (strcmp(argv[1], "--add-file") == 0 && argc > 2) {
            load_data(); cli_add_from_file(argv[2]); return 0;
        } else if (strcmp(argv[1], "--enter-marks-file") == 0 && argc > 2) {
            load_data(); cli_enter_marks_file(argv[2]); return 0;
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
