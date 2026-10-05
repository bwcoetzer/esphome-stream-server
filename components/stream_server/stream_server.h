#pragma once

#include "esphome/core/version.h"
#include "esphome/core/component.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/uart/uart.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

#include <memory>
#include <string>
#include <vector>

class StreamServerComponent : public esphome::Component {
public:
    StreamServerComponent() = default;
    explicit StreamServerComponent(esphome::uart::UARTComponent *stream) : stream_{stream} {}
    void set_uart_parent(esphome::uart::UARTComponent *parent) { this->stream_ = parent; }
    void set_buffer_size(size_t size) { this->buf_size_ = size; }
    void set_max_clients(size_t max_clients) { this->max_clients_ = max_clients; }
    void set_idle_timeout(uint32_t timeout_ms) { this->idle_timeout_ms_ = timeout_ms; }
    void set_keepalive(uint32_t idle_s) { this->keepalive_idle_s_ = idle_s; }

#ifdef USE_BINARY_SENSOR
    void set_connected_sensor(esphome::binary_sensor::BinarySensor *connected) { this->connected_sensor_ = connected; }
#endif
#ifdef USE_SENSOR
    void set_connection_count_sensor(esphome::sensor::Sensor *connection_count) { this->connection_count_sensor_ = connection_count; }
    void set_sockets_used_sensor(esphome::sensor::Sensor *sockets_used) { this->sockets_used_sensor_ = sockets_used; }
    void set_sockets_max_sensor(esphome::sensor::Sensor *sockets_max) { this->sockets_max_sensor_ = sockets_max; }
    void set_dropped_clients_sensor(esphome::sensor::Sensor *dropped) { this->dropped_clients_sensor_ = dropped; }
#endif

    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;

    float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }

    void set_port(uint16_t port) { this->port_ = port; }

protected:
    void publish_sensor();
    void publish_socket_stats();
    void drop_client(size_t index, const char *reason);
    void enforce_limits();

    void accept();
    void cleanup();
    void read();
    void flush();
    void write();

    size_t buf_index(size_t pos) { return pos & (this->buf_size_ - 1); }
    /// Return the number of consecutive elements that are ahead of @p pos in memory.
    size_t buf_ahead(size_t pos) { return (pos | (this->buf_size_ - 1)) - pos + 1; }

    struct Client {
        Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier, size_t position);

        std::unique_ptr<esphome::socket::Socket> socket{nullptr};
        std::string identifier{};
        bool disconnected{false};
        size_t position{0};
        uint32_t last_activity{0};  // millis() of the last byte moved in either direction
    };

    esphome::uart::UARTComponent *stream_{nullptr};
    uint16_t port_;
    size_t buf_size_;
    size_t max_clients_{0};          // 0 = unlimited
    uint32_t idle_timeout_ms_{0};    // 0 = never drop idle clients
    uint32_t keepalive_idle_s_{0};   // 0 = leave TCP keepalive off
    uint32_t dropped_clients_{0};
    uint32_t last_socket_stats_{0};

#ifdef USE_BINARY_SENSOR
    esphome::binary_sensor::BinarySensor *connected_sensor_;
#endif
#ifdef USE_SENSOR
    esphome::sensor::Sensor *connection_count_sensor_;
    esphome::sensor::Sensor *sockets_used_sensor_{nullptr};
    esphome::sensor::Sensor *sockets_max_sensor_{nullptr};
    esphome::sensor::Sensor *dropped_clients_sensor_{nullptr};
#endif

    std::unique_ptr<uint8_t[]> buf_{};
    size_t buf_head_{0};
    size_t buf_tail_{0};

#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 3, 0)
    esphome::socket::ListenSocket *socket_{nullptr};
#else
    std::unique_ptr<esphome::socket::Socket> socket_{};
#endif 

    std::vector<Client> clients_{};
};
