#include "core/SizeFormatter.hpp"

#include <array>
#include <cmath>
#include <cstdio>

namespace spacelens {

std::string SizeFormatter::format(ByteSize bytes)
{
    static constexpr std::array<const char*, 5> kUnits{
        "B", "KB", "MB", "GB", "TB"};

    if (bytes < 1024ULL) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
        return buffer;
    }

    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }

    char buffer[48];
    // One decimal place for KB and above keeps status lines readable.
    std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, kUnits[unit]);
    return buffer;
}

}  // namespace spacelens
