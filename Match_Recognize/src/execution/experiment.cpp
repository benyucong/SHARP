#include "execution/experiment.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/event.h"
#include "query/pattern.h"
#include "query/query.h"
#include "shedder/shedder.h"

namespace cep {
std::vector<long double> EventBuffer::latencies;
std::vector<uint64_t> EventBuffer::num_complete_matches;
std::vector<uint64_t> EventBuffer::num_violates_latencies;
std::map<uint64_t, uint64_t> EventBuffer::latency_booking;
float EventBuffer::throughput;
uint64_t EventBuffer::event_shedding_num;
uint64_t EventBuffer::max_buffer_size;
uint64_t EventBuffer::total_pm_num;

EventBuffer::EventBuffer(std::shared_ptr<PartialMatchState> starting_pm_ptr, std::shared_ptr<PartialMatchState> pm_ptr_)
    : max_latency_violation_extends_(2, 0) {
  auto init_vector = std::make_shared<EventVector>(std::move(starting_pm_ptr));
  all_pms_.push_back(init_vector);
  if (pm_ptr_ != nullptr) {
    auto another_init_vector = std::make_shared<EventVector>(std::move(pm_ptr_));
    all_pms_.push_back(another_init_vector);
  }
  starting_time_ = std::chrono::high_resolution_clock ::now();
  latencies = {0, 0};
  num_complete_matches = {0, 0};
  num_violates_latencies = {0, 0};
  load_shedding_manager_ = LoadSheddingManager();
  throughput = 0;
  event_shedding_num = 0;
  max_buffer_size = 0;
  total_pm_num = 0;
}

auto EventBuffer::ProcessNewEventNonShared(const Event &new_event) -> void {
  std::vector<std::shared_ptr<EventVector>> copies_all_pms;
  if (new_event.GetEventName() == "A") {
    auto init_vector = *all_pms_.begin();
    for (auto it = all_pms_.begin(); it != all_pms_.end(); ++it) {
      if ((*it)->state_->state_name_ == "p0" || (*it)->state_->state_name_ == "p1") {
        continue;
      }
      auto new_a_ptr = std::make_shared<EventVector>(*init_vector);
      new_a_ptr->AddEventNonShared(new_event);
      all_pms_.insert(it, new_a_ptr);
      return;
    }
  }
  for (auto it = all_pms_.begin(); it != all_pms_.end();) {
    if ((*it)->CheckValidity()) {
      auto new_ptr = std::make_shared<EventVector>(*(*it));
      copies_all_pms.push_back(new_ptr);
      ++it;
    } else {
      LoadSheddingManager::LoadSheddingBookingDeletePM(*it);
      (*it)->SetInvalidity();
      it = all_pms_.erase(it);
    }
  }

  for (size_t i = 0; i < copies_all_pms.size(); ++i) {
    auto pm = copies_all_pms[i];
    pm->AddEventNonShared(new_event);
    int8_t flag = 0;
    if (pm->state_->is_complete_state_) {
      //       if pm achieves CM
      if (!pm->CheckQ0TimeWindow(q0_time_window_)) {
        // original complete/partial match can't construct CM anymore, evict it.
        LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
        all_pms_[i]->SetInvalidity();
        pm->SetInvalidity();
        continue;
      }
      if (!pm->CheckQ1TimeWindow(q1_time_window_)) {
        LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
        all_pms_[i]->SetInvalidity();
        pm->SetInvalidity();
        continue;
      }
      //      std::cout << "Complete match achieved: ";
      auto complete_state_ptr = std::static_pointer_cast<CompleteMatchState>(pm->state_);
      auto candidate_query_id = complete_state_ptr->query_id_;
      if (pm->VerifyPredicate(candidate_query_id)) {
        // if pass predicate verification
        flag = 2;
        auto detection_time = CurrentTime() - new_event.out_queue_time_;
        auto detection_time_f = static_cast<long double>(detection_time);
        latencies[candidate_query_id] = detection_time_f;
        num_complete_matches[candidate_query_id]++;
        latencies[candidate_query_id] = std::max(detection_time_f, latencies[candidate_query_id]);
        if (detection_time > complete_state_ptr->latency_bound_) {
          num_violates_latencies[candidate_query_id]++;
          auto latency_bound_f = static_cast<long double>(complete_state_ptr->latency_bound_);
          max_latency_violation_extends_[candidate_query_id] =
              std::max((detection_time_f - latency_bound_f) / latency_bound_f,
                       max_latency_violation_extends_[candidate_query_id]);
        }
        UpdateContributionBookings(pm, candidate_query_id);
      }
    } else {
      // pm is only intermediate result
      if (pm->VerifyPredicate(0) && pm->VerifyPredicate(1)) {
        flag = 2;
      } else if (pm->VerifyPredicate(0)) {
        flag = 0;
      } else if (pm->VerifyPredicate(1)) {
        flag = 1;
      } else {
        // pm fails to satisfy any predicate
        continue;
      }
    }
    UpdateConsumptionBookings(pm, flag);
    // Update the pm cost model
    //    std::cout << "flag is " << static_cast<int>(flag) << std::endl;
    //    LoadSheddingManager::UpdatePartialMatchPriority2Shed(pm);
    all_pms_.push_back(pm);
  }
}

auto EventBuffer::ProcessNewEventShared(const Event &new_event) -> void {
  std::vector<std::shared_ptr<EventVector>> copies_all_pms;

  if (new_event.GetEventName() == "A") {
    auto init_vector = *all_pms_.begin();
    for (auto it = all_pms_.begin(); it != all_pms_.end(); ++it) {
      if ((*it)->state_->state_name_ == "p0" || (*it)->state_->state_name_ == "p1") {
        continue;
      }
      auto new_a_ptr = std::make_shared<EventVector>(*init_vector);
      new_a_ptr->AddEventShared(new_event);
      all_pms_.insert(it, new_a_ptr);
      return;
    }
  }

  for (auto it = all_pms_.begin(); it != all_pms_.end();) {
    // check if event vector is valid or earliest event timeout
    if ((*it)->CheckValidity()) {
      auto new_ptr = std::make_shared<EventVector>(*(*it));
      if ((*it)->state_->state_name_ == "p3") {
        connection_segment_ = new_ptr->shared_segment_ptr_;
      } else if ((*it)->state_->state_name_ == "p2") {
        new_ptr->shared_segment_ptr_ = connection_segment_;
        //        connection_segment_.reset();
      }
      copies_all_pms.push_back(new_ptr);
      ++it;
    } else {
      // Invalidate and Evict the original event vector
      LoadSheddingManager::LoadSheddingBookingDeletePM(*it);
      (*it)->SetInvalidity();
      it = all_pms_.erase(it);
    }
  }

  for (size_t i = 0; i < copies_all_pms.size(); ++i) {
    auto pm = copies_all_pms[i];
    if (new_event.GetEventName() == "B") {
      //      std::cout << "making shared seg" << std::endl;
      if (pm->state_->state_name_ == "p0") {
        auto new_shared_segment = std::make_shared<RawEventSegment>();
        pm->shared_segment_ptr_ = new_shared_segment;
        connection_segment_ = new_shared_segment;
      } else if (pm->state_->state_name_ == "p1") {
        pm->shared_segment_ptr_ = connection_segment_;
      } else if (pm->state_->state_name_ == "p3") {
        connection_segment_.reset();
      }
    }

    pm->AddEventShared(new_event);
    int8_t flag = 0;
    if (pm->state_->is_complete_state_) {
      //       if pm achieves CM
      if (!pm->CheckQ0TimeWindow(q0_time_window_)) {
        // original complete/partial match can't construct CM anymore, evict it.
        LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
        all_pms_[i]->SetInvalidity();
        pm->SetInvalidity();
        continue;
      }
      if (!pm->CheckQ1TimeWindow(q1_time_window_)) {
        //        std::cout << "Setting ";
        //        pm->PrintEventVector();
        //        std::cout << "============== to invalid" << std::endl;
        LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
        all_pms_[i]->SetInvalidity();
        pm->SetInvalidity();
        continue;
      }
      //      std::cout << "Complete match achieved: ";
      auto complete_state_ptr = std::static_pointer_cast<CompleteMatchState>(pm->state_);
      auto candidate_query_id = complete_state_ptr->query_id_;
      if (pm->VerifyPredicate(candidate_query_id)) {
        // if pass predicate verification
        flag = 2;
        auto detection_time = CurrentTime() - new_event.out_queue_time_;
        auto detection_time_f = static_cast<long double>(detection_time);
        latencies[candidate_query_id] = detection_time_f;
        num_complete_matches[candidate_query_id]++;
        //        std::cout << "q" << static_cast<int>(candidate_query_id) << "'s latency is " <<
        //        latencies[candidate_query_id]
        //                  << "μs" << std::endl;
        // if violates
        latencies[candidate_query_id] = std::max(detection_time_f, latencies[candidate_query_id]);
        if (detection_time > complete_state_ptr->latency_bound_) {
          // latency bound exceeds
          num_violates_latencies[candidate_query_id]++;
          auto latency_bound_f = static_cast<long double>(complete_state_ptr->latency_bound_);
          max_latency_violation_extends_[candidate_query_id] =
              std::max((detection_time_f - latency_bound_f) / latency_bound_f,
                       max_latency_violation_extends_[candidate_query_id]);
        }
        UpdateContributionBookings(pm, candidate_query_id);
      }
    } else {
      // pm is only intermediate result
      if (pm->VerifyPredicate(0) && pm->VerifyPredicate(1)) {
        flag = 2;
      } else if (pm->VerifyPredicate(0)) {
        flag = 0;
      } else if (pm->VerifyPredicate(1)) {
        flag = 1;
      } else {
        // pm fails to satisfy any predicate
        continue;
      }
    }
    UpdateConsumptionBookings(pm, flag);
    //    LoadSheddingManager::UpdatePartialMatchPriority2Shed(pm);
    all_pms_.push_back(pm);
  }
}

auto EventBuffer::ProcessNewEventDynamicShared(const Event &new_event) -> void {
  std::vector<std::shared_ptr<EventVector>> copies_all_pms;
  if (new_event.GetEventName() == "A") {
    auto init_vector = *all_pms_.begin();
    for (auto it = all_pms_.begin(); it != all_pms_.end(); ++it) {
      if ((*it)->state_->state_name_ == "p0" || (*it)->state_->state_name_ == "p1") {
        continue;
      }
      auto new_a_ptr = std::make_shared<EventVector>(*init_vector);
      new_a_ptr->AddEventShared(new_event);
      all_pms_.insert(it, new_a_ptr);
      return;
    }
  }

  for (auto it = all_pms_.begin(); it != all_pms_.end();) {
    // check if event vector is valid or earliest event timeout
    if ((*it)->DynamicCheckValidity()) {
      auto new_ptr = std::make_shared<EventVector>(*(*it));
      if ((*it)->state_->state_name_ == "p3") {
        connection_segment_ = new_ptr->shared_segment_ptr_;
      } else if ((*it)->state_->state_name_ == "p2") {
        new_ptr->shared_segment_ptr_ = connection_segment_;
        //        connection_segment_.reset();
      }
      copies_all_pms.push_back(new_ptr);
      ++it;
    } else {
      // Invalidate and Evict the original event vector
      LoadSheddingManager::LoadSheddingBookingDeletePM(*it);
      (*it)->DynamicSetInvalidity();
      it = all_pms_.erase(it);
    }
  }

  for (size_t i = 0; i < copies_all_pms.size(); ++i) {
    auto pm = copies_all_pms[i];
    if (new_event.GetEventName() == "B") {
      //      std::cout << "making shared seg" << std::endl;
      if (pm->state_->state_name_ == "p0") {
        auto new_shared_segment = std::make_shared<RawEventSegment>();
        pm->shared_segment_ptr_ = new_shared_segment;
        connection_segment_ = new_shared_segment;
      } else if (pm->state_->state_name_ == "p1") {
        pm->shared_segment_ptr_ = connection_segment_;
      } else if (pm->state_->state_name_ == "p3") {
        connection_segment_.reset();
      }
    }

    pm->AddEventShared(new_event);
    int8_t flag = 0;
    if (pm->state_->is_complete_state_) {
      //       if pm achieves CM
      if (!pm->CheckQ0TimeWindow(q0_time_window_)) {
        // original complete/partial match can't construct CM anymore, evict it.
        LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
        all_pms_[i]->DynamicSetInvalidity();
        pm->DynamicSetInvalidity();
        continue;
      }
      if (!pm->CheckQ1TimeWindow(q1_time_window_)) {
        //        std::cout << "Setting ";
        //        pm->PrintEventVector();
        //        std::cout << "============== to invalid" << std::endl;
        LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
        all_pms_[i]->DynamicSetInvalidity();
        pm->DynamicSetInvalidity();
        continue;
      }
      //      std::cout << "Complete match achieved: ";
      auto complete_state_ptr = std::static_pointer_cast<CompleteMatchState>(pm->state_);
      auto candidate_query_id = complete_state_ptr->query_id_;
      if (pm->VerifyPredicate(candidate_query_id)) {
        // if pass predicate verification
        flag = 2;
        auto detection_time = CurrentTime() - new_event.out_queue_time_;
        auto detection_time_f = static_cast<long double>(detection_time);
        latencies[candidate_query_id] = detection_time_f;
        num_complete_matches[candidate_query_id]++;
        //        std::cout << "q" << static_cast<int>(candidate_query_id) << "'s latency is " <<
        //        latencies[candidate_query_id]
        //                  << "μs" << std::endl;
        // if violates
        latencies[candidate_query_id] = std::max(detection_time_f, latencies[candidate_query_id]);
        if (detection_time > complete_state_ptr->latency_bound_) {
          // latency bound exceeds
          num_violates_latencies[candidate_query_id]++;
          auto latency_bound_f = static_cast<long double>(complete_state_ptr->latency_bound_);
          max_latency_violation_extends_[candidate_query_id] =
              std::max((detection_time_f - latency_bound_f) / latency_bound_f,
                       max_latency_violation_extends_[candidate_query_id]);
        }
        UpdateContributionBookings(pm, candidate_query_id);
      }
    } else {
      // pm is only intermediate result
      if (pm->VerifyPredicate(0) && pm->VerifyPredicate(1)) {
        flag = 2;
      } else if (pm->VerifyPredicate(0)) {
        flag = 0;
      } else if (pm->VerifyPredicate(1)) {
        flag = 1;
      } else {
        // pm fails to satisfy any predicate
        continue;
      }
    }
    UpdateConsumptionBookings(pm, flag);
    //    LoadSheddingManager::UpdatePartialMatchPriority2Shed(pm);
    all_pms_.push_back(pm);
  }
}

auto EventBuffer::PrintCurrentBuffer() -> void {
  for (auto &pm : all_pms_) {
    if (pm->CheckValidity()) {
      std::cout << "the state is " << pm->state_->state_name_ << ";  ";
      pm->PrintEventVector();
      if (pm->shared_segment_ptr_) {
        pm->shared_segment_ptr_->PrintSharedSegment();
      }
    }
  }
}

auto EventBuffer::PrintPreCurrentBuffer() -> void {
  for (auto &pm : all_pms_) {
    if (pm->CheckValidity()) {
      std::cout << "the state is " << pm->state_->state_name_ << ";  ";
      pm->PrintEventVector();
    }
  }
}

auto EventBuffer::RunLoadShedding(
    const std::function<void(long double, long double, std::vector<std::shared_ptr<EventVector>> &, int, int)>
        &shed_method,
    int if_dynamic, int _shedding_ratio_) -> void {
  bool flag0 = max_latency_violation_extends_[0] > 5e-2F;
  bool flag1 = max_latency_violation_extends_[1] > 5e-2F;
  if (flag0 || flag1) {
    for (const auto &all_pm : all_pms_) {
      // update in booking
      if (all_pm->GetSize() == 0) {
        continue;
      }
      LoadSheddingManager::UpdatePartialMatchPriority2Shed(all_pm);
    }
    shed_method(max_latency_violation_extends_[0], max_latency_violation_extends_[1], all_pms_, if_dynamic,
                _shedding_ratio_);
  }
  if (flag0) {
    max_latency_violation_extends_[0] = 0;
  }
  if (flag1) {
    max_latency_violation_extends_[1] = 0;
  }
}

auto EventBuffer::CurrentTime() const -> uint64_t {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() -
                                                               starting_time_)
      .count();
}
auto EventBuffer::UpdateContributionBookings(const std::shared_ptr<EventVector> &pm, query_t query_id) -> void {
  if (pm->GetSize() == 0) {
    return;
  }
  auto attr_vector = pm->GetAttrVector("id");
  for (int i = 0; static_cast<size_t>(i) < attr_vector.size(); ++i) {
    auto sub_attr_vector = std::vector<attr_t>(attr_vector.begin(), attr_vector.begin() + i + 1);
    if (LoadSheddingManager::contribution_booking.find(sub_attr_vector) ==
        LoadSheddingManager::contribution_booking.end()) {
      if (query_id == 0) {
        LoadSheddingManager::contribution_booking[sub_attr_vector] = std::make_pair(100, 0);
      } else {
        LoadSheddingManager::contribution_booking[sub_attr_vector] = std::make_pair(0, 100);
      }
    } else {
      if (query_id == 0) {
        LoadSheddingManager::contribution_booking[sub_attr_vector].first += 100;
      } else {
        LoadSheddingManager::contribution_booking[sub_attr_vector].second += 100;
      }
    }
  }
}

auto EventBuffer::UpdateConsumptionBookings(const std::shared_ptr<EventVector> &pm, int flag) -> void {
  if (pm->GetSize() == 0) {
    return;
  }
  auto effect_const = static_cast<long double>(pm->GetCost());
  auto q0_const = effect_const;
  //  auto q0_const = 1;
  auto q1_const = effect_const;
  //  auto q1_const = 1;
  auto attr_vector = pm->GetAttrVector();
  for (int i = 0; static_cast<size_t>(i) < attr_vector.size(); ++i) {
    auto sub_attr_vector = std::vector<attr_t>(attr_vector.begin(), attr_vector.begin() + i + 1);
    if (LoadSheddingManager::consumption_booking.find(sub_attr_vector) ==
        LoadSheddingManager::consumption_booking.end()) {
      if (flag == 0) {
        LoadSheddingManager::consumption_booking[sub_attr_vector] = {q0_const, 0};
        LoadSheddingManager::total_consumption_q0 += q0_const;
      } else if (flag == 1) {
        LoadSheddingManager::consumption_booking[sub_attr_vector] = {0, q1_const};
        LoadSheddingManager::total_consumption_q1 += q1_const;
      } else {
        LoadSheddingManager::consumption_booking[sub_attr_vector] = {q0_const, q1_const};
        LoadSheddingManager::total_consumption_q0 += q0_const;
        LoadSheddingManager::total_consumption_q1 += q1_const;
      }
    } else {
      if (flag == 0) {
        LoadSheddingManager::consumption_booking[sub_attr_vector].first += q0_const;
        LoadSheddingManager::total_consumption_q0 += q0_const;
      } else if (flag == 1) {
        LoadSheddingManager::consumption_booking[sub_attr_vector].second += q1_const;
        LoadSheddingManager::total_consumption_q1 += q1_const;
      } else {
        LoadSheddingManager::consumption_booking[sub_attr_vector].first += q0_const;
        LoadSheddingManager::consumption_booking[sub_attr_vector].second += q1_const;
        LoadSheddingManager::total_consumption_q0 += q0_const;
        LoadSheddingManager::total_consumption_q1 += q1_const;
      }
    }
  }
}

auto Experiment::GenerateSyntheticStreamDefault(std::queue<Event> &raw_event_queue,
                                                std::vector<EventType> &all_event_types, int64_t event_number) -> void {
  std::string store_path = "../../synthetic_data/events_q0.csv";
  // Define a random number generator
  std::random_device rd;
  std::mt19937_64 rng(rd());

  // Define a uniform distribution for int64_t values, min is 1, max is 100
  std::uniform_int_distribution<int> dist_id(1, 8);
  std::uniform_real_distribution<attr_t> dist_x(-90.0L, 90.0L);
  std::uniform_real_distribution<attr_t> dist_y(-180.0L, 180.0L);
  std::uniform_real_distribution<attr_t> dist_v(1.0L, 3000000.0L);

  // std::vector<attr_t> dist_values = {100, 200, 300, 400, 500};
  // std::vector<double> value_probabilities = {0.1, 0.2, 0.3, 0.25, 0.15};
  // std::discrete_distribution<> dist_v2(value_probabilities.begin(), value_probabilities.end());

  std::set<std::string> attr_names = {"id", "x", "y", "v"};

  auto et_a = EventType("A", attr_names);
  auto et_b = EventType("B", attr_names);
  auto et_c = EventType("C", attr_names);
  auto et_d = EventType("D", attr_names);
  auto et_e = EventType("E", attr_names);
  auto et_f = EventType("F", attr_names);
  auto et_g = EventType("G", attr_names);
  auto et_x = EventType("X", attr_names);
  auto et_y = EventType("Y", attr_names);
  auto et_z = EventType("Z", attr_names);
  auto et_star = EventType("*", attr_names);

  all_event_types_.push_back(et_a);
  all_event_types_.push_back(et_b);
  all_event_types_.push_back(et_c);
  all_event_types_.push_back(et_d);
  all_event_types_.push_back(et_e);
  all_event_types_.push_back(et_f);
  all_event_types_.push_back(et_g);
  all_event_types_.push_back(et_x);
  all_event_types_.push_back(et_y);
  all_event_types_.push_back(et_z);
  all_event_types_.push_back(et_star);

  if (std::filesystem::exists(store_path)) {
    // std::cout << "data csv for q0 already exists" << std::endl;
    return;
  }
  int ctrl = 0;
  for (auto current_time = 0; current_time < event_number;) {
    if (ctrl == 10000) {
      ctrl = 0;
      // Generate a random real value
      raw_event_queue.push(Event(et_a,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time++));
      raw_event_queue.push(Event(et_b,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time++));
      raw_event_queue.push(Event(et_c,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time++));
      raw_event_queue.push(Event(et_d,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time++));
      raw_event_queue.push(Event(et_e,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time++));
      raw_event_queue.push(Event(et_f,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time++));
      raw_event_queue.push(Event(et_g,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time++));
      raw_event_queue.push(Event(et_x,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time++));
      raw_event_queue.push(Event(et_y,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time++));
      raw_event_queue.push(Event(et_z,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time++));
    } else {
      raw_event_queue.push(Event(et_star,
                                 {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng)),
                                  Attribute("y", dist_y(rng)), Attribute("v", dist_v(rng))},
                                 current_time));
      ctrl += 1;
    }
  }

  std::ofstream data_file;
  data_file.open(store_path.c_str());
  while (!raw_event_queue.empty()) {
    auto e = raw_event_queue.front();
    auto event_tuple = e.GetEventName() + "," + std::to_string(e.arrival_time_) + "," +
                       std::to_string(e.GetAttributeValue("id")) + "," + std::to_string(e.GetAttributeValue("x")) +
                       "," + std::to_string(e.GetAttributeValue("y")) + "," + std::to_string(e.GetAttributeValue("v")) +
                       "\n";
    // std::cout << "writing: " << event_tuple << std::endl;
    data_file << event_tuple;
    raw_event_queue.pop();
  }
  // std::cout << "generate new data csv for q0" << std::endl;
}

auto EventBuffer::DarlingUtility(const cep::Event &new_event, int pos) -> float {
  auto name = new_event.GetEventName();
  float ans = static_cast<float>(name[0]) / static_cast<float>('Z');
  for (const auto &vec_pair : LoadSheddingManager::contribution_booking) {
    auto attr_vector = vec_pair.first;
    auto event_attr_vec = new_event.GetAllAttributeValues();
    attr_vector.insert(attr_vector.end(), event_attr_vec.begin(), event_attr_vec.end());
    if (LoadSheddingManager::contribution_booking.find(attr_vector) !=
        LoadSheddingManager::contribution_booking.end()) {
      if (pos == 0) {
        ans += LoadSheddingManager::contribution_booking[attr_vector].first;
      } else {
        ans += LoadSheddingManager::contribution_booking[attr_vector].second;
      }
    } else {
      ans += 1e-10;
    }
  }
  return ans;
}

/**
 *********************************************************************************************************************
 * Experiment for Set1
 *********************************************************************************************************************
 **/
auto Experiment::GenerateSyntheticStreamSet1(std::queue<Event> &raw_event_queue,
                                             std::vector<EventType> &all_event_types, int64_t event_number) -> void {
  // Define a random number generator
  std::random_device rd;
  std::mt19937_64 rng(rd());

  // Define a uniform distribution for int64_t values, min is 1, max is 100
  std::uniform_int_distribution<int64_t> dist_id(1, 2);
  std::uniform_real_distribution<attr_t> dist_x(1.0L, 100.0L);
  std::uniform_real_distribution<attr_t> dist_event(0, 1000.0L);

  std::set<std::string> attr_names = {"id", "x"};

  auto et_a = EventType("A", attr_names, 1, 2000);
  auto et_b = EventType("B", attr_names, 1, 2000);
  auto et_c = EventType("C", attr_names, 10, 2000);
  auto et_d = EventType("D", attr_names, 1, 2000);
  auto et_e = EventType("E", attr_names, 10, 2000);

  all_event_types.push_back(et_a);
  all_event_types.push_back(et_b);
  all_event_types.push_back(et_c);
  all_event_types.push_back(et_d);
  all_event_types.push_back(et_e);

  int ctrl = 0;
  for (auto current_time = 0; current_time < event_number;) {
    // Generate a random int64_t value
    auto attr_val_id = dist_id(rng);
    auto attr_id = Attribute("id", attr_val_id);
    auto attr_val_x = dist_x(rng);
    auto attr_x = Attribute("x", attr_val_x);

    if (ctrl == 10) {
      ctrl = 0;
      raw_event_queue.push(Event(et_c, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
    } else {
      raw_event_queue.push(Event(et_a, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
      raw_event_queue.push(Event(et_b, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
      raw_event_queue.push(Event(et_d, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
      raw_event_queue.push(Event(et_e, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
      ctrl += 1;
    }
  }
}

auto EventBuffer::ProcessNewEventSet1Shared(const Event &new_event, const std::string &l1, const std::string &l2)
    -> void {
  std::vector<std::shared_ptr<EventVector>> copies_all_pms;

  for (auto it = all_pms_.begin(); it != all_pms_.end();) {
    // check if event vector is valid or earliest event timeout
    if ((*it)->CheckValidity() && (*it)->PreCheckTimeWindow(q0_time_window_, new_event) &&
        (*it)->PreCheckTimeWindow(q1_time_window_, new_event)) {
      // if this PM can accept new event
      if ((*it)->state_->ContainChild(new_event.GetEventType())) {
        if (new_event.GetEventName() == l1 || new_event.GetEventName() == l2) {
          (*it)->unique_switch_ = true;
        }
        auto new_ptr = std::make_shared<EventVector>(*(*it));
        copies_all_pms.push_back(new_ptr);
        ++it;
      } else {
        ++it;
        continue;
      }
    } else {
      // Invalidate and Evict the original event vector
      LoadSheddingManager::LoadSheddingBookingDeletePM(*it);
      (*it)->SetInvalidity();
      it = all_pms_.erase(it);
    }
  }

  for (const auto &pm : copies_all_pms) {
    if (pm->state_->ContainChild(new_event.GetEventType())) {
      pm->PreAddEventShared(new_event, l1, l2);
      EventBuffer::total_pm_num++;
    } else {
      continue;
    }
    int8_t flag = 0;
    if (pm->state_->is_complete_state_) {
      //       if pm achieves CM
      // if (!pm->PreCheckTimeWindow(q0_time_window_) || !pm->PreCheckTimeWindow(q1_time_window_)) {
      //   // original complete/partial match can't construct CM anymore, evict it.
      //   LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
      //   all_pms_[i]->SetInvalidity();
      //   pm->SetInvalidity();
      //   continue;
      // }

      //      std::cout << "Complete match achieved: ";
      auto complete_state_ptr = std::static_pointer_cast<CompleteMatchState>(pm->state_);
      auto candidate_query_id = complete_state_ptr->query_id_;
      if (pm->VerifyPredicate(candidate_query_id)) {
        // if pass predicate verification
        flag = candidate_query_id;
        auto detection_time = CurrentTime() - new_event.out_queue_time_;
        auto detection_time_f = static_cast<long double>(detection_time);
        num_complete_matches[candidate_query_id]++;
        //        std::cout << "q" << static_cast<int>(candidate_query_id) << "'s latency is " <<
        //        latencies[candidate_query_id]
        //                  << "μs" << std::endl;
        // if violates
        latencies[candidate_query_id] = detection_time_f;
        latency_booking[detection_time]++;
        if (detection_time > complete_state_ptr->latency_bound_) {
          // latency bound exceeds
          // std::cout << "detection time: " << detection_time << " bound: " << complete_state_ptr->latency_bound_
          //           << std::endl;
          num_violates_latencies[candidate_query_id]++;
          auto latency_bound_f = static_cast<long double>(complete_state_ptr->latency_bound_);
          max_latency_violation_extends_[candidate_query_id] =
              std::max((detection_time_f - latency_bound_f) / latency_bound_f,
                       max_latency_violation_extends_[candidate_query_id]);
        }
        UpdateContributionBookings(pm, candidate_query_id);
      }
      // else {
      //   // cm fails the predicate verification
      //   LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
      //   all_pms_[i]->SetInvalidity();
      //   pm->SetInvalidity();
      //   continue;
      // }
    } else {
      // pm is only intermediate result
      if (pm->VerifyPredicate(0) && pm->VerifyPredicate(1)) {
        flag = 2;
        pm->candidate_queries_ = {0, 1};
      } else if (pm->VerifyPredicate(0)) {
        pm->candidate_queries_ = {0};
        flag = 0;
      } else if (pm->VerifyPredicate(1)) {
        pm->candidate_queries_ = {1};
        flag = 1;
      } else {
        // pm fails to satisfy any predicate
        pm->candidate_queries_.clear();
        continue;
      }
      all_pms_.push_back(pm);
      UpdateConsumptionBookings(pm, flag);
    }
    //    LoadSheddingManager::UpdatePartialMatchPriority2Shed(pm);
  }
}

auto EventBuffer::ProcessNewEventSet1Dynamic(const Event &new_event, const std::string &l1, const std::string &l2)
    -> void {
  std::vector<std::shared_ptr<EventVector>> copies_all_pms;

  for (auto it = all_pms_.begin(); it != all_pms_.end();) {
    // check if event vector is valid or earliest event timeout
    if ((*it)->DynamicCheckValidity() && (*it)->PreCheckTimeWindow(q0_time_window_, new_event) &&
        (*it)->PreCheckTimeWindow(q1_time_window_, new_event)) {
      // if this PM can accept new event
      if ((*it)->state_->ContainChild(new_event.GetEventType())) {
        if (new_event.GetEventName() == l1 || new_event.GetEventName() == l2) {
          (*it)->unique_switch_ = true;
        }
        auto new_ptr = std::make_shared<EventVector>(*(*it));
        copies_all_pms.push_back(new_ptr);
        ++it;
      } else {
        ++it;
        continue;
      }
    } else {
      // Invalidate and Evict the original event vector
      LoadSheddingManager::LoadSheddingBookingDeletePM(*it);
      (*it)->DynamicSetInvalidity();
      it = all_pms_.erase(it);
    }
  }

  for (const auto &pm : copies_all_pms) {
    if (pm->state_->ContainChild(new_event.GetEventType())) {
      pm->PreAddEventShared(new_event, l1, l2);
      EventBuffer::total_pm_num++;
    } else {
      continue;
    }
    int8_t flag = 0;
    if (pm->state_->is_complete_state_) {
      //       if pm achieves CM
      // if (!pm->PreCheckTimeWindow(q0_time_window_) || !pm->PreCheckTimeWindow(q1_time_window_)) {
      //   // original complete/partial match can't construct CM anymore, evict it.
      //   LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
      //   all_pms_[i]->DynamicSetInvalidity();
      //   pm->DynamicSetInvalidity();
      //   continue;
      // }

      //      std::cout << "Complete match achieved: ";
      auto complete_state_ptr = std::static_pointer_cast<CompleteMatchState>(pm->state_);
      auto candidate_query_id = complete_state_ptr->query_id_;
      if (pm->VerifyPredicate(candidate_query_id)) {
        // if pass predicate verification
        flag = candidate_query_id;
        auto detection_time = CurrentTime() - new_event.out_queue_time_;
        latency_booking[detection_time]++;
        auto detection_time_f = static_cast<long double>(detection_time);
        num_complete_matches[candidate_query_id]++;

        latencies[candidate_query_id] = detection_time_f;
        if (detection_time > complete_state_ptr->latency_bound_) {
          // latency bound exceeds
          num_violates_latencies[candidate_query_id]++;
          auto latency_bound_f = static_cast<long double>(complete_state_ptr->latency_bound_);
          max_latency_violation_extends_[candidate_query_id] =
              std::max((detection_time_f - latency_bound_f) / latency_bound_f,
                       max_latency_violation_extends_[candidate_query_id]);
        }
        UpdateContributionBookings(pm, candidate_query_id);
      }
      // else {
      //   LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
      //   all_pms_[i]->DynamicSetInvalidity();
      //   pm->DynamicSetInvalidity();
      //   continue;
      // }
    } else {
      // pm is only intermediate result
      if (pm->VerifyPredicate(0) && pm->VerifyPredicate(1)) {
        pm->candidate_queries_ = {0, 1};
        flag = 2;
      } else if (pm->VerifyPredicate(0)) {
        pm->candidate_queries_ = {0};
        flag = 0;
      } else if (pm->VerifyPredicate(1)) {
        pm->candidate_queries_ = {1};
        flag = 1;
      } else {
        pm->candidate_queries_.clear();
        // pm fails to satisfy any predicate
        continue;
      }
      all_pms_.push_back(pm);
      UpdateConsumptionBookings(pm, flag);
    }
    //    LoadSheddingManager::UpdatePartialMatchPriority2Shed(pm);
  }
}

auto EventBuffer::ProcessNewEventNonSharedSet1(const Event &new_event) -> void {
  std::vector<std::shared_ptr<EventVector>> copies_all_pms;

  for (auto it = all_pms_.begin(); it != all_pms_.end();) {
    // check if event vector is valid or earliest event timeout
    if ((*it)->CheckValidity() && (*it)->PreCheckTimeWindow(q0_time_window_, new_event) &&
        (*it)->PreCheckTimeWindow(q1_time_window_, new_event)) {
      auto new_ptr = std::make_shared<EventVector>(*(*it));
      copies_all_pms.push_back(new_ptr);
      ++it;
    } else {
      // Invalidate and Evict the original event vector
      LoadSheddingManager::LoadSheddingBookingDeletePM(*it);
      (*it)->SetInvalidity();
      it = all_pms_.erase(it);
    }
  }

  for (size_t i = 0; i < copies_all_pms.size(); ++i) {
    auto pm = copies_all_pms[i];
    if (pm->state_->ContainChild(new_event.GetEventType())) {
      pm->AddEventNonShared(new_event);
      EventBuffer::total_pm_num++;
    } else {
      continue;
    }
    int8_t flag = 0;
    if (pm->state_->is_complete_state_) {
      //       if pm achieves CM
      if (!pm->PreCheckTimeWindow(q0_time_window_) || !pm->PreCheckTimeWindow(q1_time_window_)) {
        // original complete/partial match can't construct CM anymore, evict it.
        LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
        all_pms_[i]->SetInvalidity();
        pm->SetInvalidity();
        continue;
      }

      //      std::cout << "Complete match achieved: ";
      auto complete_state_ptr = std::static_pointer_cast<CompleteMatchState>(pm->state_);
      auto candidate_query_id = complete_state_ptr->query_id_;
      if (pm->VerifyPredicate(candidate_query_id)) {
        // if pass predicate verification
        flag = candidate_query_id;
        auto detection_time = CurrentTime() - new_event.out_queue_time_;
        auto detection_time_f = static_cast<long double>(detection_time);
        num_complete_matches[candidate_query_id]++;
        //        std::cout << "q" << static_cast<int>(candidate_query_id) << "'s latency is " <<
        //        latencies[candidate_query_id]
        //                  << "μs" << std::endl;
        // if violates
        latencies[candidate_query_id] = detection_time_f;
        latency_booking[detection_time]++;
        if (detection_time > complete_state_ptr->latency_bound_) {
          // latency bound exceeds
          num_violates_latencies[candidate_query_id]++;
          auto latency_bound_f = static_cast<long double>(complete_state_ptr->latency_bound_);
          max_latency_violation_extends_[candidate_query_id] =
              std::max((detection_time_f - latency_bound_f) / latency_bound_f,
                       max_latency_violation_extends_[candidate_query_id]);
        }
        UpdateContributionBookings(pm, candidate_query_id);
      }
      // else {
      //   LoadSheddingManager::LoadSheddingBookingDeletePM(all_pms_[i]);
      //   all_pms_[i]->SetInvalidity();
      //   pm->SetInvalidity();
      //   continue;
      // }
    } else {
      // pm is only intermediate result
      if (pm->VerifyPredicate(0) && pm->VerifyPredicate(1)) {
        std::cout << "2" << std::endl;
        pm->candidate_queries_ = {0, 1};
        flag = 2;
      } else if (pm->VerifyPredicate(0)) {
        pm->candidate_queries_ = {0};
        flag = 0;
      } else if (pm->VerifyPredicate(1)) {
        pm->candidate_queries_ = {1};
        flag = 1;
      } else {
        pm->candidate_queries_.clear();
        // pm fails to satisfy any predicate
        continue;
      }
      all_pms_.push_back(pm);
      UpdateConsumptionBookings(pm, flag);
    }
    //    LoadSheddingManager::UpdatePartialMatchPriority2Shed(pm);
  }
}

auto EventBuffer::EventShedding(int shed_method, const Event &event, Query &q0, Query &q1) -> bool {
  if (shed_method == 2) {
    // fractional load shedding
    auto extend0 = (latencies[0] - q0.detection_latency_) / q0.detection_latency_;
    auto extend1 = (latencies[1] - q1.detection_latency_) / q1.detection_latency_;

    if (extend0 > 5e-2F && q0.IncludeEvent(event)) {
      // with the probability of max_latency_violation_extends_[0] return true
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_real_distribution<> dis(0.0, 1.0);
      // std::cout << "extend 0 :" << extend0 << std::endl;
      if (dis(gen) < extend0) {
        latencies[0] *= 1 - 6.0F / q0.detection_latency_;
        return true;
      }
    }
    if (extend1 > 5e-2F && q1.IncludeEvent(event)) {
      // with the probability of max_latency_violation_extends_[0] return true
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_real_distribution<> dis(0.0, 1.0);
      // std::cout << "extend 1 :" << extend1 << std::endl;
      if (dis(gen) < extend1) {
        latencies[1] *= 1 - 6.0F / q1.detection_latency_;
        return true;
      }
    }
    return false;
  }
  if (shed_method == 3) {
    // Darling
    time_t queue_waiting_time = event.out_queue_time_ - event.arrival_time_;

    // Calculate utility inversely proportional to queue_waiting_time
    float utility = 0;

    // Calculate the threshold for utility
    float threshold = 0;
    if (q0.IncludeEvent(event)) {
      utility = DarlingUtility(event, 0) / static_cast<float>(queue_waiting_time);
      threshold = darling_threshold_ / (q0_bound_ * 0.15);
    } else if (q1.IncludeEvent(event)) {
      utility = DarlingUtility(event, 1) / static_cast<float>(queue_waiting_time);
      threshold = darling_threshold_ / (q1_bound_ * 0.15);
    }
    // Determine if the event should be shed
    return utility < threshold;
  }
  // cost-model shedding input shedding
  if (shed_method == 0) {
    auto extend0 = (latencies[0] - q0.detection_latency_) / q0.detection_latency_;
    auto extend1 = (latencies[1] - q1.detection_latency_) / q1.detection_latency_;
    float shedding_threshold = 2.5;
    if (extend0 > shedding_threshold && q0.IncludeEvent(event)) {
      // with the probability of max_latency_violation_extends_[0] return true
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_real_distribution<> dis(0.0, 1.0);
      // std::cout << "extend 0 :" << extend0 << std::endl;
      if (dis(gen) < extend0) {
        latencies[0] *= 0.9999999;
        return true;
      }
    }
    if (extend1 > shedding_threshold && q1.IncludeEvent(event)) {
      // with the probability of max_latency_violation_extends_[0] return true
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_real_distribution<> dis(0.0, 1.0);
      // std::cout << "extend 1 :" << extend1 << std::endl;
      if (dis(gen) < extend1) {
        latencies[1] *= 0.999999;
        return true;
      }
    }
    return false;
  }
  // ICDE'20
  if (shed_method == 4) {
    auto extend0 = (latencies[0] - q0.detection_latency_) / q0.detection_latency_;
    auto extend1 = (latencies[1] - q1.detection_latency_) / q1.detection_latency_;
    float shedding_threshold = 2.7;
    if (extend0 > shedding_threshold && q0.IncludeEvent(event)) {
      // with the probability of max_latency_violation_extends_[0] return true
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_real_distribution<> dis(0.0, 1.0);
      // std::cout << "extend 0 :" << extend0 << std::endl;
      if (dis(gen) < extend0) {
        latencies[0] *= 0.9999999;
        return true;
      }
    }
    if (extend1 > shedding_threshold && q1.IncludeEvent(event)) {
      // with the probability of max_latency_violation_extends_[0] return true
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_real_distribution<> dis(0.0, 1.0);
      // std::cout << "extend 1 :" << extend1 << std::endl;
      if (dis(gen) < extend1) {
        latencies[1] *= 0.999999;
        return true;
      }
    }
    return false;
  }
  // decision tree shedding gspice
  if (shed_method == 5) {
    // Track event type frequencies
    static std::unordered_map<std::string, uint64_t> event_type_counts;
    static uint64_t total_events = 0;
    
    std::string event_name = event.GetEventName();
    event_type_counts[event_name]++;
    total_events++;
    
    // Calculate frequency ratio for this event type
    double type_frequency = static_cast<double>(event_type_counts[event_name]) / total_events;
    
    // Get event attributes (assuming first attribute as a simple decision feature)
    attr_t attribute_value = event.GetAttributeValue("x");
    
    // Check system pressure first - only shed when under significant pressure
    auto extend0 = latencies.size() > 0 ? 
        (latencies[0] - q0.detection_latency_) / q0.detection_latency_ : 0.0;
    auto extend1 = latencies.size() > 1 ? 
        (latencies[1] - q1.detection_latency_) / q1.detection_latency_ : 0.0;
    double max_pressure = std::max(extend0, extend1);
    
    // Only shed if system is under significant pressure (> 20% latency violation)
    if (max_pressure < 0.05) {
      return false; // Don't shed when system is performing well
    }
    
    // Simple decision tree logic based on:
    // 1. Event type (name)
    // 2. Type frequency 
    // 3. Event attributes (first attribute value)
    // 4. System pressure
    
    // Decision tree rules (much more conservative):
    // Rule 1: If event type frequency > 0.7 (very high frequency events), shed cautiously
    if (type_frequency > 0.6) {
      // For very high-frequency events, check attribute value and system pressure
      if (attribute_value > 80.0 && max_pressure > 0.8) {
        // Very high frequency + high attribute value + high pressure -> shed with 30% probability
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(0.0, 1.0);
        return dis(gen) < max_pressure/3;
      } else if (attribute_value > 60.0 && max_pressure > 0.5) {
        // Very high frequency + medium attribute value + very high pressure -> shed with 20% probability
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(0.0, 1.0);
        return dis(gen) < max_pressure/3;
      } else {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(0.0, 1.0);
        return dis(gen) < 0.2;
      }
    }
    
    // Rule 2: For medium frequency events (0.4 < frequency <= 0.7)
    else if (type_frequency > 0.3) {
      // Consider event type name in decision
      if (event_name == "A" || event_name == "B") {
        // Important event types - shed only under extreme conditions
        if (attribute_value > 90.0 && max_pressure > 1.0) {
          std::random_device rd;
          std::mt19937 gen(rd());
          std::uniform_real_distribution<> dis(0.0, 1.0);
          return dis(gen) < max_pressure/3; // Very low probability
        } else {
          std::random_device rd;
          std::mt19937 gen(rd());
          std::uniform_real_distribution<> dis(0.0, 1.0);
          return dis(gen) < max_pressure/5; // Even Very low probability
        }
      } else {
        // Less important event types - shed more carefully
        if (attribute_value > 85.0 && max_pressure > 0.6) {
          std::random_device rd;
          std::mt19937 gen(rd());
          std::uniform_real_distribution<> dis(0.0, 1.0);
          return dis(gen) < max_pressure/3; 
        } else {
          std::random_device rd;
          std::mt19937 gen(rd());
          std::uniform_real_distribution<> dis(0.0, 1.0);
          return dis(gen) < max_pressure/5; 
        }
      }
    }
    
    // Rule 3: For low frequency events (frequency <= 0.4)
    else {
      // Rare events - preserve almost all of them
      if (attribute_value > 90.0 && max_pressure > 1.5) {
        // Only shed rare events under extreme pressure and very high attribute values
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(0.0, 1.0);
        return dis(gen) < max_pressure/5; // Very low probability, reduced from 0.2
      } else {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> dis(0.0, 1.0);
        return dis(gen) < max_pressure/10; 
      }
    }
  }
  if (shed_method == 6) {
    // Simple utility-based adaptive shedding hspice
    static std::unordered_map<std::string, uint64_t> event_type_counts;
    static uint64_t total_events = 0;
    
    std::string event_name = event.GetEventName();
    event_type_counts[event_name]++;
    total_events++;
    
    // Calculate basic utility based on simple factors
    double utility = 1.0;
    
    // Factor 1: Query relevance (most important)
    bool relevant_to_q0 = q0.IncludeEvent(event);
    bool relevant_to_q1 = q1.IncludeEvent(event);
    
    if (relevant_to_q0 && relevant_to_q1) {
      utility *= 1.5; // High value for events relevant to both queries
    } else if (relevant_to_q0 || relevant_to_q1) {
      utility *= 1.2; // Medium value for events relevant to one query
    } else {
      utility *= 0.7; // Lower value for events not directly relevant
    }
    
    // Factor 2: Event type frequency penalty
    double frequency = static_cast<double>(event_type_counts[event_name]) / total_events;
    if (frequency > 0.3) {
      utility *= 0.8; // Penalize high-frequency events
    } else if (frequency < 0.1) {
      utility *= 1.1; // Slight boost for rare events
    }
    
    // Factor 3: System pressure based threshold
    auto extend0 = latencies.size() > 0 ? 
        (latencies[0] - q0.detection_latency_) / q0.detection_latency_ : 0.0;
    auto extend1 = latencies.size() > 1 ? 
        (latencies[1] - q1.detection_latency_) / q1.detection_latency_ : 0.0;
    double latency_pressure = std::max(extend0, extend1);
    
    // Calculate aggressive threshold
    double threshold = 1.0; // Base threshold
    
    if (latency_pressure > 0.2) {
      threshold = 1.0 + latency_pressure * 2.0; // Aggressive scaling
    } else if (latency_pressure > 0.05) {
      threshold = 1.0 + latency_pressure * 1.5; // Moderate scaling
    }
    
    // Add randomness for variability
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.8, 1.2);
    double random_factor = dis(gen) * 5.0;
    
    // Simple shedding decision: shed if utility is below threshold
    return (utility * random_factor) < threshold;
  }
  return false;
}

auto EventBuffer::LoadSheddingMonitor(int shed_method, int plan_choice) -> void {
  if (shed_method == 0) {
    // cost-model shedding
    if (plan_choice == 0 || plan_choice == 1) {
      // Static non-shared/shared
      RunLoadShedding(LoadSheddingManager::LoadSheddingBimaps, 0, shedding_ratio_);
    } else {
      // Dynamic shared
      RunLoadShedding(LoadSheddingManager::LoadSheddingBimaps, 1, shedding_ratio_);
    }
  } else if (shed_method == 1) {
    // random state shedding
    if (plan_choice == 0 || plan_choice == 1) {
      // Static non-shared/shared
      RunLoadShedding(LoadSheddingManager::LoadSheddingBimapsRandom, 0, shedding_ratio_);
    } else {
      // Dynamic shared
      RunLoadShedding(LoadSheddingManager::LoadSheddingBimapsRandom, 1, shedding_ratio_);
    }
  } else if (shed_method == 4) {
    // ICDE'20
    if (plan_choice == 0 || plan_choice == 1) {
      // Static non-shared/shared
      RunLoadShedding(LoadSheddingManager::LoadSheddingICDE, 0, shedding_ratio_);
    } else {
      // Dynamic shared
      RunLoadShedding(LoadSheddingManager::LoadSheddingICDE, 1, shedding_ratio_);
    }
  }
}

/**
 *********************************************************************************************************************
 * Experiment for Set2
 *********************************************************************************************************************
 **/

auto Experiment::GenerateSyntheticStreamSet2(std::queue<Event> &raw_event_queue,
                                             std::vector<EventType> &all_event_types, int64_t event_number) -> void {
  // Define a random number generator
  std::random_device rd;
  std::mt19937_64 rng(rd());

  // Define a uniform distribution for int64_t values, min is 1, max is 100
  std::uniform_int_distribution<int64_t> dist_id(1, 100);
  std::uniform_real_distribution<attr_t> dist_x(1.0L, 100.0L);
  std::uniform_real_distribution<attr_t> dist_event(0, 1000.0L);

  std::set<std::string> attr_names = {"id", "x"};

  auto et_a = EventType("A", attr_names, 10, 2000);
  auto et_b = EventType("B", attr_names, 10, 2000);
  auto et_c = EventType("C", attr_names, 5, 2000);
  auto et_d = EventType("D", attr_names, 10, 2000);
  auto et_e = EventType("E", attr_names, 5, 2000);
  auto et_f = EventType("F", attr_names, 10, 2000);

  all_event_types.push_back(et_a);
  all_event_types.push_back(et_b);
  all_event_types.push_back(et_c);
  all_event_types.push_back(et_d);
  all_event_types.push_back(et_e);
  all_event_types.push_back(et_f);

  int ctrl = 0;
  for (auto current_time = 0; current_time < event_number;) {
    // Generate a random int64_t value
    auto attr_val_id = dist_id(rng);
    auto attr_id = Attribute("id", attr_val_id);
    auto attr_val_x = dist_x(rng);
    auto attr_x = Attribute("x", attr_val_x);

    if (ctrl == 2) {
      ctrl = 0;
      raw_event_queue.push(Event(et_a, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
      raw_event_queue.push(Event(et_c, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
      raw_event_queue.push(Event(et_e, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
      raw_event_queue.push(Event(et_d, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
      raw_event_queue.push(Event(et_f, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
    } else {
      raw_event_queue.push(Event(et_b, {Attribute("id", dist_id(rng)), Attribute("x", dist_x(rng))}, current_time++));
      ctrl += 1;
    }
  }
}

Experiment::Experiment(int plan_choice, int query_choice, time_t q0_bound, time_t q1_bound, int shed_method,
                       uint64_t q0_window, uint64_t q1_window, int shedding_ratio)
    : plan_choice_(plan_choice),
      query_choice_(query_choice),
      q0_bound_(q0_bound),
      q1_bound_(q1_bound),
      shed_method_(shed_method),
      q0_window_(q0_window),
      q1_window_(q1_window),
      shedding_ratio_(shedding_ratio) {
  if (query_choice == 0) {
    // std::cout << " Query Set 2:" << std::endl;
    GenerateSyntheticStreamDefault(raw_event_queue_, all_event_types_, 10000);
    ReadEventsfromFileQ0("../../synthetic_data/events_q0.csv");
    // std::cout << "caller queue size: " << raw_event_queue_.size() << std::endl;
    auto et_a = all_event_types_[0];
    auto et_b = all_event_types_[1];
    auto et_c = all_event_types_[2];
    auto et_d = all_event_types_[3];
    auto et_e = all_event_types_[4];
    auto et_f = all_event_types_[5];
    auto et_g = all_event_types_[6];
    auto et_x = all_event_types_[7];
    auto et_y = all_event_types_[8];
    auto et_z = all_event_types_[9];
    std::string q1_predicate = "q0_p1";
    std::string q2_predicate = "q0_p2";
    auto q0 = Query(0, "ABCDEFG",
                    {all_event_types_[0], all_event_types_[1], all_event_types_[2], all_event_types_[3],
                     all_event_types_[4], all_event_types_[5], all_event_types_[6]},
                    q1_predicate, q0_window, q0_bound);
    auto q1 = Query(1, "ABCDXYZ",
                    {all_event_types_[0], all_event_types_[1], all_event_types_[2], all_event_types_[3],
                     all_event_types_[7], all_event_types_[8], all_event_types_[9]},
                    q2_predicate, q1_window, q1_bound);

    auto predicate1 = Predicate(q1_predicate);
    auto predicate2 = Predicate(q2_predicate);
    // Partial Matches
    if (plan_choice == 0 || plan_choice == 2) {
      // Starting State
      auto p0 = PartialMatchState();
      p0.state_name_ = "p0";
      auto p1 = PartialMatchState({et_a}, {predicate1, predicate2}, {0, 1});
      p1.state_name_ = "p1";
      auto p2 = PartialMatchState({et_a, et_b}, {predicate1, predicate2}, {0, 1});
      p2.state_name_ = "p2";
      auto p3 = PartialMatchState({et_a, et_b, et_c}, {predicate1, predicate2}, {0, 1});
      p3.state_name_ = "p3";
      auto p4 = PartialMatchState({et_a, et_b, et_c, et_d}, {predicate1, predicate2}, {0, 1});
      p4.state_name_ = "p4";

      auto p5 = PartialMatchState({et_a, et_b, et_c, et_d, et_e}, {predicate1}, {0});
      p5.state_name_ = "p5";
      auto p6 = PartialMatchState({et_a, et_b, et_c, et_d, et_e, et_f}, {predicate1}, {0});
      p6.state_name_ = "p6";

      auto p8 = PartialMatchState({et_a, et_b, et_c, et_d, et_x}, {predicate2}, {1});
      p8.state_name_ = "p8";
      auto p9 = PartialMatchState({et_a, et_b, et_c, et_d, et_x, et_y}, {predicate2}, {1});
      p9.state_name_ = "p9";

      // Complete Matches
      auto p7 =
          CompleteMatchState(0, q0_window, {et_a, et_b, et_c, et_d, et_e, et_f, et_g}, {predicate1}, {0}, q0_bound);
      p7.state_name_ = "p7";
      auto p10 =
          CompleteMatchState(1, q1_window, {et_a, et_b, et_c, et_d, et_x, et_y, et_z}, {predicate2}, {1}, q1_bound);
      p10.state_name_ = "p10";

      // make ptr for all states;
      auto p0_ptr = std::make_shared<PartialMatchState>(p0);
      auto p1_ptr = std::make_shared<PartialMatchState>(p1);
      auto p1_weak = std::weak_ptr<PartialMatchState>(p1_ptr);

      auto p2_ptr = std::make_shared<PartialMatchState>(p2);
      auto p2_weak = std::weak_ptr<PartialMatchState>(p2_ptr);

      auto p3_ptr = std::make_shared<PartialMatchState>(p3);
      auto p3_weak = std::weak_ptr<PartialMatchState>(p3_ptr);

      auto p4_ptr = std::make_shared<PartialMatchState>(p4);
      auto p4_weak = std::weak_ptr<PartialMatchState>(p4_ptr);

      auto p5_ptr = std::make_shared<PartialMatchState>(p5);
      auto p5_weak = std::weak_ptr<PartialMatchState>(p5_ptr);

      auto p6_ptr = std::make_shared<PartialMatchState>(p6);
      auto p6_weak = std::weak_ptr<PartialMatchState>(p6_ptr);

      auto p8_ptr = std::make_shared<PartialMatchState>(p8);
      auto p8_weak = std::weak_ptr<PartialMatchState>(p8_ptr);

      auto p9_ptr = std::make_shared<PartialMatchState>(p9);
      auto p9_weak = std::weak_ptr<PartialMatchState>(p9_ptr);

      auto p7_ptr = std::make_shared<CompleteMatchState>(p7);
      auto p7_weak = std::weak_ptr<PartialMatchState>(p7_ptr);

      auto p10_ptr = std::make_shared<CompleteMatchState>(p10);
      auto p10_weak = std::weak_ptr<PartialMatchState>(p10_ptr);
      //  std::cout << "p0 state name is " << p0.state_name_ << std::endl;

      QueryPlan query_plan;
      query_plan.AddStartingState(p0_ptr);
      query_plan.AddState(p1_ptr);
      query_plan.AddState(p2_ptr);
      query_plan.AddState(p3_ptr);
      query_plan.AddState(p4_ptr);
      query_plan.AddState(p5_ptr);
      query_plan.AddState(p6_ptr);
      query_plan.AddState(p7_ptr);
      query_plan.AddState(p8_ptr);
      query_plan.AddState(p9_ptr);
      query_plan.AddState(p10_ptr);
      // shared Query Plan, connect the states to form a query plan
      QueryPlan::AddEdge(p0_ptr, p1_weak, et_a);
      QueryPlan::AddEdge(p1_ptr, p2_weak, et_b);
      QueryPlan::AddEdge(p2_ptr, p3_weak, et_c);
      QueryPlan::AddEdge(p3_ptr, p4_weak, et_d);
      QueryPlan::AddEdge(p4_ptr, p5_weak, et_e);
      QueryPlan::AddEdge(p5_ptr, p6_weak, et_f);
      QueryPlan::AddEdge(p6_ptr, p7_weak, et_g);
      QueryPlan::AddEdge(p4_ptr, p8_weak, et_x);
      QueryPlan::AddEdge(p8_ptr, p9_weak, et_y);
      QueryPlan::AddEdge(p9_ptr, p10_weak, et_z);

      // Time begins
      auto buffer = EventBuffer(p0_ptr);
      buffer.q0_time_window_ = q0_window;
      buffer.q1_time_window_ = q1_window;
      buffer.q0_bound_ = q0_bound;
      buffer.q1_bound_ = q1_bound;
      buffer.shedding_ratio_ = shedding_ratio;

      while (!raw_event_queue_.empty()) {
        auto new_event = raw_event_queue_.front();
        if (new_event.GetEventName() == "*") {
          raw_event_queue_.pop();
          continue;
        }
        // std::cout << "The coming event type is " << new_event.GetEventName() << std::endl;
        new_event.out_queue_time_ = buffer.CurrentTime();
        if (buffer.EventShedding(shed_method, new_event, q0, q1)) {
          EventBuffer::event_shedding_num++;
          raw_event_queue_.pop();
          continue;
        }
        if (plan_choice == 0) {
          buffer.ProcessNewEventSet1Shared(new_event, "E", "X");
        } else {
          buffer.ProcessNewEventSet1Dynamic(new_event, "E", "X");
        }
        raw_event_queue_.pop();
        EventBuffer::max_buffer_size = buffer.all_pms_.size() > EventBuffer::max_buffer_size
                                           ? buffer.all_pms_.size()
                                           : EventBuffer::max_buffer_size;
        /***************************************************************************************************************/
        // buffer.PrintCurrentBuffer();
        // std::cout << "====================" << std::endl;
        // std::cout << "buffer size: " << buffer.all_pms_.size() << std::endl;
        /***************************************************************************************************************/
        buffer.LoadSheddingMonitor(shed_method, plan_choice);
      }
      EventBuffer::throughput = 1000000 * static_cast<float>(10010000) / static_cast<float>(buffer.CurrentTime());
    } else if (plan_choice == 1) {
      auto p0 = PartialMatchState();
      p0.state_name_ = "p0";
      auto p1 = PartialMatchState({et_a}, {predicate1}, {0});
      p1.state_name_ = "p1";
      auto p1_ = PartialMatchState({et_a}, {predicate2}, {1});
      p1_.state_name_ = "p1_";

      auto p2 = PartialMatchState({et_a, et_b}, {predicate1}, {0});
      p2.state_name_ = "p2";
      auto p2_ = PartialMatchState({et_a, et_b}, {predicate2}, {1});
      p2_.state_name_ = "p2_";

      auto p3 = PartialMatchState({et_a, et_b, et_c}, {predicate1}, {0});
      p3.state_name_ = "p3";
      auto p3_ = PartialMatchState({et_a, et_b, et_c}, {predicate2}, {1});
      p3_.state_name_ = "p3_";

      auto p4 = PartialMatchState({et_a, et_b, et_c, et_d}, {predicate1}, {0});
      p4.state_name_ = "p4";
      auto p4_ = PartialMatchState({et_a, et_b, et_c, et_d}, {predicate2}, {1});
      p4_.state_name_ = "p4_";

      auto p5 = PartialMatchState({et_a, et_b, et_c, et_d, et_e}, {predicate1}, {0});
      p5.state_name_ = "p5";
      auto p6 = PartialMatchState({et_a, et_b, et_c, et_d, et_e, et_f}, {predicate1}, {0});
      p6.state_name_ = "p6";

      auto p8 = PartialMatchState({et_a, et_b, et_c, et_d, et_x}, {predicate2}, {1});
      p8.state_name_ = "p8";
      auto p9 = PartialMatchState({et_a, et_b, et_c, et_d, et_x, et_y}, {predicate2}, {1});
      p9.state_name_ = "p9";

      // Complete Matches
      auto p7 =
          CompleteMatchState(0, q0_window, {et_a, et_b, et_c, et_d, et_e, et_f, et_g}, {predicate1}, {0}, q0_bound);
      p7.state_name_ = "p7";
      auto p10 =
          CompleteMatchState(1, q1_window, {et_a, et_b, et_c, et_d, et_x, et_y, et_z}, {predicate2}, {1}, q1_bound);
      p10.state_name_ = "p10";

      // make ptr for all states;
      auto p0_ptr = std::make_shared<PartialMatchState>(p0);
      auto p0_ptr_ = std::make_shared<PartialMatchState>(p0);

      auto p1_ptr = std::make_shared<PartialMatchState>(p1);
      auto p1_weak = std::weak_ptr<PartialMatchState>(p1_ptr);

      auto p2_ptr = std::make_shared<PartialMatchState>(p2);
      auto p2_weak = std::weak_ptr<PartialMatchState>(p2_ptr);

      auto p3_ptr = std::make_shared<PartialMatchState>(p3);
      auto p3_weak = std::weak_ptr<PartialMatchState>(p3_ptr);

      auto p4_ptr = std::make_shared<PartialMatchState>(p4);
      auto p4_weak = std::weak_ptr<PartialMatchState>(p4_ptr);

      auto p1_ptr_ = std::make_shared<PartialMatchState>(p1_);
      auto p1_weak_ = std::weak_ptr<PartialMatchState>(p1_ptr_);

      auto p2_ptr_ = std::make_shared<PartialMatchState>(p2_);
      auto p2_weak_ = std::weak_ptr<PartialMatchState>(p2_ptr_);

      auto p3_ptr_ = std::make_shared<PartialMatchState>(p3_);
      auto p3_weak_ = std::weak_ptr<PartialMatchState>(p3_ptr_);

      auto p4_ptr_ = std::make_shared<PartialMatchState>(p4_);
      auto p4_weak_ = std::weak_ptr<PartialMatchState>(p4_ptr_);

      auto p5_ptr = std::make_shared<PartialMatchState>(p5);
      auto p5_weak = std::weak_ptr<PartialMatchState>(p5_ptr);

      auto p6_ptr = std::make_shared<PartialMatchState>(p6);
      auto p6_weak = std::weak_ptr<PartialMatchState>(p6_ptr);

      auto p8_ptr = std::make_shared<PartialMatchState>(p8);
      auto p8_weak = std::weak_ptr<PartialMatchState>(p8_ptr);

      auto p9_ptr = std::make_shared<PartialMatchState>(p9);
      auto p9_weak = std::weak_ptr<PartialMatchState>(p9_ptr);

      auto p7_ptr = std::make_shared<CompleteMatchState>(p7);
      auto p7_weak = std::weak_ptr<PartialMatchState>(p7_ptr);

      auto p10_ptr = std::make_shared<CompleteMatchState>(p10);
      auto p10_weak = std::weak_ptr<PartialMatchState>(p10_ptr);
      //  std::cout << "p0 state name is " << p0.state_name_ << std::endl;

      QueryPlan query_plan;
      query_plan.AddStartingState(p0_ptr);
      query_plan.AddStartingState(p0_ptr_);
      query_plan.AddState(p1_ptr);
      query_plan.AddState(p2_ptr);
      query_plan.AddState(p3_ptr);
      query_plan.AddState(p4_ptr);
      query_plan.AddState(p1_ptr_);
      query_plan.AddState(p2_ptr_);
      query_plan.AddState(p3_ptr_);
      query_plan.AddState(p4_ptr_);
      query_plan.AddState(p5_ptr);
      query_plan.AddState(p6_ptr);
      query_plan.AddState(p7_ptr);
      query_plan.AddState(p8_ptr);
      query_plan.AddState(p9_ptr);
      query_plan.AddState(p10_ptr);
      // shared Query Plan, connect the states to form a query plan
      QueryPlan::AddEdge(p0_ptr, p1_weak, et_a);
      QueryPlan::AddEdge(p1_ptr, p2_weak, et_b);
      QueryPlan::AddEdge(p2_ptr, p3_weak, et_c);
      QueryPlan::AddEdge(p3_ptr, p4_weak, et_d);
      QueryPlan::AddEdge(p4_ptr, p5_weak, et_e);
      QueryPlan::AddEdge(p5_ptr, p6_weak, et_f);
      QueryPlan::AddEdge(p6_ptr, p7_weak, et_g);

      QueryPlan::AddEdge(p0_ptr_, p1_weak_, et_a);
      QueryPlan::AddEdge(p1_ptr_, p2_weak_, et_b);
      QueryPlan::AddEdge(p2_ptr_, p3_weak_, et_c);
      QueryPlan::AddEdge(p3_ptr_, p4_weak_, et_d);
      QueryPlan::AddEdge(p4_ptr_, p8_weak, et_x);
      QueryPlan::AddEdge(p8_ptr, p9_weak, et_y);
      QueryPlan::AddEdge(p9_ptr, p10_weak, et_z);

      // Time begins
      auto buffer = EventBuffer(p0_ptr, p0_ptr_);
      buffer.q0_time_window_ = q0_window;
      buffer.q1_time_window_ = q1_window;
      buffer.q0_bound_ = q0_bound;
      buffer.q1_bound_ = q1_bound;
      buffer.shedding_ratio_ = shedding_ratio;
      // int event_count = 0;
      while (!raw_event_queue_.empty()) {
        auto new_event = raw_event_queue_.front();
        // event_count++;
        // std::cout << "Event num: " << event_count << std::endl;
        if (new_event.GetEventName() == "*") {
          raw_event_queue_.pop();
          continue;
        }
        // std::cout << "The coming event type is " << new_event.GetEventName() << std::endl;
        new_event.out_queue_time_ = buffer.CurrentTime();
        if (buffer.EventShedding(shed_method, new_event, q0, q1)) {
          EventBuffer::event_shedding_num++;
          raw_event_queue_.pop();
          continue;
        }
        buffer.ProcessNewEventNonSharedSet1(new_event);
        raw_event_queue_.pop();
        EventBuffer::max_buffer_size = buffer.all_pms_.size() > EventBuffer::max_buffer_size
                                           ? buffer.all_pms_.size()
                                           : EventBuffer::max_buffer_size;
        /***************************************************************************************************************/
        // buffer.PrintCurrentBuffer();
        // std::cout << "====================" << std::endl;
        // std::cout << "buffer size: " << buffer.all_pms_.size() << std::endl;
        /***************************************************************************************************************/
        buffer.LoadSheddingMonitor(shed_method, plan_choice);
      }
      EventBuffer::throughput = 1000000 * static_cast<float>(10010000) / static_cast<float>(buffer.CurrentTime());
    }
  } else if (query_choice == 1) {
    std::cout << " Query Set 1:" << std::endl;
    GenerateSyntheticStreamSet1(raw_event_queue_, all_event_types_, 100000);
    std::string q1_predicate = "q1_p1";
    std::string q2_predicate = "q1_p2";
    auto q0 = Query(0, "AB~CD", all_event_types_, q1_predicate, 3, q1_bound);
    auto q1 = Query(1, "AB~CE", all_event_types_, q2_predicate, 10, q0_bound);

    auto predicate1 = Predicate(q1_predicate);
    auto predicate2 = Predicate(q2_predicate);
    // Partial Matches
    auto et_a = all_event_types_[0];
    auto et_b = all_event_types_[1];
    auto et_c = all_event_types_[2];
    auto et_d = all_event_types_[3];
    auto et_e = all_event_types_[4];
    if (plan_choice == 0 || plan_choice == 2) {
      // Starting State
      auto p0 = PartialMatchState();
      p0.state_name_ = "p0";
      auto p1 = PartialMatchState({et_a}, {predicate1, predicate2}, {0, 1});
      p1.state_name_ = "p1";
      auto p2 = PartialMatchState({et_a, et_b}, {predicate1, predicate2}, {0, 1});
      p2.state_name_ = "p2";
      auto p3 = PartialMatchState({et_a, et_b, et_c}, {predicate1, predicate2}, {0, 1});
      p3.state_name_ = "p3";
      p3.is_void_ = true;

      // Complete Matches
      auto p4 = CompleteMatchState(0, 3, {et_a, et_b, et_d}, {predicate1}, {0}, q0_bound);
      p4.state_name_ = "p4";
      auto p5 = CompleteMatchState(1, 10, {et_a, et_b, et_e}, {predicate2}, {1}, q1_bound);
      p5.state_name_ = "p5";

      // make ptr for all states;
      auto p0_ptr = std::make_shared<PartialMatchState>(p0);
      auto p1_ptr = std::make_shared<PartialMatchState>(p1);
      auto p1_weak = std::weak_ptr<PartialMatchState>(p1_ptr);

      auto p2_ptr = std::make_shared<PartialMatchState>(p2);
      auto p2_weak = std::weak_ptr<PartialMatchState>(p2_ptr);

      auto p3_ptr = std::make_shared<PartialMatchState>(p3);
      auto p3_weak = std::weak_ptr<PartialMatchState>(p3_ptr);

      auto p4_ptr = std::make_shared<CompleteMatchState>(p4);
      auto p4_weak = std::weak_ptr<PartialMatchState>(p4_ptr);

      auto p5_ptr = std::make_shared<CompleteMatchState>(p5);
      auto p5_weak = std::weak_ptr<PartialMatchState>(p5_ptr);
      //  std::cout << "p0 state name is " << p0.state_name_ << std::endl;

      QueryPlan query_plan;
      query_plan.AddStartingState(p0_ptr);
      query_plan.AddState(p1_ptr);
      query_plan.AddState(p2_ptr);
      query_plan.AddState(p3_ptr);
      query_plan.AddState(p4_ptr);
      query_plan.AddState(p5_ptr);
      // shared Query Plan, connect the states to form a query plan
      QueryPlan::AddEdge(p0_ptr, p1_weak, et_a);
      QueryPlan::AddEdge(p1_ptr, p2_weak, et_b);
      QueryPlan::AddEdge(p2_ptr, p3_weak, et_c);
      QueryPlan::AddEdge(p2_ptr, p4_weak, et_d);
      QueryPlan::AddEdge(p2_ptr, p5_weak, et_e);

      // Time begins
      auto buffer = EventBuffer(p0_ptr);
      // Set time windows
      buffer.q0_time_window_ = q0_window;
      buffer.q1_time_window_ = q1_window;
      buffer.q0_bound_ = q0_bound;
      buffer.q1_bound_ = q1_bound;
      buffer.shedding_ratio_ = shedding_ratio;

      while (!raw_event_queue_.empty()) {
        auto new_event = raw_event_queue_.front();
        //        std::cout << "The coming event type is " << new_event.GetEventName() << std::endl;
        new_event.out_queue_time_ = buffer.CurrentTime();
        if (buffer.EventShedding(shed_method, new_event, q0, q1)) {
          EventBuffer::event_shedding_num++;
          raw_event_queue_.pop();
          continue;
        }
        if (plan_choice == 0) {
          buffer.ProcessNewEventSet1Shared(new_event, "D", "E");
        } else {
          buffer.ProcessNewEventSet1Dynamic(new_event, "D", "E");
        }
        raw_event_queue_.pop();
        EventBuffer::max_buffer_size = buffer.all_pms_.size() > EventBuffer::max_buffer_size
                                           ? buffer.all_pms_.size()
                                           : EventBuffer::max_buffer_size;
        //        buffer.PrintCurrentBuffer();
        //        std::cout << "====================" << std::endl;
        //        std::cout << "buffer size: " << buffer.all_pms_.size() << std::endl;

        buffer.LoadSheddingMonitor(shed_method, plan_choice);
      }
      EventBuffer::throughput = 1000000 * static_cast<float>(100000) / static_cast<float>(buffer.CurrentTime());
    } else if (plan_choice == 1) {
      // Starting State
      auto p0 = PartialMatchState();
      p0.state_name_ = "p0";
      auto p1 = PartialMatchState({et_a}, {predicate1}, {0});
      p1.state_name_ = "p1";
      auto p2 = PartialMatchState({et_a, et_b}, {predicate1}, {0});
      p2.state_name_ = "p2";
      auto p3 = PartialMatchState({et_a, et_b, et_c}, {predicate1}, {0});
      p3.state_name_ = "p3";
      p3.is_void_ = true;

      auto p1_ = PartialMatchState({et_a}, {predicate2}, {1});
      p1_.state_name_ = "p1_";
      auto p2_ = PartialMatchState({et_a, et_b}, {predicate2}, {1});
      p2_.state_name_ = "p2_";
      auto p3_ = PartialMatchState({et_a, et_b, et_c}, {predicate2}, {1});
      p3_.state_name_ = "p3_";
      p3_.is_void_ = true;

      // Complete Matches
      auto p4 = CompleteMatchState(0, 3, {et_a, et_b, et_c, et_d}, {predicate1}, {0}, q0_bound);
      p4.state_name_ = "p4";
      auto p5 = CompleteMatchState(1, 10, {et_a, et_b, et_c, et_e}, {predicate2}, {1}, q1_bound);
      p5.state_name_ = "p5";

      // make ptr for all states;
      auto p0_ptr = std::make_shared<PartialMatchState>(p0);
      auto p0_ptr_ = std::make_shared<PartialMatchState>(p0);
      auto p1_ptr = std::make_shared<PartialMatchState>(p1);
      auto p1_weak = std::weak_ptr<PartialMatchState>(p1_ptr);

      auto p2_ptr = std::make_shared<PartialMatchState>(p2);
      auto p2_weak = std::weak_ptr<PartialMatchState>(p2_ptr);

      auto p3_ptr = std::make_shared<PartialMatchState>(p3);
      auto p3_weak = std::weak_ptr<PartialMatchState>(p3_ptr);

      auto p1_ptr_ = std::make_shared<PartialMatchState>(p1_);
      auto p1_weak_ = std::weak_ptr<PartialMatchState>(p1_ptr_);

      auto p2_ptr_ = std::make_shared<PartialMatchState>(p2_);
      auto p2_weak_ = std::weak_ptr<PartialMatchState>(p2_ptr_);

      auto p3_ptr_ = std::make_shared<PartialMatchState>(p3_);
      auto p3_weak_ = std::weak_ptr<PartialMatchState>(p3_ptr_);

      auto p4_ptr = std::make_shared<CompleteMatchState>(p4);
      auto p4_weak = std::weak_ptr<PartialMatchState>(p4_ptr);

      auto p5_ptr = std::make_shared<CompleteMatchState>(p5);
      auto p5_weak = std::weak_ptr<PartialMatchState>(p5_ptr);
      //  std::cout << "p0 state name is " << p0.state_name_ << std::endl;

      QueryPlan query_plan;
      query_plan.AddStartingState(p0_ptr);
      query_plan.AddStartingState(p0_ptr_);
      query_plan.AddState(p1_ptr);
      query_plan.AddState(p2_ptr);
      query_plan.AddState(p3_ptr);
      query_plan.AddState(p1_ptr_);
      query_plan.AddState(p2_ptr_);
      query_plan.AddState(p3_ptr_);
      query_plan.AddState(p4_ptr);
      query_plan.AddState(p5_ptr);
      // shared Query Plan, connect the states to form a query plan
      QueryPlan::AddEdge(p0_ptr, p1_weak, et_a);
      QueryPlan::AddEdge(p1_ptr, p2_weak, et_b);
      QueryPlan::AddEdge(p2_ptr, p3_weak, et_c);
      QueryPlan::AddEdge(p2_ptr, p4_weak, et_d);

      QueryPlan::AddEdge(p0_ptr_, p1_weak_, et_a);
      QueryPlan::AddEdge(p1_ptr_, p2_weak_, et_b);
      QueryPlan::AddEdge(p2_ptr_, p3_weak_, et_c);
      QueryPlan::AddEdge(p2_ptr_, p5_weak, et_e);

      // Time begins
      auto buffer = EventBuffer(p0_ptr, p0_ptr_);
      buffer.q0_time_window_ = q0_window;
      buffer.q1_time_window_ = q1_window;
      buffer.q0_bound_ = q0_bound;
      buffer.q1_bound_ = q1_bound;
      buffer.shedding_ratio_ = shedding_ratio;

      while (!raw_event_queue_.empty()) {
        auto new_event = raw_event_queue_.front();
        //        std::cout << "The coming event type is " << new_event.GetEventName() << std::endl;
        new_event.out_queue_time_ = buffer.CurrentTime();
        if (buffer.EventShedding(shed_method, new_event, q0, q1)) {
          EventBuffer::event_shedding_num++;
          raw_event_queue_.pop();
          continue;
        }

        buffer.ProcessNewEventNonSharedSet1(new_event);

        raw_event_queue_.pop();
        EventBuffer::max_buffer_size = buffer.all_pms_.size() > EventBuffer::max_buffer_size
                                           ? buffer.all_pms_.size()
                                           : EventBuffer::max_buffer_size;
        //        buffer.PrintCurrentBuffer();
        //        std::cout << "====================" << std::endl;
        // std::cout << "buffer size: " << buffer.all_pms_.size() << std::endl;

        buffer.LoadSheddingMonitor(shed_method, plan_choice);
      }
      EventBuffer::throughput = 1000000 * static_cast<float>(100000) / static_cast<float>(buffer.CurrentTime());
    }
  } else if (query_choice == 2) {
    // std::cout << " Query Set 2:" << std::endl;
    GenerateSyntheticStreamSet2(raw_event_queue_, all_event_types_, 100000);
    auto et_a = all_event_types_[0];
    auto et_b = all_event_types_[1];
    auto et_c = all_event_types_[2];
    auto et_d = all_event_types_[3];
    auto et_e = all_event_types_[4];
    auto et_f = all_event_types_[5];
    std::string q1_predicate = "a.ID=b.ID AND a.ID=c[i].ID AND a.ID=d.ID";
    std::string q2_predicate = "a.ID=b.ID AND a.ID=e[i].ID AND a.ID=f.ID";
    auto q0 = Query(0, "AB+CD", {all_event_types_[0], all_event_types_[1], all_event_types_[2], all_event_types_[3]},
                    q1_predicate, 3, q0_bound);
    auto q1 = Query(1, "AB+EF", {all_event_types_[0], all_event_types_[1], all_event_types_[4], all_event_types_[5]},
                    q2_predicate, 10, q1_bound);

    auto predicate1 = Predicate(q1_predicate);
    auto predicate2 = Predicate(q2_predicate);
    // Partial Matches
    if (plan_choice == 0 || plan_choice == 2) {
      // Starting State
      auto p0 = PartialMatchState();
      p0.state_name_ = "p0";
      auto p1 = PartialMatchState({et_a}, {predicate1, predicate2}, {0, 1});
      p1.state_name_ = "p1";
      auto p2 = PartialMatchState({et_a, et_b}, {predicate1, predicate2}, {0, 1});
      p2.state_name_ = "p2";
      auto p3 = PartialMatchState({et_a, et_b, et_c}, {predicate1}, {0});
      p3.state_name_ = "p3";
      auto p5 = PartialMatchState({et_a, et_b, et_e}, {predicate2}, {1});
      p5.state_name_ = "p5";

      // Complete Matches
      auto p4 = CompleteMatchState(0, 3, {et_a, et_b, et_c, et_d}, {predicate1}, {0}, q0_bound);
      p4.state_name_ = "p4";
      auto p6 = CompleteMatchState(1, 10, {et_a, et_b, et_e, et_f}, {predicate2}, {1}, q1_bound);
      p6.state_name_ = "p6";

      // make ptr for all states;
      auto p0_ptr = std::make_shared<PartialMatchState>(p0);
      auto p1_ptr = std::make_shared<PartialMatchState>(p1);
      auto p1_weak = std::weak_ptr<PartialMatchState>(p1_ptr);

      auto p2_ptr = std::make_shared<PartialMatchState>(p2);
      auto p2_weak = std::weak_ptr<PartialMatchState>(p2_ptr);

      auto p3_ptr = std::make_shared<PartialMatchState>(p3);
      auto p3_weak = std::weak_ptr<PartialMatchState>(p3_ptr);

      auto p5_ptr = std::make_shared<PartialMatchState>(p5);
      auto p5_weak = std::weak_ptr<PartialMatchState>(p5_ptr);

      auto p4_ptr = std::make_shared<CompleteMatchState>(p4);
      auto p4_weak = std::weak_ptr<PartialMatchState>(p4_ptr);

      auto p6_ptr = std::make_shared<CompleteMatchState>(p6);
      auto p6_weak = std::weak_ptr<PartialMatchState>(p6_ptr);
      //  std::cout << "p0 state name is " << p0.state_name_ << std::endl;

      QueryPlan query_plan;
      query_plan.AddStartingState(p0_ptr);
      query_plan.AddState(p1_ptr);
      query_plan.AddState(p2_ptr);
      query_plan.AddState(p3_ptr);
      query_plan.AddState(p4_ptr);
      query_plan.AddState(p5_ptr);
      query_plan.AddState(p6_ptr);
      // shared Query Plan, connect the states to form a query plan
      QueryPlan::AddEdge(p0_ptr, p1_weak, et_a);
      QueryPlan::AddEdge(p1_ptr, p2_weak, et_b);
      QueryPlan::AddEdge(p2_ptr, p2_weak, et_b);
      QueryPlan::AddEdge(p2_ptr, p3_weak, et_c);
      QueryPlan::AddEdge(p3_ptr, p4_weak, et_d);
      QueryPlan::AddEdge(p2_ptr, p5_weak, et_e);
      QueryPlan::AddEdge(p5_ptr, p6_weak, et_f);

      // Time begins
      auto buffer = EventBuffer(p0_ptr);
      buffer.q0_time_window_ = q0_window;
      buffer.q1_time_window_ = q1_window;
      buffer.q0_bound_ = q0_bound;
      buffer.q1_bound_ = q1_bound;
      buffer.shedding_ratio_ = shedding_ratio;

      while (!raw_event_queue_.empty()) {
        auto new_event = raw_event_queue_.front();
        // std::cout << "The coming event type is " << new_event.GetEventName() << std::endl;
        new_event.out_queue_time_ = buffer.CurrentTime();
        if (buffer.EventShedding(shed_method, new_event, q0, q1)) {
          EventBuffer::event_shedding_num++;
          raw_event_queue_.pop();
          continue;
        }
        if (plan_choice == 0) {
          buffer.ProcessNewEventSet1Shared(new_event, "C", "E");
        } else {
          buffer.ProcessNewEventSet1Dynamic(new_event, "C", "E");
        }
        raw_event_queue_.pop();
        EventBuffer::max_buffer_size = buffer.all_pms_.size() > EventBuffer::max_buffer_size
                                           ? buffer.all_pms_.size()
                                           : EventBuffer::max_buffer_size;
        /***************************************************************************************************************/
        // buffer.PrintCurrentBuffer();
        // std::cout << "====================" << std::endl;
        // std::cout << "buffer size: " << buffer.all_pms_.size() << std::endl;
        /***************************************************************************************************************/
        buffer.LoadSheddingMonitor(shed_method, plan_choice);
      }
      EventBuffer::throughput = 1000000 * static_cast<float>(100000) / static_cast<float>(buffer.CurrentTime());
    } else if (plan_choice == 1) {
      // Starting State
      auto p0 = PartialMatchState();
      p0.state_name_ = "p0";
      auto p1 = PartialMatchState({et_a}, {predicate1}, {0});
      p1.state_name_ = "p1";
      auto p2 = PartialMatchState({et_a, et_b}, {predicate1}, {0});
      p2.state_name_ = "p2";
      auto p3 = PartialMatchState({et_a, et_b, et_c}, {predicate1}, {0});
      p3.state_name_ = "p3";

      auto p1_ = PartialMatchState({et_a}, {predicate2}, {1});
      p1_.state_name_ = "p1_";
      auto p2_ = PartialMatchState({et_a, et_b}, {predicate2}, {1});
      p2_.state_name_ = "p2_";

      auto p5 = PartialMatchState({et_a, et_b, et_e}, {predicate2}, {1});
      p5.state_name_ = "p5";

      // Complete Matches
      auto p4 = CompleteMatchState(0, 3, {et_a, et_b, et_c, et_d}, {predicate1}, {0}, q0_bound);
      p4.state_name_ = "p4";
      auto p6 = CompleteMatchState(1, 10, {et_a, et_b, et_e, et_f}, {predicate2}, {1}, q1_bound);
      p6.state_name_ = "p6";

      // make ptr for all states;
      auto p0_ptr = std::make_shared<PartialMatchState>(p0);
      auto p0_ptr_ = std::make_shared<PartialMatchState>(p0);
      auto p1_ptr = std::make_shared<PartialMatchState>(p1);
      auto p1_weak = std::weak_ptr<PartialMatchState>(p1_ptr);

      auto p2_ptr = std::make_shared<PartialMatchState>(p2);
      auto p2_weak = std::weak_ptr<PartialMatchState>(p2_ptr);

      auto p3_ptr = std::make_shared<PartialMatchState>(p3);
      auto p3_weak = std::weak_ptr<PartialMatchState>(p3_ptr);

      auto p1_ptr_ = std::make_shared<PartialMatchState>(p1_);
      auto p1_weak_ = std::weak_ptr<PartialMatchState>(p1_ptr_);

      auto p2_ptr_ = std::make_shared<PartialMatchState>(p2_);
      auto p2_weak_ = std::weak_ptr<PartialMatchState>(p2_ptr_);

      auto p5_ptr = std::make_shared<PartialMatchState>(p5);
      auto p5_weak = std::weak_ptr<PartialMatchState>(p5_ptr);

      auto p4_ptr = std::make_shared<CompleteMatchState>(p4);
      auto p4_weak = std::weak_ptr<PartialMatchState>(p4_ptr);

      auto p6_ptr = std::make_shared<CompleteMatchState>(p6);
      auto p6_weak = std::weak_ptr<PartialMatchState>(p6_ptr);
      //  std::cout << "p0 state name is " << p0.state_name_ << std::endl;

      QueryPlan query_plan;
      query_plan.AddStartingState(p0_ptr);
      query_plan.AddStartingState(p0_ptr_);
      query_plan.AddState(p1_ptr);
      query_plan.AddState(p2_ptr);
      query_plan.AddState(p3_ptr);
      query_plan.AddState(p1_ptr_);
      query_plan.AddState(p2_ptr_);
      query_plan.AddState(p5_ptr);
      query_plan.AddState(p4_ptr);
      query_plan.AddState(p6_ptr);
      // shared Query Plan, connect the states to form a query plan
      QueryPlan::AddEdge(p0_ptr, p1_weak, et_a);
      QueryPlan::AddEdge(p1_ptr, p2_weak, et_b);
      QueryPlan::AddEdge(p2_ptr, p2_weak, et_b);
      QueryPlan::AddEdge(p2_ptr, p3_weak, et_c);
      QueryPlan::AddEdge(p3_ptr, p4_weak, et_d);

      QueryPlan::AddEdge(p0_ptr_, p1_weak_, et_a);
      QueryPlan::AddEdge(p1_ptr_, p2_weak_, et_b);
      QueryPlan::AddEdge(p2_ptr_, p2_weak_, et_b);
      QueryPlan::AddEdge(p2_ptr_, p5_weak, et_e);
      QueryPlan::AddEdge(p5_ptr, p6_weak, et_f);

      // Time begins
      auto buffer = EventBuffer(p0_ptr, p0_ptr_);
      buffer.q0_time_window_ = q0_window;
      buffer.q1_time_window_ = q1_window;
      buffer.q0_bound_ = q0_bound;
      buffer.q1_bound_ = q1_bound;
      buffer.shedding_ratio_ = shedding_ratio;

      while (!raw_event_queue_.empty()) {
        auto new_event = raw_event_queue_.front();
        // std::cout << "The coming event type is " << new_event.GetEventName() << std::endl;
        new_event.out_queue_time_ = buffer.CurrentTime();
        if (buffer.EventShedding(shed_method, new_event, q0, q1)) {
          EventBuffer::event_shedding_num++;
          raw_event_queue_.pop();
          continue;
        }
        buffer.ProcessNewEventNonSharedSet1(new_event);
        raw_event_queue_.pop();
        EventBuffer::max_buffer_size = buffer.all_pms_.size() > EventBuffer::max_buffer_size
                                           ? buffer.all_pms_.size()
                                           : EventBuffer::max_buffer_size;
        /***************************************************************************************************************/
        // buffer.PrintCurrentBuffer();
        // std::cout << "====================" << std::endl;
        // std::cout << "buffer size: " << buffer.all_pms_.size() << std::endl;
        /***************************************************************************************************************/
        buffer.LoadSheddingMonitor(shed_method, plan_choice);
      }
      EventBuffer::throughput = 1000000 * static_cast<float>(100000) / static_cast<float>(buffer.CurrentTime());
    }
  } else if (query_choice == 3) {
    // Query Set 3: Comprehensive 16-query set
    std::cout << " Query Set 3 (16 queries):" << std::endl;
    GenerateSyntheticStreamSet2(raw_event_queue_, all_event_types_, 100000);
    
    auto et_a = all_event_types_[0];
    auto et_b = all_event_types_[1];
    auto et_c = all_event_types_[2];
    auto et_d = all_event_types_[3];
    auto et_e = all_event_types_[4];
    auto et_f = all_event_types_[5];
    
    // Define predicates for all 16 queries
    std::string q1_predicate = "q3_p1";   // Q1: SEQ(A, B, C)
    std::string q2_predicate = "q3_p2";   // Q2: SEQ(A, B, E)
    std::string q3_predicate = "q3_p3";   // Q3: SEQ(A, !E, C)
    std::string q4_predicate = "q3_p4";   // Q4: SEQ(A, !E, D)
    std::string q5_predicate = "q3_p5";   // Q5: SEQ(A, B+, C)
    std::string q6_predicate = "q3_p6";   // Q6: SEQ(A, B+, D)
    std::string q7_predicate = "q3_p7";   // Q7: SEQ(A, B, B, C)
    std::string q8_predicate = "q3_p8";   // Q8: SEQ(A, C, D)
    std::string q9_predicate = "q3_p9";   // Q9: SEQ(A, B, C, D)
    std::string q10_predicate = "q3_p10"; // Q10: SEQ(A, B+, E)
    std::string q11_predicate = "q3_p11"; // Q11: SEQ(A, !B, C)
    std::string q12_predicate = "q3_p12"; // Q12: SEQ(A, !C, D)
    std::string q13_predicate = "q3_p13"; // Q13: SEQ(A, B, D, E)
    std::string q14_predicate = "q3_p14"; // Q14: SEQ(A, C, B, D)
    std::string q15_predicate = "q3_p15"; // Q15: SEQ(A, !B, D, E)
    std::string q16_predicate = "q3_p16"; // Q16: SEQ(A, B+, C, D)
    
    // Create primary queries (using first two as representatives)
    auto q0 = Query(0, "ABC", {et_a, et_b, et_c}, q1_predicate, q0_window, q0_bound);
    auto q1 = Query(1, "ABE", {et_a, et_b, et_e}, q2_predicate, q1_window, q1_bound);
    
    // Create all predicates
    auto predicate1 = Predicate(q1_predicate);
    auto predicate2 = Predicate(q2_predicate);
    auto predicate3 = Predicate(q3_predicate);
    auto predicate4 = Predicate(q4_predicate);
    auto predicate5 = Predicate(q5_predicate);
    auto predicate6 = Predicate(q6_predicate);
    auto predicate7 = Predicate(q7_predicate);
    auto predicate8 = Predicate(q8_predicate);
    auto predicate9 = Predicate(q9_predicate);
    auto predicate10 = Predicate(q10_predicate);
    auto predicate11 = Predicate(q11_predicate);
    auto predicate12 = Predicate(q12_predicate);
    auto predicate13 = Predicate(q13_predicate);
    auto predicate14 = Predicate(q14_predicate);
    auto predicate15 = Predicate(q15_predicate);
    auto predicate16 = Predicate(q16_predicate);
    
    if (plan_choice == 0 || plan_choice == 2) {
      // Starting State
      auto p0 = PartialMatchState();
      p0.state_name_ = "p0";
      
      // Q1: SEQ(A, B, C) - States: A -> AB -> ABC
      auto p1_1 = PartialMatchState({et_a}, {predicate1}, {0});
      p1_1.state_name_ = "p1_1";
      auto p1_2 = PartialMatchState({et_a, et_b}, {predicate1}, {0});
      p1_2.state_name_ = "p1_2";
      auto p1_3 = CompleteMatchState(0, q0_window, {et_a, et_b, et_c}, {predicate1}, {0}, q0_bound);
      p1_3.state_name_ = "p1_3";
      
      // Q2: SEQ(A, B, E) - States: A -> AB -> ABE
      auto p2_1 = PartialMatchState({et_a}, {predicate2}, {1});
      p2_1.state_name_ = "p2_1";
      auto p2_2 = PartialMatchState({et_a, et_b}, {predicate2}, {1});
      p2_2.state_name_ = "p2_2";
      auto p2_3 = CompleteMatchState(1, q1_window, {et_a, et_b, et_e}, {predicate2}, {1}, q1_bound);
      p2_3.state_name_ = "p2_3";
      
      // Q3: SEQ(A, !E, C) - States: A -> A[!E] -> A[!E]C
      auto p3_1 = PartialMatchState({et_a}, {predicate3}, {0});
      p3_1.state_name_ = "p3_1";
      auto p3_2 = CompleteMatchState(0, q0_window, {et_a, et_c}, {predicate3}, {0}, q0_bound);
      p3_2.state_name_ = "p3_2";
      
      // Q4: SEQ(A, !E, D) - States: A -> A[!E] -> A[!E]D
      auto p4_1 = PartialMatchState({et_a}, {predicate4}, {0});
      p4_1.state_name_ = "p4_1";
      auto p4_2 = CompleteMatchState(0, q0_window, {et_a, et_d}, {predicate4}, {0}, q0_bound);
      p4_2.state_name_ = "p4_2";
      
      // Q5: SEQ(A, B+, C) - States: A -> AB+ -> AB+C
      auto p5_1 = PartialMatchState({et_a}, {predicate5}, {0});
      p5_1.state_name_ = "p5_1";
      auto p5_2 = PartialMatchState({et_a, et_b}, {predicate5}, {0});
      p5_2.state_name_ = "p5_2";
      auto p5_3 = CompleteMatchState(0, q0_window, {et_a, et_b, et_c}, {predicate5}, {0}, q0_bound);
      p5_3.state_name_ = "p5_3";
      
      // Q6: SEQ(A, B+, D) - States: A -> AB+ -> AB+D
      auto p6_1 = PartialMatchState({et_a}, {predicate6}, {0});
      p6_1.state_name_ = "p6_1";
      auto p6_2 = PartialMatchState({et_a, et_b}, {predicate6}, {0});
      p6_2.state_name_ = "p6_2";
      auto p6_3 = CompleteMatchState(0, q0_window, {et_a, et_b, et_d}, {predicate6}, {0}, q0_bound);
      p6_3.state_name_ = "p6_3";
      
      // Q7: SEQ(A, B, B, C) - States: A -> AB -> ABB -> ABBC
      auto p7_1 = PartialMatchState({et_a}, {predicate7}, {0});
      p7_1.state_name_ = "p7_1";
      auto p7_2 = PartialMatchState({et_a, et_b}, {predicate7}, {0});
      p7_2.state_name_ = "p7_2";
      auto p7_3 = PartialMatchState({et_a, et_b, et_b}, {predicate7}, {0});
      p7_3.state_name_ = "p7_3";
      auto p7_4 = CompleteMatchState(0, q0_window, {et_a, et_b, et_b, et_c}, {predicate7}, {0}, q0_bound);
      p7_4.state_name_ = "p7_4";
      
      // Q8: SEQ(A, C, D) - States: A -> AC -> ACD
      auto p8_1 = PartialMatchState({et_a}, {predicate8}, {0});
      p8_1.state_name_ = "p8_1";
      auto p8_2 = PartialMatchState({et_a, et_c}, {predicate8}, {0});
      p8_2.state_name_ = "p8_2";
      auto p8_3 = CompleteMatchState(0, q0_window, {et_a, et_c, et_d}, {predicate8}, {0}, q0_bound);
      p8_3.state_name_ = "p8_3";
      
      // Q9: SEQ(A, B, C, D) - States: A -> AB -> ABC -> ABCD
      auto p9_1 = PartialMatchState({et_a}, {predicate9}, {0});
      p9_1.state_name_ = "p9_1";
      auto p9_2 = PartialMatchState({et_a, et_b}, {predicate9}, {0});
      p9_2.state_name_ = "p9_2";
      auto p9_3 = PartialMatchState({et_a, et_b, et_c}, {predicate9}, {0});
      p9_3.state_name_ = "p9_3";
      auto p9_4 = CompleteMatchState(0, q0_window, {et_a, et_b, et_c, et_d}, {predicate9}, {0}, q0_bound);
      p9_4.state_name_ = "p9_4";
      
      // Q10: SEQ(A, B+, E) - States: A -> AB+ -> AB+E
      auto p10_1 = PartialMatchState({et_a}, {predicate10}, {1});
      p10_1.state_name_ = "p10_1";
      auto p10_2 = PartialMatchState({et_a, et_b}, {predicate10}, {1});
      p10_2.state_name_ = "p10_2";
      auto p10_3 = CompleteMatchState(1, q1_window, {et_a, et_b, et_e}, {predicate10}, {1}, q1_bound);
      p10_3.state_name_ = "p10_3";
      
      // Q11: SEQ(A, !B, C) - States: A -> A[!B] -> A[!B]C
      auto p11_1 = PartialMatchState({et_a}, {predicate11}, {0});
      p11_1.state_name_ = "p11_1";
      auto p11_2 = CompleteMatchState(0, q0_window, {et_a, et_c}, {predicate11}, {0}, q0_bound);
      p11_2.state_name_ = "p11_2";
      
      // Q12: SEQ(A, !C, D) - States: A -> A[!C] -> A[!C]D
      auto p12_1 = PartialMatchState({et_a}, {predicate12}, {0});
      p12_1.state_name_ = "p12_1";
      auto p12_2 = CompleteMatchState(0, q0_window, {et_a, et_d}, {predicate12}, {0}, q0_bound);
      p12_2.state_name_ = "p12_2";
      
      // Q13: SEQ(A, B, D, E) - States: A -> AB -> ABD -> ABDE
      auto p13_1 = PartialMatchState({et_a}, {predicate13}, {1});
      p13_1.state_name_ = "p13_1";
      auto p13_2 = PartialMatchState({et_a, et_b}, {predicate13}, {1});
      p13_2.state_name_ = "p13_2";
      auto p13_3 = PartialMatchState({et_a, et_b, et_d}, {predicate13}, {1});
      p13_3.state_name_ = "p13_3";
      auto p13_4 = CompleteMatchState(1, q1_window, {et_a, et_b, et_d, et_e}, {predicate13}, {1}, q1_bound);
      p13_4.state_name_ = "p13_4";
      
      // Q14: SEQ(A, C, B, D) - States: A -> AC -> ACB -> ACBD
      auto p14_1 = PartialMatchState({et_a}, {predicate14}, {0});
      p14_1.state_name_ = "p14_1";
      auto p14_2 = PartialMatchState({et_a, et_c}, {predicate14}, {0});
      p14_2.state_name_ = "p14_2";
      auto p14_3 = PartialMatchState({et_a, et_c, et_b}, {predicate14}, {0});
      p14_3.state_name_ = "p14_3";
      auto p14_4 = CompleteMatchState(0, q0_window, {et_a, et_c, et_b, et_d}, {predicate14}, {0}, q0_bound);
      p14_4.state_name_ = "p14_4";
      
      // Q15: SEQ(A, !B, D, E) - States: A -> A[!B] -> A[!B]D -> A[!B]DE
      auto p15_1 = PartialMatchState({et_a}, {predicate15}, {1});
      p15_1.state_name_ = "p15_1";
      auto p15_2 = PartialMatchState({et_a, et_d}, {predicate15}, {1});
      p15_2.state_name_ = "p15_2";
      auto p15_3 = CompleteMatchState(1, q1_window, {et_a, et_d, et_e}, {predicate15}, {1}, q1_bound);
      p15_3.state_name_ = "p15_3";
      
      // Q16: SEQ(A, B+, C, D) - States: A -> AB+ -> AB+C -> AB+CD
      auto p16_1 = PartialMatchState({et_a}, {predicate16}, {0});
      p16_1.state_name_ = "p16_1";
      auto p16_2 = PartialMatchState({et_a, et_b}, {predicate16}, {0});
      p16_2.state_name_ = "p16_2";
      auto p16_3 = PartialMatchState({et_a, et_b, et_c}, {predicate16}, {0});
      p16_3.state_name_ = "p16_3";
      auto p16_4 = CompleteMatchState(0, q0_window, {et_a, et_b, et_c, et_d}, {predicate16}, {0}, q0_bound);
      p16_4.state_name_ = "p16_4";
      
      // Create shared pointers for all states
      auto p0_ptr = std::make_shared<PartialMatchState>(p0);
      
      // Q1 pointers
      auto p1_1_ptr = std::make_shared<PartialMatchState>(p1_1);
      auto p1_2_ptr = std::make_shared<PartialMatchState>(p1_2);
      auto p1_3_ptr = std::make_shared<CompleteMatchState>(p1_3);
      
      // Q2 pointers
      auto p2_1_ptr = std::make_shared<PartialMatchState>(p2_1);
      auto p2_2_ptr = std::make_shared<PartialMatchState>(p2_2);
      auto p2_3_ptr = std::make_shared<CompleteMatchState>(p2_3);
      
      // Q3 pointers
      auto p3_1_ptr = std::make_shared<PartialMatchState>(p3_1);
      auto p3_2_ptr = std::make_shared<CompleteMatchState>(p3_2);
      
      // Q4 pointers
      auto p4_1_ptr = std::make_shared<PartialMatchState>(p4_1);
      auto p4_2_ptr = std::make_shared<CompleteMatchState>(p4_2);
      
      // Q5 pointers
      auto p5_1_ptr = std::make_shared<PartialMatchState>(p5_1);
      auto p5_2_ptr = std::make_shared<PartialMatchState>(p5_2);
      auto p5_3_ptr = std::make_shared<CompleteMatchState>(p5_3);
      
      // Q6 pointers
      auto p6_1_ptr = std::make_shared<PartialMatchState>(p6_1);
      auto p6_2_ptr = std::make_shared<PartialMatchState>(p6_2);
      auto p6_3_ptr = std::make_shared<CompleteMatchState>(p6_3);
      
      // Q7 pointers
      auto p7_1_ptr = std::make_shared<PartialMatchState>(p7_1);
      auto p7_2_ptr = std::make_shared<PartialMatchState>(p7_2);
      auto p7_3_ptr = std::make_shared<PartialMatchState>(p7_3);
      auto p7_4_ptr = std::make_shared<CompleteMatchState>(p7_4);
      
      // Q8 pointers
      auto p8_1_ptr = std::make_shared<PartialMatchState>(p8_1);
      auto p8_2_ptr = std::make_shared<PartialMatchState>(p8_2);
      auto p8_3_ptr = std::make_shared<CompleteMatchState>(p8_3);
      
      // Q9 pointers
      auto p9_1_ptr = std::make_shared<PartialMatchState>(p9_1);
      auto p9_2_ptr = std::make_shared<PartialMatchState>(p9_2);
      auto p9_3_ptr = std::make_shared<PartialMatchState>(p9_3);
      auto p9_4_ptr = std::make_shared<CompleteMatchState>(p9_4);
      
      // Q10 pointers
      auto p10_1_ptr = std::make_shared<PartialMatchState>(p10_1);
      auto p10_2_ptr = std::make_shared<PartialMatchState>(p10_2);
      auto p10_3_ptr = std::make_shared<CompleteMatchState>(p10_3);
      
      // Q11 pointers
      auto p11_1_ptr = std::make_shared<PartialMatchState>(p11_1);
      auto p11_2_ptr = std::make_shared<CompleteMatchState>(p11_2);
      
      // Q12 pointers
      auto p12_1_ptr = std::make_shared<PartialMatchState>(p12_1);
      auto p12_2_ptr = std::make_shared<CompleteMatchState>(p12_2);
      
      // Q13 pointers
      auto p13_1_ptr = std::make_shared<PartialMatchState>(p13_1);
      auto p13_2_ptr = std::make_shared<PartialMatchState>(p13_2);
      auto p13_3_ptr = std::make_shared<PartialMatchState>(p13_3);
      auto p13_4_ptr = std::make_shared<CompleteMatchState>(p13_4);
      
      // Q14 pointers
      auto p14_1_ptr = std::make_shared<PartialMatchState>(p14_1);
      auto p14_2_ptr = std::make_shared<PartialMatchState>(p14_2);
      auto p14_3_ptr = std::make_shared<PartialMatchState>(p14_3);
      auto p14_4_ptr = std::make_shared<CompleteMatchState>(p14_4);
      
      // Q15 pointers
      auto p15_1_ptr = std::make_shared<PartialMatchState>(p15_1);
      auto p15_2_ptr = std::make_shared<PartialMatchState>(p15_2);
      auto p15_3_ptr = std::make_shared<CompleteMatchState>(p15_3);
      
      // Q16 pointers
      auto p16_1_ptr = std::make_shared<PartialMatchState>(p16_1);
      auto p16_2_ptr = std::make_shared<PartialMatchState>(p16_2);
      auto p16_3_ptr = std::make_shared<PartialMatchState>(p16_3);
      auto p16_4_ptr = std::make_shared<CompleteMatchState>(p16_4);
      
      // Create weak pointers for edges
      auto p1_1_weak = std::weak_ptr<PartialMatchState>(p1_1_ptr);
      auto p1_2_weak = std::weak_ptr<PartialMatchState>(p1_2_ptr);
      auto p1_3_weak = std::weak_ptr<PartialMatchState>(p1_3_ptr);
      
      auto p2_1_weak = std::weak_ptr<PartialMatchState>(p2_1_ptr);
      auto p2_2_weak = std::weak_ptr<PartialMatchState>(p2_2_ptr);
      auto p2_3_weak = std::weak_ptr<PartialMatchState>(p2_3_ptr);
      
      auto p3_1_weak = std::weak_ptr<PartialMatchState>(p3_1_ptr);
      auto p3_2_weak = std::weak_ptr<PartialMatchState>(p3_2_ptr);
      
      auto p4_1_weak = std::weak_ptr<PartialMatchState>(p4_1_ptr);
      auto p4_2_weak = std::weak_ptr<PartialMatchState>(p4_2_ptr);
      
      auto p5_1_weak = std::weak_ptr<PartialMatchState>(p5_1_ptr);
      auto p5_2_weak = std::weak_ptr<PartialMatchState>(p5_2_ptr);
      auto p5_3_weak = std::weak_ptr<PartialMatchState>(p5_3_ptr);
      
      auto p6_1_weak = std::weak_ptr<PartialMatchState>(p6_1_ptr);
      auto p6_2_weak = std::weak_ptr<PartialMatchState>(p6_2_ptr);
      auto p6_3_weak = std::weak_ptr<PartialMatchState>(p6_3_ptr);
      
      auto p7_1_weak = std::weak_ptr<PartialMatchState>(p7_1_ptr);
      auto p7_2_weak = std::weak_ptr<PartialMatchState>(p7_2_ptr);
      auto p7_3_weak = std::weak_ptr<PartialMatchState>(p7_3_ptr);
      auto p7_4_weak = std::weak_ptr<PartialMatchState>(p7_4_ptr);
      
      auto p8_1_weak = std::weak_ptr<PartialMatchState>(p8_1_ptr);
      auto p8_2_weak = std::weak_ptr<PartialMatchState>(p8_2_ptr);
      auto p8_3_weak = std::weak_ptr<PartialMatchState>(p8_3_ptr);
      
      auto p9_1_weak = std::weak_ptr<PartialMatchState>(p9_1_ptr);
      auto p9_2_weak = std::weak_ptr<PartialMatchState>(p9_2_ptr);
      auto p9_3_weak = std::weak_ptr<PartialMatchState>(p9_3_ptr);
      auto p9_4_weak = std::weak_ptr<PartialMatchState>(p9_4_ptr);
      
      auto p10_1_weak = std::weak_ptr<PartialMatchState>(p10_1_ptr);
      auto p10_2_weak = std::weak_ptr<PartialMatchState>(p10_2_ptr);
      auto p10_3_weak = std::weak_ptr<PartialMatchState>(p10_3_ptr);
      
      auto p11_1_weak = std::weak_ptr<PartialMatchState>(p11_1_ptr);
      auto p11_2_weak = std::weak_ptr<PartialMatchState>(p11_2_ptr);
      
      auto p12_1_weak = std::weak_ptr<PartialMatchState>(p12_1_ptr);
      auto p12_2_weak = std::weak_ptr<PartialMatchState>(p12_2_ptr);
      
      auto p13_1_weak = std::weak_ptr<PartialMatchState>(p13_1_ptr);
      auto p13_2_weak = std::weak_ptr<PartialMatchState>(p13_2_ptr);
      auto p13_3_weak = std::weak_ptr<PartialMatchState>(p13_3_ptr);
      auto p13_4_weak = std::weak_ptr<PartialMatchState>(p13_4_ptr);
      
      auto p14_1_weak = std::weak_ptr<PartialMatchState>(p14_1_ptr);
      auto p14_2_weak = std::weak_ptr<PartialMatchState>(p14_2_ptr);
      auto p14_3_weak = std::weak_ptr<PartialMatchState>(p14_3_ptr);
      auto p14_4_weak = std::weak_ptr<PartialMatchState>(p14_4_ptr);
      
      auto p15_1_weak = std::weak_ptr<PartialMatchState>(p15_1_ptr);
      auto p15_2_weak = std::weak_ptr<PartialMatchState>(p15_2_ptr);
      auto p15_3_weak = std::weak_ptr<PartialMatchState>(p15_3_ptr);
      
      auto p16_1_weak = std::weak_ptr<PartialMatchState>(p16_1_ptr);
      auto p16_2_weak = std::weak_ptr<PartialMatchState>(p16_2_ptr);
      auto p16_3_weak = std::weak_ptr<PartialMatchState>(p16_3_ptr);
      auto p16_4_weak = std::weak_ptr<PartialMatchState>(p16_4_ptr);
      
      // Create query plan
      QueryPlan query_plan;
      query_plan.AddStartingState(p0_ptr);
      
      // Add all states to query plan
      query_plan.AddState(p1_1_ptr);
      query_plan.AddState(p1_2_ptr);
      query_plan.AddState(p1_3_ptr);
      query_plan.AddState(p2_1_ptr);
      query_plan.AddState(p2_2_ptr);
      query_plan.AddState(p2_3_ptr);
      query_plan.AddState(p3_1_ptr);
      query_plan.AddState(p3_2_ptr);
      query_plan.AddState(p4_1_ptr);
      query_plan.AddState(p4_2_ptr);
      query_plan.AddState(p5_1_ptr);
      query_plan.AddState(p5_2_ptr);
      query_plan.AddState(p5_3_ptr);
      query_plan.AddState(p6_1_ptr);
      query_plan.AddState(p6_2_ptr);
      query_plan.AddState(p6_3_ptr);
      query_plan.AddState(p7_1_ptr);
      query_plan.AddState(p7_2_ptr);
      query_plan.AddState(p7_3_ptr);
      query_plan.AddState(p7_4_ptr);
      query_plan.AddState(p8_1_ptr);
      query_plan.AddState(p8_2_ptr);
      query_plan.AddState(p8_3_ptr);
      query_plan.AddState(p9_1_ptr);
      query_plan.AddState(p9_2_ptr);
      query_plan.AddState(p9_3_ptr);
      query_plan.AddState(p9_4_ptr);
      query_plan.AddState(p10_1_ptr);
      query_plan.AddState(p10_2_ptr);
      query_plan.AddState(p10_3_ptr);
      query_plan.AddState(p11_1_ptr);
      query_plan.AddState(p11_2_ptr);
      query_plan.AddState(p12_1_ptr);
      query_plan.AddState(p12_2_ptr);
      query_plan.AddState(p13_1_ptr);
      query_plan.AddState(p13_2_ptr);
      query_plan.AddState(p13_3_ptr);
      query_plan.AddState(p13_4_ptr);
      query_plan.AddState(p14_1_ptr);
      query_plan.AddState(p14_2_ptr);
      query_plan.AddState(p14_3_ptr);
      query_plan.AddState(p14_4_ptr);
      query_plan.AddState(p15_1_ptr);
      query_plan.AddState(p15_2_ptr);
      query_plan.AddState(p15_3_ptr);
      query_plan.AddState(p16_1_ptr);
      query_plan.AddState(p16_2_ptr);
      query_plan.AddState(p16_3_ptr);
      query_plan.AddState(p16_4_ptr);
      
      // Define edges for all queries
      
      // Q1: SEQ(A, B, C) - p0 -> p1_1 -> p1_2 -> p1_3
      QueryPlan::AddEdge(p0_ptr, p1_1_weak, et_a);
      QueryPlan::AddEdge(p1_1_ptr, p1_2_weak, et_b);
      QueryPlan::AddEdge(p1_2_ptr, p1_3_weak, et_c);
      
      // Q2: SEQ(A, B, E) - p0 -> p2_1 -> p2_2 -> p2_3
      QueryPlan::AddEdge(p0_ptr, p2_1_weak, et_a);
      QueryPlan::AddEdge(p2_1_ptr, p2_2_weak, et_b);
      QueryPlan::AddEdge(p2_2_ptr, p2_3_weak, et_e);
      
      // Q3: SEQ(A, !E, C) - p0 -> p3_1 -> p3_2 (skipping E)
      QueryPlan::AddEdge(p0_ptr, p3_1_weak, et_a);
      QueryPlan::AddEdge(p3_1_ptr, p3_2_weak, et_c);
      
      // Q4: SEQ(A, !E, D) - p0 -> p4_1 -> p4_2 (skipping E)
      QueryPlan::AddEdge(p0_ptr, p4_1_weak, et_a);
      QueryPlan::AddEdge(p4_1_ptr, p4_2_weak, et_d);
      
      // Q5: SEQ(A, B+, C) - p0 -> p5_1 -> p5_2 -> p5_3 (with B+ self-loop)
      QueryPlan::AddEdge(p0_ptr, p5_1_weak, et_a);
      QueryPlan::AddEdge(p5_1_ptr, p5_2_weak, et_b);
      QueryPlan::AddEdge(p5_2_ptr, p5_2_weak, et_b); // Self-loop for B+
      QueryPlan::AddEdge(p5_2_ptr, p5_3_weak, et_c);
      
      // Q6: SEQ(A, B+, D) - p0 -> p6_1 -> p6_2 -> p6_3 (with B+ self-loop)
      QueryPlan::AddEdge(p0_ptr, p6_1_weak, et_a);
      QueryPlan::AddEdge(p6_1_ptr, p6_2_weak, et_b);
      QueryPlan::AddEdge(p6_2_ptr, p6_2_weak, et_b); // Self-loop for B+
      QueryPlan::AddEdge(p6_2_ptr, p6_3_weak, et_d);
      
      // Q7: SEQ(A, B, B, C) - p0 -> p7_1 -> p7_2 -> p7_3 -> p7_4
      QueryPlan::AddEdge(p0_ptr, p7_1_weak, et_a);
      QueryPlan::AddEdge(p7_1_ptr, p7_2_weak, et_b);
      QueryPlan::AddEdge(p7_2_ptr, p7_3_weak, et_b);
      QueryPlan::AddEdge(p7_3_ptr, p7_4_weak, et_c);
      
      // Q8: SEQ(A, C, D) - p0 -> p8_1 -> p8_2 -> p8_3
      QueryPlan::AddEdge(p0_ptr, p8_1_weak, et_a);
      QueryPlan::AddEdge(p8_1_ptr, p8_2_weak, et_c);
      QueryPlan::AddEdge(p8_2_ptr, p8_3_weak, et_d);
      
      // Q9: SEQ(A, B, C, D) - p0 -> p9_1 -> p9_2 -> p9_3 -> p9_4
      QueryPlan::AddEdge(p0_ptr, p9_1_weak, et_a);
      QueryPlan::AddEdge(p9_1_ptr, p9_2_weak, et_b);
      QueryPlan::AddEdge(p9_2_ptr, p9_3_weak, et_c);
      QueryPlan::AddEdge(p9_3_ptr, p9_4_weak, et_d);
      
      // Q10: SEQ(A, B+, E) - p0 -> p10_1 -> p10_2 -> p10_3 (with B+ self-loop)
      QueryPlan::AddEdge(p0_ptr, p10_1_weak, et_a);
      QueryPlan::AddEdge(p10_1_ptr, p10_2_weak, et_b);
      QueryPlan::AddEdge(p10_2_ptr, p10_2_weak, et_b); // Self-loop for B+
      QueryPlan::AddEdge(p10_2_ptr, p10_3_weak, et_e);
      
      // Q11: SEQ(A, !B, C) - p0 -> p11_1 -> p11_2 (skipping B)
      QueryPlan::AddEdge(p0_ptr, p11_1_weak, et_a);
      QueryPlan::AddEdge(p11_1_ptr, p11_2_weak, et_c);
      
      // Q12: SEQ(A, !C, D) - p0 -> p12_1 -> p12_2 (skipping C)
      QueryPlan::AddEdge(p0_ptr, p12_1_weak, et_a);
      QueryPlan::AddEdge(p12_1_ptr, p12_2_weak, et_d);
      
      // Q13: SEQ(A, B, D, E) - p0 -> p13_1 -> p13_2 -> p13_3 -> p13_4
      QueryPlan::AddEdge(p0_ptr, p13_1_weak, et_a);
      QueryPlan::AddEdge(p13_1_ptr, p13_2_weak, et_b);
      QueryPlan::AddEdge(p13_2_ptr, p13_3_weak, et_d);
      QueryPlan::AddEdge(p13_3_ptr, p13_4_weak, et_e);
      
      // Q14: SEQ(A, C, B, D) - p0 -> p14_1 -> p14_2 -> p14_3 -> p14_4
      QueryPlan::AddEdge(p0_ptr, p14_1_weak, et_a);
      QueryPlan::AddEdge(p14_1_ptr, p14_2_weak, et_c);
      QueryPlan::AddEdge(p14_2_ptr, p14_3_weak, et_b);
      QueryPlan::AddEdge(p14_3_ptr, p14_4_weak, et_d);
      
      // Q15: SEQ(A, !B, D, E) - p0 -> p15_1 -> p15_2 -> p15_3 (skipping B)
      QueryPlan::AddEdge(p0_ptr, p15_1_weak, et_a);
      QueryPlan::AddEdge(p15_1_ptr, p15_2_weak, et_d);
      QueryPlan::AddEdge(p15_2_ptr, p15_3_weak, et_e);
      
      // Q16: SEQ(A, B+, C, D) - p0 -> p16_1 -> p16_2 -> p16_3 -> p16_4 (with B+ self-loop)
      QueryPlan::AddEdge(p0_ptr, p16_1_weak, et_a);
      QueryPlan::AddEdge(p16_1_ptr, p16_2_weak, et_b);
      QueryPlan::AddEdge(p16_2_ptr, p16_2_weak, et_b); // Self-loop for B+
      QueryPlan::AddEdge(p16_2_ptr, p16_3_weak, et_c);
      QueryPlan::AddEdge(p16_3_ptr, p16_4_weak, et_d);
      
      // Time begins
      auto buffer = EventBuffer(p0_ptr);
      buffer.q0_time_window_ = q0_window;
      buffer.q1_time_window_ = q1_window;
      buffer.q0_bound_ = q0_bound;
      buffer.q1_bound_ = q1_bound;
      buffer.shedding_ratio_ = shedding_ratio;
      
      while (!raw_event_queue_.empty()) {
        auto new_event = raw_event_queue_.front();
        new_event.out_queue_time_ = buffer.CurrentTime();
        if (buffer.EventShedding(shed_method, new_event, q0, q1)) {
          raw_event_queue_.pop();
          continue;
        }
        
        if (plan_choice == 0) {
          buffer.ProcessNewEventNonSharedSet1(new_event);
        } else {
          buffer.ProcessNewEventSet1Dynamic(new_event, "C", "E");
        }
        
        raw_event_queue_.pop();
        EventBuffer::max_buffer_size = buffer.all_pms_.size() > EventBuffer::max_buffer_size
                                           ? buffer.all_pms_.size()
                                           : EventBuffer::max_buffer_size;
        
        buffer.LoadSheddingMonitor(shed_method, plan_choice);
      }
      EventBuffer::throughput = 1000000 * static_cast<float>(100000) / static_cast<float>(buffer.CurrentTime());
    }
  }
}
auto Experiment::ReadEventsfromFileQ0(const std::string &file_path) -> void {
  std::ifstream ifs;
  ifs.open(file_path.c_str());
  std::string line;
  std::vector<std::string> attrs;
  while (getline(ifs, line)) {
    std::stringstream line_stream(line);
    std::string cell;
    // std::cout << "Parsing " << line << std::endl;
    while (getline(line_stream, cell, ',')) {
      // std::cout << "the cell is " << cell;
      attrs.push_back(cell);
    }
    // std::cout << "\n";
    auto new_event = Event(attrs[0],
                           {Attribute("id", std::stoull(attrs[2])), Attribute("x", std::stoull(attrs[3])),
                            Attribute("y", std::stoull(attrs[4])), Attribute("v", std::stoull(attrs[5]))},
                           std::stoull(attrs[1]));
    raw_event_queue_.push(new_event);
    attrs.clear();
  }
  // std::cout << "queue size: " << raw_event_queue_.size() << std::endl;
}
auto Experiment::PrintResult() const -> void {
  if (q0_bound_ >= 1844674407370955161ULL) {
    // No shedding
    std::cout << EventBuffer::num_complete_matches[0] << "," << EventBuffer::num_complete_matches[1] << ","
              << EventBuffer::throughput << std::endl;
    return;
  }
  std::string shed_method_name;

  switch (shed_method_) {
    case 0:
      shed_method_name = "Cost-Model";
      break;
    case 1:
      shed_method_name = "Random State";
      break;
    case 2:
      shed_method_name = "Fractional Load Shedding";
      break;
    case 3:
      shed_method_name = "DARLING";
      break;
    case 4:
      shed_method_name = "ICDE'20";
      break;
    case 5:
      shed_method_name = "gspice";
      break;
    case 6:
      shed_method_name = "hspice";
      break;
    default:
      break;
  }
  auto shed_ratio_str = std::to_string(shedding_ratio_) + "%";
  std::string output_str = std::to_string(plan_choice_) + "_" + std::to_string(query_choice_) + "_" +
                           std::to_string(q0_bound_) + "_" + std::to_string(q1_bound_) + "_" +
                           std::to_string(shed_method_) + "_" + std::to_string(q0_window_) + "_" +
                           std::to_string(q1_window_) + "_" + shed_ratio_str + ".csv";
  // EventBuffer::DumpLatencyBooking("../../latency_booking/" + output_str);

  long double pm_shedding_ratio =
      static_cast<long double>(LoadSheddingManager::num_shed_partial_match) / EventBuffer::total_pm_num;

  std::cout << plan_choice_ << "," << query_choice_ << "," << q0_bound_ << "," << q1_bound_ << "," << shed_method_name
            << "," << q0_window_ << "," << q1_window_ << "," << shed_ratio_str << ","
            << EventBuffer::num_complete_matches[0] << "," << EventBuffer::num_complete_matches[1] << ","
            << pm_shedding_ratio << "," << EventBuffer::event_shedding_num << "," << EventBuffer::throughput;
}
}  // namespace cep
