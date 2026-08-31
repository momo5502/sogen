#include "../std_include.hpp"
#include "macos_window_server.hpp"

#include "../macos_platform.hpp"

#include <algorithm>
#include <ranges>

namespace sogen
{
    size_t macos_window::backing_bytes() const
    {
        if (this->width <= 0 || this->height <= 0 || this->backing_stride == 0)
        {
            return 0;
        }

        return static_cast<size_t>(this->backing_stride) * static_cast<size_t>(this->height);
    }

    RECT macos_window::rect() const
    {
        return RECT{
            .left = this->x,
            .top = this->y,
            .right = this->x + this->width,
            .bottom = this->y + this->height,
        };
    }

    void macos_window::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->id);
        buffer.write(this->connection);
        buffer.write(this->x);
        buffer.write(this->y);
        buffer.write(this->width);
        buffer.write(this->height);
        buffer.write(this->level);
        buffer.write(this->ordered_in);
        buffer.write(this->opaque);
        buffer.write_string(this->title);
        buffer.write(this->backing_address);
        buffer.write(this->backing_stride);
        buffer.write(this->context);
        buffer.write(this->layer_context);
        buffer.write(this->shmem_address);
        buffer.write(this->shmem_entry);
        buffer.write(this->event_mask);
        buffer.write(this->perceived_type);
    }

    void macos_window::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->id);
        buffer.read(this->connection);
        buffer.read(this->x);
        buffer.read(this->y);
        buffer.read(this->width);
        buffer.read(this->height);
        buffer.read(this->level);
        buffer.read(this->ordered_in);
        buffer.read(this->opaque);
        buffer.read_string(this->title);
        buffer.read(this->backing_address);
        buffer.read(this->backing_stride);
        buffer.read(this->context);
        buffer.read(this->layer_context);
        buffer.read(this->shmem_address);
        buffer.read(this->shmem_entry);
        buffer.read(this->event_mask);
        buffer.read(this->perceived_type);
    }

    void macos_pending_shape::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->window_id);
        buffer.write(this->x);
        buffer.write(this->y);
        buffer.write(this->width);
        buffer.write(this->height);
    }

    void macos_pending_shape::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->window_id);
        buffer.read(this->x);
        buffer.read(this->y);
        buffer.read(this->width);
        buffer.read(this->height);
    }

    void macos_transaction::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->id);
        buffer.write(this->connection);
        buffer.write_vector(this->shapes);
    }

    void macos_transaction::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->id);
        buffer.read(this->connection);
        buffer.read_vector(this->shapes);
    }

    void macos_region::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->id);
        buffer.write(this->x);
        buffer.write(this->y);
        buffer.write(this->width);
        buffer.write(this->height);
    }

    void macos_region::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->id);
        buffer.read(this->x);
        buffer.read(this->y);
        buffer.read(this->width);
        buffer.read(this->height);
    }

    void macos_connection::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->id);
        buffer.write(this->port);
        buffer.write(this->event_port);
        buffer.write(this->shmem_entry);
        buffer.write(this->ca_context);
    }

    void macos_connection::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->id);
        buffer.read(this->port);
        buffer.read(this->event_port);
        buffer.read(this->shmem_entry);
        buffer.read(this->ca_context);
    }

    uint32_t macos_window_server::create_connection()
    {
        const auto id = this->next_connection_++;
        this->connections_.push_back(macos_connection{.id = id});
        return id;
    }

    bool macos_window_server::has_connection(const uint32_t connection) const
    {
        return connection == MACOS_MAIN_CONNECTION_ID || this->find_connection(connection) != nullptr;
    }

    macos_connection* macos_window_server::find_connection(const uint32_t connection)
    {
        const auto found = std::ranges::find(this->connections_, connection, &macos_connection::id);
        return found == this->connections_.end() ? nullptr : &*found;
    }

    const macos_connection* macos_window_server::find_connection(const uint32_t connection) const
    {
        const auto found = std::ranges::find(this->connections_, connection, &macos_connection::id);
        return found == this->connections_.end() ? nullptr : &*found;
    }

    const macos_connection* macos_window_server::connection_for_port(const uint32_t port) const
    {
        if (port == 0)
        {
            return nullptr;
        }

        const auto found = std::ranges::find(this->connections_, port, &macos_connection::port);
        return found == this->connections_.end() ? nullptr : &*found;
    }

    macos_window* macos_window_server::create_window(const uint32_t connection, const int32_t x, const int32_t y, const int32_t width,
                                                     const int32_t height)
    {
        if (!this->has_connection(connection))
        {
            return nullptr;
        }

        if (width <= 0 || height <= 0 || width > MACOS_GUI_MAX_WINDOW_DIMENSION || height > MACOS_GUI_MAX_WINDOW_DIMENSION)
        {
            return nullptr;
        }

        this->windows_.push_back(macos_window{
            .id = this->next_window_++,
            .connection = connection,
            .x = x,
            .y = y,
            .width = width,
            .height = height,
        });

        return &this->windows_.back();
    }

    macos_window* macos_window_server::find_window(const uint32_t id)
    {
        const auto found = std::ranges::find(this->windows_, id, &macos_window::id);
        return found == this->windows_.end() ? nullptr : &*found;
    }

    const macos_window* macos_window_server::find_window(const uint32_t id) const
    {
        const auto found = std::ranges::find(this->windows_, id, &macos_window::id);
        return found == this->windows_.end() ? nullptr : &*found;
    }

    bool macos_window_server::destroy_window(const uint32_t id)
    {
        return std::erase_if(this->windows_, [id](const macos_window& window) { return window.id == id; }) > 0;
    }

    const macos_window* macos_window_server::window_at(const int32_t x, const int32_t y) const
    {
        for (const auto& window : std::ranges::reverse_view(this->windows_))
        {
            if (!window.ordered_in || window.width <= 0 || window.height <= 0)
            {
                continue;
            }

            if (x >= window.x && x < window.x + window.width && y >= window.y && y < window.y + window.height)
            {
                return &window;
            }
        }

        return nullptr;
    }

    uint32_t macos_window_server::key_connection() const
    {
        for (const auto& window : std::ranges::reverse_view(this->windows_))
        {
            if (window.ordered_in && window.width > 0 && window.height > 0)
            {
                return window.connection;
            }
        }

        return 0;
    }

    uint32_t macos_window_server::create_region(const int32_t x, const int32_t y, const int32_t width, const int32_t height)
    {
        if (width < 0 || height < 0 || width > MACOS_GUI_MAX_WINDOW_DIMENSION || height > MACOS_GUI_MAX_WINDOW_DIMENSION)
        {
            return 0;
        }

        const auto id = this->next_region_++;
        this->regions_.push_back(macos_region{.id = id, .x = x, .y = y, .width = width, .height = height});
        return id;
    }

    const macos_region* macos_window_server::find_region(const uint32_t id) const
    {
        const auto found = std::ranges::find(this->regions_, id, &macos_region::id);
        return found == this->regions_.end() ? nullptr : &*found;
    }

    uint64_t macos_window_server::create_transaction(const uint32_t connection)
    {
        if (!this->has_connection(connection))
        {
            return 0;
        }

        const auto id = this->next_transaction_++;
        this->transactions_.push_back(macos_transaction{.id = id, .connection = connection});
        return id;
    }

    macos_transaction& macos_window_server::adopt_transaction(const uint64_t id, const uint32_t connection)
    {
        if (auto* existing = this->find_transaction(id))
        {
            return *existing;
        }

        this->transactions_.push_back(macos_transaction{.id = id, .connection = connection});
        return this->transactions_.back();
    }

    macos_transaction* macos_window_server::find_transaction(const uint64_t id)
    {
        const auto found = std::ranges::find(this->transactions_, id, &macos_transaction::id);
        return found == this->transactions_.end() ? nullptr : &*found;
    }

    const macos_transaction* macos_window_server::find_transaction(const uint64_t id) const
    {
        const auto found = std::ranges::find(this->transactions_, id, &macos_transaction::id);
        return found == this->transactions_.end() ? nullptr : &*found;
    }

    bool macos_window_server::commit_transaction(const uint64_t id)
    {
        auto* transaction = this->find_transaction(id);
        if (transaction == nullptr)
        {
            return false;
        }

        for (const auto& shape : transaction->shapes)
        {
            if (auto* window = this->find_window(shape.window_id))
            {
                window->x = shape.x;
                window->y = shape.y;
                window->width = shape.width;
                window->height = shape.height;
            }
        }

        // clear() keeps the capacity: commit is the hot path (60+ wire transactions to first frame,
        // measured) and must not allocate once warm.
        transaction->shapes.clear();
        return true;
    }

    void macos_window_server::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_vector(this->windows_);
        buffer.write_vector(this->regions_);
        buffer.write_vector(this->connections_);
        buffer.write(this->next_window_);
        buffer.write(this->next_region_);
        buffer.write(this->next_connection_);
        buffer.write_vector(this->transactions_);
        buffer.write(this->next_transaction_);
        buffer.write(this->server_port);
        buffer.write(this->render_server_port);
        buffer.write(this->event_port);
        buffer.write(this->session_death_watch_port);
        buffer.write_vector(this->event_shmem_entries);
        buffer.write_vector(this->display_shmem_entries);
        buffer.write(this->scoreboard_address);
        buffer.write(this->next_render_client);
        buffer.write(this->process_registered);
        buffer.write(this->main_application_connection);
        buffer.write(this->front_process_set);
    }

    void macos_window_server::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read_vector(this->windows_);
        buffer.read_vector(this->regions_);
        buffer.read_vector(this->connections_);
        buffer.read(this->next_window_);
        buffer.read(this->next_region_);
        buffer.read(this->next_connection_);
        buffer.read_vector(this->transactions_);
        buffer.read(this->next_transaction_);
        buffer.read(this->server_port);
        buffer.read(this->render_server_port);
        buffer.read(this->event_port);
        buffer.read(this->session_death_watch_port);
        buffer.read_vector(this->event_shmem_entries);
        buffer.read_vector(this->display_shmem_entries);
        buffer.read(this->scoreboard_address);
        buffer.read(this->next_render_client);
        buffer.read(this->process_registered);
        buffer.read(this->main_application_connection);
        buffer.read(this->front_process_set);
    }
}
