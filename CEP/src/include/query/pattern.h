#pragma once

#include <chrono>  // NOLINT [build/c++11]
#include <cstddef>
#include <future>  // NOLINT [build/c++11]
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/event.h"
#include "query/query.h"

namespace cep {
using Event_Attr = std::pair<EventType, std::string>;
using index_pair = std::pair<std::string, std::string>;

enum Operator { OP_LESS, OP_LESSEQUAL, OP_GREATER, OP_GREATEREQUAL, OP_EQUAL, OP_NOTEQUAL, OP_ADD, OP_SUB };

// forward declaration
class EventVector;
/**
 * Condition is used to capture the relation between 2 attributes of events.
 * For simplicity we only consider 2-ary condition
 */
class Condition {
 public:
  Event_Attr operand1_;
  Event_Attr operand2_;
  Operator op1_{OP_ADD};
  Operator op2_{OP_LESS};
  attr_t constant_{};
  Condition() = default;
  Condition(Event_Attr left, Operator op1, Event_Attr right, Operator op2, attr_t constant)
      : operand1_(std::move(left)), operand2_(std::move(right)), op1_(op1), op2_(op2), constant_(constant) {}
  [[nodiscard]] auto VerifyCondition(const std::shared_ptr<Event> &event1,
                                     const std::shared_ptr<Event> &event2) const -> bool;
  static auto ValidateCondition(attr_t l, Operator op1, attr_t r, Operator op2, attr_t constant) -> bool;
  auto operator<(const Condition &other) const -> bool { return operand1_ < other.operand1_; }
};

/**
 * Predicate of Query
 */
class Predicate {
 private:
  std::map<Condition, std::vector<index_pair>> conditions_;
  std::string pr_str_;

 public:
  Predicate() = default;
  // Transform the predicate represented as string to Predicate Class
  explicit Predicate(const std::string &pr_str);
  auto AddCondition(const Condition &cond, index_pair pair);
  static auto GetIntAfterIPlus(const std::string &str) -> int;
  auto Verify(EventVector &seq) -> bool;
  auto operator<(const Predicate &other) const -> bool { return conditions_.size() < other.conditions_.size(); }
};

/**
 * PartialMatchState is the intermediate valid match state of a query pattern
 */
class PartialMatchState : public std::enable_shared_from_this<PartialMatchState> {
 private:
  std::vector<EventType> event_sequence_;
  std::map<EventType, std::weak_ptr<PartialMatchState>> children_;
  std::vector<Predicate> predicates_;

 public:
  PartialMatchState() = default;
  explicit PartialMatchState(std::vector<EventType> event_types, std::vector<Predicate> prs,
                             std::vector<query_t> shared_queries)
      : event_sequence_(std::move(event_types)),
        predicates_(std::move(prs)),
        shared_queries_(std::move(shared_queries)) {
    for (size_t i = 0; i < predicates_.size(); ++i) {
      query_predicate_map_[shared_queries_[i]] = predicates_[i];
    }
  }
  auto GetNextPartialMatchState(const EventType &et) -> std::shared_ptr<PartialMatchState>;
  auto AddChild(const EventType &et, std::weak_ptr<PartialMatchState> child);
  auto ContainChild(const EventType &target) -> bool;
  auto GetSharedQueryNumber() const -> size_t { return shared_queries_.size(); }
  // Indicate if the state is the complete match state.
  bool is_complete_state_{false};
  // Indicate if the state is the void state after NEG
  bool is_void_{false};
  std::vector<query_t> shared_queries_;
  std::string state_name_{};
  auto PrintChildren() -> void {
    std::cout << "Print Children of " << state_name_ << ": " << std::endl;
    for (auto &[key, val] : children_) {
      if (!val.expired()) {
        std::cout << key.name_ << " - " << val.lock()->state_name_ << std::endl;
      }
    }
  }
  std::map<query_t, Predicate> query_predicate_map_;
};

/**
 * Query's accepted state
 */
class CompleteMatchState : public PartialMatchState {
 private:
 public:
  explicit CompleteMatchState(query_t id, time_t window, std::vector<EventType> event_types,
                              const std::vector<Predicate> &prs, const std::vector<query_t> &queries, time_t latency);
  query_t query_id_;
  time_t time_window_;
  time_t latency_bound_;
  //  long double latency_violation_extend_{0};
  bool if_time_out_{false};
  bool if_pass_predicate_{false};
};

/**
 * QueryPlan is composed of PartialMatchStates,
 * either in the form of NFA or tree,
 * a.k.a. DAG.
 */
class QueryPlan {
 private:
  std::vector<std::shared_ptr<PartialMatchState>> states_;
  // Vertices that have 0 in-degree
  std::vector<std::shared_ptr<PartialMatchState>> starting_states_;

 public:
  QueryPlan() = default;
  auto AddState(const std::shared_ptr<PartialMatchState> &state_ptr) -> void;
  auto AddStartingState(const std::shared_ptr<PartialMatchState> &state_ptr) -> void;
  static auto AddEdge(const std::shared_ptr<PartialMatchState> &src, std::weak_ptr<PartialMatchState> dst,
                      const EventType &et) -> void;
  bool is_shared_{false};
};

class RawEventSegment {
 public:
  bool valid_{true};
  std::vector<Event> raw_event_vectors_;
  RawEventSegment() = default;
  auto AddEvent(const Event &event) -> void;
  auto SetShed() -> void { valid_ = false; }
  auto PrintSharedSegment() -> void {
    if (raw_event_vectors_.empty()) {
      std::cout << "no shared segment" << std::endl;
    } else {
      std::cout << "Shared segment is: ";
      for (auto &e : raw_event_vectors_) {
        std::cout << e.GetEventName() << " ";
      }
      std::cout << "\n";
    }
  }
};

class EventVector {
 public:
  std::vector<Event> unique_event_seq_;
  std::shared_ptr<RawEventSegment> shared_segment_ptr_{nullptr};
  std::shared_ptr<PartialMatchState> state_;

  int8_t query_num_{2};
  std::vector<long double> contributions_{0, 0};
  std::vector<long double> consumptions_{0, 0};
  std::vector<long double> cc_ratio_{0, 0};
  std::set<query_t> candidate_queries_;
  bool valid_{true};
  bool unique_switch_{false};

  explicit EventVector(int8_t num = 2);
  explicit EventVector(std::shared_ptr<PartialMatchState> state, int8_t num = 2);
  // copy constructor
  EventVector(const EventVector &other);
  auto AddEventShared(const Event &new_event) -> void;
  auto AddEventNonShared(const Event &new_event) -> void;
  auto PreAddEventShared(const Event &new_event, std::string l1, std::string l2) -> void;
  auto GetLastEventArrivalTime() -> uint64_t;
  // Verify specified query's predicate
  auto VerifyPredicate(query_t query_id) -> bool;
  // Verify the only candidate query's predicate
  auto VerifyPredicate() -> bool;
  // check if exist in time window
  auto CheckQ0TimeWindow(time_t q0_window) -> bool;
  auto CheckQ1TimeWindow(time_t q1_window) -> bool;

  auto PreCheckTimeWindow(time_t q_window) -> bool;
  [[nodiscard]] auto PreCheckTimeWindow(time_t q_window, const Event &new_event) const -> bool;

  auto CheckValidity() -> bool;
  [[nodiscard]] auto DynamicCheckValidity() const -> bool;
  auto PrintEventVector() -> void;
  auto SetInvalidity() -> void;
  auto DynamicSetInvalidity() -> void;
  auto GetAttrVector(const std::string &attr_name = "") -> std::vector<attr_t>;
  auto CheckEventLife(time_t coming_B_time, time_t q0_window) -> bool;
  [[nodiscard]] auto GetSize() const -> size_t;
  [[nodiscard]] auto GetCost() const -> size_t;
};
}  // namespace cep
