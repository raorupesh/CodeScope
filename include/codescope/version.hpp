#pragma once

#include <string_view>

namespace codescope {

// Semantic version of the build, taken from project(VERSION ...) in CMakeLists.txt.
std::string_view version() noexcept;

}  // namespace codescope
