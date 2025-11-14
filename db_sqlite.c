/* db_sqlite.c
   Minimal SQLite helpers for Student System (C)
   Compile: gcc -o student_system student_system.c db_sqlite.c -lsqlite3
*/

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_PATH "./data/student_system.db"

/* Open DB (creates file if missing) */
int open_db(sqlite3 **db) {
    int rc = sqlite3_open(DB_PATH, db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open DB: %s\n", sqlite3_errmsg(*db));
        return rc;
    }
    return SQLITE_OK;
}

/* Initialize schema (call once at startup) */
void init_schema(sqlite3 *db) {
    const char *sql =
    "BEGIN TRANSACTION;"
    "CREATE TABLE IF NOT EXISTS users ("
    " id TEXT PRIMARY KEY, name TEXT, email TEXT UNIQUE, phone TEXT, role TEXT, pwd_hash INTEGER, salt INTEGER);"
    "CREATE TABLE IF NOT EXISTS students ("
    " id TEXT PRIMARY KEY, user_id TEXT REFERENCES users(id), roll TEXT, program TEXT);"
    "CREATE TABLE IF NOT EXISTS subjects ("
    " id TEXT PRIMARY KEY, code TEXT UNIQUE, title TEXT, credits INTEGER, semester INTEGER);"
    "CREATE TABLE IF NOT EXISTS marks ("
    " student_id TEXT REFERENCES students(id), subject_id TEXT REFERENCES subjects(id), marks REAL,"
    " PRIMARY KEY(student_id, subject_id));"
    "CREATE TABLE IF NOT EXISTS attendance ("
    " student_id TEXT, subject_id TEXT, present_days INTEGER, total_days INTEGER,"
    " PRIMARY KEY(student_id, subject_id));"
    "COMMIT;";
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "init_schema error: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
}

/* Example: insert user (uses prepared statement) */
int db_insert_user(sqlite3 *db, const char *id, const char *name, const char *email,
                   const char *phone, const char *role, unsigned long pwd_hash, unsigned long salt) {
    const char *sql = "INSERT INTO users(id,name,email,phone,role,pwd_hash,salt) VALUES(?,?,?,?,?,?,?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare insert user failed: %s\n", sqlite3_errmsg(db)); return -1;
    }
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, email, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, phone, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, role, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)pwd_hash);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)salt);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { fprintf(stderr, "insert user failed: %s\n", sqlite3_errmsg(db)); return -1; }
    return 0;
}

/* Example: find user by email. Caller must free returned strings if non-NULL. */
int db_find_user_by_email(sqlite3 *db, const char *email, char *out_id, size_t idlen,
                          char *out_name, size_t namelen, char *out_role, size_t rolelen,
                          unsigned long *out_pwd_hash, unsigned long *out_salt, char *out_phone, size_t phonelen) {
    const char *sql = "SELECT id,name,role,pwd_hash,salt,phone FROM users WHERE email = ? LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) { fprintf(stderr, "prepare find user: %s\n", sqlite3_errmsg(db)); return -1; }
    sqlite3_bind_text(stmt, 1, email, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *id = sqlite3_column_text(stmt, 0);
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        const unsigned char *role = sqlite3_column_text(stmt, 2);
        sqlite3_int64 phash = sqlite3_column_int64(stmt, 3);
        sqlite3_int64 salt = sqlite3_column_int64(stmt, 4);
        const unsigned char *phone = sqlite3_column_text(stmt, 5);
        if (out_id) strncpy(out_id, (const char*)id, idlen);
        if (out_name) strncpy(out_name, (const char*)name, namelen);
        if (out_role) strncpy(out_role, (const char*)role, rolelen);
        if (out_pwd_hash) *out_pwd_hash = (unsigned long)phash;
        if (out_salt) *out_salt = (unsigned long)salt;
        if (out_phone) { if (phone) strncpy(out_phone, (const char*)phone, phonelen); else out_phone[0]=0; }
        sqlite3_finalize(stmt); return 0;
    }
    sqlite3_finalize(stmt);
    return 1; /* not found */
}

/* Insert or update mark (UPSERT via SQLite) */
int db_upsert_mark(sqlite3 *db, const char *student_id, const char *subject_id, double marks) {
    const char *sql = "INSERT INTO marks(student_id,subject_id,marks) VALUES(?,?,?) "
                      "ON CONFLICT(student_id,subject_id) DO UPDATE SET marks=excluded.marks;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) { fprintf(stderr,"prepare upsert mark: %s\n", sqlite3_errmsg(db)); return -1;}
    sqlite3_bind_text(stmt,1,student_id,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,subject_id,-1,SQLITE_STATIC);
    sqlite3_bind_double(stmt,3,marks);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { fprintf(stderr,"upsert mark failed: %s\n", sqlite3_errmsg(db)); return -1; }
    return 0;
}

/* Insert or update attendance */
int db_upsert_att(sqlite3 *db, const char *student_id, const char *subject_id, int pd, int td) {
    const char *sql = "INSERT INTO attendance(student_id,subject_id,present_days,total_days) VALUES(?,?,?,?) "
                      "ON CONFLICT(student_id,subject_id) DO UPDATE SET present_days=excluded.present_days, total_days=excluded.total_days;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) { fprintf(stderr,"prepare upsert att: %s\n", sqlite3_errmsg(db)); return -1;}
    sqlite3_bind_text(stmt,1,student_id,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,subject_id,-1,SQLITE_STATIC);
    sqlite3_bind_int(stmt,3,pd);
    sqlite3_bind_int(stmt,4,td);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { fprintf(stderr,"upsert att failed: %s\n", sqlite3_errmsg(db)); return -1; }
    return 0;
}

/* Example: compute SGPA for a student & semester (linear gp=(marks/100)*10) */
double db_compute_sgpa(sqlite3 *db, const char *student_id, int semester) {
    const char *sql =
        "SELECT SUM((m.marks/100.0)*10.0 * s.credits) as weighted_sum, SUM(s.credits) as total_credits "
        "FROM marks m JOIN subjects s ON m.subject_id = s.id "
        "WHERE m.student_id = ? AND s.semester = ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) { fprintf(stderr,"prepare sgpa: %s\n", sqlite3_errmsg(db)); return -1;}
    sqlite3_bind_text(stmt,1,student_id,-1,SQLITE_STATIC);
    sqlite3_bind_int(stmt,2,semester);
    double sgpa = -1.0;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        double weighted = sqlite3_column_double(stmt,0);
        int total_credits = sqlite3_column_int(stmt,1);
        if (total_credits > 0) sgpa = weighted / total_credits;
    }
    sqlite3_finalize(stmt);
    return sgpa;
}

/* compute CGPA across all semesters (same linear mapping) */
double db_compute_cgpa(sqlite3 *db, const char *student_id) {
    const char *sql =
        "SELECT SUM((m.marks/100.0)*10.0 * s.credits) as weighted_sum, SUM(s.credits) as total_credits "
        "FROM marks m JOIN subjects s ON m.subject_id = s.id WHERE m.student_id = ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) { fprintf(stderr,"prepare cgpa: %s\n", sqlite3_errmsg(db)); return -1;}
    sqlite3_bind_text(stmt,1,student_id,-1,SQLITE_STATIC);
    double cgpa = -1.0;
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        double weighted = sqlite3_column_double(stmt,0);
        int total_credits = sqlite3_column_int(stmt,1);
        if (total_credits > 0) cgpa = weighted / total_credits;
    }
    sqlite3_finalize(stmt);
    return cgpa;
}

/* Close DB */
void close_db(sqlite3 *db) { if (db) sqlite3_close(db); }

