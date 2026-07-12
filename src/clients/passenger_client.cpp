#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
const int MAP_W = 10;
const int MAP_H = 10;
const int WATCH_POLL_MS = 1000;

const int AUTO_POLL_MS = 500;
const int AUTO_BOOKING_TIMEOUT_MS = 45000;
const int AUTO_MAP_W = 100;
const int AUTO_MAP_H = 100;

atomic<bool> auto_running(true);

void handle_auto_stop(int) { auto_running.store(false); }

//Networking 

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

//BOOKING status

struct BookingStatus {
  bool ok = false;
  string state;
  int driver_id = -1;
  int retry_count = 0;
  string raw;
};

BookingStatus parse_booking_status(const string &resp) {
  BookingStatus status;
  status.raw = resp;

  if (starts_with(resp, "ERR"))
    return status;

  istringstream iss(resp);
  if (iss >> status.state >> status.driver_id >> status.retry_count)
    status.ok = true;
  return status;
}

BookingStatus fetch_booking_status(const string &host, int port,
                                   int booking_id) {
  return parse_booking_status(
      send_command(host, port, "BOOKING " + to_string(booking_id)));
}

//BOOK px py dx dy -> OK booking_id

int parse_booking_id(const string &resp) {
  if (!starts_with(resp, "OK"))
    return -1;
  istringstream iss(resp);
  string tag;
  int id;
  iss >> tag >> id;
  return id;
}

//Input helpers

int prompt_coord(const string &label, int max_val) {
  int v;
  while (true) {
    cout << label << " (0-" << max_val - 1 << "): ";
    if (cin >> v && v >= 0 && v < max_val)
      return v;
    cout << "  invalid value, try again.\n";
    cin.clear();
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
  }
}

//Printing

void print_help() {
  cout << "\nCommands:\n"
       << "  book [px py dx dy]  request a ride (prompts if no args)\n"
       << "  status <booking>    show current state of a booking\n"
       << "  watch [booking]     poll a booking until it settles\n"
       << "  list                show all bookings (testing only)\n"
       << "  help                show this menu\n"
       << "  quit                exit\n\n";
}

void print_booking_status(const BookingStatus &status) {
  if (!status.ok) {
    cout << status.raw << "\n";
    return;
  }

  cout << "Booking is " << status.state;
  if (status.driver_id >= 0)
    cout << " (driver #" << status.driver_id << ")";
  if (status.retry_count > 0)
    cout << " [retries=" << status.retry_count << "]";
  cout << ".\n";
}

// States per README: QUEUED -> MATCHING -> WAITING -> BOOKED -> COMPLETED
// (or -> FAILED after MAX_BOOKING_RETRIES).
void narrate_status(const BookingStatus &s) {
  if (s.state == "QUEUED") {
    cout << "  [queued] waiting to be picked up by a dispatcher...\n";
  } else if (s.state == "MATCHING") {
    cout << "  [matching] looking for the nearest available driver...\n";
  } else if (s.state == "WAITING") {
    cout << "  [driver found] driver #" << s.driver_id
         << " has been offered your ride, waiting for them to accept...\n";
  } else if (s.state == "BOOKED") {
    cout << "  [confirmed] driver #" << s.driver_id
         << " accepted! Your ride is booked.\n";
  } else if (s.state == "COMPLETED") {
    cout << "  [completed] ride finished. Thanks for riding!\n";
  } else if (s.state == "FAILED") {
    cout << "  [failed] sorry, no driver could be found after "
         << s.retry_count << " attempts.\n";
  } else {
    cout << "  [" << s.state << "]\n";
  }
}

//Command handlers

void do_book(const string &host, int port, istringstream &args,
            int *last_booking_id) {
  int px, py, dx, dy;
  bool inline_args = static_cast<bool>(args >> px >> py >> dx >> dy);

  if (!inline_args) {
    cout << "Pickup location:\n";
    px = prompt_coord("  x", MAP_W);
    py = prompt_coord("  y", MAP_H);
    cout << "Destination:\n";
    dx = prompt_coord("  x", MAP_W);
    dy = prompt_coord("  y", MAP_H);
  }

  string resp = send_command(host, port,
                             "BOOK " + to_string(px) + " " + to_string(py) +
                                 " " + to_string(dx) + " " + to_string(dy));
  int id = parse_booking_id(resp);

  if (id < 0) {
    cout << resp << "\n";
    return;
  }

  cout << "Booking created: #" << id << "\n";
  *last_booking_id = id;
}

int resolve_booking_id(istringstream &args, int last_booking_id) {
  int id = -1;
  args >> id;
  if (id >= 0)
    return id;
  return last_booking_id;
}

void do_status(const string &host, int port, istringstream &args,
              int last_booking_id) {
  int id = resolve_booking_id(args, last_booking_id);
  if (id < 0) {
    cout << "No booking id given and no previous booking this session.\n";
    return;
  }
  print_booking_status(fetch_booking_status(host, port, id));
}

void do_watch(const string &host, int port, istringstream &args,
             int last_booking_id) {
  int id = resolve_booking_id(args, last_booking_id);
  if (id < 0) {
    cout << "No booking id given and no previous booking this session.\n";
    return;
  }

  cout << "Watching booking #" << id << " (Ctrl+C to stop the client)...\n";

  string last_state;
  int last_driver = -2;

  while (true) {
    BookingStatus s = fetch_booking_status(host, port, id);
    if (!s.ok) {
      cout << "  " << s.raw << "\n";
      break;
    }

    if (s.state != last_state || s.driver_id != last_driver) {
      narrate_status(s);
      last_state = s.state;
      last_driver = s.driver_id;
    }

    if (s.state == "COMPLETED" || s.state == "FAILED")
      break;

    this_thread::sleep_for(chrono::milliseconds(WATCH_POLL_MS));
  }
}

struct AutoBookingRow {
  string state = "STARTING";
  int booking_id = -1;
  int driver_id = -1;
  int retry_count = 0;
};

mutex auto_booking_mtx;
vector<AutoBookingRow> auto_booking_rows;
atomic<int> auto_created_count(0);
atomic<int> auto_timeout_count(0);
atomic<bool> auto_done(false);

void set_auto_booking_row(int index, const AutoBookingRow &row) {
  lock_guard<mutex> lock(auto_booking_mtx);
  if (index >= 0 && index < static_cast<int>(auto_booking_rows.size()))
    auto_booking_rows[index] = row;
}

void render_auto_passengers(int request_count) {
  auto started = chrono::steady_clock::now();
  while (auto_running.load()) {
    vector<AutoBookingRow> snapshot;
    {
      lock_guard<mutex> lock(auto_booking_mtx);
      snapshot = auto_booking_rows;
    }

    int starting = 0, queued = 0, matching = 0, waiting = 0, booked = 0;
    int completed = 0, failed = 0, timeout = auto_timeout_count.load();
    int other = 0;

    for (const AutoBookingRow &r : snapshot) {
      if (r.state == "STARTING") starting++;
      else if (r.state == "QUEUED") queued++;
      else if (r.state == "MATCHING") matching++;
      else if (r.state == "WAITING") waiting++;
      else if (r.state == "BOOKED") booked++;
      else if (r.state == "COMPLETED") completed++;
      else if (r.state == "FAILED") failed++;
      else if (r.state == "TIMEOUT") {}
      else other++;
    }

    int settled = completed + failed + timeout;
    int active = request_count - settled;
    int elapsed = static_cast<int>(chrono::duration_cast<chrono::seconds>(
                      chrono::steady_clock::now() - started).count());

    cout << "\033[H\033[J";
    cout << "Passenger Client --auto\n\n";
    cout << "requests=" << request_count << "  poll=" << AUTO_POLL_MS
         << "ms  status="
         << (auto_done.load() ? "DONE - Ctrl-C to exit" : "RUNNING")
         << "  elapsed=" << elapsed << "s\n";
    cout << "created=" << auto_created_count.load() << "  settled=" << settled
         << "  active=" << active << "\n\n";

    cout << left << setw(12) << "STATE" << "COUNT\n";
    cout << "------------------\n";
    cout << left << setw(12) << "STARTING" << starting << "\n";
    cout << left << setw(12) << "QUEUED" << queued << "\n";
    cout << left << setw(12) << "MATCHING" << matching << "\n";
    cout << left << setw(12) << "WAITING" << waiting << "\n";
    cout << left << setw(12) << "BOOKED" << booked << "\n";
    cout << left << setw(12) << "COMPLETED" << completed << "\n";
    cout << left << setw(12) << "FAILED" << failed << "\n";
    cout << left << setw(12) << "TIMEOUT" << timeout << "\n";
    cout << left << setw(12) << "OTHER" << other << "\n\n";

    cout << "Ctrl-C to stop\n";
    cout.flush();
    this_thread::sleep_for(chrono::milliseconds(AUTO_POLL_MS));
  }
}

void auto_passenger_worker(int index, const string &host, int port) {
  mt19937 rng(static_cast<unsigned>(time(nullptr)) + (index + 1) * 104729);
  uniform_int_distribution<int> pos(0, AUTO_MAP_W - 1);

  AutoBookingRow row;
  set_auto_booking_row(index, row);

  int px = pos(rng);
  int py = pos(rng);
  int dx = pos(rng);
  int dy = pos(rng);

  string resp = send_command(host, port,
                             "BOOK " + to_string(px) + " " + to_string(py) +
                                 " " + to_string(dx) + " " + to_string(dy));
  int id = parse_booking_id(resp);
  if (id < 0) {
    row.state = "FAILED";
    set_auto_booking_row(index, row);
    return;
  }

  auto_created_count++;
  row.booking_id = id;
  row.state = "QUEUED";
  set_auto_booking_row(index, row);

  auto deadline = chrono::steady_clock::now() +
                  chrono::milliseconds(AUTO_BOOKING_TIMEOUT_MS);

  while (auto_running.load() && chrono::steady_clock::now() < deadline) {
    BookingStatus s = fetch_booking_status(host, port, id);
    if (s.ok) {
      row.state = s.state;
      row.driver_id = s.driver_id;
      row.retry_count = s.retry_count;
      set_auto_booking_row(index, row);

      if (s.state == "COMPLETED" || s.state == "FAILED")
        return;
    }

    this_thread::sleep_for(chrono::milliseconds(AUTO_POLL_MS));
  }

  if (row.state != "COMPLETED" && row.state != "FAILED") {
    row.state = "TIMEOUT";
    auto_timeout_count++;
    set_auto_booking_row(index, row);
  }
}

int run_auto_passengers(int count, const string &host, int port) {
  if (count < 1)
    count = 1;

  signal(SIGINT, handle_auto_stop);
  signal(SIGTERM, handle_auto_stop);

  auto_booking_rows.assign(count, AutoBookingRow{});

  vector<thread> workers;
  thread renderer(render_auto_passengers, count);

  for (int i = 0; i < count; i++)
    workers.emplace_back(auto_passenger_worker, i, cref(host), port);

  for (thread &t : workers)
    t.join();

  auto_done.store(true);
  while (auto_running.load())
    this_thread::sleep_for(chrono::milliseconds(200));

  renderer.join();
  return 0;
}

//Main

int main(int argc, char **argv) {
  if (argc >= 2 && string(argv[1]) == "--auto") {
    int count = argc >= 3 ? atoi(argv[2]) : 100;
    string host = argc >= 4 ? argv[3] : DEFAULT_HOST;
    int port = argc >= 5 ? atoi(argv[4]) : DEFAULT_PORT;
    return run_auto_passengers(count, host, port);
  }

  string host = DEFAULT_HOST;
  int port = DEFAULT_PORT;

  if (argc >= 2)
    host = argv[1];
  if (argc >= 3)
    port = atoi(argv[2]);

  cout << "Passenger client connected to " << host << ":" << port << "\n";
  print_help();

  int last_booking_id = -1;
  string line;

  while (true) {
    cout << "passenger> ";
    if (!getline(cin, line))
      break;

    istringstream iss(line);
    string cmd;
    iss >> cmd;

    if (cmd.empty())
      continue;

    if (cmd == "quit" || cmd == "exit")
      break;

    if (cmd == "help") {
      print_help();
      continue;
    }

    if (cmd == "book") {
      do_book(host, port, iss, &last_booking_id);
      continue;
    }

    if (cmd == "status") {
      do_status(host, port, iss, last_booking_id);
      continue;
    }

    if (cmd == "watch") {
      do_watch(host, port, iss, last_booking_id);
      continue;
    }

    if (cmd == "list") {
      cout << send_command(host, port, "BOOKINGS") << "\n";
      continue;
    }

    cout << "Unknown command. Type `help`.\n";
  }

  return 0;
}
