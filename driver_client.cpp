#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace std;

#ifdef _WIN32
using SocketHandle = SOCKET;
const SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;

bool socket_startup() {
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void socket_cleanup() { WSACleanup(); }

void close_socket(SocketHandle fd) { closesocket(fd); }

bool socket_call_failed(int result) { return result == SOCKET_ERROR; }
#else
using SocketHandle = int;
const SocketHandle INVALID_SOCKET_HANDLE = -1;

bool socket_startup() { return true; }

void socket_cleanup() {}

void close_socket(SocketHandle fd) { close(fd); }

bool socket_call_failed(int result) { return result < 0; }
#endif

bool socket_is_invalid(SocketHandle fd) {
  return fd == INVALID_SOCKET_HANDLE;
}

const char *DEFAULT_HOST = "127.0.0.1";
const int DEFAULT_PORT = 8000;

string send_command(const string &host, int port, const string &command) {
  if (!socket_startup())
    return "ERR socket_init";

  SocketHandle fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_is_invalid(fd)) {
    socket_cleanup();
    return "ERR socket";
  }

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    close_socket(fd);
    socket_cleanup();
    return "ERR bad_host";
  }

  if (socket_call_failed(
          connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))) {
    close_socket(fd);
    socket_cleanup();
    return "ERR connect";
  }

  string wire = command + "\n";
  send(fd, wire.c_str(), static_cast<int>(wire.size()), 0);

  char buf[4096];
  int n = recv(fd, buf, static_cast<int>(sizeof(buf) - 1), 0);
  close_socket(fd);
  socket_cleanup();

  if (n <= 0)
    return "ERR no_response";

  buf[n] = '\0';
  string resp(buf);
  while (!resp.empty() && (resp.back() == '\n' || resp.back() == '\r'))
    resp.pop_back();
  return resp;
}

bool starts_with(const string &s, const string &prefix) {
  return s.rfind(prefix, 0) == 0;
}

struct DriverStatus {
  bool ok = false;
  string state;
  int booking_id = -1;
  int x = 0;
  int y = 0;
  string raw;
};

DriverStatus parse_driver_status(const string &resp) {
  DriverStatus status;
  status.raw = resp;

  if (starts_with(resp, "ERR"))
    return status;

  istringstream iss(resp);
  if (iss >> status.state >> status.booking_id >> status.x >> status.y)
    status.ok = true;
  return status;
}

DriverStatus fetch_driver_status(const string &host, int port, int driver_id) {
  return parse_driver_status(
      send_command(host, port, "DRIVER " + to_string(driver_id)));
}

void print_help() {
  cout << "\nCommands:\n"
       << "  status             show driver state\n"
       << "  offer              show current booking offer\n"
       << "  watch              poll until an offer arrives\n"
       << "  accept [booking]   accept current/specified offer\n"
       << "  reject [booking]   reject current/specified offer\n"
       << "  finish [booking]   complete current/specified ride\n"
       << "  demo [px py dx dy] create a sample passenger booking\n"
       << "  online [x y]       go available for booking\n"
       << "  offline            stop receiving bookings\n"
       << "  help               show this menu\n"
       << "  quit               exit when not booked\n\n";
}

void print_driver_status(const DriverStatus &status) {
  if (!status.ok) {
    cout << status.raw << "\n";
    return;
  }

  cout << "Driver is " << status.state << " at (" << status.x << ","
       << status.y << ")";
  if (status.booking_id >= 0)
    cout << " with booking #" << status.booking_id;
  cout << ".\n";
}

void print_offer(const DriverStatus &status) {
  if (!status.ok) {
    cout << status.raw << "\n";
    return;
  }

  if (status.state != "RESERVED" || status.booking_id < 0) {
    cout << "No current offer. Driver is " << status.state << ".\n";
    return;
  }

  cout << "Offer #" << status.booking_id
       << " is reserved for this driver. Use `accept` or `reject`.\n";
}

int resolve_booking_id(const string &host, int port, int driver_id,
                       const string &action, istringstream &args) {
  int booking_id = -1;
  args >> booking_id;

  if (booking_id >= 0)
    return booking_id;

  DriverStatus status = fetch_driver_status(host, port, driver_id);
  if (!status.ok) {
    cout << status.raw << "\n";
    return -1;
  }

  if ((action == "accept" || action == "reject") &&
      status.state != "RESERVED") {
    cout << "No reserved offer to " << action << ". Driver is " << status.state
         << ".\n";
    return -1;
  }

  if (action == "finish" && status.state != "BOOKED") {
    cout << "No booked ride to finish. Driver is " << status.state << ".\n";
    return -1;
  }

  return status.booking_id;
}

int main(int argc, char **argv) {
  string host = DEFAULT_HOST;
  int port = DEFAULT_PORT;
  int driver_id = 1;

  if (argc >= 2)
    driver_id = atoi(argv[1]);
  if (argc >= 3)
    host = argv[2];
  if (argc >= 4)
    port = atoi(argv[3]);

  cout << "Driver client connected to " << host << ":" << port << "\n";
  cout << "Using driver id " << driver_id << "\n";
  print_help();

  string line;
  while (true) {
    cout << "driver#" << driver_id << "> ";
    if (!getline(cin, line))
      break;

    istringstream iss(line);
    string cmd;
    iss >> cmd;

    if (cmd.empty())
      continue;

    if (cmd == "quit" || cmd == "exit") {
      DriverStatus status = fetch_driver_status(host, port, driver_id);
      if (status.ok && status.state == "BOOKED") {
        cout << "Cannot quit while booked";
        if (status.booking_id >= 0)
          cout << " on booking #" << status.booking_id;
        cout << ". Finish the ride first.\n";
        continue;
      }
      break;
    }

    if (cmd == "help") {
      print_help();
      continue;
    }

    if (cmd == "online" || cmd == "available") {
      int x, y;
      string command = "ONLINE " + to_string(driver_id);
      if (iss >> x >> y)
        command += " " + to_string(x) + " " + to_string(y);
      cout << send_command(host, port, command) << "\n";
      continue;
    }

    if (cmd == "offline") {
      cout << send_command(host, port, "OFFLINE " + to_string(driver_id))
           << "\n";
      continue;
    }

    if (cmd == "status") {
      print_driver_status(fetch_driver_status(host, port, driver_id));
      continue;
    }

    if (cmd == "offer") {
      print_offer(fetch_driver_status(host, port, driver_id));
      continue;
    }

    if (cmd == "watch") {
      cout << "Watching for offers. Press Ctrl+C to stop the client.\n";
      while (true) {
        DriverStatus status = fetch_driver_status(host, port, driver_id);
        if (!status.ok) {
          cout << "\n" << status.raw << "\n";
          break;
        }

        if (status.state == "RESERVED" && status.booking_id >= 0) {
          print_offer(status);
          cout << "[a]ccept, [r]eject, [s]kip watch? ";

          string choice;
          getline(cin, choice);

          if (choice == "a" || choice == "accept") {
            cout << send_command(host, port,
                                 "ACCEPT " + to_string(driver_id) + " " +
                                     to_string(status.booking_id))
                 << "\n";
            break;
          }

          if (choice == "r" || choice == "reject") {
            cout << send_command(host, port,
                                 "REJECT " + to_string(driver_id) + " " +
                                     to_string(status.booking_id))
                 << "\n";
            break;
          }

          if (choice == "s" || choice == "skip")
            break;
        } else {
          cout << ".";
          cout.flush();
          this_thread::sleep_for(chrono::seconds(1));
        }
      }
      cout << "\n";
      continue;
    }

    if (cmd == "accept" || cmd == "reject" || cmd == "finish") {
      int booking_id = resolve_booking_id(host, port, driver_id, cmd, iss);

      if (booking_id < 0) {
        cout << "No booking id found. Try `offer` or pass the booking id.\n";
        continue;
      }

      string wire_cmd;
      if (cmd == "accept")
        wire_cmd = "ACCEPT";
      else if (cmd == "reject")
        wire_cmd = "REJECT";
      else
        wire_cmd = "FINISH";

      cout << send_command(host, port,
                           wire_cmd + " " + to_string(driver_id) + " " +
                               to_string(booking_id))
           << "\n";
      continue;
    }

    if (cmd == "demo") {
      int px = 1;
      int py = 1;
      int dx = 8;
      int dy = 8;
      iss >> px >> py >> dx >> dy;

      cout << send_command(host, port,
                           "BOOK " + to_string(px) + " " + to_string(py) +
                               " " + to_string(dx) + " " + to_string(dy))
           << "\n";
      continue;
    }

    cout << "Unknown command. Type `help`.\n";
  }

  return 0;
}
