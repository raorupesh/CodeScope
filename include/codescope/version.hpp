#pragma once

#include <string_view>

namespace codescope {

// From project(VERSION ...) in CMakeLists.txt.
std::string_view version() noexcept;

}  // namespace codescope
