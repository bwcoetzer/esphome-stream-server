#include "stream_server.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"

#include "esphome/core/hal.h"
#ifdef USE_ESP32
#include <lwip/sockets.h>
#include <lwip/opt.h>
#include <lwip/tcp.h>
#endif
#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"

static const char *TAG = "stream_server";

using namespace esphome;

void StreamServerComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up stream server...");

    // The make_unique() wrapper doesn't like arrays, so initialize the unique_ptr directly.
    this->buf_ = std::unique_ptr<uint8_t[]>{new uint8_t[this->buf_size_]};

    struct sockaddr_storage bind_addr;
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2023, 4, 0)
    socklen_t bind_addrlen = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr), this->port_);
#else
    socklen_t bind_addrlen = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr), htons(this->port_));
#endif

#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 3, 0)    
    this->socket_ = socket::socket_ip_loop_monitored(SOCK_STREAM, PF_INET).release();    
#else
    this->socket_ = socket::socket_ip(SOCK_STREAM, PF_INET);
#endif 
      
    this->socket_->setblocking(false);
    this->socket_->bind(reinterpret_cast<struct sockaddr *>(&bind_addr), bind_addrlen);
    this->socket_->listen(8);

    this->publish_sensor();
}

void StreamServerComponent::loop() {
    this->accept();
    this->read();
    this->flush();
    this->write();
    this->enforce_limits();
    this->cleanup();
    this->publish_socket_stats();
}

void StreamServerComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "Stream Server:");
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 7, 0)
    char use_address[esphome::network::USE_ADDRESS_BUFFER_SIZE];
    ESP_LOGCONFIG(TAG, "  Address: %s:%u", esphome::network::get_use_address_to(use_address), this->port_);
#elif ESPHOME_VERSION_CODE >= VERSION_CODE(2025, 11, 0)
    ESP_LOGCONFIG(TAG, "  Address: %s:%u", esphome::network::get_use_address(), this->port_);
#else
    ESP_LOGCONFIG(TAG, "  Address: %s:%u", esphome::network::get_use_address().c_str(), this->port_);
#endif
#ifdef USE_BINARY_SENSOR
    LOG_BINARY_SENSOR("  ", "Connected:", this->connected_sensor_);
#endif
#ifdef USE_SENSOR
    LOG_SENSOR("  ", "Connection count:", this->connection_count_sensor_);
    LOG_SENSOR("  ", "Sockets used:", this->sockets_used_sensor_);
    LOG_SENSOR("  ", "Sockets max:", this->sockets_max_sensor_);
    LOG_SENSOR("  ", "Dropped clients:", this->dropped_clients_sensor_);
#endif
    ESP_LOGCONFIG(TAG, "  Max clients: %u (0 = unlimited)", (unsigned) this->max_clients_);
    ESP_LOGCONFIG(TAG, "  Idle timeout: %u ms (0 = off)", (unsigned) this->idle_timeout_ms_);
    ESP_LOGCONFIG(TAG, "  TCP keepalive idle: %u s (0 = off)", (unsigned) this->keepalive_idle_s_);
}

void StreamServerComponent::on_shutdown() {
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 3, 0)
    delete this->socket_;
    this->socket_ = nullptr;    
#endif 

    for (const Client &client : this->clients_)
        client.socket->shutdown(SHUT_RDWR);
}

void StreamServerComponent::publish_sensor() {
#ifdef USE_BINARY_SENSOR
    if (this->connected_sensor_)
        this->connected_sensor_->publish_state(this->clients_.size() > 0);
#endif
#ifdef USE_SENSOR
    if (this->connection_count_sensor_)
        this->connection_count_sensor_->publish_state(this->clients_.size());
#endif
}

void StreamServerComponent::accept() {
    struct sockaddr_storage client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    std::unique_ptr<socket::Socket> socket = this->socket_->accept(reinterpret_cast<struct sockaddr *>(&client_addr), &client_addrlen);
    if (!socket)
        return;

    socket->setblocking(false);

    if (this->keepalive_idle_s_ > 0) {
        // Let lwIP notice peers that vanished without a FIN/RST (dead link, killed container), instead of
        // keeping the client - and its socket table entry - forever.
        int enable = 1;
        int idle = this->keepalive_idle_s_;
        int interval = std::max(1, idle / 3);
        int count = 3;
        socket->setsockopt(SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
        socket->setsockopt(IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        socket->setsockopt(IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
        socket->setsockopt(IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    }

#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 1, 0)
    std::string identifier = std::string{esphome::socket::SOCKADDR_STR_LEN, 0};
    auto identifier_span = std::span<char, esphome::socket::SOCKADDR_STR_LEN>(identifier.data(), identifier.size());
    identifier.resize(socket->getpeername_to(identifier_span));
#else
    std::string identifier = socket->getpeername();
#endif

    this->clients_.emplace_back(std::move(socket), identifier, this->buf_head_);
    this->clients_.back().last_activity = esphome::millis();
    ESP_LOGD(TAG, "New client connected from %s", identifier.c_str());
    this->publish_sensor();
}

void StreamServerComponent::cleanup() {
    auto discriminator = [](const Client &client) { return !client.disconnected; };
    auto last_client = std::partition(this->clients_.begin(), this->clients_.end(), discriminator);
    if (last_client != this->clients_.end()) {
        this->clients_.erase(last_client, this->clients_.end());
        this->publish_sensor();
    }
}

void StreamServerComponent::read() {
    size_t len = 0;
    int available;
    while ((available = this->stream_->available()) > 0) {
        size_t free = this->buf_size_ - (this->buf_head_ - this->buf_tail_);
        if (free == 0) {
            // Only overwrite if nothing has been added yet, otherwise give flush() a chance to empty the buffer first.
            if (len > 0)
                return;

            ESP_LOGE(TAG, "Incoming bytes available, but outgoing buffer is full: stream will be corrupted!");
            free = std::min<size_t>(available, this->buf_size_);
            this->buf_tail_ += free;
            for (Client &client : this->clients_) {
                if (client.position < this->buf_tail_) {
                    ESP_LOGW(TAG, "Dropped %u pending bytes for client %s", this->buf_tail_ - client.position, client.identifier.c_str());
                    client.position = this->buf_tail_;
                }
            }
        }

        // Fill all available contiguous space in the ring buffer.
        len = std::min<size_t>(available, std::min<size_t>(this->buf_ahead(this->buf_head_), free));
        this->stream_->read_array(&this->buf_[this->buf_index(this->buf_head_)], len);
        this->buf_head_ += len;
    }
}

void StreamServerComponent::flush() {
    ssize_t written;
    this->buf_tail_ = this->buf_head_;
    for (Client &client : this->clients_) {
        if (client.disconnected || client.position == this->buf_head_)
            continue;

        // Split the write into two parts: from the current position to the end of the ring buffer, and from the start
        // of the ring buffer until the head. The second part might be zero if no wraparound is necessary.
        struct iovec iov[2];
        iov[0].iov_base = &this->buf_[this->buf_index(client.position)];
        iov[0].iov_len = std::min(this->buf_head_ - client.position, this->buf_ahead(client.position));
        iov[1].iov_base = &this->buf_[0];
        iov[1].iov_len = this->buf_head_ - (client.position + iov[0].iov_len);
        if ((written = client.socket->writev(iov, 2)) > 0) {
            client.position += written;
            client.last_activity = esphome::millis();
        } else if (written == 0 || errno == ECONNRESET) {
            ESP_LOGD(TAG, "Client %s disconnected", client.identifier.c_str());
            client.disconnected = true;
            continue;  // don't consider this client when calculating the tail position
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // Expected if the (TCP) transmit buffer is full, nothing to do.
        } else {
            // Any other error (ENOTCONN, EPIPE, ETIMEDOUT, ...) is fatal for this connection. Keeping the client
            // would pin buf_tail_ behind its frozen position and hold on to its socket table entry forever.
            ESP_LOGW(TAG, "Failed to write to client %s with error %d, dropping it", client.identifier.c_str(), errno);
            client.disconnected = true;
            this->dropped_clients_++;
            continue;
        }

        this->buf_tail_ = std::min(this->buf_tail_, client.position);
    }
}

void StreamServerComponent::write() {
    uint8_t buf[128];
    ssize_t read;
    for (Client &client : this->clients_) {
        if (client.disconnected)
            continue;

        while ((read = client.socket->read(&buf, sizeof(buf))) > 0) {
            this->stream_->write_array(buf, read);
            client.last_activity = esphome::millis();
        }

        if (read == 0 || errno == ECONNRESET) {
            ESP_LOGD(TAG, "Client %s disconnected", client.identifier.c_str());
            client.disconnected = true;
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // Expected if the (TCP) receive buffer is empty, nothing to do.
        } else {
            ESP_LOGW(TAG, "Failed to read from client %s with error %d, dropping it", client.identifier.c_str(), errno);
            client.disconnected = true;
            this->dropped_clients_++;
        }
    }
}

void StreamServerComponent::enforce_limits() {
    uint32_t now = esphome::millis();

    if (this->idle_timeout_ms_ > 0) {
        for (Client &client : this->clients_) {
            if (!client.disconnected && now - client.last_activity > this->idle_timeout_ms_) {
                ESP_LOGW(TAG, "Client %s idle for over %u ms, dropping it", client.identifier.c_str(),
                         (unsigned) this->idle_timeout_ms_);
                client.disconnected = true;
                this->dropped_clients_++;
            }
        }
    }

    if (this->max_clients_ > 0) {
        // Evict the least recently active clients first: a reconnecting consumer is far more likely to be the
        // live one than the oldest connection.
        while (true) {
            size_t live = 0;
            Client *stalest = nullptr;
            for (Client &client : this->clients_) {
                if (client.disconnected)
                    continue;
                live++;
                if (stalest == nullptr || (int32_t)(stalest->last_activity - client.last_activity) > 0)
                    stalest = &client;
            }
            if (live <= this->max_clients_ || stalest == nullptr)
                break;
            ESP_LOGW(TAG, "More than %u clients, dropping least active client %s", (unsigned) this->max_clients_,
                     stalest->identifier.c_str());
            stalest->disconnected = true;
            this->dropped_clients_++;
        }
    }
}

void StreamServerComponent::publish_socket_stats() {
#ifdef USE_SENSOR
    uint32_t now = esphome::millis();
    if (now - this->last_socket_stats_ < 10000)
        return;
    this->last_socket_stats_ = now;

    if (this->dropped_clients_sensor_)
        this->dropped_clients_sensor_->publish_state(this->dropped_clients_);

#ifdef USE_ESP32
    if (this->sockets_used_sensor_ || this->sockets_max_sensor_) {
        // lwIP hands out file descriptors in [LWIP_SOCKET_OFFSET, LWIP_SOCKET_OFFSET + MEMP_NUM_NETCONN). A slot
        // that is free reports EBADF.
        int used = 0;
        for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + MEMP_NUM_NETCONN; fd++) {
            int type;
            socklen_t len = sizeof(type);
            if (lwip_getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0)
                used++;
        }
        if (this->sockets_used_sensor_)
            this->sockets_used_sensor_->publish_state(used);
        if (this->sockets_max_sensor_)
            this->sockets_max_sensor_->publish_state(MEMP_NUM_NETCONN);
    }
#endif
#endif
}

StreamServerComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier, size_t position)
    : socket(std::move(socket)), identifier{identifier}, position{position} {}
