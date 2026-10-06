#include "common/Store.h"

#include "sqlite3.h"

namespace rpg {

Db::~Db() {
    if (db_) sqlite3_close(db_);
}

bool Db::Open(const std::string& path, std::string* error) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        if (error) *error = "cannot open " + path + ": " + LastError();
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    sqlite3_busy_timeout(db_, 2000);
    return Exec("PRAGMA journal_mode = WAL", error) && Exec("PRAGMA synchronous = NORMAL", error) && Exec("PRAGMA foreign_keys = ON", error);
}

bool Db::Exec(const char* sql, std::string* error) {
    char* message = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) == SQLITE_OK) return true;
    if (error) *error = message ? message : "SQL error";
    sqlite3_free(message);
    return false;
}

int64_t Db::LastInsertId() const { return sqlite3_last_insert_rowid(db_); }
std::string Db::LastError() const { return db_ ? sqlite3_errmsg(db_) : "no database"; }

Statement::~Statement() {
    if (statement_) sqlite3_finalize(statement_);
}

bool Statement::Prepare(Db& db, const char* sql, std::string* error) {
    if (sqlite3_prepare_v2(db.Handle(), sql, -1, &statement_, nullptr) == SQLITE_OK) return true;
    if (error) *error = db.LastError();
    return false;
}

void Statement::Bind(int index, int64_t value) { sqlite3_bind_int64(statement_, index, value); }
void Statement::Bind(int index, const std::string& text) { sqlite3_bind_text(statement_, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT); }
void Statement::BindBlob(int index, const void* data, size_t size) { sqlite3_bind_blob(statement_, index, data, static_cast<int>(size), SQLITE_TRANSIENT); }

bool Statement::Step() {
    const int result = sqlite3_step(statement_);
    done_ = result == SQLITE_DONE;
    return result == SQLITE_ROW;
}

int64_t Statement::Int(int column) { return sqlite3_column_int64(statement_, column); }
std::string Statement::Text(int column) {
    const unsigned char* text = sqlite3_column_text(statement_, column);
    return text ? std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(sqlite3_column_bytes(statement_, column))) : std::string();
}
std::vector<uint8_t> Statement::Blob(int column) {
    const uint8_t* data = static_cast<const uint8_t*>(sqlite3_column_blob(statement_, column));
    const int size = sqlite3_column_bytes(statement_, column);
    return data && size > 0 ? std::vector<uint8_t>(data, data + size) : std::vector<uint8_t>();
}

void Statement::Reset() {
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
}

}  // namespace rpg
