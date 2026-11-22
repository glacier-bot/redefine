#include "object_identity.hpp"

#include <wil/resource.h>

#include <cstring>

std::optional<ObjectIdentity> ReadObjectIdentity(const std::wstring& path, std::wstring& error)
{
    error.clear();
    if (path.empty())
    {
        error = L"当前没有可绑定的文件系统对象";
        return std::nullopt;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        error = L"所选文件或文件夹当前不可访问";
        return std::nullopt;
    }

    const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    wil::unique_handle handle(CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!handle)
    {
        error = L"无法读取所选对象的文件身份";
        return std::nullopt;
    }

    FILE_ID_INFO information = {};
    if (!GetFileInformationByHandleEx(handle.get(), FileIdInfo, &information, sizeof(information)))
    {
        error = L"当前位置不支持稳定的对象身份，暂不能编辑对象备注";
        return std::nullopt;
    }

    ObjectIdentity identity;
    identity.volumeId = information.VolumeSerialNumber;
    static_assert(sizeof(identity.fileId) == sizeof(information.FileId.Identifier));
    memcpy(identity.fileId.data(), information.FileId.Identifier, sizeof(identity.fileId));
    identity.kind = isDirectory ? NoteObjectKind::Directory : NoteObjectKind::File;
    return identity;
}
