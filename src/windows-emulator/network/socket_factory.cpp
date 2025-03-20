#include "socket_factory.hpp"
#include "socket_wrapper.hpp"

namespace network
{
    socket_factory::socket_factory()
    {
        initialize_wsa();
    }

    std::unique_ptr<i_socket> socket_factory::create_socket(int af, int type, int protocol)
    {
        return std::make_unique<socket_wrapper>(af, type, protocol);
    }

    int socket_factory::poll_sockets(const std::span<poll_entry> entries)
    {
        std::vector<pollfd> poll_data{};
        poll_data.reserve(entries.size());

        for (const auto& entry : entries)
        {
            if (!entry.s)
            {
                throw std::runtime_error("Bad socket given!");
            }

            const auto* wrapper = dynamic_cast<socket_wrapper*>(entry.s);
            if (!wrapper)
            {
                throw std::runtime_error("Socket was not created using the given factory");
            }

            pollfd fd{};
            fd.fd = wrapper->get().get_socket();
            fd.events = entry.events;
            fd.revents = entry.revents;

            poll_data.push_back(fd);
        }

        const auto res = poll(poll_data.data(), static_cast<uint32_t>(poll_data.size()), 0);

        for (size_t i = 0; i < poll_data.size() && i < entries.size(); ++i)
        {
            auto& entry = entries[i];
            const auto& fd = poll_data[i];

            entry.events = fd.events;
            entry.revents = fd.revents;
        }

        return res;
    }
}
