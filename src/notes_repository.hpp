#pragma once

#include "object_identity.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct StoredNote
{
    std::int64_t id = 0;
    std::wstring markdown;
};

class NotesRepository
{
public:
    NotesRepository();

    std::optional<StoredNote> Load(
        const ObjectIdentity& identity,
        const std::wstring& path,
        std::wstring& error) const;
    bool Save(
        const ObjectIdentity& identity,
        const std::wstring& path,
        const std::wstring& markdown,
        std::wstring& error) const;

private:
    std::filesystem::path databasePath_;
    std::wstring initializationError_;
};
