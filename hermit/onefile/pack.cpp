// Builds the Hermit single-file exe: launcher stub + the deploy folder as one LZMS-compressed
// archive + trailer (see onefile.h).
//
// Usage: pack.exe <launcher.exe> <deploy folder> <main exe relative path> <output.exe>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "onefile.h"

namespace {

bool readWholeFile(const std::wstring& path, std::vector<unsigned char>& out)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size) != FALSE;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 30));
            DWORD read = 0;
            ok = ReadFile(file, out.data() + done, chunk, &read, nullptr) && read > 0;
            done += read;
        }
    }
    CloseHandle(file);
    return ok;
}

void collect(const std::wstring& root, const std::wstring& relative, std::vector<std::wstring>& files)
{
    WIN32_FIND_DATAW data;
    std::wstring folder = relative.empty() ? root : root + L"\\" + relative;
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        std::wstring child = relative.empty() ? name : relative + L"\\" + name;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collect(root, child, files);
        } else {
            files.push_back(child);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

void append(std::vector<unsigned char>& out, const void* data, size_t size)
{
    const auto* p = static_cast<const unsigned char*>(data);
    out.insert(out.end(), p, p + size);
}

void appendString(std::vector<unsigned char>& out, const std::wstring& text)
{
    uint16_t length = static_cast<uint16_t>(text.size());
    append(out, &length, sizeof(length));
    append(out, text.data(), text.size() * sizeof(wchar_t));
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) {
        fwprintf(stderr, L"Usage: pack.exe <launcher.exe> <deploy folder> <main exe> <output.exe>\n");
        return 2;
    }
    const std::wstring stubPath = argv[1], root = argv[2], mainExe = argv[3], outPath = argv[4];

    std::vector<unsigned char> stub;
    if (!readWholeFile(stubPath, stub)) {
        fwprintf(stderr, L"Cannot read %ls\n", stubPath.c_str());
        return 1;
    }

    std::vector<std::wstring> files;
    collect(root, L"", files);
    std::sort(files.begin(), files.end());  // stable archive, stable hash
    if (std::find(files.begin(), files.end(), mainExe) == files.end()) {
        fwprintf(stderr, L"%ls is not in %ls\n", mainExe.c_str(), root.c_str());
        return 1;
    }

    std::vector<unsigned char> archive;
    append(archive, onefile::kArchiveMagic, sizeof(onefile::kArchiveMagic));
    appendString(archive, mainExe);
    uint32_t count = static_cast<uint32_t>(files.size());
    append(archive, &count, sizeof(count));
    for (const std::wstring& relative : files) {
        std::vector<unsigned char> data;
        if (!readWholeFile(root + L"\\" + relative, data)) {
            fwprintf(stderr, L"Cannot read %ls\n", relative.c_str());
            return 1;
        }
        appendString(archive, relative);
        uint64_t size = data.size();
        append(archive, &size, sizeof(size));
        append(archive, data.data(), data.size());
    }

    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) {
        fwprintf(stderr, L"CreateCompressor failed: %lu\n", GetLastError());
        return 1;
    }
    SIZE_T needed = 0;
    Compress(compressor, archive.data(), archive.size(), nullptr, 0, &needed);
    std::vector<unsigned char> payload(needed);
    SIZE_T compressedSize = 0;
    if (!Compress(compressor, archive.data(), archive.size(), payload.data(), payload.size(), &compressedSize)) {
        fwprintf(stderr, L"Compress failed: %lu\n", GetLastError());
        CloseCompressor(compressor);
        return 1;
    }
    CloseCompressor(compressor);
    payload.resize(compressedSize);

    onefile::Trailer trailer = {};
    trailer.payloadOffset = stub.size();
    trailer.payloadSize = payload.size();
    trailer.archiveSize = archive.size();
    trailer.payloadHash = onefile::fnv1a(payload.data(), payload.size());
    memcpy(trailer.magic, onefile::kTrailerMagic, sizeof(trailer.magic));

    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"Cannot write %ls\n", outPath.c_str());
        return 1;
    }
    bool ok = true;
    for (const std::vector<unsigned char>* part : {&stub, &payload}) {
        size_t done = 0;
        while (ok && done < part->size()) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(part->size() - done, 1u << 30));
            DWORD written = 0;
            ok = WriteFile(out, part->data() + done, chunk, &written, nullptr) && written == chunk;
            done += chunk;
        }
    }
    DWORD written = 0;
    ok = ok && WriteFile(out, &trailer, sizeof(trailer), &written, nullptr) && written == sizeof(trailer);
    CloseHandle(out);
    if (!ok) {
        fwprintf(stderr, L"Writing %ls failed\n", outPath.c_str());
        return 1;
    }

    wprintf(L"%zu files, %.1f MB -> %.1f MB (%ls)\n", files.size(), archive.size() / 1048576.0,
            (stub.size() + payload.size()) / 1048576.0, outPath.c_str());
    return 0;
}
