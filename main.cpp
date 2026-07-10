#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using Socket = SOCKET;
const Socket INVALID_SOCKET_FD = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
const Socket INVALID_SOCKET_FD = -1;
#endif

using namespace std;

// ─── Constants ──────────────────────────────────────────────────────────────

const int MAP_W = 100;
const int MAP_H = 100;
const int DISPATCHER_THREADS = 4;
const int DRIVER_CONFIRM_TIMEOUT_MS = 5000;
const int MAX_BOOKING_RETRIES = 5;
const int SERVER_PORT = 8000;

// ─── Platform Socket Helpers ────────────────────────────────────────────────

bool socket_init() {
#ifdef _WIN32
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  signal(SIGPIPE, SIG_IGN);
  return true;
#endif
}

void socket_cleanup() {
#ifdef _WIN32
  WSACleanup();
#endif
}

void socket_close(Socket s) {
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

// ─── Domain Types ───────────────────────────────────────────────────────────

struct Position {
  int x;
  int y;
};

int grid_distance(Position a, Position b) {
  return abs(a.x - b.x) + abs(a.y - b.y);
}

enum class DriverState { AVAILABLE, RESERVED, BOOKED, OFFLINE };

enum class BookingState {
  QUEUED,
  MATCHING,
  WAITING,
  BOOKED,
  COMPLETED,
  FAILED
};

string driver_state_name(DriverState s) {
  switch (s) {
  case DriverState::AVAILABLE:
    return "AVAILABLE";
  case DriverState::RESERVED:
    return "RESERVED";
  case DriverState::BOOKED:
    return "BOOKED";
  case DriverState::OFFLINE:
    return "OFFLINE";
  }
  return "UNKNOWN";
}

string booking_state_name(BookingState s) {
  switch (s) {
  case BookingState::QUEUED:
    return "QUEUED";
  case BookingState::MATCHING:
    return "MATCHING";
  case BookingState::WAITING:
    return "WAITING";
  case BookingState::BOOKED:
    return "BOOKED";
  case BookingState::COMPLETED:
    return "COMPLETED";
  case BookingState::FAILED:
    return "FAILED";
  }
  return "UNKNOWN";
}

struct Driver {
  int id;
  DriverState state;
  int booking_id;
  Position pos;
  chrono::steady_clock::time_point offer_start_time;
  mutex mtx;
};

struct Booking {
  int id;
  BookingState state;
  int driver_id;
  int retry_count;
  Position pickup;
  Position destination;
  mutex mtx;
};

struct BookingView {
  int id;
  string state;
  int driver_id;
  int retry_count;
};

struct DriverView {
  int id;
  string state;
  int booking_id;
  int x;
  int y;
};

// ─── Logger ─────────────────────────────────────────────────────────────────

mutex log_mtx;

void log_event(const string &level, const string &event, int booking_id,
               int driver_id, const string &message) {
  lock_guard<mutex> lock(log_mtx);
  cerr << "[" << level << "] " << event << " booking=" << booking_id
       << " driver=" << driver_id << " " << message << endl;
}

// ─── BookingQueue ───────────────────────────────────────────────────────────

class BookingQueue {
private:
  queue<int> q;
  mutex mtx;
  condition_variable cv;

public:
  void push(int id) {
    {
      lock_guard<mutex> lock(mtx);
      q.push(id);
    }
    cv.notify_one();
  }

  int wait_pop() {
    unique_lock<mutex> lock(mtx);
    cv.wait(lock, [&] { return !q.empty(); });
    int id = q.front();
    q.pop();
    return id;
  }
};

// ─── BookingSystem ──────────────────────────────────────────────────────────

class BookingSystem {
private:
  BookingQueue booking_queue;

  mutex booking_reg_mtx;
  map<int, unique_ptr<Booking>> bookings;

  mutex driver_reg_mtx;
  map<int, unique_ptr<Driver>> drivers;

  mutex next_id_mtx;
  int next_booking_id;

public:
  BookingSystem() : next_booking_id(1) {
    log_event("INFO", "SERVER_STARTED", -1, -1,
              "drivers=0 waiting_for_driver_clients");
  }

  int create_booking(Position pickup, Position destination) {
    int id;
    {
      lock_guard<mutex> lock(next_id_mtx);
      id = next_booking_id++;
    }

    auto b = make_unique<Booking>();
    b->id = id;
    b->state = BookingState::QUEUED;
    b->driver_id = -1;
    b->retry_count = 0;
    b->pickup = pickup;
    b->destination = destination;

    {
      lock_guard<mutex> lock(booking_reg_mtx);
      bookings[id] = move(b);
    }

    booking_queue.push(id);
    log_event("INFO", "BOOKING_CREATED", id, -1, "queued");
    return id;
  }

  bool get_booking(int id, BookingView &out) {
    Booking *b = booking_ptr(id);
    if (!b)
      return false;

    lock_guard<mutex> lock(b->mtx);
    out.id = b->id;
    out.state = booking_state_name(b->state);
    out.driver_id = b->driver_id;
    out.retry_count = b->retry_count;
    return true;
  }

  vector<BookingView> list_bookings() {
    vector<BookingView> result;
    lock_guard<mutex> reg_lock(booking_reg_mtx);

    for (auto &entry : bookings) {
      Booking &b = *entry.second;
      lock_guard<mutex> lock(b.mtx);
      result.push_back(
          {b.id, booking_state_name(b.state), b.driver_id, b.retry_count});
    }

    return result;
  }

  bool get_driver(int id, DriverView &out) {
    Driver *d = driver_ptr(id);
    if (!d)
      return false;

    lock_guard<mutex> lock(d->mtx);
    out.id = d->id;
    out.state = driver_state_name(d->state);
    out.booking_id = d->booking_id;
    out.x = d->pos.x;
    out.y = d->pos.y;
    return true;
  }

  vector<DriverView> list_drivers() {
    vector<DriverView> result;
    lock_guard<mutex> reg_lock(driver_reg_mtx);

    for (auto &entry : drivers) {
      Driver &d = *entry.second;
      lock_guard<mutex> lock(d.mtx);
      result.push_back(
          {d.id, driver_state_name(d.state), d.booking_id, d.pos.x, d.pos.y});
    }

    return result;
  }

  bool set_driver_online(int driver_id, bool has_pos, Position pos) {
    if (driver_id <= 0)
      return false;

    Position initial_pos =
        has_pos ? pos : Position{rand() % MAP_W, rand() % MAP_H};
    Driver *d = driver_ptr_or_create(driver_id, initial_pos);
    if (!d)
      return false;

    unique_lock<mutex> driver_lock(d->mtx);
    if (d->state == DriverState::RESERVED || d->state == DriverState::BOOKED)
      return false;

    if (has_pos)
      d->pos = pos;
    d->state = DriverState::AVAILABLE;
    d->booking_id = -1;

    log_event("INFO", "DRIVER_ONLINE", -1, driver_id, "driver available");
    return true;
  }

  bool set_driver_offline(int driver_id) {
    Driver *d = driver_ptr(driver_id);
    if (!d)
      return false;

    bool should_requeue = false;
    int booking_id = -1;

    {
      unique_lock<mutex> driver_lock(d->mtx);
      if (d->state == DriverState::BOOKED)
        return false;

      if (d->state == DriverState::RESERVED) {
        booking_id = d->booking_id;
        Booking *b = booking_ptr(booking_id);
        if (!b)
          return false;

        unique_lock<mutex> booking_lock(b->mtx);
        if (b->state == BookingState::WAITING && b->driver_id == driver_id)
          should_requeue = requeue_or_fail_booking_locked(*b);
      }

      d->state = DriverState::OFFLINE;
      d->booking_id = -1;
    }

    if (should_requeue)
      booking_queue.push(booking_id);

    log_event("INFO", "DRIVER_OFFLINE", booking_id, driver_id,
              "driver offline");
    return true;
  }

  bool accept_booking(int driver_id, int booking_id) {
    Driver *d = driver_ptr(driver_id);
    if (!d)
      return false;

    unique_lock<mutex> driver_lock(d->mtx);
    if (d->state != DriverState::RESERVED || d->booking_id != booking_id)
      return false;

    Booking *b = booking_ptr(booking_id);
    if (!b)
      return false;

    unique_lock<mutex> booking_lock(b->mtx);
    if (b->state != BookingState::WAITING || b->driver_id != driver_id)
      return false;

    d->state = DriverState::BOOKED;
    b->state = BookingState::BOOKED;

    log_event("INFO", "DRIVER_ACCEPTED", booking_id, driver_id, "ride booked");
    return true;
  }

  bool reject_booking(int driver_id, int booking_id) {
    Driver *d = driver_ptr(driver_id);
    if (!d)
      return false;

    bool should_requeue = false;

    {
      unique_lock<mutex> driver_lock(d->mtx);
      if (d->state != DriverState::RESERVED || d->booking_id != booking_id)
        return false;

      Booking *b = booking_ptr(booking_id);
      if (!b)
        return false;

      unique_lock<mutex> booking_lock(b->mtx);
      if (b->state != BookingState::WAITING || b->driver_id != driver_id)
        return false;

      d->state = DriverState::AVAILABLE;
      d->booking_id = -1;

      should_requeue = requeue_or_fail_booking_locked(*b);
    }

    if (should_requeue)
      booking_queue.push(booking_id);

    log_event("INFO", "DRIVER_REJECTED", booking_id, driver_id,
              "offer rejected");
    return true;
  }

  bool finish_booking(int driver_id, int booking_id) {
    Driver *d = driver_ptr(driver_id);
    if (!d)
      return false;

    unique_lock<mutex> driver_lock(d->mtx);
    if (d->state != DriverState::BOOKED || d->booking_id != booking_id)
      return false;

    Booking *b = booking_ptr(booking_id);
    if (!b)
      return false;

    unique_lock<mutex> booking_lock(b->mtx);
    if (b->state != BookingState::BOOKED || b->driver_id != driver_id)
      return false;

    d->pos = b->destination;
    d->state = DriverState::AVAILABLE;
    d->booking_id = -1;
    b->state = BookingState::COMPLETED;

    log_event("INFO", "BOOKING_COMPLETED", booking_id, driver_id,
              "ride finished");
    return true;
  }

  void start_workers() {
    for (int i = 0; i < DISPATCHER_THREADS; i++) {
      thread t(&BookingSystem::dispatcher_loop, this);
      t.detach();
    }

    thread monitor(&BookingSystem::monitor_loop, this);
    monitor.detach();
  }

private:
  Booking *booking_ptr(int id) {
    lock_guard<mutex> lock(booking_reg_mtx);
    auto it = bookings.find(id);
    return it == bookings.end() ? nullptr : it->second.get();
  }

  Driver *driver_ptr(int id) {
    lock_guard<mutex> lock(driver_reg_mtx);
    auto it = drivers.find(id);
    return it == drivers.end() ? nullptr : it->second.get();
  }

  Driver *driver_ptr_or_create(int id, Position pos) {
    lock_guard<mutex> lock(driver_reg_mtx);
    auto it = drivers.find(id);
    if (it != drivers.end())
      return it->second.get();

    auto d = make_unique<Driver>();
    d->id = id;
    d->state = DriverState::OFFLINE;
    d->booking_id = -1;
    d->pos = pos;
    Driver *ptr = d.get();
    drivers[id] = move(d);
    return ptr;
  }

  bool requeue_or_fail(int booking_id) {
    Booking *b = booking_ptr(booking_id);
    if (!b)
      return false;

    bool should_requeue = false;
    {
      lock_guard<mutex> lock(b->mtx);
      if (b->state == BookingState::BOOKED ||
          b->state == BookingState::COMPLETED ||
          b->state == BookingState::FAILED)
        return false;

      should_requeue = requeue_or_fail_booking_locked(*b);
    }

    if (should_requeue)
      booking_queue.push(booking_id);
    return true;
  }

  bool requeue_or_fail_booking_locked(Booking &b) {
    b.retry_count++;
    b.driver_id = -1;

    if (b.retry_count >= MAX_BOOKING_RETRIES) {
      b.state = BookingState::FAILED;
      log_event("INFO", "BOOKING_FAILED", b.id, -1, "retry limit reached");
      return false;
    }

    b.state = BookingState::QUEUED;
    log_event("INFO", "BOOKING_REQUEUED", b.id, -1,
              "retry=" + to_string(b.retry_count));
    return true;
  }

  int find_nearest_driver(Position pickup) {
    int best_id = -1;
    int best_distance = INT_MAX;

    for (int pass = 0; pass < 3; pass++) {
      lock_guard<mutex> reg_lock(driver_reg_mtx);
      for (auto &entry : drivers) {
        Driver &d = *entry.second;
        if (!d.mtx.try_lock())
          continue;

        if (d.state == DriverState::AVAILABLE) {
          int dd = grid_distance(d.pos, pickup);
          if (dd < best_distance) {
            best_distance = dd;
            best_id = d.id;
          }
        }

        d.mtx.unlock();
      }

      if (best_id != -1)
        break;
    }

    return best_id;
  }

  void dispatcher_loop() {
    while (true) {
      int booking_id = booking_queue.wait_pop();
      Booking *b = booking_ptr(booking_id);
      if (!b)
        continue;

      Position pickup;
      {
        lock_guard<mutex> booking_lock(b->mtx);
        if (b->state != BookingState::QUEUED)
          continue;
        b->state = BookingState::MATCHING;
        pickup = b->pickup;
      }

      log_event("INFO", "BOOKING_MATCHING", booking_id, -1,
                "dispatcher started matching");

      while (true) {
        int driver_id = find_nearest_driver(pickup);

        if (driver_id < 0) {
          this_thread::sleep_for(chrono::milliseconds(2000));
          requeue_or_fail(booking_id);
          break;
        }

        Driver *d = driver_ptr(driver_id);
        if (!d)
          continue;

        unique_lock<mutex> driver_lock(d->mtx);
        if (d->state != DriverState::AVAILABLE) {
          driver_lock.unlock();
          this_thread::sleep_for(chrono::milliseconds(100));
          continue;
        }

        Booking *b2 = booking_ptr(booking_id);
        if (!b2)
          break;

        unique_lock<mutex> booking_lock(b2->mtx);
        if (b2->state != BookingState::MATCHING)
          break;

        b2->state = BookingState::WAITING;
        b2->driver_id = d->id;
        d->state = DriverState::RESERVED;
        d->booking_id = booking_id;
        d->offer_start_time = chrono::steady_clock::now();

        log_event("INFO", "DRIVER_RESERVED", booking_id, driver_id,
                  "dispatcher reserved driver");
        break;
      }
    }
  }

  void monitor_loop() {
    while (true) {
      vector<int> requeue_ids;
      vector<pair<int, int>> expired_events;

      {
        lock_guard<mutex> reg_lock(driver_reg_mtx);
        for (auto &entry : drivers) {
          Driver &d = *entry.second;
          if (!d.mtx.try_lock())
            continue;

          if (d.state == DriverState::RESERVED) {
            long long elapsed =
                chrono::duration_cast<chrono::milliseconds>(
                    chrono::steady_clock::now() - d.offer_start_time)
                    .count();

            if (elapsed >= DRIVER_CONFIRM_TIMEOUT_MS) {
              int booking_id = d.booking_id;
              int driver_id = d.id;
              Booking *b = booking_ptr(booking_id);

              if (b) {
                unique_lock<mutex> booking_lock(b->mtx);
                if (b->state == BookingState::WAITING &&
                    b->driver_id == driver_id) {
                  d.state = DriverState::AVAILABLE;
                  d.booking_id = -1;

                  bool should_requeue = requeue_or_fail_booking_locked(*b);
                  if (should_requeue)
                    requeue_ids.push_back(booking_id);
                  expired_events.push_back({booking_id, driver_id});
                }
              }
            }
          }

          d.mtx.unlock();
        }
      }

      for (int booking_id : requeue_ids)
        booking_queue.push(booking_id);

      for (auto &event : expired_events) {
        log_event("INFO", "DRIVER_TIMEOUT", event.first, event.second,
                  "reservation expired");
      }

      this_thread::sleep_for(chrono::milliseconds(50));
    }
  }
};

// ─── Protocol Parsing ───────────────────────────────────────────────────────

void uppercase(string &s) {
  for (char &c : s) {
    if (c >= 'a' && c <= 'z')
      c = char(c - 'a' + 'A');
  }
}

bool has_extra(istringstream &iss) {
  string extra;
  return bool(iss >> extra);
}

string format_booking(const BookingView &b) {
  return b.state + " " + to_string(b.driver_id) + " " +
         to_string(b.retry_count);
}

string format_driver(const DriverView &d) {
  return d.state + " " + to_string(d.booking_id) + " " + to_string(d.x) + " " +
         to_string(d.y);
}

string handle_command(BookingSystem &system, const string &line) {
  istringstream iss(line);
  string cmd;
  iss >> cmd;

  if (cmd.empty())
    return "ERR EMPTY";
  uppercase(cmd);

  if (cmd == "BOOK") {
    int px, py, dx, dy;
    if (!(iss >> px >> py >> dx >> dy) || has_extra(iss))
      return "ERR ARGS";

    int id = system.create_booking({px, py}, {dx, dy});
    return "OK " + to_string(id);
  }

  if (cmd == "BOOKING") {
    int id;
    if (!(iss >> id) || has_extra(iss))
      return "ERR ARGS";

    BookingView view;
    if (!system.get_booking(id, view))
      return "ERR NOT_FOUND";
    return format_booking(view);
  }

  if (cmd == "BOOKINGS") {
    if (has_extra(iss))
      return "ERR ARGS";

    vector<BookingView> views = system.list_bookings();
    string resp = "OK " + to_string(views.size());
    for (const BookingView &b : views) {
      resp += " " + to_string(b.id) + " " + b.state + " " +
              to_string(b.driver_id) + " " + to_string(b.retry_count);
    }
    return resp;
  }

  if (cmd == "ONLINE") {
    int id;
    if (!(iss >> id))
      return "ERR ARGS";

    int x, y;
    bool has_pos = false;
    Position pos = {0, 0};
    if (iss >> x) {
      if (!(iss >> y) || has_extra(iss))
        return "ERR ARGS";
      has_pos = true;
      pos = {x, y};
    }

    return system.set_driver_online(id, has_pos, pos) ? "OK" : "ERR";
  }

  if (cmd == "OFFLINE") {
    int id;
    if (!(iss >> id) || has_extra(iss))
      return "ERR ARGS";
    return system.set_driver_offline(id) ? "OK" : "ERR";
  }

  if (cmd == "DRIVER") {
    int id;
    if (!(iss >> id) || has_extra(iss))
      return "ERR ARGS";

    DriverView view;
    if (!system.get_driver(id, view))
      return "ERR NOT_FOUND";
    return format_driver(view);
  }

  if (cmd == "DRIVERS") {
    if (has_extra(iss))
      return "ERR ARGS";

    vector<DriverView> views = system.list_drivers();
    string resp = "OK " + to_string(views.size());
    for (const DriverView &d : views) {
      resp += " " + to_string(d.id) + " " + d.state + " " +
              to_string(d.booking_id) + " " + to_string(d.x) + " " +
              to_string(d.y);
    }
    return resp;
  }

  if (cmd == "ACCEPT" || cmd == "REJECT" || cmd == "FINISH") {
    int driver_id, booking_id;
    if (!(iss >> driver_id >> booking_id) || has_extra(iss))
      return "ERR ARGS";

    bool ok = false;
    if (cmd == "ACCEPT")
      ok = system.accept_booking(driver_id, booking_id);
    else if (cmd == "REJECT")
      ok = system.reject_booking(driver_id, booking_id);
    else
      ok = system.finish_booking(driver_id, booking_id);

    return ok ? "OK" : "ERR";
  }

  return "ERR UNKNOWN";
}

// ─── TCP Server ──────────────────────────────────────────────────────────────

class TcpServer {
private:
  Socket server_fd;

public:
  TcpServer() : server_fd(INVALID_SOCKET_FD) {}

  ~TcpServer() {
    if (server_fd != INVALID_SOCKET_FD)
      socket_close(server_fd);
  }

  bool start(int port) {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET_FD) {
      cerr << "socket() failed" << endl;
      return false;
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
               sizeof(yes));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

#ifdef _WIN32
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
#else
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif

    if (bind(server_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
      cerr << "bind() failed" << endl;
      return false;
    }

    if (listen(server_fd, 10) < 0) {
      cerr << "listen() failed" << endl;
      return false;
    }

    return true;
  }

  void run(BookingSystem &system) {
    while (true) {
      sockaddr_in client_addr;
      memset(&client_addr, 0, sizeof(client_addr));
#ifdef _WIN32
      int client_len = sizeof(client_addr);
#else
      socklen_t client_len = sizeof(client_addr);
#endif

      Socket client_fd =
          accept(server_fd, (sockaddr *)&client_addr, &client_len);
      if (client_fd == INVALID_SOCKET_FD)
        continue;

      char buffer[4096];
      int n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

      if (n > 0) {
        buffer[n] = '\0';
        char *nl = strchr(buffer, '\n');
        if (nl)
          *nl = '\0';

        string response = handle_command(system, string(buffer)) + "\n";
        send(client_fd, response.c_str(), (int)response.size(), 0);
      }

      socket_close(client_fd);
    }
  }
};

// ─── Main ───────────────────────────────────────────────────────────────────

int main() {
  srand((unsigned)time(nullptr));

  if (!socket_init()) {
    cerr << "socket init failed" << endl;
    return 1;
  }

  BookingSystem system;

  TcpServer server;
  if (!server.start(SERVER_PORT)) {
    socket_cleanup();
    return 1;
  }

  system.start_workers();
  server.run(system);

  socket_cleanup();
  return 0;
}
