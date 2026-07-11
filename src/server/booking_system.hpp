#pragma once

#include "../common/types.hpp"
#include "booking.hpp"
#include "booking_queue.hpp"
#include "driver.hpp"
#include "logger.hpp"

#include <chrono>
#include <climits>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std;

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
