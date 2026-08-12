#pragma once

#include "core/Types.hpp"

#include <string>

namespace spacelens {

/// Formats logical byte sizes for UI/status text.
/// Uses binary units (1 KB = 1024 B) with one fractional digit for larger values.
class SizeFormatter {
public:
    [[nodiscard]] static std::string format(ByteSize bytes);
};

}  // namespace spacelens
