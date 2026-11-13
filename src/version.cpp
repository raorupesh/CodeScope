#include "codescope/version.hpp"

#ifndef CODESCOPE_VERSION
#define CODESCOPE_VERSION "0.0.0-dev"
#endif

namespace codescope {

std::string_view version() noexcept {
    return CODESCOPE_VERSION;
}

}  // namespace codescope
