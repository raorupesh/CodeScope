#pragma once

#include <string>

namespace net {

class HttpRequest {
public:
    explicit HttpRequest(std::string url);
    const std::string& url() const;

private:
    std::string url_;
};

}  // namespace net
