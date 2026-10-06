#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace rpg {

// The little of SQLite the servers use. A Db belongs to one thread (the store thread).
class Db {
public:
    Db() = default;
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    // Opens or creates the file with WAL journaling and synchronous = NORMAL: a commit is one
    // sequential append, durable against a process crash, and readers never block the writer.
    bool Open(const std::string& path, std::string* error);
    bool Exec(const char* sql, std::string* error = nullptr);
    int64_t LastInsertId() const;
    std::string LastError() const;
    sqlite3* Handle() { return db_; }

private:
    sqlite3* db_ = nullptr;
};

// A prepared statement, compiled once and reused: bind, step, read, Reset.
class Statement {
public:
    Statement() = default;
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    bool Prepare(Db& db, const char* sql, std::string* error);
    // Parameters are numbered from 1.
    void Bind(int index, int64_t value);
    void Bind(int index, const std::string& text);
    void BindBlob(int index, const void* data, size_t size);
    // True while there is a row. After false, Done() tells success from an error (a constraint).
    bool Step();
    bool Done() const { return done_; }
    // Columns are numbered from 0.
    int64_t Int(int column);
    std::string Text(int column);
    std::vector<uint8_t> Blob(int column);
    void Reset();

private:
    sqlite3_stmt* statement_ = nullptr;
    bool done_ = false;
};

}  // namespace rpg
