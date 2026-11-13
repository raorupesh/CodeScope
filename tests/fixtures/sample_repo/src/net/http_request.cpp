#include "net/http_request.hpp"

#include <utility>

namespace net {

HttpRequest::HttpRequest(std::string url) : url_(std::move(url)) {}

const std::string& HttpRequest::url() const {
    return url_;
}

}  // namespace net
