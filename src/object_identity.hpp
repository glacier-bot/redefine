#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

enum class NoteObjectKind
{
    File,
    Directory,
};

struct ObjectIdentity
{
    std::uint64_t volumeId = 0;
    std::array<std::uint8_t, 16> fileId = {};
    NoteObjectKind kind = NoteObjectKind::File;
    std::uint32_t linkCount = 1;

    bool operator==(const ObjectIdentity& other) const
    {
        return volumeId == other.volumeId && fileId == other.fileId && kind == other.kind;
    }
};

std::optional<ObjectIdentity> ReadObjectIdentity(const std::wstring& path, std::wstring& error);
