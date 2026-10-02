// Hermit single-file executable: shared definitions of the launcher stub and the packer.
//
// Layout of the single-file exe:
//   [launcher stub exe][compressed payload][Trailer]
// The payload is an archive (see below) compressed as one LZMS buffer with the Windows
// Compression API (cabinet.dll, Windows 8+). Uncompressed archive:
//   char     magic[8] = "HMTPACK1"
//   uint16   mainExeLength; wchar_t mainExe[mainExeLength]   (relative path, e.g. Hermit.exe)
//   uint32   fileCount
//   per file: uint16 pathLength; wchar_t path[pathLength]; uint64 size; bytes[size]
#pragma once

#include <cstdint>

namespace onefile {

constexpr char kArchiveMagic[8] = {'H', 'M', 'T', 'P', 'A', 'C', 'K', '1'};
constexpr char kTrailerMagic[8] = {'H', 'M', 'T', 'O', 'N', 'E', '0', '1'};

#pragma pack(push, 1)
struct Trailer {
    uint64_t payloadOffset;     // from the start of the file
    uint64_t payloadSize;       // compressed
    uint64_t archiveSize;       // uncompressed
    uint64_t payloadHash;       // FNV-1a 64 of the compressed payload: names the cache folder
    char magic[8];
};
#pragma pack(pop)

inline uint64_t fnv1a(const unsigned char* data, size_t size, uint64_t hash = 0xcbf29ce484222325ULL)
{
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

}  // namespace onefile
