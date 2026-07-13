#pragma once

#include "../common/types.hpp"

#include <mutex>
#include <vector>

using namespace std;

struct Booking {
  int id;
  BookingState state;
  int driver_id;
  int retry_count;
  Position pickup;
  Position destination;
  vector<int> skipped_driver_ids;
  mutex mtx;
};
