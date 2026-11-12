#pragma once

#include "elf_loader.h"
#include <filesystem>
#include <string>
#include <vector>

namespace runtime {
inline const char* abi_directory(bool hard_float) {
    return hard_float ? "hard-float" : "soft-float";
}

// Packaged engines use the adjacent bundle. A development engine under .build
// can also use the repository's bundle, without depending on the current cwd.
inline std::vector<std::filesystem::path> bundle_roots(const std::filesystem::path& engine_directory) {
    std::vector<std::filesystem::path> roots{engine_directory / "bada_libs"};
    for (auto directory = engine_directory; !directory.empty();) {
        if (directory.filename() == ".build") {
            roots.push_back(directory.parent_path() / "bada_libs");
            break;
        }
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return roots;
}

inline bool compatible_library(const std::filesystem::path& path, bool hard_float) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    ElfImage image;
    std::string error;
    return image.load(path.string(), error) &&
           !(image.float_abi == ArmFloatAbi::hard && !hard_float) &&
           !(image.float_abi == ArmFloatAbi::soft && hard_float);
}

inline std::string bundled_library(const ElfImage& game, const std::filesystem::path& engine_directory,
                                   const char* filename, std::vector<std::string>* searched = nullptr) {
    const bool hard_float = game.uses_hard_float();
    for (const auto& root : bundle_roots(engine_directory)) {
        const auto path = root / abi_directory(hard_float) / filename;
        if (searched) searched->push_back(path.string());
        if (compatible_library(path, hard_float)) return path.string();
    }
    return {};
}
} // namespace runtime
