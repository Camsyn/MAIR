#include <sqlite3.h>

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "sqlite3 oracle: %s failed at line %d: %s\n",            \
              #condition, __LINE__, sqlite3_errmsg(db));                       \
      goto failed;                                                             \
    }                                                                          \
  } while (0)

static int finish(sqlite3_stmt **stmt) {
  int rc = sqlite3_finalize(*stmt);
  *stmt = NULL;
  return rc;
}

int main(void) {
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  const sqlite3_int64 id = 9007199254740993LL;
  const unsigned char blob[] = {0x00, 0x7f, 0x80, 0xff};
  int rc;

  CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  CHECK(
      sqlite3_exec(db,
                   "PRAGMA foreign_keys=ON;"
                   "CREATE TABLE groups(id INTEGER PRIMARY KEY, name TEXT);"
                   "CREATE TABLE records(id INTEGER PRIMARY KEY,"
                   "group_id INTEGER REFERENCES groups(id), name TEXT UNIQUE,"
                   "payload BLOB, note TEXT, amount INTEGER CHECK(amount>=0));"
                   "INSERT INTO groups VALUES(1,'crew'),(2,'solo');"
                   "INSERT INTO records VALUES(2,1,'second',NULL,NULL,25),"
                   "(3,2,'third',NULL,NULL,9);",
                   NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(db, "INSERT INTO records VALUES(?1,1,?2,?3,?4,17)",
                           -1, &stmt, NULL) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 1, id) == SQLITE_OK);
  CHECK(sqlite3_bind_text(stmt, 2, "o'brien", -1, SQLITE_TRANSIENT) ==
        SQLITE_OK);
  CHECK(sqlite3_bind_blob(stmt, 3, blob, sizeof(blob), SQLITE_TRANSIENT) ==
        SQLITE_OK);
  CHECK(sqlite3_bind_null(stmt, 4) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(sqlite3_changes(db) == 1);
  CHECK(finish(&stmt) == SQLITE_OK);

  CHECK(sqlite3_prepare_v2(
            db, "SELECT id,name,payload,note FROM records WHERE id=?1", -1,
            &stmt, NULL) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(stmt, 1, id) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  CHECK(sqlite3_column_type(stmt, 0) == SQLITE_INTEGER);
  CHECK(sqlite3_column_int64(stmt, 0) == id);
  CHECK(sqlite3_column_type(stmt, 1) == SQLITE_TEXT);
  CHECK(sqlite3_column_bytes(stmt, 1) == 7);
  CHECK(sqlite3_column_text(stmt, 1) != NULL &&
        strcmp((const char *)sqlite3_column_text(stmt, 1), "o'brien") == 0);
  CHECK(sqlite3_column_type(stmt, 2) == SQLITE_BLOB);
  CHECK(sqlite3_column_bytes(stmt, 2) == (int)sizeof(blob));
  CHECK(sqlite3_column_blob(stmt, 2) != NULL &&
        memcmp(sqlite3_column_blob(stmt, 2), blob, sizeof(blob)) == 0);
  CHECK(sqlite3_column_type(stmt, 3) == SQLITE_NULL);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(finish(&stmt) == SQLITE_OK);

  CHECK(sqlite3_prepare_v2(db,
                           "SELECT g.name,count(*),sum(r.amount) FROM groups g "
                           "JOIN records r ON r.group_id=g.id "
                           "GROUP BY g.id,g.name ORDER BY g.id",
                           -1, &stmt, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  CHECK(sqlite3_column_text(stmt, 0) != NULL &&
        strcmp((const char *)sqlite3_column_text(stmt, 0), "crew") == 0);
  CHECK(sqlite3_column_int(stmt, 1) == 2);
  CHECK(sqlite3_column_int(stmt, 2) == 42);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  CHECK(sqlite3_column_text(stmt, 0) != NULL &&
        strcmp((const char *)sqlite3_column_text(stmt, 0), "solo") == 0);
  CHECK(sqlite3_column_int(stmt, 1) == 1);
  CHECK(sqlite3_column_int(stmt, 2) == 9);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(finish(&stmt) == SQLITE_OK);

  CHECK(sqlite3_exec(db,
                     "BEGIN; UPDATE records SET amount=100;"
                     "INSERT INTO records VALUES(4,1,'rollback',NULL,NULL,5);"
                     "ROLLBACK;",
                     NULL, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_get_autocommit(db) != 0);
  CHECK(sqlite3_exec(db, "INSERT INTO groups VALUES(1,'duplicate')", NULL, NULL,
                     NULL) == SQLITE_CONSTRAINT);
  CHECK(sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_PRIMARYKEY);
  CHECK(sqlite3_exec(db, "UPDATE records SET amount=-1 WHERE id=2", NULL, NULL,
                     NULL) == SQLITE_CONSTRAINT);
  CHECK(sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_CHECK);
  CHECK(sqlite3_exec(db, "UPDATE records SET group_id=99 WHERE id=2", NULL,
                     NULL, NULL) == SQLITE_CONSTRAINT);
  CHECK(sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_FOREIGNKEY);
  CHECK(sqlite3_exec(db, "SELECT FROM records", NULL, NULL, NULL) ==
        SQLITE_ERROR);

  CHECK(sqlite3_prepare_v2(db, "SELECT count(*),sum(amount) FROM records", -1,
                           &stmt, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  CHECK(sqlite3_column_int(stmt, 0) == 3);
  CHECK(sqlite3_column_int(stmt, 1) == 51);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  CHECK(finish(&stmt) == SQLITE_OK);
  rc = sqlite3_close(db);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "sqlite3 oracle: close failed: %s\n", sqlite3_errmsg(db));
    return 1;
  }
  puts("failures=0");
  return 0;

failed:
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return 1;
}
