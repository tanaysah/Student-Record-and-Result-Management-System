/* student_system.c
   Student Record, Attendance, Grades & Printable Result Report
   - Interactive console program (admin + student)
   - Non-interactive CLI modes for web hosting / automated use
   - API wrappers for linking with a separate web-server binary

   Build:
     Console: gcc student_system.c -o student_system
     Web: gcc -DBUILD_WEB student_system.c student_system_web.c -o student_system_web

   Notes:
     - Data persisted to students.dat (binary)
     - Reports written to reports/<id>_result.html
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
#define MAX_SUBJECTS 8
#define MAX_SUB_NAME 50
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
    int num_subjects;
    Subject subjects[MAX_SUBJECTS];
    char password[50];
    int exists;
    double cgpa;
    int total_credits_completed;
} Student;

/* Global in-memory storage */
static Student students[MAX_STUDENTS];
static int student_count = 0;

/* ---------- Utility ---------- */
static void safe_strncpy(char *dst, const char *src, size_t n) {
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
    for (; *s; ++s) *s = tolower((unsigned char)*s);
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
        /* truncated file — tolerate */
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
    printf("ID      : %d\nName    : %s\nAge     : %d\nDepartment: %s\nYear    : %d\nSubjects: %d\n",
           s->id, s->name, s->age, s->dept, s->year, s->num_subjects);
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

void add_student_custom(Student *s) {
    if (!s) return;
    int idx = next_free_spot();
    if (idx == -1) { printf("Maximum students reached.\n"); return; }
    if (s->id == 0) s->id = generate_unique_id();
    students[idx] = *s;
    save_data();
    printf("Student added successfully. ID: %d\n", s->id);
}

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

    if (find_duplicate_by_details(s.name, s.dept, s.year) != -1) { printf("Duplicate found. Registration cancelled.\n"); return; }
    printf("How many subjects (max %d)? ", MAX_SUBJECTS);
    while (scanf("%d", &s.num_subjects) != 1 || s.num_subjects < 1 || s.num_subjects > MAX_SUBJECTS) {
        clear_stdin(); printf("Invalid. Enter number between 1 and %d: ", MAX_SUBJECTS);
    }
    clear_stdin();
    for (int i = 0; i < s.num_subjects; ++i) {
        printf("Enter name of subject %d: ", i+1);
        fgets(s.subjects[i].name, sizeof(s.subjects[i].name), stdin); s.subjects[i].name[strcspn(s.subjects[i].name, "\n")] = 0;
        s.subjects[i].classes_held = s.subjects[i].classes_attended = s.subjects[i].marks = s.subjects[i].credits = 0;
    }
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

    if (find_duplicate_by_details(s.name, s.dept, s.year) != -1) { printf("Duplicate. Registration cancelled.\n"); return; }
    printf("How many subjects (max %d)? ", MAX_SUBJECTS);
    while (scanf("%d", &s.num_subjects) != 1 || s.num_subjects < 1 || s.num_subjects > MAX_SUBJECTS) {
        clear_stdin(); printf("Invalid. Enter number between 1 and %d: ", MAX_SUBJECTS);
    }
    clear_stdin();
    for (int i = 0; i < s.num_subjects; ++i) {
        printf("Enter name of subject %d: ", i+1);
        fgets(s.subjects[i].name, sizeof(s.subjects[i].name), stdin); s.subjects[i].name[strcspn(s.subjects[i].name, "\n")] = 0;
        s.subjects[i].classes_held = s.subjects[i].classes_attended = s.subjects[i].marks = s.subjects[i].credits = 0;
    }
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
    printf("What to edit?\n1) Name\n2) Age\n3) Dept\n4) Year\n5) Subjects\n6) Password\n7) Cancel\nChoose: ");
    int ch;
    while (scanf("%d", &ch) != 1) { clear_stdin(); printf("Invalid. Choose: "); }
    clear_stdin();
    switch (ch) {
        case 1: printf("New name: "); fgets(s->name, sizeof(s->name), stdin); s->name[strcspn(s->name, "\n")] = 0; break;
        case 2: printf("New age: "); while (scanf("%d", &s->age) != 1) { clear_stdin(); printf("Invalid. Enter age: "); } clear_stdin(); break;
        case 3: printf("New dept: "); fgets(s->dept, sizeof(s->dept), stdin); s->dept[strcspn(s->dept, "\n")] = 0; break;
        case 4: printf("New year: "); while (scanf("%d", &s->year) != 1) { clear_stdin(); printf("Invalid. Enter year: "); } clear_stdin(); break;
        case 5: {
            printf("Subjects:\n"); for (int i = 0; i < s->num_subjects; ++i) printf("%d) %s\n", i+1, s->subjects[i].name);
            printf("Enter subject number to rename: ");
            int sn; while (scanf("%d", &sn) != 1 || sn < 1 || sn > s->num_subjects) { clear_stdin(); printf("Invalid. Enter subject number: "); }
            clear_stdin(); printf("New subject name: "); fgets(s->subjects[sn-1].name, sizeof(s->subjects[sn-1].name), stdin); s->subjects[sn-1].name[strcspn(s->subjects[sn-1].name, "\n")] = 0;
            break;
        }
        case 6: printf("New password: "); fgets(s->password, sizeof(s->password), stdin); s->password[strcspn(s->password, "\n")] = 0; break;
        case 7: printf("Edit cancelled.\n"); return;
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

/* Attendance functions kept similar to previous implementation (omitted here for brevity) */
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

/* Menus & auth */
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
            case 6: mark_attendance_for_class(); break;
            case 7: mark_attendance_single_student(); break;
            case 8: increment_classes_held_only(); break;
            case 9: attendance_report_subject(); break;
            case 10: {
                /* reuse admin_enter_marks_and_update_cgpa behavior */
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
   name|age|dept|year|num_subjects|subject1,subject2,...|password
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
int api_add_student(Student *s) { if (!s) return -1; if (s->id == 0) s->id = generate_unique_id(); add_student_custom(s); return s->id; }
void api_generate_report(int idx, const char* college, const char* semester, const char* exam) { generate_html_report(idx, college, semester, exam); }
int api_calculate_update_cgpa(int idx) { calculate_and_update_cgpa_for_student(idx); return 0; }

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
