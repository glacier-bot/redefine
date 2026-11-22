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

    bool operator==(const ObjectIdentity&) const = default;
};

std::optional<ObjectIdentity> ReadObjectIdentity(const std::wstring& path, std::wstring& error);
