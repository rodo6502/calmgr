#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Keep the native boundary buildable on small production hosts without
   development headers.  These declarations are the stable public SQLite and
   libsodium ABIs used below. */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef struct sqlite3_backup sqlite3_backup;
int sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
int sqlite3_close(sqlite3 *); int sqlite3_busy_timeout(sqlite3 *, int);
int sqlite3_exec(sqlite3 *, const char *, int (*)(void*,int,char**,char**), void *, char **);
void sqlite3_free(void *); const char *sqlite3_errmsg(sqlite3 *);
int sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
int sqlite3_bind_text(sqlite3_stmt *, int, const char *, int, void (*)(void*));
int sqlite3_bind_int(sqlite3_stmt *, int, int); int sqlite3_step(sqlite3_stmt *);
int sqlite3_finalize(sqlite3_stmt *); const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
int sqlite3_column_int(sqlite3_stmt *, int); int sqlite3_changes(sqlite3 *);
sqlite3_backup *sqlite3_backup_init(sqlite3 *, const char *, sqlite3 *, const char *);
int sqlite3_backup_step(sqlite3_backup *, int); int sqlite3_backup_finish(sqlite3_backup *);
int sqlite3_errcode(sqlite3 *);
int sqlite3_sleep(int);
int sodium_init(void);
int crypto_pwhash_str_alg(char *, const char *, unsigned long long,
                          unsigned long long, size_t, int);
int crypto_pwhash_str_verify(const char *, const char *, unsigned long long);

#define SQLITE_OK 0
#define SQLITE_ERROR 1
#define SQLITE_BUSY 5
#define SQLITE_LOCKED 6
#define SQLITE_CONSTRAINT 19
#define SQLITE_NOTADB 26
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_OPEN_READONLY 1
#define SQLITE_OPEN_READWRITE 2
#define SQLITE_OPEN_CREATE 4
#define SQLITE_OPEN_FULLMUTEX 0x10000
#define SQLITE_TRANSIENT ((void(*)(void*))-1)
#define APPT_OK 0
#define APPT_INVALID_REQUEST 10
#define APPT_NOT_FOUND 12
#define APPT_DUPLICATE 13
#define APPT_FORBIDDEN 15
#define APPT_REPOSITORY_ERROR 20
#define APPT_LOCKED 21
#define APPT_DAMAGED_DATA 22
#define APPT_IO_ERROR 23
#define RECORD_SIZE 389
#define PATH_SIZE 256
#define MESSAGE_SIZE 160
#define PASSWORD_SIZE 256
#define HASH_SIZE 128

static sqlite3 *transaction_db;
static char transaction_path[PATH_SIZE + 1];
static int transaction_depth;
static sqlite3 *appointment_scan_db;
static sqlite3_stmt *appointment_scan;
static char appointment_scan_path[PATH_SIZE + 1];
static sqlite3_stmt *user_scan;

void appt_read_stdin(char *line, char *at_end) {
    char buffer[1025];
    memset(line, ' ', 1024);
    if (!fgets(buffer, sizeof buffer, stdin)) { *at_end = 'Y'; return; }
    size_t n = strcspn(buffer, "\r\n");
    if (n > 1024) n = 1024;
    memcpy(line, buffer, n); *at_end = 'N';
}

static void fixed_to_c(char *out, size_t cap, const char *in, size_t n) {
    while (n && in[n - 1] == ' ') n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, in, n); out[n] = 0;
}
static void c_to_fixed(char *out, size_t n, const char *in) {
    size_t len = in ? strlen(in) : 0; if (len > n) len = n;
    memset(out, ' ', n); if (len) memcpy(out, in, len);
}
static void set_result(char *status, char *message, int code, const char *text) {
    char tmp[4]; snprintf(tmp, sizeof tmp, "%03d", code);
    memcpy(status, tmp, 3); c_to_fixed(message, MESSAGE_SIZE, text ? text : "");
}
static int mapped_error(sqlite3 *db, int rc, char *status, char *message, const char *context) {
    char text[MESSAGE_SIZE + 1];
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
        set_result(status, message, APPT_LOCKED, "Database is busy; retry the operation"); return APPT_LOCKED;
    }
    if (rc == SQLITE_NOTADB) {
        set_result(status, message, APPT_DAMAGED_DATA, "File is not a valid SQLite database"); return APPT_DAMAGED_DATA;
    }
    snprintf(text, sizeof text, "%s: %.120s", context, db ? sqlite3_errmsg(db) : "database error");
    set_result(status, message, APPT_REPOSITORY_ERROR, text); return APPT_REPOSITORY_ERROR;
}
static int exec_sql(sqlite3 *db, const char *sql, char *status, char *message, const char *context) {
    char *error = NULL; int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) { if (error) sqlite3_free(error); return mapped_error(db, rc, status, message, context); }
    return APPT_OK;
}
static const char schema_sql[] =
 "PRAGMA foreign_keys=ON;"
 "PRAGMA journal_mode=WAL;"
 "CREATE TABLE IF NOT EXISTS appointments("
 "schema_version INTEGER NOT NULL CHECK(schema_version=3),"
 "appointment_id TEXT PRIMARY KEY CHECK(length(appointment_id)=9),"
 "series_id TEXT NOT NULL,start_date TEXT NOT NULL,end_date TEXT NOT NULL,"
 "iso_year TEXT NOT NULL,iso_week TEXT NOT NULL,client TEXT NOT NULL,subject TEXT NOT NULL,"
 "status TEXT NOT NULL CHECK(status IN('A','C')),created_at TEXT NOT NULL,updated_at TEXT NOT NULL,"
 "cancelled_on TEXT NOT NULL,cancel_note TEXT NOT NULL,recurrence TEXT NOT NULL,"
 "series_until TEXT NOT NULL,revision INTEGER NOT NULL CHECK(revision>0));"
 "CREATE INDEX IF NOT EXISTS appointments_date_idx ON appointments(start_date,end_date);"
 "CREATE INDEX IF NOT EXISTS appointments_iso_idx ON appointments(iso_year,iso_week);"
 "CREATE INDEX IF NOT EXISTS appointments_series_idx ON appointments(series_id);"
 "CREATE INDEX IF NOT EXISTS appointments_status_idx ON appointments(status);"
 "CREATE INDEX IF NOT EXISTS appointments_client_idx ON appointments(client COLLATE NOCASE);"
 "CREATE INDEX IF NOT EXISTS appointments_subject_idx ON appointments(subject COLLATE NOCASE);"
 "CREATE TABLE IF NOT EXISTS users(user_id INTEGER PRIMARY KEY AUTOINCREMENT,"
 "username TEXT NOT NULL UNIQUE COLLATE NOCASE,password_hash TEXT NOT NULL,"
 "role TEXT NOT NULL CHECK(role IN('admin','user')),active INTEGER NOT NULL CHECK(active IN(0,1)),"
 "created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')),"
 "updated_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')),"
 "password_changed_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')),"
 "revision INTEGER NOT NULL DEFAULT 1);"
 "CREATE TABLE IF NOT EXISTS auth_failures(username TEXT PRIMARY KEY COLLATE NOCASE,"
 "failures INTEGER NOT NULL,first_failed_at INTEGER NOT NULL,last_failed_at INTEGER NOT NULL);"
 "CREATE TABLE IF NOT EXISTS audit_events(audit_id INTEGER PRIMARY KEY AUTOINCREMENT,"
 "event_type TEXT NOT NULL,actor_username TEXT NOT NULL,target_username TEXT NOT NULL,"
 "created_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%SZ','now')));"
 "PRAGMA user_version=3;";

static int verify_schema(sqlite3 *db, int fresh, char *status, char *message) {
    sqlite3_stmt *st = NULL; int rc, version = 0, count = 0;
    rc = sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL);
    if (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) version = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'", -1, &st, NULL);
    if (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (!fresh && count > 0 && version != 3) {
        set_result(status, message, APPT_DAMAGED_DATA, "Unsupported or damaged database schema"); return APPT_DAMAGED_DATA;
    }
    if (exec_sql(db, schema_sql, status, message, "Schema initialization") != APPT_OK) return APPT_REPOSITORY_ERROR;
    rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL);
    if (rc != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(st, 0), "ok") != 0) {
        sqlite3_finalize(st); set_result(status, message, APPT_DAMAGED_DATA, "Database integrity check failed"); return APPT_DAMAGED_DATA;
    }
    sqlite3_finalize(st); return APPT_OK;
}
static sqlite3 *open_database(const char *fixed_path, int create, char *status, char *message, int *owned) {
    char path[PATH_SIZE + 1]; sqlite3 *db = NULL; int rc; struct stat sb;
    fixed_to_c(path, sizeof path, fixed_path, PATH_SIZE);
    if (!path[0]) { set_result(status,message,APPT_INVALID_REQUEST,"Database path is empty"); return NULL; }
    if (transaction_db && strcmp(path, transaction_path) == 0) { *owned = 0; return transaction_db; }
    int fresh = stat(path, &sb) != 0;
    rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0) | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) { mapped_error(db,rc,status,message,"Opening database"); if(db)sqlite3_close(db); return NULL; }
    sqlite3_busy_timeout(db, 2500);
    if (exec_sql(db,"PRAGMA foreign_keys=ON;PRAGMA journal_mode=WAL;",status,message,"Database configuration") != APPT_OK ||
        verify_schema(db, fresh, status, message) != APPT_OK) { sqlite3_close(db); return NULL; }
    *owned = 1; return db;
}
static void close_owned(sqlite3 *db, int owned) { if (owned && db) sqlite3_close(db); }
static void field(char *dest, size_t size, const char *record, size_t off, size_t len) {
    fixed_to_c(dest, size, record + off, len);
}
static void put_field(char *record, size_t off, size_t len, const unsigned char *value) {
    c_to_fixed(record + off, len, value ? (const char *)value : "");
}

void appt_db_ensure(char *path, char *status, char *message) {
    int owned=0; sqlite3 *db=open_database(path,1,status,message,&owned);
    if(db){set_result(status,message,APPT_OK,"SQLite database is ready");close_owned(db,owned);}
}
void appt_db_get(char *path, char *id_fixed, char *record, char *status, char *message) {
    int owned=0,rc; char id[10]; sqlite3_stmt *st=NULL; sqlite3 *db=open_database(path,1,status,message,&owned);
    if (!db) return;
    fixed_to_c(id,sizeof id,id_fixed,9);
    const char *sql="SELECT schema_version,appointment_id,series_id,start_date,end_date,iso_year,iso_week,client,subject,status,created_at,updated_at,cancelled_on,cancel_note,recurrence,series_until,revision FROM appointments WHERE appointment_id=?";
    rc=sqlite3_prepare_v2(db,sql,-1,&st,NULL); if(rc==SQLITE_OK)sqlite3_bind_text(st,1,id,-1,SQLITE_TRANSIENT);
    if(rc!=SQLITE_OK){mapped_error(db,rc,status,message,"Reading appointment");goto out;}
    rc=sqlite3_step(st); if(rc==SQLITE_DONE){set_result(status,message,APPT_NOT_FOUND,"Appointment not found");goto out;}
    if(rc!=SQLITE_ROW){mapped_error(db,rc,status,message,"Reading appointment");goto out;}
    memset(record,' ',RECORD_SIZE); char num[16]; snprintf(num,sizeof num,"%02d",sqlite3_column_int(st,0));memcpy(record,num,2);
    put_field(record,2,9,sqlite3_column_text(st,1));put_field(record,11,9,sqlite3_column_text(st,2));put_field(record,20,8,sqlite3_column_text(st,3));put_field(record,28,8,sqlite3_column_text(st,4));put_field(record,36,4,sqlite3_column_text(st,5));put_field(record,40,2,sqlite3_column_text(st,6));put_field(record,42,60,sqlite3_column_text(st,7));put_field(record,102,100,sqlite3_column_text(st,8));put_field(record,202,1,sqlite3_column_text(st,9));put_field(record,203,20,sqlite3_column_text(st,10));put_field(record,223,20,sqlite3_column_text(st,11));put_field(record,243,8,sqlite3_column_text(st,12));put_field(record,251,120,sqlite3_column_text(st,13));put_field(record,371,1,sqlite3_column_text(st,14));put_field(record,372,8,sqlite3_column_text(st,15));snprintf(num,sizeof num,"%09d",sqlite3_column_int(st,16));memcpy(record+380,num,9);
    set_result(status,message,APPT_OK,"");
out: sqlite3_finalize(st);close_owned(db,owned);
}
static void write_appointment(char *path,char *record,char *status,char *message,int update) {
    int owned=0,rc,i; sqlite3_stmt *st=NULL; sqlite3 *db=open_database(path,1,status,message,&owned); if(!db)return;
    char v[17][257]; const size_t off[]={0,2,11,20,28,36,40,42,102,202,203,223,243,251,371,372,380};
    const size_t len[]={2,9,9,8,8,4,2,60,100,1,20,20,8,120,1,8,9};
    for(i=0;i<17;i++)field(v[i],sizeof v[i],record,off[i],len[i]);
    const char *insert="INSERT INTO appointments(schema_version,appointment_id,series_id,start_date,end_date,iso_year,iso_week,client,subject,status,created_at,updated_at,cancelled_on,cancel_note,recurrence,series_until,revision) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    const char *upd="UPDATE appointments SET schema_version=?,series_id=?,start_date=?,end_date=?,iso_year=?,iso_week=?,client=?,subject=?,status=?,created_at=?,updated_at=?,cancelled_on=?,cancel_note=?,recurrence=?,series_until=?,revision=? WHERE appointment_id=?";
    rc=sqlite3_prepare_v2(db,update?upd:insert,-1,&st,NULL);
    if(rc==SQLITE_OK){ if(update){for(i=0;i<16;i++)sqlite3_bind_text(st,i+1,v[i==0?0:i+1],-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,17,v[1],-1,SQLITE_TRANSIENT);}else{for(i=0;i<17;i++)sqlite3_bind_text(st,i+1,v[i],-1,SQLITE_TRANSIENT);} rc=sqlite3_step(st); }
    if(rc!=SQLITE_DONE){if(!update&&sqlite3_errcode(db)==SQLITE_CONSTRAINT)set_result(status,message,APPT_DUPLICATE,"Duplicate appointment ID");else mapped_error(db,rc,status,message,update?"Updating appointment":"Creating appointment");}
    else if(update&&sqlite3_changes(db)==0)set_result(status,message,APPT_NOT_FOUND,"Appointment not found");
    else set_result(status,message,APPT_OK,"");
    sqlite3_finalize(st);close_owned(db,owned);
}
void appt_db_put(char*p,char*r,char*s,char*m){write_appointment(p,r,s,m,0);} void appt_db_update(char*p,char*r,char*s,char*m){write_appointment(p,r,s,m,1);}
void appt_db_delete(char *path,char *id_fixed,char *status,char *message){int owned=0,rc;char id[10];sqlite3_stmt*st=NULL;sqlite3*db=open_database(path,1,status,message,&owned);if(!db)return;fixed_to_c(id,sizeof id,id_fixed,9);rc=sqlite3_prepare_v2(db,"DELETE FROM appointments WHERE appointment_id=?",-1,&st,NULL);if(rc==SQLITE_OK){sqlite3_bind_text(st,1,id,-1,SQLITE_TRANSIENT);rc=sqlite3_step(st);}if(rc!=SQLITE_DONE)mapped_error(db,rc,status,message,"Deleting appointment");else if(sqlite3_changes(db)==0)set_result(status,message,APPT_NOT_FOUND,"Appointment not found");else set_result(status,message,APPT_OK,"");sqlite3_finalize(st);close_owned(db,owned);}
void appt_db_scan_open(char*status,char*message){
    sqlite3_finalize(appointment_scan); appointment_scan=NULL;
    if(appointment_scan_db){sqlite3_close(appointment_scan_db);appointment_scan_db=NULL;}
    appointment_scan_path[0]=0;
    set_result(status,message,APPT_OK,"");
}
void appt_db_scan_next(char *path,char *record,char *status,char *message){
    int rc; char p[PATH_SIZE+1];
    fixed_to_c(p,sizeof p,path,PATH_SIZE);
    if(!appointment_scan){
        int owned=0;
        appointment_scan_db=open_database(path,1,status,message,&owned);
        if(!appointment_scan_db)return;
        if(!owned){
            appointment_scan_db=NULL;
            set_result(status,message,APPT_REPOSITORY_ERROR,"Appointment scan cannot share an active transaction");
            return;
        }
        strncpy(appointment_scan_path,p,PATH_SIZE); appointment_scan_path[PATH_SIZE]=0;
        const char *sql="SELECT schema_version,appointment_id,series_id,start_date,end_date,iso_year,iso_week,client,subject,status,created_at,updated_at,cancelled_on,cancel_note,recurrence,series_until,revision FROM appointments ORDER BY appointment_id";
        rc=sqlite3_prepare_v2(appointment_scan_db,sql,-1,&appointment_scan,NULL);
        if(rc!=SQLITE_OK){
            mapped_error(appointment_scan_db,rc,status,message,"Scanning appointments");
            sqlite3_close(appointment_scan_db); appointment_scan_db=NULL; appointment_scan_path[0]=0;
            return;
        }
    }else if(strcmp(p,appointment_scan_path)!=0){
        set_result(status,message,APPT_REPOSITORY_ERROR,"Appointment scan database path changed");
        return;
    }
    rc=sqlite3_step(appointment_scan);
    if(rc==SQLITE_DONE){set_result(status,message,APPT_NOT_FOUND,"End of repository");return;}
    if(rc!=SQLITE_ROW){mapped_error(appointment_scan_db,rc,status,message,"Scanning appointments");return;}
    memset(record,' ',RECORD_SIZE); char num[16];
    snprintf(num,sizeof num,"%02d",sqlite3_column_int(appointment_scan,0));memcpy(record,num,2);
    put_field(record,2,9,sqlite3_column_text(appointment_scan,1));put_field(record,11,9,sqlite3_column_text(appointment_scan,2));put_field(record,20,8,sqlite3_column_text(appointment_scan,3));put_field(record,28,8,sqlite3_column_text(appointment_scan,4));put_field(record,36,4,sqlite3_column_text(appointment_scan,5));put_field(record,40,2,sqlite3_column_text(appointment_scan,6));put_field(record,42,60,sqlite3_column_text(appointment_scan,7));put_field(record,102,100,sqlite3_column_text(appointment_scan,8));put_field(record,202,1,sqlite3_column_text(appointment_scan,9));put_field(record,203,20,sqlite3_column_text(appointment_scan,10));put_field(record,223,20,sqlite3_column_text(appointment_scan,11));put_field(record,243,8,sqlite3_column_text(appointment_scan,12));put_field(record,251,120,sqlite3_column_text(appointment_scan,13));put_field(record,371,1,sqlite3_column_text(appointment_scan,14));put_field(record,372,8,sqlite3_column_text(appointment_scan,15));snprintf(num,sizeof num,"%09d",sqlite3_column_int(appointment_scan,16));memcpy(record+380,num,9);
    set_result(status,message,APPT_OK,"");
}
void appt_db_scan_close(char*status,char*message){
    sqlite3_finalize(appointment_scan); appointment_scan=NULL;
    if(appointment_scan_db)sqlite3_close(appointment_scan_db);
    appointment_scan_db=NULL; appointment_scan_path[0]=0;
    set_result(status,message,APPT_OK,"");
}
void appt_db_begin(char *path,char *status,char *message){char p[PATH_SIZE+1];fixed_to_c(p,sizeof p,path,PATH_SIZE);if(transaction_db){if(strcmp(p,transaction_path)){set_result(status,message,APPT_REPOSITORY_ERROR,"A different database transaction is active");return;}transaction_depth++;set_result(status,message,APPT_OK,"");return;}int owned=0;transaction_db=open_database(path,1,status,message,&owned);if(!transaction_db)return;strcpy(transaction_path,p);if(exec_sql(transaction_db,"BEGIN IMMEDIATE",status,message,"Starting transaction")!=APPT_OK){sqlite3_close(transaction_db);transaction_db=NULL;transaction_path[0]=0;return;}transaction_depth=1;set_result(status,message,APPT_OK,"");}
void appt_db_commit(char*status,char*message){if(!transaction_db){set_result(status,message,APPT_REPOSITORY_ERROR,"No database transaction is active");return;}if(--transaction_depth>0){set_result(status,message,APPT_OK,"");return;}if(exec_sql(transaction_db,"COMMIT",status,message,"Committing transaction")!=APPT_OK){sqlite3_exec(transaction_db,"ROLLBACK",NULL,NULL,NULL);}else set_result(status,message,APPT_OK,"");sqlite3_close(transaction_db);transaction_db=NULL;transaction_path[0]=0;transaction_depth=0;}
void appt_db_rollback(char*status,char*message){if(transaction_db){sqlite3_exec(transaction_db,"ROLLBACK",NULL,NULL,NULL);sqlite3_close(transaction_db);}transaction_db=NULL;transaction_path[0]=0;transaction_depth=0;set_result(status,message,APPT_OK,"");}

static int validate_database_file(const char *path,char *status,char *message){sqlite3*db=NULL;sqlite3_stmt*st=NULL;int rc,version=0,tables=0;rc=sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL);if(rc!=SQLITE_OK){mapped_error(db,rc,status,message,"Opening SQLite snapshot");goto fail;}sqlite3_busy_timeout(db,2500);rc=sqlite3_prepare_v2(db,"PRAGMA user_version",-1,&st,NULL);if(rc==SQLITE_OK&&sqlite3_step(st)==SQLITE_ROW)version=sqlite3_column_int(st,0);sqlite3_finalize(st);st=NULL;if(version!=3){set_result(status,message,APPT_DAMAGED_DATA,"Snapshot has an unsupported database schema");goto fail;}rc=sqlite3_prepare_v2(db,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN('appointments','users','auth_failures','audit_events')",-1,&st,NULL);if(rc==SQLITE_OK&&sqlite3_step(st)==SQLITE_ROW)tables=sqlite3_column_int(st,0);sqlite3_finalize(st);st=NULL;if(tables!=4){set_result(status,message,APPT_DAMAGED_DATA,"Snapshot is missing required database tables");goto fail;}rc=sqlite3_prepare_v2(db,"PRAGMA integrity_check",-1,&st,NULL);if(rc!=SQLITE_OK||sqlite3_step(st)!=SQLITE_ROW||strcmp((const char*)sqlite3_column_text(st,0),"ok")){set_result(status,message,APPT_DAMAGED_DATA,"Snapshot integrity check failed");goto fail;}sqlite3_finalize(st);sqlite3_close(db);return 0;fail:sqlite3_finalize(st);if(db)sqlite3_close(db);return -1;}
static int backup_copy(const char *source,const char *dest,char*status,char*message){sqlite3 *src=NULL,*dst=NULL;sqlite3_backup*b=NULL;int rc,attempts=0;if(validate_database_file(source,status,message)!=0)return -1;rc=sqlite3_open_v2(source,&src,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL);if(rc!=SQLITE_OK){mapped_error(src,rc,status,message,"Opening backup source");goto fail;}sqlite3_busy_timeout(src,2500);rc=sqlite3_open_v2(dest,&dst,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL);if(rc!=SQLITE_OK){mapped_error(dst,rc,status,message,"Opening backup destination");goto fail;}sqlite3_busy_timeout(dst,2500);b=sqlite3_backup_init(dst,"main",src,"main");if(!b){mapped_error(dst,sqlite3_errcode(dst),status,message,"Starting SQLite backup");goto fail;}do{rc=sqlite3_backup_step(b,-1);if(rc==SQLITE_BUSY||rc==SQLITE_LOCKED)sqlite3_sleep(50);}while((rc==SQLITE_BUSY||rc==SQLITE_LOCKED)&&++attempts<50);sqlite3_backup_finish(b);b=NULL;if(rc!=SQLITE_DONE){mapped_error(dst,rc,status,message,"Copying SQLite backup");goto fail;}sqlite3_close(dst);sqlite3_close(src);if(validate_database_file(dest,status,message)!=0){unlink(dest);return -1;}set_result(status,message,APPT_OK,"SQLite backup completed");return 0;fail:if(b)sqlite3_backup_finish(b);if(dst)sqlite3_close(dst);if(src)sqlite3_close(src);unlink(dest);return -1;}
void appt_db_backup(char*src_fixed,char*dst_fixed,char*status,char*message){char src[257],dst[257],tmp[300];fixed_to_c(src,sizeof src,src_fixed,256);fixed_to_c(dst,sizeof dst,dst_fixed,256);snprintf(tmp,sizeof tmp,"%s.tmp.%ld",dst,(long)getpid());unlink(tmp);if(backup_copy(src,tmp,status,message)==0){chmod(tmp,0600);if(rename(tmp,dst)!=0){unlink(tmp);set_result(status,message,APPT_IO_ERROR,"Backup could not be installed atomically");}}}
void appt_db_restore(char*src_fixed,char*dst_fixed,char*status,char*message){char src[257],dst[257],tmp[300],side[300];fixed_to_c(src,sizeof src,src_fixed,256);fixed_to_c(dst,sizeof dst,dst_fixed,256);if(validate_database_file(src,status,message)!=0)return;snprintf(tmp,sizeof tmp,"%s.restore.%ld",dst,(long)getpid());unlink(tmp);if(backup_copy(src,tmp,status,message)!=0)return;chmod(tmp,0600);snprintf(side,sizeof side,"%s-wal",dst);unlink(side);snprintf(side,sizeof side,"%s-shm",dst);unlink(side);if(rename(tmp,dst)!=0){unlink(tmp);set_result(status,message,APPT_IO_ERROR,"Restored database could not be installed atomically");}else set_result(status,message,APPT_OK,"SQLite database restored");}
void appt_db_reset(char*live_fixed,char*safety_fixed,char*status,char*message){char live[257],safety[257],tmp[300],safety_tmp[300],side[300];fixed_to_c(live,sizeof live,live_fixed,256);fixed_to_c(safety,sizeof safety,safety_fixed,256);struct stat sb;if(stat(live,&sb)==0){snprintf(safety_tmp,sizeof safety_tmp,"%s.tmp.%ld",safety,(long)getpid());unlink(safety_tmp);if(backup_copy(live,safety_tmp,status,message)!=0)return;chmod(safety_tmp,0600);if(rename(safety_tmp,safety)!=0){unlink(safety_tmp);set_result(status,message,APPT_IO_ERROR,"Safety backup could not be installed atomically");return;}}snprintf(tmp,sizeof tmp,"%s.reset.%ld",live,(long)getpid());unlink(tmp);char fixed[256];c_to_fixed(fixed,256,tmp);int owned=0;sqlite3*db=open_database(fixed,1,status,message,&owned);if(!db)return;close_owned(db,owned);if(validate_database_file(tmp,status,message)!=0){unlink(tmp);return;}chmod(tmp,0600);snprintf(side,sizeof side,"%s-wal",live);unlink(side);snprintf(side,sizeof side,"%s-shm",live);unlink(side);if(rename(tmp,live)!=0){unlink(tmp);set_result(status,message,APPT_IO_ERROR,"Fresh database could not be installed atomically");}else set_result(status,message,APPT_OK,"Database reset completed");}

static int valid_username(const char*u){size_t n=strlen(u),i;if(n<3||n>64)return 0;for(i=0;i<n;i++)if(!((u[i]>='A'&&u[i]<='Z')||(u[i]>='a'&&u[i]<='z')||(u[i]>='0'&&u[i]<='9')||u[i]=='.'||u[i]=='_'||u[i]=='-'))return 0;return 1;}
static int valid_password(const char*p){size_t n=strlen(p);return n>=12&&n<=PASSWORD_SIZE;}
static int auth(sqlite3*db,const char*u,const char*p,int require_admin,int *uid,char*uname,char*role){sqlite3_stmt*st=NULL;int rc;const char*hash;rc=sqlite3_prepare_v2(db,"SELECT user_id,username,password_hash,role,active FROM users WHERE username=? COLLATE NOCASE",-1,&st,NULL);if(rc==SQLITE_OK){sqlite3_bind_text(st,1,u,-1,SQLITE_TRANSIENT);rc=sqlite3_step(st);}if(rc!=SQLITE_ROW||sqlite3_column_int(st,4)!=1){sqlite3_finalize(st);return 0;}hash=(const char*)sqlite3_column_text(st,2);int ok=hash&&crypto_pwhash_str_verify(hash,p,strlen(p))==0;if(ok&&require_admin&&strcmp((const char*)sqlite3_column_text(st,3),"admin"))ok=0;if(ok){if(uid)*uid=sqlite3_column_int(st,0);if(uname)strcpy(uname,(const char*)sqlite3_column_text(st,1));if(role)strcpy(role,(const char*)sqlite3_column_text(st,3));}sqlite3_finalize(st);return ok;}
static void audit(sqlite3*db,const char*event,const char*actor,const char*target){sqlite3_stmt*st=NULL;if(sqlite3_prepare_v2(db,"INSERT INTO audit_events(event_type,actor_username,target_username) VALUES(?,?,?)",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_text(st,1,event,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,2,actor,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,target,-1,SQLITE_TRANSIENT);sqlite3_step(st);}sqlite3_finalize(st);}
static int make_hash(const char*p,char*out){if(sodium_init()<0)return -1;return crypto_pwhash_str_alg(out,p,strlen(p),2,67108864,2);}
/* operation(24), username(64), password(256), actor username/password,
   role(8), confirm(16), new password(256), user id(12), active(1), revision(9). */
void appt_user_command(char*path_f,char*op_f,char*u_f,char*p_f,char*au_f,char*ap_f,char*role_f,char*confirm_f,char*np_f,char*uid_f,char*out_u,char*out_role,char*out_active,char*out_rev,char*status,char*message){char op[25],u[65],p[257],au[65],ap[257],role[9],confirm[17],np[257],uidtext[13],canonical[65]={0},authrole[9]={0},hash[HASH_SIZE];int owned=0,rc,uid=0;sqlite3_stmt*st=NULL;sqlite3*db=open_database(path_f,1,status,message,&owned);if(!db)return;fixed_to_c(op,sizeof op,op_f,24);fixed_to_c(u,sizeof u,u_f,64);fixed_to_c(p,sizeof p,p_f,256);fixed_to_c(au,sizeof au,au_f,64);fixed_to_c(ap,sizeof ap,ap_f,256);fixed_to_c(role,sizeof role,role_f,8);fixed_to_c(confirm,sizeof confirm,confirm_f,16);fixed_to_c(np,sizeof np,np_f,256);fixed_to_c(uidtext,sizeof uidtext,uid_f,12);c_to_fixed(out_u,64,"");c_to_fixed(out_role,8,"");c_to_fixed(out_active,1,"");c_to_fixed(out_rev,9,"");
 if(!strcmp(op,"USER-BOOTSTRAP")){if(strcmp(confirm,"CREATE-ADMIN")||!valid_username(u)||!valid_password(p)){set_result(status,message,APPT_INVALID_REQUEST,"Bootstrap confirmation, username, or password is invalid");goto done;}if(exec_sql(db,"BEGIN IMMEDIATE",status,message,"Starting user bootstrap")!=APPT_OK)goto done;sqlite3_prepare_v2(db,"SELECT count(*) FROM users",-1,&st,NULL);sqlite3_step(st);int n=sqlite3_column_int(st,0);sqlite3_finalize(st);st=NULL;if(n){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);set_result(status,message,APPT_FORBIDDEN,"User bootstrap is no longer available");goto done;}if(make_hash(p,hash)){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);set_result(status,message,APPT_REPOSITORY_ERROR,"Password hashing failed");goto done;}rc=sqlite3_prepare_v2(db,"INSERT INTO users(username,password_hash,role,active) VALUES(?,?,'admin',1)",-1,&st,NULL);if(rc==SQLITE_OK){sqlite3_bind_text(st,1,u,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,2,hash,-1,SQLITE_TRANSIENT);rc=sqlite3_step(st);}sqlite3_finalize(st);st=NULL;memset(hash,0,sizeof hash);if(rc!=SQLITE_DONE){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);mapped_error(db,rc,status,message,"Creating administrator");goto done;}audit(db,"USER_BOOTSTRAP",u,u);if(exec_sql(db,"COMMIT",status,message,"Committing user bootstrap")!=APPT_OK)goto done;set_result(status,message,APPT_OK,"Administrator created");goto done;}
 if(!strcmp(op,"AUTH-LOGIN")){int blocked=0;sqlite3_prepare_v2(db,"SELECT failures,(CAST(strftime('%s','now') AS INTEGER)-last_failed_at) FROM auth_failures WHERE username=? COLLATE NOCASE",-1,&st,NULL);sqlite3_bind_text(st,1,u,-1,SQLITE_TRANSIENT);if(sqlite3_step(st)==SQLITE_ROW&&sqlite3_column_int(st,0)>=5&&sqlite3_column_int(st,1)<900)blocked=1;sqlite3_finalize(st);st=NULL;if(blocked||!valid_username(u)||!valid_password(p)||!auth(db,u,p,0,&uid,canonical,authrole)){sqlite3_prepare_v2(db,"INSERT INTO auth_failures(username,failures,first_failed_at,last_failed_at) VALUES(?,1,CAST(strftime('%s','now') AS INTEGER),CAST(strftime('%s','now') AS INTEGER)) ON CONFLICT(username) DO UPDATE SET failures=CASE WHEN CAST(strftime('%s','now') AS INTEGER)-last_failed_at>=900 THEN 1 ELSE failures+1 END,first_failed_at=CASE WHEN CAST(strftime('%s','now') AS INTEGER)-last_failed_at>=900 THEN CAST(strftime('%s','now') AS INTEGER) ELSE first_failed_at END,last_failed_at=CAST(strftime('%s','now') AS INTEGER)",-1,&st,NULL);sqlite3_bind_text(st,1,u,-1,SQLITE_TRANSIENT);sqlite3_step(st);set_result(status,message,APPT_FORBIDDEN,"Invalid username or password");goto done;}sqlite3_prepare_v2(db,"DELETE FROM auth_failures WHERE username=? COLLATE NOCASE",-1,&st,NULL);sqlite3_bind_text(st,1,u,-1,SQLITE_TRANSIENT);sqlite3_step(st);sqlite3_finalize(st);st=NULL;snprintf(uidtext,sizeof uidtext,"%d",uid);c_to_fixed(uid_f,12,uidtext);c_to_fixed(out_u,64,canonical);c_to_fixed(out_role,8,authrole);set_result(status,message,APPT_OK,"Authentication successful");goto done;}
 if(!strcmp(op,"AUTH-CHECK")){rc=sqlite3_prepare_v2(db,"SELECT user_id,username,role,active,revision FROM users WHERE user_id=?",-1,&st,NULL);if(rc==SQLITE_OK){sqlite3_bind_int(st,1,atoi(uidtext));rc=sqlite3_step(st);}if(rc!=SQLITE_ROW||sqlite3_column_int(st,3)!=1){set_result(status,message,APPT_FORBIDDEN,"Authentication is no longer valid");goto done;}snprintf(uidtext,sizeof uidtext,"%d",sqlite3_column_int(st,0));c_to_fixed(uid_f,12,uidtext);c_to_fixed(out_u,64,(const char*)sqlite3_column_text(st,1));c_to_fixed(out_role,8,(const char*)sqlite3_column_text(st,2));c_to_fixed(out_active,1,"1");char rv[10];snprintf(rv,sizeof rv,"%09d",sqlite3_column_int(st,4));c_to_fixed(out_rev,9,rv);set_result(status,message,APPT_OK,"Authentication is valid");goto done;}
 if(!strcmp(op,"USER-LIST-OPEN")){if(!auth(db,au,ap,1,&uid,canonical,authrole)){set_result(status,message,APPT_FORBIDDEN,"Invalid administrator credentials");goto done;}sqlite3_finalize(user_scan);user_scan=NULL;rc=sqlite3_prepare_v2(db,"SELECT user_id,username,role,active,revision FROM users ORDER BY username COLLATE NOCASE",-1,&user_scan,NULL);if(rc!=SQLITE_OK)mapped_error(db,rc,status,message,"Listing users");else set_result(status,message,APPT_OK,"");goto done;}
 if(!strcmp(op,"USER-LIST-NEXT")){if(!user_scan){set_result(status,message,APPT_REPOSITORY_ERROR,"User list is not open");goto done;}rc=sqlite3_step(user_scan);if(rc==SQLITE_DONE){set_result(status,message,APPT_NOT_FOUND,"End of user list");goto done;}if(rc!=SQLITE_ROW){mapped_error(db,rc,status,message,"Listing users");goto done;}snprintf(uidtext,sizeof uidtext,"%d",sqlite3_column_int(user_scan,0));c_to_fixed(uid_f,12,uidtext);c_to_fixed(out_u,64,(const char*)sqlite3_column_text(user_scan,1));c_to_fixed(out_role,8,(const char*)sqlite3_column_text(user_scan,2));c_to_fixed(out_active,1,sqlite3_column_int(user_scan,3)?"1":"0");char rv[10];snprintf(rv,sizeof rv,"%09d",sqlite3_column_int(user_scan,4));c_to_fixed(out_rev,9,rv);set_result(status,message,APPT_OK,"");goto done;}
 if(!strcmp(op,"USER-LIST-CLOSE")){sqlite3_finalize(user_scan);user_scan=NULL;set_result(status,message,APPT_OK,"");goto done;}
 if(!strcmp(op,"USER-ADD")){if(!auth(db,au,ap,1,&uid,canonical,authrole)){set_result(status,message,APPT_FORBIDDEN,"Invalid administrator credentials");goto done;}if(!valid_username(u)||!valid_password(p)||(strcmp(role,"admin")&&strcmp(role,"user"))){set_result(status,message,APPT_INVALID_REQUEST,"Username, password, or role is invalid");goto done;}if(make_hash(p,hash)){set_result(status,message,APPT_REPOSITORY_ERROR,"Password hashing failed");goto done;}rc=sqlite3_prepare_v2(db,"INSERT INTO users(username,password_hash,role,active) VALUES(?,?,?,1)",-1,&st,NULL);if(rc==SQLITE_OK){sqlite3_bind_text(st,1,u,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,2,hash,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,role,-1,SQLITE_TRANSIENT);rc=sqlite3_step(st);}memset(hash,0,sizeof hash);if(rc!=SQLITE_DONE){if(sqlite3_errcode(db)==SQLITE_CONSTRAINT)set_result(status,message,APPT_DUPLICATE,"Username is already in use");else mapped_error(db,rc,status,message,"Creating user");goto done;}audit(db,"USER_ADD",canonical,u);set_result(status,message,APPT_OK,"User created");goto done;}
 if(!strcmp(op,"USER-PASSWORD")){int admin_reset=au[0]&&ap[0]&&auth(db,au,ap,1,&uid,canonical,authrole);if(!valid_password(np)||(!admin_reset&&!auth(db,u,p,0,&uid,canonical,authrole))){set_result(status,message,APPT_FORBIDDEN,"Invalid credentials or new password");goto done;}if(make_hash(np,hash)){set_result(status,message,APPT_REPOSITORY_ERROR,"Password hashing failed");goto done;}rc=sqlite3_prepare_v2(db,"UPDATE users SET password_hash=?,password_changed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),revision=revision+1 WHERE username=? COLLATE NOCASE",-1,&st,NULL);if(rc==SQLITE_OK){sqlite3_bind_text(st,1,hash,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,2,u,-1,SQLITE_TRANSIENT);rc=sqlite3_step(st);}memset(hash,0,sizeof hash);if(rc!=SQLITE_DONE||sqlite3_changes(db)!=1){set_result(status,message,APPT_FORBIDDEN,"Invalid credentials or new password");goto done;}audit(db,"USER_PASSWORD",canonical,u);set_result(status,message,APPT_OK,"Password changed");goto done;}
 if(!strcmp(op,"USER-DISABLE")||!strcmp(op,"USER-ENABLE")){int enabling=!strcmp(op,"USER-ENABLE");if(exec_sql(db,"BEGIN IMMEDIATE",status,message,"Starting user state change")!=APPT_OK)goto done;if(!auth(db,au,ap,1,&uid,canonical,authrole)){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);set_result(status,message,APPT_FORBIDDEN,"Invalid administrator credentials");goto done;}if(!enabling){sqlite3_prepare_v2(db,"SELECT role,active FROM users WHERE username=? COLLATE NOCASE",-1,&st,NULL);sqlite3_bind_text(st,1,u,-1,SQLITE_TRANSIENT);rc=sqlite3_step(st);int last=0;if(rc==SQLITE_ROW&&sqlite3_column_int(st,1)&&!strcmp((const char*)sqlite3_column_text(st,0),"admin")){sqlite3_stmt*c=NULL;sqlite3_prepare_v2(db,"SELECT count(*) FROM users WHERE role='admin' AND active=1",-1,&c,NULL);sqlite3_step(c);last=sqlite3_column_int(c,0)<=1;sqlite3_finalize(c);}sqlite3_finalize(st);st=NULL;if(last){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);set_result(status,message,APPT_FORBIDDEN,"The last active administrator cannot be disabled");goto done;}}rc=sqlite3_prepare_v2(db,"UPDATE users SET active=?,updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),revision=revision+1 WHERE username=? COLLATE NOCASE",-1,&st,NULL);if(rc==SQLITE_OK){sqlite3_bind_int(st,1,enabling);sqlite3_bind_text(st,2,u,-1,SQLITE_TRANSIENT);rc=sqlite3_step(st);}if(rc!=SQLITE_DONE||sqlite3_changes(db)!=1){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);set_result(status,message,APPT_NOT_FOUND,"User not found");goto done;}audit(db,enabling?"USER_ENABLE":"USER_DISABLE",canonical,u);if(exec_sql(db,"COMMIT",status,message,"Committing user state change")!=APPT_OK)goto done;set_result(status,message,APPT_OK,enabling?"User enabled":"User disabled");goto done;}
 set_result(status,message,APPT_INVALID_REQUEST,"Unsupported user operation");
done: sqlite3_finalize(st);close_owned(db,owned);memset(p,0,sizeof p);memset(ap,0,sizeof ap);memset(np,0,sizeof np);memset(hash,0,sizeof hash);
}
