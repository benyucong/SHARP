#include "query/pattern.h"
#include <cmath>
#include <cstddef>
#include <future>  // NOLINT [build/c++11]
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/event.h"
#include "execution/experiment.h"

namespace cep {

auto Condition::ValidateCondition(attr_t l, Operator op1, attr_t r, Operator op2, attr_t constant) -> bool {
  auto res = l;
  switch (op1) {
    case OP_ADD:
      res += r;
      break;
    case OP_SUB:
      res -= r;
      break;
    default:
      return false;
  }
  bool compare_res;
  switch (op2) {
    case OP_LESS:
      compare_res = res < constant;
      break;
    case OP_LESSEQUAL:
      compare_res = res <= constant;
      break;
    case OP_GREATER:
      compare_res = res > constant;
      break;
    case OP_GREATEREQUAL:
      compare_res = res >= constant;
      break;
    case OP_EQUAL:
      compare_res = res == constant;
      break;
    case OP_NOTEQUAL:
      compare_res = res != constant;
      break;
    default:
      return false;
  }
  return compare_res;
}

auto Condition::VerifyCondition(const std::shared_ptr<Event> &event1, const std::shared_ptr<Event> &event2) const
    -> bool {
  auto left = event1->GetAttributeValue(operand1_.second);
  auto right = event2->GetAttributeValue(operand2_.second);
  return ValidateCondition(left, op1_, right, op2_, constant_);
}

Predicate::Predicate(const std::string &pr_str) {
  pr_str_ = pr_str;
  auto et_attr_a = std::make_pair(EventType("A"), "x");
  auto et_attr_b = std::make_pair(EventType("B"), "x");
  if (pr_str == "a.x > b[i].x && b[i].x > b[i+1].x") {
    auto condition1 = Condition(et_attr_a, OP_SUB, et_attr_b, OP_GREATER, 0);
    auto condition2 = Condition(et_attr_b, OP_SUB, et_attr_b, OP_GREATER, 0);
    index_pair pair1 = {"0", "i"};
    index_pair pair2 = {"i", "i+1"};
    conditions_[condition1] = {pair1};
    conditions_[condition2] = {pair2};
  } else if (pr_str == "b[i].x > b[i+1].x && b[i].x + b[i+1].x > 10") {
    auto condition1 = Condition(et_attr_b, OP_SUB, et_attr_b, OP_GREATER, 0);
    auto condition2 = Condition(et_attr_b, OP_ADD, et_attr_b, OP_GREATER, 10);
    index_pair pair1 = {"i", "i+1"};
    index_pair pair2 = {"i", "i+1"};
    conditions_[condition1] = {pair1};
    conditions_[condition2] = {pair2};
  }
}

auto Predicate::AddCondition(const Condition &cond, index_pair pair) {
  auto it = conditions_.find(cond);
  if (it == conditions_.end()) {
    std::vector<index_pair> tmp{std::move(pair)};
    conditions_[cond] = tmp;
  } else {
    conditions_[cond].push_back(pair);
  }
}

auto Predicate::GetIntAfterIPlus(const std::string &str) -> int {
  // Find the position of "i+"
  size_t pos = str.find("i+");
  if (pos == std::string::npos) {
    return 0;  // "i+" not found
  }

  // Extract the substring after "i+"
  std::string substr = str.substr(pos + 2);  // Start from the character after "i+"

  // Parse the integer from the substring
  std::stringstream ss(substr);
  int num;
  ss >> num;

  // Check if the parsing was successful
  if (ss.fail() || !ss.eof()) {
    // Parsing failed or there are extra characters after the integer
    return 0;  // Return 0 or handle the error accordingly
  }
  return num;
}

auto Predicate::Verify(EventVector &seq) -> bool {
  // to do: try to use incremental way
  // modify the shared queries after checking
  std::vector<Event> events_vec;
  // events_vec.reserve(seq.unique_event_seq_.size() + seq.shared_segment_ptr_->raw_event_vectors_.size());
  if (seq.shared_segment_ptr_) {
    events_vec.insert(events_vec.end(), seq.shared_segment_ptr_->raw_event_vectors_.begin(),
                      seq.shared_segment_ptr_->raw_event_vectors_.end());
  }
  if (!seq.unique_event_seq_.empty()) {
    events_vec.insert(events_vec.end(), seq.unique_event_seq_.begin(), seq.unique_event_seq_.end());
  }
  if (pr_str_ == "q0_p1" || pr_str_ == "q0_p2") {
    if (std::fabs(events_vec.begin()->GetAttributeValue("id") - events_vec.rbegin()->GetAttributeValue("id")) > 1e-5L) {
      // std::cout << "id failed " << std::endl;
      return false;
    }

    if (events_vec.size() == 2 && events_vec[0].GetAttributeValue("v") >= events_vec[1].GetAttributeValue("v")) {
      // std::cout << "p1 failed " << std::endl;
      return false;
    }

    if (events_vec.size() == 4 && events_vec[1].GetAttributeValue("v") + events_vec[2].GetAttributeValue("v") >=
                                      events_vec[3].GetAttributeValue("v")) {
      // std::cout << "p2 failed " << std::endl;
      return false;
    }

    if (events_vec.size() == 6) {
      auto lat1 = events_vec[3].GetAttributeValue("x") * M_PI / 180.0L;
      auto lon1 = events_vec[3].GetAttributeValue("y") * M_PI / 180.0L;
      auto lat2 = events_vec[4].GetAttributeValue("x") * M_PI / 180.0L;
      auto lon2 = events_vec[4].GetAttributeValue("y") * M_PI / 180.0L;
      // Haversine formula
      auto distance =
          2 * 6371 * 1000 *
          std::asin(std::sqrt(std::pow(std::sin(0.5 * (lat2 - lat1)), 2) +
                              std::cos(lat1) * std::cos(lat2) * std::pow(std::sin(0.5 * (lon2 - lon1)), 2)));
      // std::cout << "distance is" << distance << std::endl;
      return distance <= events_vec[5].GetAttributeValue("v");
    }
  } else if (pr_str_ == "q2_p1" || pr_str_ == "q2_p2") {
    if (std::fabs(events_vec.begin()->GetAttributeValue("id") - events_vec.rbegin()->GetAttributeValue("id")) > 1e-5L) {
      // std::cout << "id failed " << std::endl;
      return false;
    }
    if (events_vec.rbegin()->GetEventName() == "C" || events_vec.rbegin()->GetEventName() == "E") {
      auto right_sum = events_vec.rbegin()->GetAttributeValue("x");
      events_vec.pop_back();
      attr_t left_sum = 0;
      for (const auto &it : events_vec) {
        left_sum += it.GetAttributeValue("x");
      }
      return left_sum < right_sum;
    }
    if (seq.state_->is_complete_state_) {
      auto right_sum = events_vec.rbegin()->GetAttributeValue("x");
      events_vec.pop_back();
      right_sum += events_vec.rbegin()->GetAttributeValue("x");
      events_vec.pop_back();
      attr_t left_sum = 0;
      for (const auto &it : events_vec) {
        left_sum += it.GetAttributeValue("x");
      }
      return left_sum < right_sum;
    }
  } else if (pr_str_ == "q1_p1" || pr_str_ == "q1_p2") {
    if (std::fabs(events_vec.begin()->GetAttributeValue("id") - events_vec.rbegin()->GetAttributeValue("id")) > 1e-5L) {
      // std::cout << "id failed " << std::endl;
      return false;
    }
  }
  return true;
}

/**
 * Partial Match
 */
auto PartialMatchState::GetNextPartialMatchState(const EventType &et) -> std::shared_ptr<PartialMatchState> {
  //  std::cout << "adding event type: " << et.name_ << std::endl;
  for (const auto &[key, val] : children_) {
    //    std::cout << " children's name:" << key.name_ << std::endl;
    if (key.name_ == et.name_ && !val.expired()) {
      return val.lock();
    }
  }
  return shared_from_this();
}

auto PartialMatchState::AddChild(const cep::EventType &et, std::weak_ptr<PartialMatchState> child) {
  if (children_.find(et) == children_.end()) {
    children_[et] = std::move(child);
  } else {
    std::cerr << "error in adding transition edge" << std::endl;
    std::exit(EXIT_FAILURE);
  }
}

auto PartialMatchState::ContainChild(const EventType &target) -> bool {
  return std::any_of(children_.begin(), children_.end(),
                     [&target](const auto &child) { return child.first.name_ == target.name_; });
}

CompleteMatchState::CompleteMatchState(cep::query_t id, time_t window, std::vector<EventType> event_types,
                                       const std::vector<Predicate> &prs, const std::vector<query_t> &queries,
                                       time_t latency)
    : PartialMatchState(std::move(event_types), prs, queries),
      query_id_(id),
      time_window_(window),
      latency_bound_(latency) {
  is_complete_state_ = true;
}
auto QueryPlan::AddState(const std::shared_ptr<PartialMatchState> &state_ptr) -> void { states_.push_back(state_ptr); }
auto QueryPlan::AddStartingState(const std::shared_ptr<PartialMatchState> &state_ptr) -> void {
  starting_states_.push_back(state_ptr);
  states_.push_back(state_ptr);
}
auto QueryPlan::AddEdge(const std::shared_ptr<PartialMatchState> &src, std::weak_ptr<PartialMatchState> dst,
                        const EventType &et) -> void {
  src->AddChild(et, std::move(dst));
}

/**
 * EventVector
 */
auto RawEventSegment::AddEvent(const Event &event) -> void { raw_event_vectors_.push_back(event); }

EventVector::EventVector(int8_t num) : query_num_(num), consumptions_(num), cc_ratio_(num) {
  for (query_t i = 0; i < query_num_; ++i) {
    candidate_queries_.insert(i);
  }
}

EventVector::EventVector(std::shared_ptr<PartialMatchState> state, int8_t num)
    : state_(std::move(state)), query_num_(num), consumptions_(num), cc_ratio_(num) {
  for (query_t i = 0; i < query_num_; ++i) {
    candidate_queries_.insert(i);
  }
}
auto EventVector::AddEventShared(const Event &new_event) -> void {
  // need to verify predicate efficiently
  if (shared_segment_ptr_ == nullptr) {
    unique_event_seq_.push_back(new_event);
  } else if (shared_segment_ptr_->raw_event_vectors_.empty() ||
             shared_segment_ptr_->raw_event_vectors_.back().arrival_time_ != new_event.arrival_time_) {
    shared_segment_ptr_->AddEvent(new_event);
  }

  state_ = state_->GetNextPartialMatchState(new_event.GetEventType());
}

auto EventVector::AddEventNonShared(const Event &new_event) -> void {
  // simply insert event to unique event seq, keep shared seg as nullptr
  unique_event_seq_.push_back(new_event);
  state_ = state_->GetNextPartialMatchState(new_event.GetEventType());
}

auto EventVector::VerifyPredicate(query_t query_id) -> bool {
  if (std::find(state_->shared_queries_.begin(), state_->shared_queries_.end(), query_id) ==
      state_->shared_queries_.end()) {
    return false;
  }
  auto it = state_->query_predicate_map_.find(query_id);
  if (it == state_->query_predicate_map_.end()) {
    return true;
  }
  return it->second.Verify(*this);
}

auto EventVector::VerifyPredicate() -> bool {
  auto num = state_->GetSharedQueryNumber();
  if (num != 1) {
    return false;
  }

  auto query_id = state_->shared_queries_[0];
  return VerifyPredicate(query_id);
}

auto EventVector::PrintEventVector() -> void {
  if (shared_segment_ptr_) {
    for (const auto &event : shared_segment_ptr_->raw_event_vectors_) {
      std::cout << event.GetEventName() << " ";
    }
  }
  for (const Event &event : unique_event_seq_) {
    std::cout << event.GetEventName() << " ";
  }
  std::cout << std::endl;
}

auto EventVector::CheckValidity() -> bool {
  if (!valid_ || state_->is_void_) {
    return false;
  }

  if (shared_segment_ptr_ && !shared_segment_ptr_->valid_) {
    // shared segment has been invalid
    shared_segment_ptr_.reset();
    valid_ = false;
    return false;
  }

  return true;
}

auto EventVector::SetInvalidity() -> void {
  valid_ = false;
  if (shared_segment_ptr_) {
    shared_segment_ptr_->valid_ = false;
    shared_segment_ptr_.reset();
  }
}

auto EventVector::GetLastEventArrivalTime() -> uint64_t {
  if (shared_segment_ptr_ == nullptr) {
    return unique_event_seq_.back().arrival_time_;
  }
  return shared_segment_ptr_->raw_event_vectors_.back().arrival_time_;
}
auto EventVector::GetAttrVector(const std::string &attr_name) -> std::vector<attr_t> {
  std::vector<attr_t> attr_vec;
  if (shared_segment_ptr_) {
    for (const auto &event : shared_segment_ptr_->raw_event_vectors_) {
      auto tmp_vec = event.GetAllAttributeValues();
      attr_vec.insert(attr_vec.end(), tmp_vec.begin(), tmp_vec.end());
    }
  }
  for (const auto &event : unique_event_seq_) {
    auto tmp_vec = event.GetAllAttributeValues();
    attr_vec.insert(attr_vec.end(), tmp_vec.begin(), tmp_vec.end());
  }
  // auto earliest_time = shared_segment_ptr_ ? shared_segment_ptr_->raw_event_vectors_.begin()->arrival_time_
  //                                          : unique_event_seq_.begin()->arrival_time_;
  // if (shared_segment_ptr_) {
  //   for (const auto &e : shared_segment_ptr_->raw_event_vectors_) {
  //     attr_vec.push_back(static_cast<attr_t>(e.arrival_time_ - earliest_time));
  //   }
  // }
  // for (const auto &e : unique_event_seq_) {
  //   attr_vec.push_back(static_cast<attr_t>(e.arrival_time_ - earliest_time));
  // }
  return attr_vec;
}

EventVector::EventVector(const EventVector &other)
    : unique_event_seq_(other.unique_event_seq_),
      state_(other.state_),
      query_num_(other.query_num_),
      contributions_(other.contributions_),
      consumptions_(other.consumptions_),
      cc_ratio_(other.cc_ratio_),
      candidate_queries_(other.candidate_queries_),
      valid_(other.valid_),
      unique_switch_(other.unique_switch_) {
  // deep copy todo
  if (other.shared_segment_ptr_ && !other.unique_switch_ && other.unique_event_seq_.empty()) {
    shared_segment_ptr_ = std::make_shared<RawEventSegment>(*other.shared_segment_ptr_);
  } else {
    shared_segment_ptr_ = other.shared_segment_ptr_;
  }
}
auto EventVector::GetSize() const -> size_t {
  if (shared_segment_ptr_ == nullptr) {
    return unique_event_seq_.size();
  }
  return unique_event_seq_.size() + shared_segment_ptr_->raw_event_vectors_.size();
}

auto EventVector::GetCost() const -> size_t {
  if (shared_segment_ptr_ && shared_segment_ptr_.use_count() == 1) {
    return unique_event_seq_.size() + shared_segment_ptr_->raw_event_vectors_.size();
  }
  return unique_event_seq_.size();
}

auto EventVector::CheckQ1TimeWindow(time_t q1_window) -> bool {
  if (state_->state_name_ == "p3") {
    //    std::cout << "====checking p3 window!!!!!! ";
    time_t earliest_event_time;
    time_t last_event_time;
    if (shared_segment_ptr_) {
      earliest_event_time = shared_segment_ptr_->raw_event_vectors_.begin()->arrival_time_;
      last_event_time = shared_segment_ptr_->raw_event_vectors_.rbegin()->arrival_time_;
    } else {
      earliest_event_time = unique_event_seq_.begin()->arrival_time_;
      last_event_time = unique_event_seq_.rbegin()->arrival_time_;
    }
    return (last_event_time - earliest_event_time) <= q1_window;
  }
  // only set window time for q1
  return true;
}

auto EventVector::CheckQ0TimeWindow(time_t q_window) -> bool {
  if (state_->state_name_ == "p2") {
    time_t last_event_time;
    auto earliest_event_time = unique_event_seq_.begin()->arrival_time_;
    if (shared_segment_ptr_) {
      last_event_time = shared_segment_ptr_->raw_event_vectors_.rbegin()->arrival_time_;
    } else {
      last_event_time = unique_event_seq_.rbegin()->arrival_time_;
    }

    return (last_event_time - earliest_event_time) <= q_window;
  }
  return true;
}

auto EventVector::PreCheckTimeWindow(cep::time_t q0_window) -> bool {
  if (shared_segment_ptr_ == nullptr && unique_event_seq_.empty()) {
    return true;
  }
  time_t last_event_time = unique_event_seq_.empty() ? shared_segment_ptr_->raw_event_vectors_.rbegin()->arrival_time_
                                                     : unique_event_seq_.rbegin()->arrival_time_;
  time_t earliest_event_time = shared_segment_ptr_ ? shared_segment_ptr_->raw_event_vectors_.begin()->arrival_time_
                                                   : unique_event_seq_.begin()->arrival_time_;
  return last_event_time - earliest_event_time <= q0_window;
}

auto EventVector::CheckEventLife(time_t coming_B_time, time_t q0_window) -> bool {
  if (state_->state_name_ == "p1") {
    return (coming_B_time - unique_event_seq_.begin()->arrival_time_) <= q0_window;
  }
  return true;
}

auto EventVector::DynamicSetInvalidity() -> void {
  valid_ = false;
  if (shared_segment_ptr_) {
    shared_segment_ptr_.reset();
  }
}

auto EventVector::DynamicCheckValidity() const -> bool { return valid_ && (!state_->is_void_); }

auto EventVector::PreAddEventShared(const Event &new_event, std::string l1, std::string l2) -> void {
  if (shared_segment_ptr_ == nullptr) {
    shared_segment_ptr_ = std::make_shared<RawEventSegment>();
  }
  if (unique_switch_ || new_event.GetEventName() == l1 || new_event.GetEventName() == l2) {
    unique_switch_ = true;
    unique_event_seq_.push_back(new_event);
  } else if (shared_segment_ptr_->raw_event_vectors_.empty() ||
             shared_segment_ptr_->raw_event_vectors_.back().arrival_time_ != new_event.arrival_time_) {
    shared_segment_ptr_->AddEvent(new_event);
  }
  state_ = state_->GetNextPartialMatchState(new_event.GetEventType());
}

auto EventVector::PreCheckTimeWindow(time_t q_window, const Event &new_event) const -> bool {
  if ((shared_segment_ptr_ == nullptr) && unique_event_seq_.empty()) {
    return true;
  }
  time_t last_event_time = new_event.arrival_time_;
  time_t earliest_event_time = shared_segment_ptr_ ? shared_segment_ptr_->raw_event_vectors_.begin()->arrival_time_
                                                   : unique_event_seq_.begin()->arrival_time_;
  return last_event_time - earliest_event_time <= q_window;
}
}  // namespace cep
