# Ride Booking System

Concurrent ride-booking simulation for CS0051 Parallel and Distributed Computing.

The project demonstrates how mutex synchronization prevents race conditions when multiple passenger requests try to reserve available drivers at the same time.

## Files

| File | Purpose |
|---|---|
| `main.cpp` | C++17 backend with dispatcher threads, booking/driver registries, mutex-protected state, monitor actor, and TCP command handler. |
| `driver_client.cpp` | C++17 terminal driver client that connects to the backend over TCP. |
| `passenger_client.cpp` | C++17 terminal passenger client that creates and watches ride bookings over TCP. |
| `SYSTEM_DESIGN.docx` | Server architecture and concurrency design document. |
| `README.md` | Project overview and run instructions. |

## Build and run

### macOS/Linux

```bash
g++ -std=c++17 -Wall -Wextra -pthread main.cpp -o core2
g++ -std=c++17 -Wall -Wextra -pthread driver_client.cpp -o driver_client
g++ -std=c++17 -Wall -Wextra -pthread passenger_client.cpp -o passenger_client
./core2
```

In another terminal:

```bash
./driver_client 1
```

In another terminal:

```bash
./passenger_client
```

### Windows with MinGW g++

```bash
g++ -std=c++17 -Wall -Wextra main.cpp -o core2.exe -lws2_32
g++ -std=c++17 -Wall -Wextra driver_client.cpp -o driver_client.exe -lws2_32
g++ -std=c++17 -Wall -Wextra passenger_client.cpp -o passenger_client.exe -lws2_32
core2.exe
```

In another terminal:

```bash
driver_client.exe 1
```

In another terminal:

```bash
passenger_client.exe
```

### Windows with MSVC

Run these commands from a Developer Command Prompt for Visual Studio:

```bat
cl /EHsc /std:c++17 main.cpp /Fe:core2.exe ws2_32.lib
cl /EHsc /std:c++17 driver_client.cpp /Fe:driver_client.exe ws2_32.lib
cl /EHsc /std:c++17 passenger_client.cpp /Fe:passenger_client.exe ws2_32.lib
core2.exe
```

In another terminal:

```bat
driver_client.exe 1
```

In another terminal:

```bat
passenger_client.exe
```

The backend listens on:

```text
127.0.0.1:8000
```

## TCP command protocol

One newline-terminated spaced command per TCP connection.

| Command | Arguments | Response |
|---|---|---|
| `BOOK` | `px py dx dy` | `OK booking_id` |
| `BOOKING` | `booking_id` | `STATE driver_id retry_count` |
| `BOOKINGS` | — | `OK count booking_id state driver_id retry_count ...` |
| `ONLINE` | `driver_id [x y]` | `OK` or `ERR` |
| `OFFLINE` | `driver_id` | `OK` or `ERR` |
| `DRIVER` | `driver_id` | `STATE booking_id pos_x pos_y` |
| `DRIVERS` | — | `OK count driver_id state booking_id pos_x pos_y ...` |
| `ACCEPT` | `driver_id booking_id` | `OK` or `ERR` |
| `REJECT` | `driver_id booking_id` | `OK` or `ERR` |
| `FINISH` | `driver_id booking_id` | `OK` or `ERR` |

### Command protocol examples

The command protocol is easiest to understand as client-to-server messages.

#### Passenger client flow

Start a passenger client:

```bash
./passenger_client
```

The passenger client defaults to `127.0.0.1:8000`. To connect to a different server:

```bash
./passenger_client 127.0.0.1 8000
```

Passenger client commands:

```text
book [px py dx dy]  request a ride (prompts if no args)
status <booking>    show current state of a booking
watch [booking]     poll a booking until it settles
list                show all bookings (testing only)
help                show the command menu
quit                exit
```

```text
Passenger client books a ride
  -> BOOK pickup_x pickup_y destination_x destination_y
  -> Server

Example:
Passenger client -> BOOK 1 2 8 9 -> Server
Server -> OK 42
```

The passenger can then poll the booking status:

```text
Passenger client asks for booking 42
  -> BOOKING 42
  -> Server

Possible responses:
Server -> QUEUED -1 0
Server -> MATCHING -1 0
Server -> WAITING 3 0
Server -> BOOKED 3 0
Server -> COMPLETED 3 0
Server -> FAILED -1 5
```

Example passenger terminal flow:

```text
passenger> book 1 1 8 8
Booking created: #42
passenger> watch
```

#### Driver client flow

A driver client first asks for its current state:

```text
Driver client puts driver 3 online
  -> ONLINE 3 4 7
  -> Server
Server -> OK

Driver client asks for driver 3
  -> DRIVER 3
  -> Server

Example responses:
Server -> OFFLINE -1 4 7
Server -> AVAILABLE -1 4 7
Server -> RESERVED 42 4 7
Server -> BOOKED 42 4 7
```

If the driver has a pending offer, it accepts or rejects the booking:

```text
Driver client accepts booking 42
  -> ACCEPT 3 42
  -> Server
Server -> OK

Driver client rejects booking 42
  -> REJECT 3 42
  -> Server
Server -> OK
```

After an accepted ride is complete, the driver finishes it:

```text
Driver client finishes booking 42
  -> FINISH 3 42
  -> Server
Server -> OK
```

The driver client blocks `quit` and `exit` while the driver is `BOOKED`. The ride must be finished first.

When the driver is no longer accepting rides:

```text
Driver client goes offline
  -> OFFLINE 3
  -> Server
Server -> OK
```

#### Registry queries

Clients can also request all bookings or all drivers: (FOR TESTING ONLY)

```text
Passenger/admin client -> BOOKINGS -> Server
Server -> OK 1 42 WAITING 3 0

Driver/admin client -> DRIVERS -> Server
Server -> OK 2 1 AVAILABLE -1 0 5 3 RESERVED 42 4 7
```

#### Netcat examples

```bash
printf 'BOOK 1 2 8 9\n' | nc 127.0.0.1 8000
printf 'BOOKING 42\n' | nc 127.0.0.1 8000
printf 'ONLINE 3 4 7\n' | nc 127.0.0.1 8000
printf 'DRIVER 3\n' | nc 127.0.0.1 8000
printf 'ACCEPT 3 42\n' | nc 127.0.0.1 8000
printf 'FINISH 3 42\n' | nc 127.0.0.1 8000
printf 'OFFLINE 3\n' | nc 127.0.0.1 8000
```

## Concurrency design

The core race condition is double-booking: two dispatcher threads could otherwise observe the same driver as available and assign that driver to different bookings.

The backend prevents this with:

- per-driver mutexes for driver state and assignment
- per-booking mutexes for booking state
- registry mutexes for container access
- dispatcher threads that reserve drivers atomically
- monitor actor that expires unaccepted reservations

Primary lock order:

```text
Driver::mtx -> BookingRegistry::mtx -> Booking::mtx
```
