#pragma once

#include <cstdint>
#include <limits>

namespace spacelens {

/// Logical file size in bytes (not "size on disk" / allocation size).
using ByteSize = std::uint64_t;

/// Stable index into DirectoryTree storage. InvalidIndex means "none".
using DirIndex = std::uint32_t;
using FileIndex = std::uint32_t;

inline constexpr DirIndex InvalidDirIndex = std::numeric_limits<DirIndex>::max();
inline constexpr FileIndex InvalidFileIndex = std::numeric_limits<FileIndex>::max();

}  // namespace spacelens
