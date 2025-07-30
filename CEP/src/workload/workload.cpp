#include "workload/workload.h"
#include <algorithm>

namespace cep {
auto Workload::GetAllEventTypes() -> std::set<EventType> {
  std::set<EventType> all_event_types;
  std::set_union(q1_.event_types_.begin(), q1_.event_types_.end(), q2_.event_types_.begin(), q2_.event_types_.end(),
                 std::inserter(all_event_types, all_event_types.begin()));
  return all_event_types;
}
}  // namespace cep
