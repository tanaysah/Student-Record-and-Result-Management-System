/*
 Student Record, Attendance, Grades & Printable Result Report
 Compile: gcc student_system.c -o student_system
 Run: ./student_system
 Notes:
  - Admin can enter marks -> SGPA & CGPA auto-calculated -> HTML report generated at reports/<id>_result.html
  - Students can self-register and view their report path from student menu.
  - Backup old students.dat if needed.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
  #include <direct.h>
  #define MKDIR(dir) _mkdir(dir)
  #include <io.h>
  #define isatty _isatty
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #define MKDIR(dir) mkdir(dir, 0755)
  #include <unistd.h>
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
    int marks;      // marks obtained in this subject for the semester (0-100)
    int credits;    // credit points for this subject (e.g., 3, 4)
} Subject;

typedef struct {
    int id;
    char name[MAX_NAME];
    int age;
    char dept[MAX_NAME];
    int year;
    int num_subjects;
    Subject subjects[MAX_SUBJECTS];
    char password[50]; // plain text for demo
    int exists; // flag to indicate record exists

    /* New fields for grades */
    double cgpa;                  // cumulative GPA so far
    int total_credits_completed;  // total credits counted in CGPA so far
} Student;

Student students[MAX_STUDENTS];
int student_count = 0;

/* ----- Utility functions ----- */

void clear_stdin() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
}

void pause_console() {
    printf("\nPress Enter to continue...");
    getchar();
}

void to_lower_str(char *s) {
    for (; *s; ++s) *s = tolower((unsigned char)*s);
}

/* ----- File operations ----- */

void load_data() {
    FILE *fp = fopen(DATA_FILE, "rb");
    if (!fp) {
        student_count = 0;
        for (int i = 0; i < MAX_STUDENTS; ++i) students[i].exists = 0;
        return;
    }
    fread(&student_count, sizeof(int), 1, fp);
    if (student_count > MAX_STUDENTS) student_count = MAX_STUDENTS;
    fread(students, sizeof(Student), student_count, fp);
    fclose(fp);
}

void save_data() {
    FILE *fp = fopen(DATA_FILE, "wb");
    if (!fp) {
        perror("Unable to save data");
        return;
    }
    fwrite(&student_count, sizeof(int), 1, fp);
    fwrite(students, sizeof(Student), student_count, fp);
    fclose(fp);
}

/* Find first free index or index by ID */
int find_index_by_id(int id) {
    for (int i = 0; i < student_count; ++i) {
        if (students[i].exists && students[i].id == id) return i;
    }
    return -1;
}

int next_free_spot() {
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) return i;
    }
    if (student_count < MAX_STUDENTS) {
        students[student_count].exists = 0;
        return student_count++;
    }
    return -1;
}

/* generate a unique ID (start at 1001) */
int generate_unique_id() {
    int id = 1001;
    while (find_index_by_id(id) != -1) id++;
    return id;
}

/* check duplicate by name+dept+year (case-insensitive). returns index if found, -1 otherwise */
int find_duplicate_by_details(const char *name, const char *dept, int year) {
    char name_low[MAX_NAME], dept_low[MAX_NAME];
    strncpy(name_low, name, sizeof(name_low)); name_low[sizeof(name_low)-1] = 0;
    strncpy(dept_low, dept, sizeof(dept_low)); dept_low[sizeof(dept_low)-1] = 0;
    to_lower_str(name_low);
    to_lower_str(dept_low);

    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        char other_name[MAX_NAME], other_dept[MAX_NAME];
        strncpy(other_name, students[i].name, sizeof(other_name)); other_name[sizeof(other_name)-1] = 0;
        strncpy(other_dept, students[i].dept, sizeof(other_dept)); other_dept[sizeof(other_dept)-1] = 0;
        to_lower_str(other_name);
        to_lower_str(other_dept);
        if (strcmp(name_low, other_name) == 0 && strcmp(dept_low, other_dept) == 0 && students[i].year == year) {
            return i;
        }
    }
    return -1;
}

/* ----- SGPA / CGPA functions ----- */

int marks_to_grade_point(int marks) {
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
        weighted_sum += gp * cr;
        total_credits += cr;
    }
    if (total_credits == 0) return 0.0;
    return weighted_sum / (double)total_credits;
}

/* Forward declare HTML generation */
void generate_html_report(int idx, const char* college, const char* semester, const char* exam);

/* Calculate SGPA & update student's CGPA using current semester data.
   After updating, optionally generate a printable report (caller controls that). */
void calculate_and_update_cgpa_for_student(int idx) {
    if (idx < 0 || idx >= student_count) return;
    Student *s = &students[idx];

    int semester_credits = 0;
    double semester_weighted = 0.0;
    for (int i = 0; i < s->num_subjects; ++i) {
        int cr = s->subjects[i].credits;
        int mk = s->subjects[i].marks;
        if (cr <= 0) continue;
        int gp = marks_to_grade_point(mk);
        semester_weighted += gp * cr;
        semester_credits += cr;
    }

    double sgpa = 0.0;
    if (semester_credits > 0) sgpa = semester_weighted / (double)semester_credits;

    int old_credits = s->total_credits_completed;
    double old_cgpa = s->cgpa;

    if (old_credits + semester_credits > 0) {
        double new_cgpa = ((old_cgpa * old_credits) + (sgpa * semester_credits)) / (double)(old_credits + semester_credits);
        s->cgpa = new_cgpa;
        s->total_credits_completed = old_credits + semester_credits;
    } else {
        s->cgpa = sgpa;
        s->total_credits_completed = semester_credits;
    }

    save_data();
    printf("SGPA for student %d (%s): %.3f\n", s->id, s->name, sgpa);
    printf("Updated CGPA: %.3f (Total credits: %d)\n", s->cgpa, s->total_credits_completed);
}

/* Convenience: admin triggers enter marks then calculate/update CGPA and ask to generate report */
/* Robust admin marks entry + SGPA/CGPA update + report generation
   Uses fgets everywhere to avoid scanf/fgets mixups that cause crashes. */
void admin_enter_marks_and_update_cgpa() {
    char buf[256];
    int id = -1;
    int idx = -1;

    /* ask for student id */
    while (1) {
        printf("Enter student ID to input marks for: ");
        if (!fgets(buf, sizeof(buf), stdin)) {
            printf("Input error. Aborting marks entry.\n");
            return;
        }
        if (sscanf(buf, "%d", &id) == 1) {
            idx = find_index_by_id(id);
            if (idx == -1) {
                printf("Student with ID %d not found. Try again or type 0 to cancel.\n", id);
                continue;
            }
            break;
        } else {
            printf("Invalid ID. Try again.\n");
        }
    }

    Student *s = &students[idx];
    printf("Entering marks & credits for %s (ID %d)\n", s->name, s->id);

    /* For each subject, get marks and credits using fgets+sscanf */
    for (int i = 0; i < s->num_subjects; ++i) {
        int mk = -1, cr = -1;
        /* marks */
        while (1) {
            printf("Subject %d: %s\n", i + 1, s->subjects[i].name);
            printf("  Enter marks (0-100): ");
            if (!fgets(buf, sizeof(buf), stdin)) {
                printf("Input error. Aborting.\n");
                return;
            }
            if (sscanf(buf, "%d", &mk) == 1 && mk >= 0 && mk <= 100) break;
            printf("  Invalid marks. Please enter an integer 0-100.\n");
        }

        /* credits */
        while (1) {
            printf("  Enter credits for this subject (positive integer): ");
            if (!fgets(buf, sizeof(buf), stdin)) {
                printf("Input error. Aborting.\n");
                return;
            }
            if (sscanf(buf, "%d", &cr) == 1 && cr > 0) break;
            printf("  Invalid credits. Please enter a positive integer.\n");
        }

        s->subjects[i].marks = mk;
        s->subjects[i].credits = cr;
    }

    /* Calculate & update CGPA */
    calculate_and_update_cgpa_for_student(idx);

    /* Ask admin if they want to generate a printable report now */
    printf("Do you want to generate a printable HTML result for this student now? (y/n): ");
    if (!fgets(buf, sizeof(buf), stdin)) {
        printf("Input error. Skipping report generation.\n");
        return;
    }
    if (buf[0] == 'y' || buf[0] == 'Y') {
        char college[200] = {0}, semester[100] = {0}, exam[100] = {0};

        printf("Enter College/Institute Name for report (press Enter for 'Your College'): ");
        if (!fgets(college, sizeof(college), stdin)) college[0] = '\0';
        college[strcspn(college, "\n")] = '\0';
        if (strlen(college) == 0) strcpy(college, "Your College");

        printf("Enter Semester (e.g., 'Semester 2') or press Enter: ");
        if (!fgets(semester, sizeof(semester), stdin)) semester[0] = '\0';
        semester[strcspn(semester, "\n")] = '\0';
        if (strlen(semester) == 0) strcpy(semester, "Semester -");

        printf("Enter Exam name (e.g., 'Midterm / End Semester') or press Enter: ");
        if (!fgets(exam, sizeof(exam), stdin)) exam[0] = '\0';
        exam[strcspn(exam, "\n")] = '\0';
        if (strlen(exam) == 0) strcpy(exam, "Exam -");

        /* Ensure reports dir exists */
        if (MKDIR(REPORTS_DIR) != 0) {
            /* ignore potential error; dir may already exist */
        }
        generate_html_report(idx, college, semester, exam);
        printf("If successful, report is at '%s/%d_result.html'\n", REPORTS_DIR, s->id);
    } else {
        printf("Skipping report generation.\n");
    }
}

/* For students: view SGPA (calculated from stored marks) without updating CGPA */
void student_view_sgpa_and_cgpa(int idx) {
    if (idx < 0 || idx >= student_count) return;
    Student *s = &students[idx];
    double sgpa = calculate_sgpa_for_student(s);
    printf("Student: %s (ID %d)\n", s->name, s->id);
    printf("SGPA (current semester based on stored marks): %.3f\n", sgpa);
    printf("CGPA (stored): %.3f (Total credits: %d)\n", s->cgpa, s->total_credits_completed);
    // show path to report (if present)
    char path[512];
    snprintf(path, sizeof(path), "%s/%d_result.html", REPORTS_DIR, s->id);
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        printf("Printable result available at: %s\n", path);
    } else {
        printf("No printable result generated yet for this student.\n");
    }
}

/* ----- Print / CRUD / Attendance functions ----- */

void print_student_short(Student *s) {
    printf("ID: %d | Name: %s | Year: %d | Dept: %s\n", s->id, s->name, s->year, s->dept);
}

void print_student_full(Student *s) {
    printf("------------- Student Profile -------------\n");
    printf("ID      : %d\n", s->id);
    printf("Name    : %s\n", s->name);
    printf("Age     : %d\n", s->age);
    printf("Department: %s\n", s->dept);
    printf("Year    : %d\n", s->year);
    printf("Subjects: %d\n", s->num_subjects);
    for (int i = 0; i < s->num_subjects; ++i) {
        Subject *sub = &s->subjects[i];
        int held = sub->classes_held;
        int att = sub->classes_attended;
        double pct = (held == 0) ? 0.0 : ((double)att / (double)held) * 100.0;
        printf("  %d) %s - Attended %d / %d (%.2f%%) | Marks: %d | Credits: %d\n",
               i + 1, sub->name, att, held, pct, sub->marks, sub->credits);
    }
    double sgpa = calculate_sgpa_for_student(s);
    printf("Current semester SGPA (based on stored marks): %.3f\n", sgpa);
    printf("Stored CGPA: %.3f (Total credits: %d)\n", s->cgpa, s->total_credits_completed);
    printf("-------------------------------------------\n");
}

/* Create a new student (used by admin and registration) */
void add_student_custom(Student *s) {
    int idx = next_free_spot();
    if (idx == -1) {
        printf("Maximum student limit reached.\n");
        return;
    }
    students[idx] = *s;
    save_data();
    printf("Student added successfully. ID: %d\n", s->id);
}

/* Create a new student via Admin (keeps old flow) */
void add_student() {
    Student s;
    s.exists = 1;
    s.cgpa = 0.0;
    s.total_credits_completed = 0;
    for (int i = 0; i < MAX_SUBJECTS; ++i) {
        s.subjects[i].classes_held = 0;
        s.subjects[i].classes_attended = 0;
        s.subjects[i].marks = 0;
        s.subjects[i].credits = 0;
        s.subjects[i].name[0] = '\0';
    }

    printf("Enter student ID (integer) or 0 to auto-generate: ");
    while (scanf("%d", &s.id) != 1) {
        clear_stdin();
        printf("Invalid. Enter student ID (integer) or 0 to auto-generate: ");
    }
    clear_stdin();
    if (s.id == 0) {
        s.id = generate_unique_id();
        printf("Assigned ID: %d\n", s.id);
    } else if (find_index_by_id(s.id) != -1) {
        printf("Student with ID %d already exists.\n", s.id);
        return;
    }

    printf("Enter full name: ");
    fgets(s.name, sizeof(s.name), stdin);
    s.name[strcspn(s.name, "\n")] = 0;
    printf("Enter age: ");
    while (scanf("%d", &s.age) != 1) {
        clear_stdin();
        printf("Invalid. Enter age: ");
    }
    clear_stdin();
    printf("Enter department: ");
    fgets(s.dept, sizeof(s.dept), stdin);
    s.dept[strcspn(s.dept, "\n")] = 0;
    printf("Enter year (e.g., 1,2,3,4): ");
    while (scanf("%d", &s.year) != 1) {
        clear_stdin();
        printf("Invalid. Enter year: ");
    }
    clear_stdin();

    if (find_duplicate_by_details(s.name, s.dept, s.year) != -1) {
        printf("A student with the same name, department and year already exists. Registration cancelled.\n");
        return;
    }

    printf("How many subjects (max %d)? ", MAX_SUBJECTS);
    while (scanf("%d", &s.num_subjects) != 1 || s.num_subjects < 1 || s.num_subjects > MAX_SUBJECTS) {
        clear_stdin();
        printf("Invalid. Enter number between 1 and %d: ", MAX_SUBJECTS);
    }
    clear_stdin();
    for (int i = 0; i < s.num_subjects; ++i) {
        printf("Enter name of subject %d: ", i + 1);
        fgets(s.subjects[i].name, sizeof(s.subjects[i].name), stdin);
        s.subjects[i].name[strcspn(s.subjects[i].name, "\n")] = 0;
        s.subjects[i].classes_held = 0;
        s.subjects[i].classes_attended = 0;
        s.subjects[i].marks = 0;
        s.subjects[i].credits = 0;
    }
    printf("Set password for this student (no spaces): ");
    fgets(s.password, sizeof(s.password), stdin);
    s.password[strcspn(s.password, "\n")] = 0;

    s.exists = 1;
    add_student_custom(&s);
}

/* Student self-registration (new) */
void student_self_register() {
    Student s;
    s.exists = 1;
    s.cgpa = 0.0;
    s.total_credits_completed = 0;
    for (int i = 0; i < MAX_SUBJECTS; ++i) {
        s.subjects[i].classes_held = 0;
        s.subjects[i].classes_attended = 0;
        s.subjects[i].marks = 0;
        s.subjects[i].credits = 0;
        s.subjects[i].name[0] = '\0';
    }

    printf("Student Self-Registration\n");
    printf("Enter student ID (integer) or 0 to auto-generate: ");
    while (scanf("%d", &s.id) != 1) {
        clear_stdin();
        printf("Invalid. Enter student ID (integer) or 0 to auto-generate: ");
    }
    clear_stdin();

    if (s.id == 0) {
        s.id = generate_unique_id();
        printf("Assigned ID: %d\n", s.id);
    } else if (find_index_by_id(s.id) != -1) {
        printf("A student with ID %d already exists. If this is you, please login instead of registering.\n", s.id);
        return;
    }

    printf("Enter full name: ");
    fgets(s.name, sizeof(s.name), stdin);
    s.name[strcspn(s.name, "\n")] = 0;
    printf("Enter age: ");
    while (scanf("%d", &s.age) != 1) {
        clear_stdin();
        printf("Invalid. Enter age: ");
    }
    clear_stdin();
    printf("Enter department: ");
    fgets(s.dept, sizeof(s.dept), stdin);
    s.dept[strcspn(s.dept, "\n")] = 0;
    printf("Enter year (e.g., 1,2,3,4): ");
    while (scanf("%d", &s.year) != 1) {
        clear_stdin();
        printf("Invalid. Enter year: ");
    }
    clear_stdin();

    if (find_duplicate_by_details(s.name, s.dept, s.year) != -1) {
        printf("A student with the same name, department and year already exists. Registration cancelled.\n");
        return;
    }

    printf("How many subjects (max %d)? ", MAX_SUBJECTS);
    while (scanf("%d", &s.num_subjects) != 1 || s.num_subjects < 1 || s.num_subjects > MAX_SUBJECTS) {
        clear_stdin();
        printf("Invalid. Enter number between 1 and %d: ", MAX_SUBJECTS);
    }
    clear_stdin();
    for (int i = 0; i < s.num_subjects; ++i) {
        printf("Enter name of subject %d: ", i + 1);
        fgets(s.subjects[i].name, sizeof(s.subjects[i].name), stdin);
        s.subjects[i].name[strcspn(s.subjects[i].name, "\n")] = 0;
        s.subjects[i].classes_held = 0;
        s.subjects[i].classes_attended = 0;
        s.subjects[i].marks = 0;
        s.subjects[i].credits = 0;
    }

    printf("Set password for this student (no spaces): ");
    fgets(s.password, sizeof(s.password), stdin);
    s.password[strcspn(s.password, "\n")] = 0;

    add_student_custom(&s);
    printf("Registration complete. Use your ID and password to login.\n");
}

/* Edit student - unchanged from earlier (keeps full behavior) */
void edit_student() {
    int id;
    printf("Enter student ID to edit: ");
    while (scanf("%d", &id) != 1) {
        clear_stdin();
        printf("Invalid. Enter student ID: ");
    }
    clear_stdin();
    int idx = find_index_by_id(id);
    if (idx == -1) {
        printf("Student not found.\n");
        return;
    }
    Student *s = &students[idx];
    print_student_full(s);
    printf("What do you want to edit?\n");
    printf("1) Name\n2) Age\n3) Department\n4) Year\n5) Subjects (rename)\n6) Password\n7) Cancel\nChoose: ");
    int choice;
    while (scanf("%d", &choice) != 1) {
        clear_stdin();
        printf("Invalid. Choose: ");
    }
    clear_stdin();
    switch (choice) {
        case 1:
            printf("New name: ");
            fgets(s->name, sizeof(s->name), stdin);
            s->name[strcspn(s->name, "\n")] = 0;
            break;
        case 2:
            printf("New age: ");
            while (scanf("%d", &s->age) != 1) {
                clear_stdin();
                printf("Invalid. Enter age: ");
            }
            clear_stdin();
            break;
        case 3:
            printf("New department: ");
            fgets(s->dept, sizeof(s->dept), stdin);
            s->dept[strcspn(s->dept, "\n")] = 0;
            break;
        case 4:
            printf("New year: ");
            while (scanf("%d", &s->year) != 1) {
                clear_stdin();
                printf("Invalid. Enter year: ");
            }
            clear_stdin();
            break;
        case 5:
            printf("Subjects list:\n");
            for (int i = 0; i < s->num_subjects; ++i) {
                printf("%d) %s\n", i + 1, s->subjects[i].name);
            }
            printf("Enter subject number to rename (1 - %d): ", s->num_subjects);
            {
                int sn;
                while (scanf("%d", &sn) != 1 || sn < 1 || sn > s->num_subjects) {
                    clear_stdin();
                    printf("Invalid. Enter subject number: ");
                }
                clear_stdin();
                printf("New subject name: ");
                fgets(s->subjects[sn - 1].name, sizeof(s->subjects[sn - 1].name), stdin);
                s->subjects[sn - 1].name[strcspn(s->subjects[sn - 1].name, "\n")] = 0;
            }
            break;
        case 6:
            printf("New password: ");
            fgets(s->password, sizeof(s->password), stdin);
            s->password[strcspn(s->password, "\n")] = 0;
            break;
        case 7:
            printf("Edit cancelled.\n");
            return;
        default:
            printf("Invalid option.\n");
            return;
    }
    save_data();
    printf("Student updated.\n");
}

/* Delete student - unchanged */
void delete_student() {
    int id;
    printf("Enter student ID to delete: ");
    while (scanf("%d", &id) != 1) {
        clear_stdin();
        printf("Invalid. Enter student ID: ");
    }
    clear_stdin();
    int idx = find_index_by_id(id);
    if (idx == -1) {
        printf("Student not found.\n");
        return;
    }
    print_student_short(&students[idx]);
    printf("Are you sure you want to delete this student? (y/n): ");
    char c = getchar();
    clear_stdin();
    if (c == 'y' || c == 'Y') {
        students[idx].exists = 0;
        save_data();
        printf("Student deleted.\n");
    } else {
        printf("Operation cancelled.\n");
    }
}

/* Search student - unchanged */
void search_student() {
    printf("Search by:\n1) ID\n2) Name substring\nChoose: ");
    int ch;
    while (scanf("%d", &ch) != 1) {
        clear_stdin();
        printf("Invalid. Choose: ");
    }
    clear_stdin();
    if (ch == 1) {
        int id;
        printf("Enter ID: ");
        while (scanf("%d", &id) != 1) {
            clear_stdin();
            printf("Invalid. Enter ID: ");
        }
        clear_stdin();
        int idx = find_index_by_id(id);
        if (idx == -1) {
            printf("Not found.\n");
        } else {
            print_student_full(&students[idx]);
        }
    } else if (ch == 2) {
        char q[128];
        printf("Enter name substring (case-insensitive): ");
        fgets(q, sizeof(q), stdin);
        q[strcspn(q, "\n")] = 0;
        char qlow[128];
        strcpy(qlow, q);
        to_lower_str(qlow);
        int found = 0;
        for (int i = 0; i < student_count; ++i) {
            if (!students[i].exists) continue;
            char lname[128];
            strncpy(lname, students[i].name, sizeof(lname));
            lname[sizeof(lname)-1] = 0;
            to_lower_str(lname);
            if (strstr(lname, qlow) != NULL) {
                print_student_short(&students[i]);
                found = 1;
            }
        }
        if (!found) printf("No students matched.\n");
    } else {
        printf("Invalid option.\n");
    }
}

/* ----- Attendance functions (unchanged) ----- */

/* Mark attendance for class */
void mark_attendance_for_class() {
    printf("Mark attendance for all students for a specific subject index (subject indexes may differ across students).\n");
    printf("Approach: choose a subject name, then for each student who has that subject, mark present/absent.\n");
    char sname[MAX_SUB_NAME];
    printf("Enter exact subject name to mark (case-sensitive): ");
    fgets(sname, sizeof(sname), stdin);
    sname[strcspn(sname, "\n")] = 0;

    int any = 0;
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        Student *st = &students[i];
        for (int j = 0; j < st->num_subjects; ++j) {
            if (strcmp(st->subjects[j].name, sname) == 0) {
                any = 1;
                printf("Student ID %d | %s : Present? (y/n) : ", st->id, st->name);
                char c = getchar();
                clear_stdin();
                st->subjects[j].classes_held += 1;
                if (c == 'y' || c == 'Y') st->subjects[j].classes_attended += 1;
                break;
            }
        }
    }
    if (!any) {
        printf("No students have the subject '%s'.\n", sname);
        return;
    }
    save_data();
    printf("Attendance recorded for subject '%s'.\n", sname);
}

/* Mark attendance single student */
void mark_attendance_single_student() {
    int id;
    printf("Enter student ID: ");
    while (scanf("%d", &id) != 1) {
        clear_stdin();
        printf("Invalid. Enter student ID: ");
    }
    clear_stdin();
    int idx = find_index_by_id(id);
    if (idx == -1) {
        printf("Student not found.\n");
        return;
    }
    Student *s = &students[idx];
    printf("Student: %s\n", s->name);
    for (int i = 0; i < s->num_subjects; ++i) {
        printf("%d) %s (Attended %d / Held %d)\n", i + 1, s->subjects[i].name, s->subjects[i].classes_attended, s->subjects[i].classes_held);
    }
    printf("Choose subject number to mark attendance for: ");
    int sn;
    while (scanf("%d", &sn) != 1 || sn < 1 || sn > s->num_subjects) {
        clear_stdin();
        printf("Invalid. Choose subject number: ");
    }
    clear_stdin();
    int idxs = sn - 1;
    s->subjects[idxs].classes_held += 1;
    printf("Present? (y/n): ");
    char c = getchar();
    clear_stdin();
    if (c == 'y' || c == 'Y') s->subjects[idxs].classes_attended += 1;
    save_data();
    printf("Attendance updated for %s - %s.\n", s->name, s->subjects[idxs].name);
}

/* Increment classes held only */
void increment_classes_held_only() {
    char sname[MAX_SUB_NAME];
    printf("Enter exact subject name to increment classes held (no attendance marking): ");
    fgets(sname, sizeof(sname), stdin);
    sname[strcspn(sname, "\n")] = 0;
    int any = 0;
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        Student *st = &students[i];
        for (int j = 0; j < st->num_subjects; ++j) {
            if (strcmp(st->subjects[j].name, sname) == 0) {
                st->subjects[j].classes_held += 1;
                any = 1;
            }
        }
    }
    if (!any) {
        printf("No students have the subject '%s'.\n", sname);
        return;
    }
    save_data();
    printf("Classes held incremented for subject '%s'.\n", sname);
}

/* Student view: view their own attendance percentages */
void student_view_profile(int idx) {
    Student *s = &students[idx];
    print_student_full(s);
}

/* Admin list */
void admin_list_students() {
    printf("List of students:\n");
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        print_student_short(&students[i]);
    }
}

/* Attendance report for a subject */
void attendance_report_subject() {
    char sname[MAX_SUB_NAME];
    printf("Enter exact subject name for report: ");
    fgets(sname, sizeof(sname), stdin);
    sname[strcspn(sname, "\n")] = 0;
    int found = 0;
    printf("Attendance report for subject '%s'\n", sname);
    printf("ID | Name | Attended | Held | %%\n");
    for (int i = 0; i < student_count; ++i) {
        if (!students[i].exists) continue;
        Student *st = &students[i];
        for (int j = 0; j < st->num_subjects; ++j) {
            if (strcmp(st->subjects[j].name, sname) == 0) {
                int att = st->subjects[j].classes_attended;
                int held = st->subjects[j].classes_held;
                double pct = (held == 0) ? 0.0 : ((double)att / held) * 100.0;
                printf("%d | %s | %d | %d | %.2f%%\n", st->id, st->name, att, held, pct);
                found = 1;
            }
        }
    }
    if (!found) printf("No records for subject '%s'\n", sname);
}

/* ----- HTML report generation ----- */

void html_escape(FILE *f, const char *s) {
    // naive escaping for a few chars
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

    // ensure reports dir exists
    MKDIR(REPORTS_DIR);

    char path[512];
    snprintf(path, sizeof(path), "%s/%d_result.html", REPORTS_DIR, s->id);

    FILE *f = fopen(path, "w");
    if (!f) {
        perror("Unable to create report file");
        printf("Tried to write report to: %s\n", path);
        printf("Hint: Check if the 'reports' folder exists and you have write permissions.\n");
        return;
    }

    // current date
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char datebuf[64];
    snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d", tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday);

    // Write HTML (simple A4-friendly CSS)
    fprintf(f, "<!doctype html>\n<html>\n<head>\n<meta charset='utf-8'>\n");
    fprintf(f, "<title>Result - %d - %s</title>\n", s->id, s->name);
    fprintf(f, "<style>\n@page { size: A4; margin: 20mm; }\nbody { font-family: Arial, sans-serif; font-size: 12px; }\n.container { width: 100%%; }\n.header { text-align: center; margin-bottom: 10px; }\n.title { font-size: 18px; font-weight: bold; }\n.sub { font-size: 14px; }\n.table { width: 100%%; border-collapse: collapse; margin-top: 10px; }\n.table th, .table td { border: 1px solid #333; padding: 6px; text-align: left; }\n.meta { margin-top: 10px; }\n.footer { margin-top: 20px; font-size: 12px; }\n</style>\n</head>\n<body>\n<div class='container'>\n  <div class='header'>\n    <div class='title'>");
    html_escape(f, college);
    fprintf(f, "</div>\n    <div class='sub'>%s - %s</div>\n    <div class='sub'>Result / Marksheet</div>\n  </div>\n  <div class='meta'>\n    <strong>Student Name:</strong> ");
    html_escape(f, s->name);
    fprintf(f, "<br>\n    <strong>Student ID:</strong> %d<br>\n    <strong>Department:</strong> ", s->id);
    html_escape(f, s->dept);
    fprintf(f, "<br>\n    <strong>Year:</strong> %d<br>\n    <strong>Semester:</strong> ", s->year);
    html_escape(f, semester);
    fprintf(f, "<br>\n    <strong>Exam:</strong> ");
    html_escape(f, exam);
    fprintf(f, "<br>\n    <strong>Date:</strong> %s\n  </div>\n", datebuf);

    fprintf(f, "  <table class='table'>\n  <tr><th>#</th><th>Subject</th><th>Marks</th><th>Credits</th><th>Grade Point</th></tr>\n");
    for (int i = 0; i < s->num_subjects; ++i) {
        int gp = marks_to_grade_point(s->subjects[i].marks);
        fprintf(f, "  <tr>\n    <td>%d</td>\n    <td>", i+1);
        html_escape(f, s->subjects[i].name);
        fprintf(f, "</td>\n    <td>%d</td>\n    <td>%d</td>\n    <td>%d</td>\n  </tr>\n", s->subjects[i].marks, s->subjects[i].credits, gp);
    }
    double sgpa = calculate_sgpa_for_student(s);
    fprintf(f, "  </table>\n  <div class='footer'>\n    <strong>SGPA (this report):</strong> %.3f<br>\n    <strong>CGPA (cumulative):</strong> %.3f<br>\n    <strong>Total Credits Counted:</strong> %d\n  </div>\n", sgpa, s->cgpa, s->total_credits_completed);

    fprintf(f, "\n  <div style='margin-top:30px;'>\n    <div style='float:left'>_________________________<br>Examiner/Authority</div>\n    <div style='float:right'>_________________________<br>Principal</div>\n  </div>\n</div>\n</body>\n</html>\n");

    fclose(f);
}

/* ----- Authentication & Menus ----- */

int admin_menu();
int student_menu(int student_idx);

/* Admin panel */
int admin_menu() {
    while (1) {
        printf("\n=== ADMIN MENU ===\n");
        printf("1) Add student\n2) Edit student\n3) Delete student\n4) List students\n5) Search student\n6) Mark attendance (class)\n7) Mark attendance (single student)\n8) Increment classes held only\n9) Attendance report (subject)\n10) Enter marks & update CGPA for a student (generate report)\n11) Logout\nChoose: ");
        int ch;
        while (scanf("%d", &ch) != 1) {
            clear_stdin();
            printf("Invalid. Choose: ");
        }
        clear_stdin();
        switch (ch) {
            case 1: add_student(); break;
            case 2: edit_student(); break;
            case 3: delete_student(); break;
            case 4: admin_list_students(); break;
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

/* Student menu */
int student_menu(int student_idx) {
    while (1) {
        printf("\n=== STUDENT MENU ===\n");
        printf("1) View profile & attendance\n2) View SGPA & CGPA (based on stored marks)\n3) Download/See printable report path\n4) Change password\n5) Logout\nChoose: ");
        int ch;
        while (scanf("%d", &ch) != 1) {
            clear_stdin();
            printf("Invalid. Choose: ");
        }
        clear_stdin();
        Student *s = &students[student_idx];
        switch (ch) {
            case 1:
                student_view_profile(student_idx);
                break;
            case 2:
                student_view_sgpa_and_cgpa(student_idx);
                break;
            case 3: {
                char path[512];
                snprintf(path, sizeof(path), "%s/%d_result.html", REPORTS_DIR, s->id);
                FILE *f = fopen(path, "r");
                if (f) {
                    fclose(f);
                    printf("Printable result available at: %s\n", path);
                    printf("Open this file in a browser and print to PDF or paper (A4 recommended).\n");
                } else {
                    printf("No printable result generated yet for this student.\n");
                }
                break;
            }
            case 4: {
                char oldp[50], newp[50];
                printf("Enter current password: ");
                fgets(oldp, sizeof(oldp), stdin); oldp[strcspn(oldp, "\n")] = 0;
                if (strcmp(oldp, s->password) != 0) {
                    printf("Wrong password.\n");
                } else {
                    printf("Enter new password: ");
                    fgets(newp, sizeof(newp), stdin); newp[strcspn(newp, "\n")] = 0;
                    strcpy(s->password, newp);
                    save_data();
                    printf("Password changed.\n");
                }
                break;
            }
            case 5:
                return 0;
            default:
                printf("Invalid option.\n");
        }
        pause_console();
    }
    return 0;
}

/* Login screens */
void admin_login() {
    char user[50], pass[50];
    printf("Admin Username: ");
    fgets(user, sizeof(user), stdin); user[strcspn(user, "\n")] = 0;
    printf("Admin Password: ");
    fgets(pass, sizeof(pass), stdin); pass[strcspn(pass, "\n")] = 0;
    if (strcmp(user, ADMIN_USER) == 0 && strcmp(pass, ADMIN_PASS) == 0) {
        printf("Admin authenticated.\n");
        admin_menu();
    } else {
        printf("Invalid admin credentials.\n");
    }
}

void student_login() {
    int id;
    char pass[50];
    printf("Enter student ID: ");
    while (scanf("%d", &id) != 1) {
        clear_stdin();
        printf("Invalid. Enter student ID: ");
    }
    clear_stdin();
    int idx = find_index_by_id(id);
    if (idx == -1) {
        printf("Student ID not found.\n");
        return;
    }
    printf("Enter password: ");
    fgets(pass, sizeof(pass), stdin); pass[strcspn(pass, "\n")] = 0;
    if (strcmp(pass, students[idx].password) == 0) {
        printf("Welcome, %s!\n", students[idx].name);
        student_menu(idx);
    } else {
        printf("Wrong password.\n");
    }
}

/* Main entry menu */
void main_menu() {
    while (1) {
        printf("\n=== STUDENT MANAGEMENT SYSTEM ===\n");
        printf("1) Admin login\n2) Student login\n3) Exit\n4) Student self-register\nChoose: ");
        int ch;
        while (scanf("%d", &ch) != 1) {
            clear_stdin();
            printf("Invalid. Choose: ");
        }
        clear_stdin();
        switch (ch) {
            case 1: admin_login(); break;
            case 2: student_login(); break;
            case 3: printf("Exiting... Goodbye.\n"); return;
            case 4: student_self_register(); break;
            default: printf("Invalid option.\n"); break;
        }
    }
}

/* --- Web API glue (add to student_system.c) --- */
/* Make sure these are placed after the functions they call (e.g., add_student_custom, find_index_by_id, generate_html_report, calculate_and_update_cgpa_for_student etc.) */

int api_find_index_by_id(int id) {
    return find_index_by_id(id);
}

int api_add_student(Student *s) {
    if (!s) return -1;
    add_student_custom(s);
    return s->id;
}

/* Generate HTML report for index (calls your generate_html_report) */
void api_generate_report(int idx, const char* college, const char* semester, const char* exam) {
    generate_html_report(idx, college ? college : "Your College", semester ? semester : "Semester -", exam ? exam : "Exam -");
}

/* Update CGPA for student index using stored marks (calls your function) */
int api_calculate_update_cgpa(int idx) {
    calculate_and_update_cgpa_for_student(idx);
    return 0;
}

/* ----- New main with demo mode + non-interactive guard ----- */

int main(int argc, char **argv) {
    /* Demo mode: quick non-interactive sample output for hosting / previews */
    if (argc > 1 && strcmp(argv[1], "--demo") == 0) {
        printf("Demo Mode: Student Management System\n");
        printf("1) Add Student: ID=1001, Name=Tanay Sah, Year=1, Dept=CS\n");
        printf("2) Add Student: ID=1002, Name=Riya Sharma, Year=1, Dept=CS\n");
        printf("3) Total students (sample): 2\n");
        printf("Commands available in interactive mode: add, edit, delete, attendance, marks, report\n");
        return 0;
    }

    /* If running non-interactively (no TTY), exit with a short message.
       This prevents the program from looping waiting for stdin when hosted. */
    if (!isatty(fileno(stdin))) {
        fprintf(stderr, "Non-interactive environment detected. Run with --demo for demo output.\n");
        return 1;
    }

    /* Normal interactive startup */
    // Ensure reports dir exists so students see message path even if empty
    MKDIR(REPORTS_DIR);

    for (int i = 0; i < MAX_STUDENTS; ++i) students[i].exists = 0;
    load_data();
    printf("Welcome to Student Record & Attendance Management System\n");
    printf("(Note: Default admin credentials -> username: %s | password: %s)\n", ADMIN_USER, ADMIN_PASS);
    main_menu();
    return 0;
}

