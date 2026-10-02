// Hermit single-file launcher (stub). Finds the compressed Hermit folder appended to its own
// exe, unpacks it once to %LOCALAPPDATA%\Hermit\app\<hash> and runs Hermit.exe from there
// with the same command line, returning its exit code. Later starts reuse the unpacked copy.
// See onefile.h for the file layout and pack.cpp for the packer.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>
#include <shlobj.h>

#include <cstdio>
#include <string>
#include <vector>

#include "onefile.h"

namespace {

// Messages are in English, or in Korean when the Windows display language is Korean
// (the launcher runs before Hermit and its translations are available).
const wchar_t* tr(const wchar_t* english, const wchar_t* korean)
{
    static const bool s_Korean = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
    return s_Korean ? korean : english;
}

void fail(const std::wstring& message)
{
    MessageBoxW(nullptr, message.c_str(), L"Hermit", MB_OK | MB_ICONERROR);
}

std::wstring lastErrorText(DWORD error = GetLastError())
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = text != nullptr ? text : L"";
    LocalFree(text);
    wchar_t code[32];
    swprintf_s(code, L" (0x%08lX)", error);
    return result + code;
}

bool readAt(HANDLE file, uint64_t offset, void* buffer, size_t size)
{
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        return false;
    }
    auto* out = static_cast<unsigned char*>(buffer);
    while (size > 0) {
        DWORD chunk = size > (1u << 30) ? (1u << 30) : static_cast<DWORD>(size);
        DWORD read = 0;
        if (!ReadFile(file, out, chunk, &read, nullptr) || read == 0) {
            return false;
        }
        out += read;
        size -= read;
    }
    return true;
}

bool writeFile(const std::wstring& path, const unsigned char* data, uint64_t size)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = true;
    while (size > 0 && ok) {
        DWORD chunk = size > (1u << 30) ? (1u << 30) : static_cast<DWORD>(size);
        DWORD written = 0;
        ok = WriteFile(file, data, chunk, &written, nullptr) && written == chunk;
        data += chunk;
        size -= chunk;
    }
    CloseHandle(file);
    return ok;
}

bool createDirectories(const std::wstring& path)
{
    int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

void deleteTree(const std::wstring& path)
{
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            std::wstring child = path + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                deleteTree(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(path.c_str());
}

bool fileExists(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Everything after argv[0] in our own command line, passed on unchanged.
std::wstring argumentsAfterProgram()
{
    const wchar_t* p = GetCommandLineW();
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"') {
            p++;
        }
        if (*p == L'"') {
            p++;
        }
    } else {
        while (*p && *p != L' ' && *p != L'\t') {
            p++;
        }
    }
    while (*p == L' ' || *p == L'\t') {
        p++;
    }
    return p;
}

class ArchiveReader {
public:
    ArchiveReader(const unsigned char* data, size_t size) : m_Data(data), m_Size(size) {}

    bool read(void* out, size_t size)
    {
        if (size > m_Size - m_Pos) {
            return false;
        }
        memcpy(out, m_Data + m_Pos, size);
        m_Pos += size;
        return true;
    }

    bool readString(std::wstring& out)
    {
        uint16_t length = 0;
        if (!read(&length, sizeof(length)) || length * sizeof(wchar_t) > m_Size - m_Pos) {
            return false;
        }
        out.assign(reinterpret_cast<const wchar_t*>(m_Data + m_Pos), length);
        m_Pos += length * sizeof(wchar_t);
        return true;
    }

    const unsigned char* take(uint64_t size)
    {
        if (size > m_Size - m_Pos) {
            return nullptr;
        }
        const unsigned char* p = m_Data + m_Pos;
        m_Pos += static_cast<size_t>(size);
        return p;
    }

private:
    const unsigned char* m_Data;
    size_t m_Size;
    size_t m_Pos = 0;
};

// Relative archive paths must stay inside the target folder.
bool isSafeRelativePath(const std::wstring& path)
{
    if (path.empty() || path[0] == L'\\' || path[0] == L'/' || path.find(L':') != std::wstring::npos) {
        return false;
    }
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of(L"\\/", start);
        std::wstring part = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty() || part == L"." || part == L"..") {
            return false;
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

// Unpacks the archive into folder; returns the main exe's relative path or empty on failure.
std::wstring unpack(const std::vector<unsigned char>& archive, const std::wstring& folder, std::wstring& error)
{
    ArchiveReader reader(archive.data(), archive.size());
    char magic[8];
    std::wstring mainExe;
    uint32_t count = 0;
    if (!reader.read(magic, sizeof(magic)) || memcmp(magic, onefile::kArchiveMagic, sizeof(magic)) != 0 ||
        !reader.readString(mainExe) || !reader.read(&count, sizeof(count))) {
        error = tr(L"The compressed data has an invalid format.", L"압축 데이터의 형식이 올바르지 않습니다.");
        return L"";
    }
    for (uint32_t i = 0; i < count; i++) {
        std::wstring relative;
        uint64_t size = 0;
        if (!reader.readString(relative) || !reader.read(&size, sizeof(size)) || !isSafeRelativePath(relative)) {
            error = tr(L"The file list in the compressed data is invalid.", L"압축 데이터의 파일 목록이 올바르지 않습니다.");
            return L"";
        }
        const unsigned char* data = reader.take(size);
        if (data == nullptr) {
            error = tr(L"The compressed data is truncated.", L"압축 데이터가 잘렸습니다.");
            return L"";
        }
        std::wstring path = folder + L"\\" + relative;
        size_t slash = path.find_last_of(L'\\');
        if (!createDirectories(path.substr(0, slash)) || !writeFile(path, data, size)) {
            error = tr(L"Cannot write the file: ", L"파일을 쓸 수 없습니다: ") + path + L"\n" + lastErrorText();
            return L"";
        }
    }
    return mainExe;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Lets Hermit's command-line mode (hermit list, --help, ...) print to the calling console.
    AttachConsole(ATTACH_PARENT_PROCESS);

    wchar_t selfPath[MAX_PATH * 4];
    DWORD length = GetModuleFileNameW(nullptr, selfPath, ARRAYSIZE(selfPath));
    if (length == 0 || length >= ARRAYSIZE(selfPath)) {
        fail(tr(L"Cannot determine the path of this program.", L"실행 파일 경로를 확인할 수 없습니다."));
        return 1;
    }

    HANDLE self = CreateFileW(selfPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (self == INVALID_HANDLE_VALUE) {
        fail(tr(L"Cannot read this program file.\n", L"실행 파일을 읽을 수 없습니다.\n") + lastErrorText());
        return 1;
    }
    LARGE_INTEGER fileSize;
    onefile::Trailer trailer = {};
    if (!GetFileSizeEx(self, &fileSize) || fileSize.QuadPart < static_cast<LONGLONG>(sizeof(trailer)) ||
        !readAt(self, fileSize.QuadPart - sizeof(trailer), &trailer, sizeof(trailer)) ||
        memcmp(trailer.magic, onefile::kTrailerMagic, sizeof(trailer.magic)) != 0 ||
        trailer.payloadOffset + trailer.payloadSize + sizeof(trailer) != static_cast<uint64_t>(fileSize.QuadPart)) {
        CloseHandle(self);
        fail(tr(L"This file contains no Hermit data. It may be damaged; please download it again.",
                 L"이 파일에는 Hermit 데이터가 없습니다. 파일이 손상되었을 수 있으니 다시 받아 주세요."));
        return 1;
    }

    wchar_t* localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        CloseHandle(self);
        fail(tr(L"The %LOCALAPPDATA% folder could not be found.", L"%LOCALAPPDATA% 폴더를 찾을 수 없습니다."));
        return 1;
    }
    const std::wstring appRoot = std::wstring(localAppData) + L"\\Hermit\\app";
    CoTaskMemFree(localAppData);

    wchar_t hashText[17];
    swprintf_s(hashText, L"%016llx", static_cast<unsigned long long>(trailer.payloadHash));
    const std::wstring target = appRoot + L"\\" + hashText;
    const std::wstring marker = target + L"\\.hermit-unpacked";

    if (!fileExists(marker)) {
        std::vector<unsigned char> payload(static_cast<size_t>(trailer.payloadSize));
        std::vector<unsigned char> archive(static_cast<size_t>(trailer.archiveSize));
        if (!readAt(self, trailer.payloadOffset, payload.data(), payload.size())) {
            CloseHandle(self);
            fail(tr(L"Cannot read this program file.\n", L"실행 파일을 읽을 수 없습니다.\n") + lastErrorText());
            return 1;
        }
        CloseHandle(self);
        self = INVALID_HANDLE_VALUE;

        if (onefile::fnv1a(payload.data(), payload.size()) != trailer.payloadHash) {
            fail(tr(L"This program file is damaged. Please download it again.", L"실행 파일이 손상되었습니다. 다시 받아 주세요."));
            return 1;
        }

        DECOMPRESSOR_HANDLE decompressor = nullptr;
        SIZE_T decompressed = 0;
        if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor) ||
            !Decompress(decompressor, payload.data(), payload.size(), archive.data(), archive.size(), &decompressed) ||
            decompressed != archive.size()) {
            DWORD error = GetLastError();
            if (decompressor != nullptr) {
                CloseDecompressor(decompressor);
            }
            fail(tr(L"Cannot unpack the files.\n", L"압축을 풀 수 없습니다.\n") + lastErrorText(error));
            return 1;
        }
        CloseDecompressor(decompressor);
        payload.clear();
        payload.shrink_to_fit();

        // Unpack next to the final folder, then rename, so a half-written copy is never used.
        wchar_t tempName[64];
        swprintf_s(tempName, L"\\%s.tmp-%lu", hashText, GetCurrentProcessId());
        const std::wstring temp = appRoot + tempName;
        deleteTree(temp);
        if (!createDirectories(temp)) {
            fail(tr(L"Cannot create the folder: ", L"폴더를 만들 수 없습니다: ") + temp + L"\n" + lastErrorText());
            return 1;
        }
        std::wstring error;
        if (unpack(archive, temp, error).empty()) {
            deleteTree(temp);
            fail(error);
            return 1;
        }
        const unsigned char done = '1';
        writeFile(temp + L"\\.hermit-unpacked", &done, 1);
        if (!MoveFileExW(temp.c_str(), target.c_str(), 0)) {
            // Another start unpacked the same version at the same time, or an incomplete
            // folder is in the way: replace it unless the finished one is already there.
            if (!fileExists(marker)) {
                deleteTree(target);
                if (!MoveFileExW(temp.c_str(), target.c_str(), 0)) {
                    DWORD moveError = GetLastError();
                    deleteTree(temp);
                    fail(tr(L"Cannot prepare the folder: ", L"폴더를 준비할 수 없습니다: ") + target + L"\n" + lastErrorText(moveError));
                    return 1;
                }
            } else {
                deleteTree(temp);
            }
        }

        // Remove older unpacked versions (skipped quietly while one is still running).
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((appRoot + L"\\*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = data.cFileName;
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L".." &&
                    name != hashText && name.find(L".tmp-") == std::wstring::npos) {
                    std::wstring old = appRoot + L"\\" + name;
                    // Only delete a folder whose main exe is not running (it could not be removed).
                    std::wstring oldExe = old + L"\\Hermit.exe";
                    if (!fileExists(oldExe) || DeleteFileW(oldExe.c_str())) {
                        deleteTree(old);
                    }
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    }
    if (self != INVALID_HANDLE_VALUE) {
        CloseHandle(self);
    }

    std::wstring exe = target + L"\\Hermit.exe";
    std::wstring commandLine = L"\"" + exe + L"\"";
    std::wstring arguments = argumentsAfterProgram();
    if (!arguments.empty()) {
        commandLine += L" " + arguments;
    }

    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process)) {
        fail(tr(L"Cannot start Hermit: ", L"Hermit을 시작할 수 없습니다: ") + exe + L"\n" + lastErrorText());
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
