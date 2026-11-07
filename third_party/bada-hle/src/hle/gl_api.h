#pragma once
#include <string>
#include <vector>

namespace hle {

// Samsung exports glFoo_1 for the fixed pipeline and glFoo_2 for shaders.
// Canvas-only applications use the ES 2 presenter.
inline int gles_context_version(const std::vector<std::string>& imports) {
    bool fixed_pipeline = false;
    for (const auto& name : imports) {
        if (name.compare(0, 2, "gl") != 0 || name.size() < 4) continue;
        if (name.compare(name.size() - 2, 2, "_2") == 0) return 2;
        if (name.compare(name.size() - 2, 2, "_1") == 0) fixed_pipeline = true;
    }
    return fixed_pipeline ? 1 : 2;
}

} // namespace hle
