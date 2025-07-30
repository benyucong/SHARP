#pragma once
#include <chrono>  // NOLINT [build/c++11]
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/event.h"

namespace cep {
using query_t = int8_t;
/**
 * Hardcode queries here
 */

class Query {
 public:
  query_t query_id_{};
  std::string pattern_;
  std::vector<EventType> event_types_;
  std::set<std::string> event_type_names_;
  std::string predicate_;
  time_t time_window_{};
  time_t detection_latency_{};
  Query() = default;
  Query(query_t id, std::string pattern, std::vector<EventType> ets, std::string predicate, time_t window,
        time_t latency)
      : query_id_(id),
        pattern_(std::move(pattern)),
        event_types_(std::move(ets)),
        predicate_(std::move(predicate)),
        time_window_(window),
        detection_latency_(latency) {
    for (const auto &et : event_types_) {
      event_type_names_.insert(et.name_);
    }
  }
  auto IncludeEvent(const Event &new_event) -> bool {
    return event_type_names_.find(new_event.GetEventName()) != event_type_names_.end();
  }
};
}  // namespace cep
