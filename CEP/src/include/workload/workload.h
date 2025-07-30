#pragma once
#include <set>
#include "common/event.h"
#include "query/pattern.h"
#include "query/query.h"

namespace cep {
class Workload {
 public:
  std::set<EventType> all_event_types_;
  // hardcode 2 queries
  Query q1_;
  Query q2_;
  QueryPlan non_shared_plan_;
  QueryPlan shared_plan_;
  QueryPlan dynamic_plan_;
  auto GetAllEventTypes() -> std::set<EventType>;
};
}  // namespace cep
