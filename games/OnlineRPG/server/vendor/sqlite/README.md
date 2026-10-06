# SQLite

`sqlite3.c` and `sqlite3.h` are the SQLite 3.50.4 amalgamation, unmodified, from
<https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip>. SQLite is in the public domain
(<https://www.sqlite.org/copyright.html>).

Only the OnlineRPG servers use it (`../../CMakeLists.txt`, target `rpg_sqlite`); the engine and
the game do not. Compile options: `SQLITE_THREADSAFE=2` (one connection, one thread at a time),
`SQLITE_DQS=0`, `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_OMIT_LOAD_EXTENSION`,
`SQLITE_OMIT_DEPRECATED`, `SQLITE_OMIT_SHARED_CACHE`, `SQLITE_LIKE_DOESNT_MATCH_BLOBS`,
`SQLITE_USE_ALLOCA`.

To update: replace both files with a newer amalgamation, change the version here, run
`oe_server_tests` and `test/login_test.py`.
