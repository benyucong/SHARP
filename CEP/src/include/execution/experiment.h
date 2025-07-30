#pragma once

#include <chrono>  // NOLINT [build/c++11]
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/event.h"
#include "query/pattern.h"
#include "query/query.h"
#include "shedder/shedder.h"
#include "workload/workload.h"

namespace cep {

class EventBuffer {
 public:
  //  std::vector<Event> raw_event_vector_;
  std::vector<std::shared_ptr<EventVector>> all_pms_;
  std::unordered_map<int, RawEventSegment> shared_segment_by_len_;
  std::vector<std::shared_ptr<RawEventSegment>> all_shared_segments_;
  std::shared_ptr<RawEventSegment> connection_segment_{nullptr};

  //  std::function<void(long double, long double)> load_shedding_method_;
  std::vector<long double> max_latency_violation_extends_;
  static std::vector<long double> latencies;

  bool load_shedding_switch_{false};
  LoadSheddingManager load_shedding_manager_;

  // Init an empty buffer
  explicit EventBuffer(std::shared_ptr<PartialMatchState> pm_ptr, std::shared_ptr<PartialMatchState> pm_ptr_ = nullptr);
  auto ProcessNewEventShared(const Event &new_event) -> void;
  auto ProcessNewEventNonShared(const Event &new_event) -> void;
  auto ProcessNewEventDynamicShared(const Event &new_event) -> void;
  auto ProcessNewEventSet1Shared(const Event &new_event, const std::string &l1, const std::string &l2) -> void;
  auto ProcessNewEventSet1Dynamic(const Event &new_event, const std::string &l1, const std::string &l2) -> void;
  auto ProcessNewEventNonSharedSet1(const Event &new_event) -> void;
  auto GetAllPms() -> std::vector<std::shared_ptr<EventVector>> & { return all_pms_; }
  auto PrintCurrentBuffer() -> void;
  auto PrintPreCurrentBuffer() -> void;
  auto RunLoadShedding(const std::function<void(long double, long double, std::vector<std::shared_ptr<EventVector>> &,
                                                int, int)> &shed_method = LoadSheddingManager::LoadSheddingBimaps,
                       int if_dynamic = 0, int _shedding_ratio_ = 100) -> void;
  auto LoadSheddingMonitor(int shed_method, int plan_choice) -> void;
  auto EventShedding(int shed_method, const Event &event, Query &q0, Query &q1) -> bool;
  static auto UpdateContributionBookings(const std::shared_ptr<EventVector> &pm, query_t query_id) -> void;
  static auto UpdateConsumptionBookings(const std::shared_ptr<EventVector> &pm, int flag) -> void;
  auto DarlingUtility(const cep::Event &new_event, int pos) -> float;
  static std::vector<uint64_t> num_complete_matches;
  static std::vector<uint64_t> num_violates_latencies;
  uint64_t q0_time_window_{25};
  uint64_t q1_time_window_{25};
  time_t q0_bound_{0};
  time_t q1_bound_{0};
  int shedding_ratio_ = 100;
  float darling_threshold_ = 0.8e-5;
  static std::map<uint64_t, uint64_t> latency_booking;
  static float throughput;
  static uint64_t event_shedding_num;
  static uint64_t max_buffer_size;
  static uint64_t total_pm_num;
  static auto DumpLatencyBooking(const std::string &file) -> void {
    std::ofstream out_file;
    out_file.open(file.c_str());
    for (const auto &[latency, count] : latency_booking) {
      out_file << latency << "," << count << "\n";
    }
    out_file << "max buffer size: " << max_buffer_size << "\n";
  }

  std::chrono::time_point<std::chrono::high_resolution_clock> starting_time_;
  [[nodiscard]] auto CurrentTime() const -> uint64_t;
};

class Experiment {
 public:
  Workload workload_;
  std::vector<EventType> all_event_types_;
  std::queue<Event> raw_event_queue_;
  // default constructor
  int plan_choice_{0};
  int query_choice_{0};
  time_t q0_bound_{0};
  time_t q1_bound_{0};
  int shed_method_{0};
  uint64_t q0_window_{0};
  uint64_t q1_window_{0};
  int shedding_ratio_{100};

  Experiment(int plan_choice, int query_choice, time_t q0_bound, time_t q1_bound, int shed_method, uint64_t q0_window,
             uint64_t q1_window, int shedding_ratio);
  auto GenerateSyntheticStreamDefault(std::queue<Event> &raw_event_queue, std::vector<EventType> &all_event_types,
                                      int64_t event_number) -> void;
  auto GenerateSyntheticStreamSet1(std::queue<Event> &raw_event_queue, std::vector<EventType> &all_event_types,
                                   int64_t event_number) -> void;
  auto GenerateSyntheticStreamSet2(std::queue<Event> &raw_event_queue, std::vector<EventType> &all_event_types,
                                   int64_t event_number) -> void;
  auto ReadEventsfromFileQ0(const std::string &file_path) -> void;
  auto PrintResult() const -> void;
};
}  // namespace cep
