#pragma once

#include "object_identity.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

struct StoredNote
{
    std::int64_t id = 0;
    std::wstring markdown;
};

struct OrphanNote
{
    std::int64_t id = 0;
    std::wstring markdown;
    std::wstring originalPath;
    NoteObjectKind kind = NoteObjectKind::File;
    std::int64_t orphanSince = 0;
};

class NotesRepository
{
public:
    NotesRepository();
    explicit NotesRepository(std::filesystem::path databasePath);

    std::optional<StoredNote> Load(
        const ObjectIdentity& identity,
        const std::wstring& path,
        std::wstring& error) const;
    bool Save(
        const ObjectIdentity& identity,
        const std::wstring& path,
        const std::wstring& markdown,
        std::wstring& error) const;
    bool ApplyShellChange(
        LONG event,
        const std::wstring& path,
        const std::wstring& newPath,
        std::wstring& error) const;
    bool VisitOrphanNotes(
        const std::function<bool(const OrphanNote&)>& visitor,
        std::wstring& error) const;
    bool RebindOrphan(std::int64_t noteId, const std::wstring& path, std::wstring& error) const;
    std::uint64_t Revision() const;

private:
    std::filesystem::path databasePath_;
    std::wstring initializationError_;
};
