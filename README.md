Stream server for ESPHome
=========================

Custom component for ESPHome to expose a UART stream over WiFi or Ethernet. Provides a serial-to-wifi bridge as known
from ESPLink or ser2net, using ESPHome.

This component creates a TCP server listening on port 6638 (by default), and relays all data between the connected
clients and the serial port. It doesn't support any control sequences, telnet options or RFC 2217, just raw data.

Usage
-----

Requires ESPHome v2022.3.0 or newer.

```yaml
external_components:
  - source: github://oxan/esphome-stream-server

stream_server:
```

You can set the UART ID and port to be used under the `stream_server` component.

```yaml
uart:
   id: uart_bus
   # add further configuration for the UART here

stream_server:
   uart_id: uart_bus
   port: 1234
```

Sensors
-------
The server provides a binary sensor that signals whether there currently is a client connected:

```yaml
binary_sensor:
  - platform: stream_server
    connected:
      name: Connected
```

It also provides a numeric sensor that indicates the number of connected clients:

```yaml
sensor:
  - platform: stream_server
    connection_count:
      name: Number of connections
```

Socket diagnostics
------------------
The sensor platform can also report how close the device is to running out of sockets (ESP32 only for the first two):

```yaml
sensor:
  - platform: stream_server
    connection_count:
      name: Number of connections
    sockets_used:        # lwIP sockets currently open on the whole device (API, OTA, web server, clients, ...)
      name: Sockets used
    sockets_max:         # CONFIG_LWIP_MAX_SOCKETS
      name: Sockets max
    dropped_clients:     # clients closed by the server because of an error, a timeout or `max_clients`
      name: Dropped clients
```

Dead clients
------------
A client that disappears without closing the connection (unplugged cable, killed container, lost route) used to stay in
the client list forever: only a zero-byte read/write or `ECONNRESET` removed it. Any other socket error (`ENOTCONN`,
`EPIPE`, `ETIMEDOUT`, ...) is now treated as fatal and the client is dropped, so it no longer pins the ring buffer and
its socket table entry is released. Three optional settings add further protection:

```yaml
stream_server:
    max_clients: 4      # drop the least recently active client when more connect (default 0: unlimited)
    idle_timeout: 2min  # drop clients that neither sent nor received anything for this long (default 0: off)
    keepalive: 30s      # enable TCP keepalive so lwIP detects silent peers (default 0: off)
```

Only enable `idle_timeout` if every consumer is expected to exchange data regularly.

Advanced
--------
It is possible to define multiple stream servers for multiple UARTs simultaneously:

```yaml
uart:
  - id: uart1
    # ...
  - id: uart2
    # ...

stream_server:
  - uart_id: uart1
    port: 1234
  - uart_id: uart2
    port: 1235
```

The stream server has an internal buffer into which UART data is read before it is transmitted over TCP. The size of
this buffer can be changed using the `buffer_size` option, and must be a power of two. Increasing the buffer size above
the default of 128 bytes can help to achieve optimal throughput, and is especially helpful when using high baudrates. It
can also be necessary to increase the [`rx_buffer_size`][uart-config] option of the UART itself.

```yaml
stream_server:
    buffer_size: 2048
```

[uart-config]: https://esphome.io/components/uart.html#configuration-variables
