#include "../std_include.hpp"
#include "emscripten_http_socket_factory.hpp"
#include "static_socket_factory.hpp"

#ifdef OS_EMSCRIPTEN

#include <emscripten.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Browsers do not expose TCP, so the only egress a WebAssembly build can have is fetch(). This
// factory accepts a guest TCP socket, buffers what the guest writes, and once that forms a
// complete HTTP/1.1 request it performs the equivalent fetch() and hands the raw response back as
// if it had arrived over the wire.
//
// Deliberately limited:
//   - plaintext HTTP only. TLS is end-to-end, so an HTTPS request is opaque here and will simply
//     never complete. Point the guest at an http:// endpoint.
//   - one request per connection. The response always carries "Connection: close", so a guest
//     that tries to reuse the connection sees it close, which is a case every HTTP client handles.
//   - request bodies must be delimited by Content-Length. A chunked request body is not reassembled.
//   - in a browser the endpoint must send permissive CORS headers. Outside a browser there is
//     no such restriction.
namespace sogen
{
    namespace network
    {
        namespace
        {
            // The bodies below are JavaScript. clang-format parses them as C++ and rewrites them into
            // something that no longer parses - "!==" becomes "!= =" - and the damage only shows up at
            // runtime, when the host refuses to load the generated module.
            //
            // Split in two on purpose. An EM_ASYNC_JS function returns a placeholder value while
            // ASYNCIFY unwinds, and under MEMORY64 a pointer return type cannot represent that
            // placeholder, so the fetch returns a plain length and the bytes are copied out
            // afterwards by a second, synchronous call.

            // clang-format off
            EM_ASYNC_JS(int, sogen_http_bridge_fetch,
                        (const char* url, const char* method, const char* headers_json, const char* body, int body_length), {
                Module.sogenHttpBridgeResult = null;
                try {
                    const requestUrl = UTF8ToString(Number(url));
                    const requestMethod = UTF8ToString(Number(method));
                    const requestHeaders = JSON.parse(UTF8ToString(Number(headers_json)));

                    const init = { method: requestMethod, headers: requestHeaders };
                    if (body_length > 0) {
                        const bodyPtr = Number(body);
                        init.body = HEAPU8.slice(bodyPtr, bodyPtr + body_length);
                    }

                    const response = await fetch(requestUrl, init);
                    const payload = new Uint8Array(await response.arrayBuffer());
                    const headRequest = requestMethod.toUpperCase() === "HEAD";

                    let head = "HTTP/1.1 " + response.status + " " + (response.statusText || "") + "\r\n";
                    response.headers.forEach(function (value, name) {
                        // fetch() removes transfer framing and decodes response bodies.
                        const lower = name.toLowerCase();
                        const controlled = ["transfer-encoding", "connection", "keep-alive"];
                        if (!headRequest) {
                            controlled.push("content-encoding", "content-length");
                        }
                        if (controlled.indexOf(lower) < 0) {
                            head += name + ": " + value + "\r\n";
                        }
                    });
                    if (!headRequest) {
                        head += "Content-Length: " + payload.length + "\r\n";
                    }
                    head += "Connection: close\r\n\r\n";

                    const headBytes = new TextEncoder().encode(head);
                    const result = new Uint8Array(headBytes.length + payload.length);
                    result.set(headBytes, 0);
                    result.set(payload, headBytes.length);

                    Module.sogenHttpBridgeResult = result;
                    return result.length;
                } catch (error) {
                    console.error("sogen http bridge:", error);
                    return 0;
                }
            });

            EM_JS(void, sogen_http_bridge_take, (char* destination, int length), {
                const result = Module.sogenHttpBridgeResult;
                Module.sogenHttpBridgeResult = null;
                if (result && length > 0) {
                    HEAPU8.set(result.subarray(0, length), Number(destination));
                }
            });
            // clang-format on

            std::string to_lower(std::string value)
            {
                std::transform(value.begin(), value.end(), value.begin(),
                               [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return value;
            }

            std::string json_escape(const std::string_view value)
            {
                std::string result{};
                result.reserve(value.size() + 2);
                for (const auto c : value)
                {
                    switch (c)
                    {
                    case '"':
                        result += "\\\"";
                        break;
                    case '\\':
                        result += "\\\\";
                        break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20)
                        {
                            char escape[7]{};
                            (void)snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned char>(c));
                            result += escape;
                        }
                        else
                        {
                            result.push_back(c);
                        }
                        break;
                    }
                }
                return result;
            }

            struct parsed_request
            {
                std::string method{};
                std::string target{};
                std::string host{};
                std::string headers_json{};
                size_t body_offset{};
                size_t body_length{};
            };

            // Returns false while the request is still incomplete, which is the normal case for the
            // first few send() calls.
            bool parse_request(const std::string& buffer, parsed_request& out)
            {
                const auto header_end = buffer.find("\r\n\r\n");
                if (header_end == std::string::npos)
                {
                    return false;
                }

                const auto request_line_end = buffer.find("\r\n");
                const auto request_line = buffer.substr(0, request_line_end);

                const auto first_space = request_line.find(' ');
                if (first_space == std::string::npos)
                {
                    return false;
                }
                const auto second_space = request_line.find(' ', first_space + 1);
                if (second_space == std::string::npos)
                {
                    return false;
                }

                out.method = request_line.substr(0, first_space);
                out.target = request_line.substr(first_space + 1, second_space - first_space - 1);

                std::string headers_json = "{";
                bool first_header = true;
                size_t content_length = 0;

                auto cursor = request_line_end + 2;
                while (cursor < header_end)
                {
                    const auto line_end = buffer.find("\r\n", cursor);
                    const auto line = buffer.substr(cursor, line_end - cursor);
                    cursor = line_end + 2;

                    const auto colon = line.find(':');
                    if (colon == std::string::npos)
                    {
                        continue;
                    }

                    auto name = line.substr(0, colon);
                    auto value = line.substr(colon + 1);
                    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    {
                        value.erase(value.begin());
                    }

                    const auto lower_name = to_lower(name);
                    if (lower_name == "content-length")
                    {
                        content_length = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
                    }

                    if (lower_name == "host")
                    {
                        out.host = value;
                    }

                    // fetch() sets these itself, and refuses some of them outright.
                    if (lower_name == "host" || lower_name == "connection" || lower_name == "content-length" ||
                        lower_name == "transfer-encoding" || lower_name == "upgrade" || lower_name == "keep-alive" ||
                        lower_name == "proxy-connection")
                    {
                        continue;
                    }

                    if (!first_header)
                    {
                        headers_json += ",";
                    }
                    first_header = false;
                    headers_json += "\"" + json_escape(name) + "\":\"" + json_escape(value) + "\"";
                }

                headers_json += "}";

                out.body_offset = header_end + 4;
                out.body_length = content_length;
                out.headers_json = std::move(headers_json);

                return buffer.size() >= out.body_offset + out.body_length;
            }

            class http_bridge_socket final : public i_socket
            {
              public:
                void set_blocking(const bool blocking) override
                {
                    this->blocking_ = blocking;
                }

                int get_last_error() override
                {
                    return this->last_error_;
                }

                std::optional<int> get_socket_error() override
                {
                    return this->last_error_ ? std::optional{this->last_error_} : std::nullopt;
                }

                bool is_ready(const bool in_poll) override
                {
                    return in_poll ? this->dispatched_ : this->connected_;
                }

                int16_t poll_events(const int16_t events) const
                {
                    constexpr auto read_mask = static_cast<int16_t>(POLLIN | POLLRDNORM | POLLRDBAND);
                    constexpr auto write_mask = static_cast<int16_t>(POLLOUT | POLLWRNORM);
                    const auto readable = this->dispatched_ ? events & read_mask : 0;
                    const auto writable = this->connected_ ? events & write_mask : 0;
                    return static_cast<int16_t>(readable | writable);
                }

                bool is_listening() override
                {
                    return false;
                }

                std::optional<address> get_local_address() override
                {
                    return this->local_;
                }

                std::optional<address> get_remote_address() override
                {
                    if (!this->connected_)
                    {
                        return {};
                    }

                    return this->peer_;
                }

                bool bind(const address& addr) override
                {
                    this->local_ = addr;
                    return true;
                }

                bool connect(const address& addr) override
                {
                    this->peer_ = addr;
                    this->connected_ = true;
                    this->last_error_ = 0;
                    return true;
                }

                bool listen(int /*backlog*/) override
                {
                    this->last_error_ = 1;
                    return false;
                }

                std::unique_ptr<i_socket> accept(address& /*address*/) override
                {
                    this->last_error_ = 1;
                    return nullptr;
                }

                sent_size send(const std::span<const std::byte> data) override
                {
                    if (!this->connected_)
                    {
                        this->last_error_ = 1;
                        return -1;
                    }

                    this->request_.append(reinterpret_cast<const char*>(data.data()), data.size());
                    this->try_dispatch();
                    return static_cast<sent_size>(data.size());
                }

                sent_size sendto(const address& destination, const std::span<const std::byte> data) override
                {
                    if (!this->connected_)
                    {
                        this->peer_ = destination;
                        this->connected_ = true;
                    }
                    return this->send(data);
                }

                sent_size recv(const std::span<std::byte> data) override
                {
                    if (!this->dispatched_)
                    {
                        // Nothing can arrive before a complete request has gone out, and the guest is
                        // mid-write, so report "would block" rather than a closed connection.
                        this->last_error_ = SERR(EWOULDBLOCK);
                        return -1;
                    }

                    const auto remaining = this->response_.size() - this->response_offset_;
                    if (remaining == 0)
                    {
                        return 0;
                    }

                    const auto count = std::min(remaining, data.size());
                    std::memcpy(data.data(), this->response_.data() + this->response_offset_, count);
                    this->response_offset_ += count;
                    return static_cast<sent_size>(count);
                }

                sent_size recvfrom(address& source, const std::span<std::byte> data) override
                {
                    source = this->peer_;
                    return this->recv(data);
                }

              private:
                void try_dispatch()
                {
                    if (this->dispatched_)
                    {
                        return;
                    }

                    parsed_request request{};
                    if (!parse_request(this->request_, request))
                    {
                        return;
                    }

                    const auto authority = request.host.empty() ? this->peer_.to_string() : request.host;
                    const auto url = "http://" + authority + request.target;

                    const auto* body = this->request_.data() + request.body_offset;

                    const auto length = sogen_http_bridge_fetch(url.c_str(), request.method.c_str(), request.headers_json.c_str(), body,
                                                                static_cast<int>(request.body_length));

                    this->dispatched_ = true;

                    if (length <= 0)
                    {
                        // Surface a failed fetch as a closed connection: recv() returns 0 and the guest
                        // treats it as the peer hanging up, which is a path every client handles.
                        this->response_.clear();
                        return;
                    }

                    this->response_.resize(static_cast<size_t>(length));
                    sogen_http_bridge_take(this->response_.data(), length);
                }

                bool blocking_{true};
                bool connected_{false};
                bool dispatched_{false};
                int last_error_{0};
                address peer_{};
                address local_{};
                std::string request_{};
                std::vector<char> response_{};
                size_t response_offset_{0};
            };

            class http_bridge_socket_factory final : public socket_factory
            {
              public:
                std::unique_ptr<i_socket> create_socket(const int af, const int type, const int protocol) override
                {
                    if (type != SOCK_STREAM)
                    {
                        // UDP has no fetch() equivalent; fall back so DNS and friends keep working.
                        return this->fallback_->create_socket(af, type, protocol);
                    }

                    return std::make_unique<http_bridge_socket>();
                }

                int poll_sockets(const std::span<poll_entry> entries) override
                {
                    int ready = 0;
                    for (auto& entry : entries)
                    {
                        entry.revents = 0;
                        if (const auto* socket = dynamic_cast<http_bridge_socket*>(entry.s))
                        {
                            entry.revents = socket->poll_events(entry.events);
                            ready += entry.revents != 0;
                        }
                        else
                        {
                            ready += this->fallback_->poll_sockets(std::span{&entry, 1});
                        }
                    }
                    return ready;
                }

              private:
                std::unique_ptr<socket_factory> fallback_{create_static_socket_factory()};
            };
        }

        std::unique_ptr<socket_factory> create_emscripten_http_socket_factory()
        {
            return std::make_unique<http_bridge_socket_factory>();
        }
    }
} // namespace sogen

#endif
