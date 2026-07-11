#pragma once

#include "../common/types.hpp"
#include "booking_system.hpp"

#include <sstream>
#include <string>
#include <vector>

using namespace std;

inline void uppercase(string &s) {
  for (char &c : s) {
    if (c >= 'a' && c <= 'z')
      c = char(c - 'a' + 'A');
  }
}

inline bool has_extra(istringstream &iss) {
  string extra;
  return bool(iss >> extra);
}

inline string format_booking(const BookingView &b) {
  return b.state + " " + to_string(b.driver_id) + " " +
         to_string(b.retry_count);
}

inline string format_driver(const DriverView &d) {
  return d.state + " " + to_string(d.booking_id) + " " + to_string(d.x) +
         " " + to_string(d.y);
}

inline string handle_command(BookingSystem &system, const string &line) {
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
