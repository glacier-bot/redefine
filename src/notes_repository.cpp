#include "notes_repository.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <shlwapi.h>
#include <sqlite3.h>
#include <wil/resource.h>

#include <atomic>
#include <ctime>
#include <cstring>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

std::atomic_uint64_t repositoryRevision{1};

void NotifyRepositoryChanged()
{
    repositoryRevision.fetch_add(1, std::memory_order_release);
}

std::wstring FromUtf8(const char* text)
{
    if (text == nullptr || *text == '\0')
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1)
    {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    result.pop_back();
    return result;
}

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

Database OpenDatabase(const std::filesystem::path& path, std::wstring& error)
{
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError)
    {
        error = L"无法创建本地备注数据目录";
        return Database(nullptr, sqlite3_close);
    }

    sqlite3* rawDatabase = nullptr;
    if (sqlite3_open16(path.c_str(), &rawDatabase) != SQLITE_OK)
    {
        if (rawDatabase != nullptr)
        {
            sqlite3_close(rawDatabase);
        }
        error = L"无法打开本地备注数据库";
        return Database(nullptr, sqlite3_close);
    }

    Database database(rawDatabase, sqlite3_close);
    sqlite3_busy_timeout(database.get(), 3000);
    const char* setup =
        "PRAGMA foreign_keys = ON;"
        "PRAGMA journal_mode = WAL;"
        "CREATE TABLE IF NOT EXISTS notes ("
        "note_id INTEGER PRIMARY KEY,"
        "markdown TEXT NOT NULL DEFAULT '',"
        "object_kind INTEGER NOT NULL,"
        "status TEXT NOT NULL,"
        "orphan_reason TEXT,"
        "orphan_since INTEGER,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS object_bindings ("
        "note_id INTEGER NOT NULL UNIQUE REFERENCES notes(note_id) ON DELETE CASCADE,"
        "volume_id INTEGER NOT NULL,"
        "file_id BLOB NOT NULL,"
        "last_path TEXT NOT NULL,"
        "creation_time INTEGER,"
        "bound_at INTEGER NOT NULL,"
        "UNIQUE(volume_id, file_id));";
    if (sqlite3_exec(database.get(), setup, nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        error = L"无法初始化本地备注数据库";
        database.reset();
        return database;
    }

    sqlite3_stmt* columnCheck = nullptr;
    const bool hasOriginalPath = sqlite3_prepare_v2(
        database.get(),
        "SELECT original_path FROM notes LIMIT 0;",
        -1,
        &columnCheck,
        nullptr) == SQLITE_OK;
    sqlite3_finalize(columnCheck);
    if (!hasOriginalPath && sqlite3_exec(
            database.get(),
            "ALTER TABLE notes ADD COLUMN original_path TEXT;",
            nullptr,
            nullptr,
            nullptr) != SQLITE_OK)
    {
        error = L"无法升级本地备注数据库";
        database.reset();
        return database;
    }
    sqlite3_stmt* linkCountCheck = nullptr;
    const bool hasLinkCount = sqlite3_prepare_v2(
        database.get(),
        "SELECT link_count FROM object_bindings LIMIT 0;",
        -1,
        &linkCountCheck,
        nullptr) == SQLITE_OK;
    sqlite3_finalize(linkCountCheck);
    if (!hasLinkCount && sqlite3_exec(
            database.get(),
            "ALTER TABLE object_bindings ADD COLUMN link_count INTEGER NOT NULL DEFAULT 1;",
            nullptr,
            nullptr,
            nullptr) != SQLITE_OK)
    {
        error = L"无法升级本地备注链接信息";
        database.reset();
        return database;
    }
    sqlite3_stmt* originalIdentityCheck = nullptr;
    const bool hasOriginalIdentity = sqlite3_prepare_v2(
        database.get(),
        "SELECT original_volume_id, original_file_id FROM notes LIMIT 0;",
        -1,
        &originalIdentityCheck,
        nullptr) == SQLITE_OK;
    sqlite3_finalize(originalIdentityCheck);
    if (!hasOriginalIdentity && sqlite3_exec(
            database.get(),
            "ALTER TABLE notes ADD COLUMN original_volume_id INTEGER;"
            "ALTER TABLE notes ADD COLUMN original_file_id BLOB;",
            nullptr,
            nullptr,
            nullptr) != SQLITE_OK)
    {
        error = L"无法升级本地无主身份记录";
        database.reset();
        return database;
    }
    sqlite3_exec(database.get(), "PRAGMA user_version = 4;", nullptr, nullptr, nullptr);
    return database;
}

bool Prepare(sqlite3* database, const char* sql, Statement& statement, std::wstring& error)
{
    sqlite3_stmt* rawStatement = nullptr;
    const int result = sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr);
    statement.reset(rawStatement);
    if (result != SQLITE_OK)
    {
        error = L"本地备注数据库操作失败";
        return false;
    }
    return true;
}

void BindIdentity(sqlite3_stmt* statement, int firstIndex, const ObjectIdentity& identity)
{
    sqlite3_bind_int64(statement, firstIndex, static_cast<sqlite3_int64>(identity.volumeId));
    sqlite3_bind_blob(statement, firstIndex + 1, identity.fileId.data(), static_cast<int>(identity.fileId.size()), SQLITE_TRANSIENT);
}

std::int64_t FindNote(sqlite3_stmt* statement, const ObjectIdentity& identity)
{
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    BindIdentity(statement, 1, identity);
    return sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : 0;
}

bool UpdateBoundPaths(
    sqlite3* database,
    const std::wstring& oldPath,
    const std::wstring& newPath,
    bool directory,
    std::wstring& error)
{
    Statement statement(nullptr, sqlite3_finalize);
    const char* exactSql = "UPDATE object_bindings SET last_path = ?1 WHERE last_path = ?2 COLLATE NOCASE;";
    const char* directorySql =
        "UPDATE object_bindings SET last_path = ?1 || substr(last_path, length(?2) + 1) "
        "WHERE last_path = ?2 COLLATE NOCASE "
        "OR (substr(last_path, 1, length(?2)) = ?2 COLLATE NOCASE "
        "AND substr(last_path, length(?2) + 1, 1) = '\\');";
    if (!Prepare(database, directory ? directorySql : exactSql, statement, error))
    {
        return false;
    }
    const std::string utf8NewPath = ToUtf8(newPath);
    const std::string utf8OldPath = ToUtf8(oldPath);
    sqlite3_bind_text(statement.get(), 1, utf8NewPath.c_str(), static_cast<int>(utf8NewPath.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(statement.get(), 2, utf8OldPath.c_str(), static_cast<int>(utf8OldPath.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
    {
        error = L"无法更新移动后的备注路径";
        return false;
    }
    return true;
}

bool OrphanBoundNotes(
    sqlite3* database,
    const std::wstring& path,
    bool directory,
    const std::wstring& reason,
    std::wstring& error,
    bool respectLinks = true,
    const ObjectIdentity* identity = nullptr)
{
    Statement query(nullptr, sqlite3_finalize);
    const char* exactSql = "SELECT note_id, last_path, link_count, volume_id, file_id FROM object_bindings WHERE last_path = ?1 COLLATE NOCASE;";
    const char* directorySql =
        "SELECT note_id, last_path, link_count, volume_id, file_id FROM object_bindings "
        "WHERE last_path = ?1 COLLATE NOCASE "
        "OR (substr(last_path, 1, length(?1)) = ?1 COLLATE NOCASE "
        "AND substr(last_path, length(?1) + 1, 1) = '\\');";
    if (identity != nullptr)
    {
        if (!Prepare(
                database,
                "SELECT note_id, last_path, link_count, volume_id, file_id FROM object_bindings WHERE volume_id = ?1 AND file_id = ?2;",
                query,
                error))
        {
            return false;
        }
        BindIdentity(query.get(), 1, *identity);
    }
    else
    {
        if (!Prepare(database, directory ? directorySql : exactSql, query, error))
        {
            return false;
        }
        const std::string utf8Path = ToUtf8(path);
        sqlite3_bind_text(query.get(), 1, utf8Path.c_str(), static_cast<int>(utf8Path.size()), SQLITE_TRANSIENT);
    }

    struct Candidate
    {
        std::int64_t noteId;
        std::wstring originalPath;
        int linkCount;
        ObjectIdentity identity;
    };
    std::vector<Candidate> candidates;
    int result = SQLITE_ERROR;
    while ((result = sqlite3_step(query.get())) == SQLITE_ROW)
    {
        candidates.push_back(Candidate{
            sqlite3_column_int64(query.get(), 0),
            FromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1))),
            sqlite3_column_int(query.get(), 2)});
        auto& candidate = candidates.back();
        candidate.identity.volumeId = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 3));
        const auto* fileId = static_cast<const std::uint8_t*>(sqlite3_column_blob(query.get(), 4));
        if (fileId != nullptr)
        {
            memcpy(candidate.identity.fileId.data(), fileId, candidate.identity.fileId.size());
        }
    }
    if (result != SQLITE_DONE)
    {
        error = L"无法查找需要转为无主的备注";
        return false;
    }
    if (candidates.empty())
    {
        return true;
    }
    if (sqlite3_exec(database, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        error = L"无法开始无主备注事务";
        return false;
    }

    Statement update(nullptr, sqlite3_finalize);
    Statement remove(nullptr, sqlite3_finalize);
    Statement decrement(nullptr, sqlite3_finalize);
    const bool prepared = Prepare(
        database,
        "UPDATE notes SET status = 'Orphan', orphan_reason = ?1, orphan_since = ?2, "
        "updated_at = ?2, original_path = ?3, original_volume_id = ?5, original_file_id = ?6 WHERE note_id = ?4;",
        update,
        error) && Prepare(database, "DELETE FROM object_bindings WHERE note_id = ?1;", remove, error) &&
        Prepare(database, "UPDATE object_bindings SET link_count = link_count - 1 WHERE note_id = ?1;", decrement, error);
    if (!prepared)
    {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const std::string utf8Reason = ToUtf8(reason);
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    for (const auto& candidate : candidates)
    {
        if (respectLinks && !directory && candidate.linkCount > 1)
        {
            sqlite3_bind_int64(decrement.get(), 1, candidate.noteId);
            const int decrementResult = sqlite3_step(decrement.get());
            sqlite3_reset(decrement.get());
            sqlite3_clear_bindings(decrement.get());
            if (decrementResult != SQLITE_DONE)
            {
                error = L"无法更新文件链接计数";
                sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
                return false;
            }
            continue;
        }

        const std::string utf8OriginalPath = ToUtf8(candidate.originalPath);
        sqlite3_bind_text(update.get(), 1, utf8Reason.c_str(), static_cast<int>(utf8Reason.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(update.get(), 2, now);
        sqlite3_bind_text(update.get(), 3, utf8OriginalPath.c_str(), static_cast<int>(utf8OriginalPath.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(update.get(), 4, candidate.noteId);
        sqlite3_bind_int64(update.get(), 5, static_cast<sqlite3_int64>(candidate.identity.volumeId));
        sqlite3_bind_blob(
            update.get(),
            6,
            candidate.identity.fileId.data(),
            static_cast<int>(candidate.identity.fileId.size()),
            SQLITE_TRANSIENT);
        sqlite3_bind_int64(remove.get(), 1, candidate.noteId);
        if (sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_step(remove.get()) != SQLITE_DONE)
        {
            error = L"无法保存无主备注状态";
            sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_reset(update.get());
        sqlite3_clear_bindings(update.get());
        sqlite3_reset(remove.get());
        sqlite3_clear_bindings(remove.get());
    }

    if (sqlite3_exec(database, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        error = L"无法提交无主备注事务";
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    NotifyRepositoryChanged();
    return true;
}

bool RestoreMovedNotes(
    sqlite3* database,
    const ObjectIdentity& identity,
    const std::wstring& newPath,
    bool directory,
    std::wstring& error)
{
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    Statement rootQuery(nullptr, sqlite3_finalize);
    if (!Prepare(
            database,
            "SELECT note_id, original_path, orphan_reason FROM notes "
            "WHERE status = 'Orphan' AND original_volume_id = ?1 AND original_file_id = ?2 "
            "AND orphan_since >= ?3 LIMIT 1;",
            rootQuery,
            error))
    {
        return false;
    }
    BindIdentity(rootQuery.get(), 1, identity);
    sqlite3_bind_int64(rootQuery.get(), 3, now - 10);
    if (sqlite3_step(rootQuery.get()) != SQLITE_ROW)
    {
        return true;
    }
    const std::wstring oldRoot = FromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(rootQuery.get(), 1)));
    const std::wstring reason = FromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(rootQuery.get(), 2)));
    const std::wstring expectedReason = directory ? L"文件夹已删除" : L"文件已删除";
    if (oldRoot.empty() || reason != expectedReason || _wcsicmp(oldRoot.c_str(), newPath.c_str()) == 0 ||
        wcscmp(PathFindFileNameW(oldRoot.c_str()), PathFindFileNameW(newPath.c_str())) != 0)
    {
        return true;
    }

    Statement query(nullptr, sqlite3_finalize);
    const char* fileSql =
        "SELECT note_id, original_path, original_volume_id, original_file_id FROM notes "
        "WHERE status = 'Orphan' AND original_path = ?1 COLLATE NOCASE AND orphan_since >= ?2;";
    const char* directorySql =
        "SELECT note_id, original_path, original_volume_id, original_file_id FROM notes "
        "WHERE status = 'Orphan' AND orphan_since >= ?2 AND (original_path = ?1 COLLATE NOCASE "
        "OR (substr(original_path, 1, length(?1)) = ?1 COLLATE NOCASE "
        "AND substr(original_path, length(?1) + 1, 1) = '\\'));";
    if (!Prepare(database, directory ? directorySql : fileSql, query, error))
    {
        return false;
    }
    const std::string utf8OldRoot = ToUtf8(oldRoot);
    sqlite3_bind_text(query.get(), 1, utf8OldRoot.c_str(), static_cast<int>(utf8OldRoot.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(query.get(), 2, now - 10);

    struct Candidate
    {
        std::int64_t noteId;
        std::wstring oldPath;
        ObjectIdentity identity;
    };
    std::vector<Candidate> candidates;
    int result = SQLITE_ERROR;
    while ((result = sqlite3_step(query.get())) == SQLITE_ROW)
    {
        Candidate candidate;
        candidate.noteId = sqlite3_column_int64(query.get(), 0);
        candidate.oldPath = FromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1)));
        candidate.identity.volumeId = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 2));
        const auto* fileId = static_cast<const std::uint8_t*>(sqlite3_column_blob(query.get(), 3));
        if (fileId != nullptr)
        {
            memcpy(candidate.identity.fileId.data(), fileId, candidate.identity.fileId.size());
        }
        candidates.push_back(std::move(candidate));
    }
    if (result != SQLITE_DONE || candidates.empty())
    {
        return true;
    }

    if (sqlite3_exec(database, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        error = L"无法开始同名移动恢复事务";
        return false;
    }
    Statement update(nullptr, sqlite3_finalize);
    Statement insert(nullptr, sqlite3_finalize);
    const bool prepared = Prepare(
        database,
        "UPDATE notes SET status = 'Bound', orphan_reason = NULL, orphan_since = NULL, original_path = NULL, "
        "original_volume_id = NULL, original_file_id = NULL, updated_at = ?2 WHERE note_id = ?1;",
        update,
        error) && Prepare(
            database,
            "INSERT INTO object_bindings(note_id, volume_id, file_id, last_path, bound_at, link_count) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6);",
            insert,
            error);
    if (!prepared)
    {
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    for (const auto& candidate : candidates)
    {
        const std::wstring targetPath = directory ? newPath + candidate.oldPath.substr(oldRoot.size()) : newPath;
        std::wstring identityError;
        const auto currentIdentity = ReadObjectIdentity(targetPath, identityError);
        if (!currentIdentity || currentIdentity->volumeId != candidate.identity.volumeId ||
            currentIdentity->fileId != candidate.identity.fileId)
        {
            continue;
        }
        const std::string utf8TargetPath = ToUtf8(targetPath);
        sqlite3_bind_int64(update.get(), 1, candidate.noteId);
        sqlite3_bind_int64(update.get(), 2, now);
        sqlite3_bind_int64(insert.get(), 1, candidate.noteId);
        BindIdentity(insert.get(), 2, *currentIdentity);
        sqlite3_bind_text(insert.get(), 4, utf8TargetPath.c_str(), static_cast<int>(utf8TargetPath.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert.get(), 5, now);
        sqlite3_bind_int(insert.get(), 6, static_cast<int>(currentIdentity->linkCount));
        if (sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_step(insert.get()) != SQLITE_DONE)
        {
            error = L"无法恢复同名移动绑定";
            sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_reset(update.get());
        sqlite3_clear_bindings(update.get());
        sqlite3_reset(insert.get());
        sqlite3_clear_bindings(insert.get());
    }
    if (sqlite3_exec(database, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        error = L"无法提交同名移动恢复事务";
        sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    NotifyRepositoryChanged();
    return true;
}
}

NotesRepository::NotesRepository()
{
    wil::unique_cotaskmem_string localAppData;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, localAppData.put())))
    {
        initializationError_ = L"无法定位本地应用数据目录";
        return;
    }
    databasePath_ = std::filesystem::path(localAppData.get()) / L"Redefine" / L"notes.db";
}

NotesRepository::NotesRepository(std::filesystem::path databasePath) : databasePath_(std::move(databasePath))
{
}

std::optional<StoredNote> NotesRepository::Load(
    const ObjectIdentity& identity,
    const std::wstring& path,
    std::wstring& error) const
{
    error.clear();
    if (!initializationError_.empty())
    {
        error = initializationError_;
        return std::nullopt;
    }
    Database database = OpenDatabase(databasePath_, error);
    if (!database)
    {
        return std::nullopt;
    }

    Statement query(nullptr, sqlite3_finalize);
    if (!Prepare(
            database.get(),
            "SELECT n.note_id, n.markdown FROM notes n "
            "JOIN object_bindings b ON b.note_id = n.note_id "
            "WHERE b.volume_id = ?1 AND b.file_id = ?2 AND n.status = 'Bound';",
            query,
            error))
    {
        return std::nullopt;
    }
    BindIdentity(query.get(), 1, identity);
    const int result = sqlite3_step(query.get());
    if (result == SQLITE_DONE)
    {
        return std::nullopt;
    }
    if (result != SQLITE_ROW)
    {
        error = L"无法读取本地备注";
        return std::nullopt;
    }

    StoredNote note;
    note.id = sqlite3_column_int64(query.get(), 0);
    note.markdown = FromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1)));

    Statement update(nullptr, sqlite3_finalize);
    if (Prepare(database.get(), "UPDATE object_bindings SET last_path = ?1, link_count = ?2 WHERE note_id = ?3;", update, error))
    {
        const std::string utf8Path = ToUtf8(path);
        sqlite3_bind_text(update.get(), 1, utf8Path.c_str(), static_cast<int>(utf8Path.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(update.get(), 2, static_cast<int>(identity.linkCount));
        sqlite3_bind_int64(update.get(), 3, note.id);
        sqlite3_step(update.get());
    }
    return note;
}

bool NotesRepository::Save(
    const ObjectIdentity& identity,
    const std::wstring& path,
    const std::wstring& markdown,
    std::wstring& error) const
{
    error.clear();
    if (!initializationError_.empty())
    {
        error = initializationError_;
        return false;
    }
    Database database = OpenDatabase(databasePath_, error);
    if (!database)
    {
        return false;
    }
    if (sqlite3_exec(database.get(), "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        error = L"无法开始备注保存事务";
        return false;
    }

    const auto rollback = [&database]() { sqlite3_exec(database.get(), "ROLLBACK;", nullptr, nullptr, nullptr); };
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::string utf8Path = ToUtf8(path);
    const std::string utf8Markdown = ToUtf8(markdown);

    Statement find(nullptr, sqlite3_finalize);
    Statement updateNote(nullptr, sqlite3_finalize);
    Statement updatePath(nullptr, sqlite3_finalize);
    Statement insertNote(nullptr, sqlite3_finalize);
    Statement insertBinding(nullptr, sqlite3_finalize);
    const bool prepared =
        Prepare(database.get(), "SELECT note_id FROM object_bindings WHERE volume_id = ?1 AND file_id = ?2;", find, error) &&
        Prepare(database.get(), "UPDATE notes SET markdown = ?1, updated_at = ?2 WHERE note_id = ?3;", updateNote, error) &&
        Prepare(database.get(), "UPDATE object_bindings SET last_path = ?1, link_count = ?2 WHERE note_id = ?3;", updatePath, error) &&
        Prepare(
            database.get(),
            "INSERT INTO notes(markdown, object_kind, status, created_at, updated_at) VALUES(?1, ?2, 'Bound', ?3, ?3);",
            insertNote,
            error) &&
        Prepare(
            database.get(),
            "INSERT INTO object_bindings(note_id, volume_id, file_id, last_path, bound_at, link_count) VALUES(?1, ?2, ?3, ?4, ?5, ?6);",
            insertBinding,
            error);
    if (!prepared)
    {
        rollback();
        return false;
    }

    std::int64_t noteId = FindNote(find.get(), identity);
    if (noteId != 0)
    {
        sqlite3_bind_text(updateNote.get(), 1, utf8Markdown.c_str(), static_cast<int>(utf8Markdown.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(updateNote.get(), 2, now);
        sqlite3_bind_int64(updateNote.get(), 3, noteId);
        if (sqlite3_step(updateNote.get()) != SQLITE_DONE)
        {
            error = L"无法更新备注内容";
            rollback();
            return false;
        }

        sqlite3_bind_text(updatePath.get(), 1, utf8Path.c_str(), static_cast<int>(utf8Path.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(updatePath.get(), 2, static_cast<int>(identity.linkCount));
        sqlite3_bind_int64(updatePath.get(), 3, noteId);
        if (sqlite3_step(updatePath.get()) != SQLITE_DONE)
        {
            error = L"无法更新备注路径";
            rollback();
            return false;
        }
    }
    else
    {
        sqlite3_bind_text(insertNote.get(), 1, utf8Markdown.c_str(), static_cast<int>(utf8Markdown.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(insertNote.get(), 2, identity.kind == NoteObjectKind::Directory ? 1 : 0);
        sqlite3_bind_int64(insertNote.get(), 3, now);
        if (sqlite3_step(insertNote.get()) != SQLITE_DONE)
        {
            error = L"无法创建备注";
            rollback();
            return false;
        }
        noteId = sqlite3_last_insert_rowid(database.get());

        sqlite3_bind_int64(insertBinding.get(), 1, noteId);
        BindIdentity(insertBinding.get(), 2, identity);
        sqlite3_bind_text(insertBinding.get(), 4, utf8Path.c_str(), static_cast<int>(utf8Path.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insertBinding.get(), 5, now);
        sqlite3_bind_int(insertBinding.get(), 6, static_cast<int>(identity.linkCount));
        if (sqlite3_step(insertBinding.get()) != SQLITE_DONE)
        {
            error = L"无法绑定备注与文件对象";
            rollback();
            return false;
        }
    }

    if (sqlite3_exec(database.get(), "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        error = L"无法提交备注保存事务";
        rollback();
        return false;
    }
    return true;
}

bool NotesRepository::Unbind(
    const ObjectIdentity& identity,
    const std::wstring& path,
    std::wstring& error) const
{
    error.clear();
    if (!initializationError_.empty())
    {
        error = initializationError_;
        return false;
    }
    Database database = OpenDatabase(databasePath_, error);
    if (!database)
    {
        return false;
    }
    return OrphanBoundNotes(
        database.get(),
        path,
        identity.kind == NoteObjectKind::Directory,
        L"用户主动解除绑定",
        error,
        false,
        &identity);
}

bool NotesRepository::ApplyShellChange(
    LONG event,
    const std::wstring& path,
    const std::wstring& newPath,
    std::wstring& error) const
{
    error.clear();
    const bool rename = event == SHCNE_RENAMEITEM || event == SHCNE_RENAMEFOLDER;
    const bool remove = event == SHCNE_DELETE || event == SHCNE_RMDIR;
    const bool create = event == SHCNE_CREATE || event == SHCNE_MKDIR;
    if (path.empty() || (!rename && !remove && !create))
    {
        return true;
    }
    if (!initializationError_.empty())
    {
        error = initializationError_;
        return false;
    }
    Database database = OpenDatabase(databasePath_, error);
    if (!database)
    {
        return false;
    }

    const bool directory = event == SHCNE_RENAMEFOLDER || event == SHCNE_RMDIR || event == SHCNE_MKDIR;
    if (create)
    {
        std::wstring identityError;
        const auto identity = ReadObjectIdentity(path, identityError);
        return identity ? RestoreMovedNotes(database.get(), *identity, path, directory, error) : true;
    }
    if (rename)
    {
        const bool recycle = !newPath.empty() && StrStrIW(newPath.c_str(), L":\\$Recycle.Bin\\") != nullptr;
        const bool crossVolume = !newPath.empty() && !PathIsSameRootW(path.c_str(), newPath.c_str());
        if (newPath.empty() || recycle || crossVolume)
        {
            return OrphanBoundNotes(
                database.get(),
                path,
                directory,
                crossVolume ? L"跨卷移动后源对象已删除" : directory ? L"文件夹已移入回收站" : L"文件已移入回收站",
                error);
        }
        const wchar_t* oldName = PathFindFileNameW(path.c_str());
        const wchar_t* newName = PathFindFileNameW(newPath.c_str());
        if (wcscmp(oldName, newName) == 0)
        {
            return UpdateBoundPaths(database.get(), path, newPath, directory, error);
        }

        const wchar_t* reason = directory ? L"文件夹重命名后视为新对象" : L"文件重命名后视为新对象";
        if (!directory)
        {
            std::wstring identityError;
            const auto identity = ReadObjectIdentity(newPath, identityError);
            if (identity && !OrphanBoundNotes(
                    database.get(),
                    path,
                    false,
                    reason,
                    error,
                    false,
                    &*identity))
            {
                return false;
            }
        }
        return OrphanBoundNotes(database.get(), path, directory, reason, error, false);
    }

    return OrphanBoundNotes(
        database.get(),
        path,
        directory,
        directory ? L"文件夹已删除" : L"文件已删除",
        error);
}

bool NotesRepository::VisitOrphanNotes(
    const std::function<bool(const OrphanNote&)>& visitor,
    std::wstring& error) const
{
    error.clear();
    if (!initializationError_.empty())
    {
        error = initializationError_;
        return false;
    }
    Database database = OpenDatabase(databasePath_, error);
    if (!database)
    {
        return false;
    }

    Statement query(nullptr, sqlite3_finalize);
    if (!Prepare(
            database.get(),
            "SELECT note_id, markdown, original_path, object_kind, orphan_since FROM notes "
            "WHERE status = 'Orphan' ORDER BY orphan_since DESC, note_id DESC;",
            query,
            error))
    {
        return false;
    }

    int result = SQLITE_ERROR;
    while ((result = sqlite3_step(query.get())) == SQLITE_ROW)
    {
        OrphanNote note;
        note.id = sqlite3_column_int64(query.get(), 0);
        note.markdown = FromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1)));
        note.originalPath = FromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 2)));
        note.kind = sqlite3_column_int(query.get(), 3) == 1 ? NoteObjectKind::Directory : NoteObjectKind::File;
        note.orphanSince = sqlite3_column_int64(query.get(), 4);
        if (!visitor(note))
        {
            return true;
        }
    }
    if (result != SQLITE_DONE)
    {
        error = L"无法读取无主备注";
        return false;
    }
    return true;
}

bool NotesRepository::RebindOrphan(std::int64_t noteId, const std::wstring& path, std::wstring& error) const
{
    error.clear();
    if (!initializationError_.empty())
    {
        error = initializationError_;
        return false;
    }

    const auto identity = ReadObjectIdentity(path, error);
    if (!identity)
    {
        return false;
    }

    Database database = OpenDatabase(databasePath_, error);
    if (!database)
    {
        return false;
    }
    if (sqlite3_exec(database.get(), "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        error = L"无法开始重新绑定事务";
        return false;
    }
    const auto rollback = [&database]() { sqlite3_exec(database.get(), "ROLLBACK;", nullptr, nullptr, nullptr); };

    Statement noteQuery(nullptr, sqlite3_finalize);
    Statement bindingQuery(nullptr, sqlite3_finalize);
    Statement update(nullptr, sqlite3_finalize);
    Statement insert(nullptr, sqlite3_finalize);
    const bool prepared = Prepare(
        database.get(),
        "SELECT object_kind, status FROM notes WHERE note_id = ?1;",
        noteQuery,
        error) && Prepare(
            database.get(),
            "SELECT note_id FROM object_bindings WHERE volume_id = ?1 AND file_id = ?2;",
            bindingQuery,
            error) && Prepare(
            database.get(),
            "UPDATE notes SET status = 'Bound', orphan_reason = NULL, orphan_since = NULL, "
            "original_path = NULL, original_volume_id = NULL, original_file_id = NULL, updated_at = ?2 "
            "WHERE note_id = ?1 AND status = 'Orphan' AND object_kind = ?3;",
            update,
            error) && Prepare(
            database.get(),
            "INSERT INTO object_bindings(note_id, volume_id, file_id, last_path, bound_at, link_count) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6);",
            insert,
            error);
    if (!prepared)
    {
        rollback();
        return false;
    }

    sqlite3_bind_int64(noteQuery.get(), 1, noteId);
    if (sqlite3_step(noteQuery.get()) != SQLITE_ROW)
    {
        error = L"无主备注已不存在";
        rollback();
        return false;
    }
    const NoteObjectKind expectedKind = sqlite3_column_int(noteQuery.get(), 0) == 1
        ? NoteObjectKind::Directory
        : NoteObjectKind::File;
    const std::string status = reinterpret_cast<const char*>(sqlite3_column_text(noteQuery.get(), 1));
    if (status != "Orphan")
    {
        error = L"该备注已不再是无主状态，请刷新后重试";
        rollback();
        return false;
    }
    if (identity->kind != expectedKind)
    {
        error = L"拖入对象的类型与原备注不一致，文件和文件夹不能互相绑定";
        rollback();
        return false;
    }

    BindIdentity(bindingQuery.get(), 1, *identity);
    const int bindingResult = sqlite3_step(bindingQuery.get());
    if (bindingResult == SQLITE_ROW)
    {
        error = L"该对象已经绑定了其他备注，不能覆盖或抢占";
        rollback();
        return false;
    }
    if (bindingResult != SQLITE_DONE)
    {
        error = L"无法检查拖入对象的绑定状态";
        rollback();
        return false;
    }

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::string utf8Path = ToUtf8(path);
    sqlite3_bind_int64(update.get(), 1, noteId);
    sqlite3_bind_int64(update.get(), 2, now);
    sqlite3_bind_int(update.get(), 3, expectedKind == NoteObjectKind::Directory ? 1 : 0);
    sqlite3_bind_int64(insert.get(), 1, noteId);
    BindIdentity(insert.get(), 2, *identity);
    sqlite3_bind_text(insert.get(), 4, utf8Path.c_str(), static_cast<int>(utf8Path.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.get(), 5, now);
    sqlite3_bind_int(insert.get(), 6, static_cast<int>(identity->linkCount));
    if (sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(database.get()) != 1)
    {
        error = L"备注状态已变化，未能完成重新绑定";
        rollback();
        return false;
    }
    if (sqlite3_step(insert.get()) != SQLITE_DONE)
    {
        error = L"无法写入新的对象绑定，备注仍保持无主状态";
        rollback();
        return false;
    }
    if (sqlite3_exec(database.get(), "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        error = L"无法提交重新绑定事务";
        rollback();
        return false;
    }
    NotifyRepositoryChanged();
    return true;
}

std::uint64_t NotesRepository::Revision() const
{
    return repositoryRevision.load(std::memory_order_acquire);
}
