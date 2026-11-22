// Read-only access to the game's .kpf archives (zip files with stored entries).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace ss2rtx {

struct KpfEntry {
  std::string name;        // path inside the archive, forward slashes
  uint64_t local_offset = 0;
  uint32_t size = 0;
  bool stored = false;     // uncompressed; only these can be read
};

// Lists the archive's entries. Returns false if the file is not a readable zip.
bool kpf_list(const std::wstring& archive, std::vector<KpfEntry>* out);

// Reads `size` bytes starting `offset` bytes into a stored entry.
bool kpf_read(const std::wstring& archive, const KpfEntry& entry, uint64_t offset, size_t size, void* dst);

// Every .kpf the game loads: the game folder and its mods folder.
std::vector<std::wstring> kpf_archives();

}  // namespace ss2rtx
