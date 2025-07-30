#pragma once
#include <boost/bimap.hpp>
#include <boost/bimap/multiset_of.hpp>
#include <boost/bimap/set_of.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <memory>
#include <queue>
#include <set>
#include <utility>
#include <vector>
#include "query/pattern.h"
#include "query/query.h"

namespace cep {

struct CompareEventVectorPtr {
  auto operator()(const std::shared_ptr<EventVector> &lhs, const std::shared_ptr<EventVector> &rhs) const -> bool {
    return lhs->GetSize() < rhs->GetSize();
  }
};

using contribution_pair = std::pair<long double, long double>;
using consumption_pair = std::pair<long double, long double>;
using shedding_pair = std::pair<std::shared_ptr<EventVector>, long double>;
using pm_quality_container_type_ = boost::bimap<boost::bimaps::unordered_set_of<std::shared_ptr<EventVector>>,
                                                boost::bimaps::multiset_of<long double, std::less<>>>;
/**
 * @brief Cluster is used to partition Partial matches according to their contribution
 * and consumption(after quantization), which have the same set of candidate queries, a.k.a. in the same bucket.
 */
class Cluster {
 public:
  int partial_match_number_{0};
  // values and weights of 2 queries
  std::vector<long double> values_;
  std::vector<long double> weights_;
  long double v_w_ratio_{0};

  int size{0};
  static std::queue<std::shared_ptr<EventVector>> event_vectors;

  // lazy Shed, mark the EventVector and RawEventSegment to be invalid.
  static auto ShedByNum(int shedding_number) -> void;
  static auto ShedOne() -> void;
  Cluster() = default;
  Cluster(std::vector<long double> vals, std::vector<long double> weights);
  static auto AddNewPartialMatch(const std::shared_ptr<EventVector> &new_pm_ptr) -> void {
    event_vectors.push(new_pm_ptr);
  }
  auto SetRatio(long double w0, long double w1);
};

/**
 * @brief Bucket partitions Partial Matches according to their candidate queries
 *
 */

// using shedding_pair = std::pair<std::shared_ptr<EventVector>, long double>;
struct Comparator {
  auto operator()(const shedding_pair &lhs, const shedding_pair &rhs) const -> bool {
    // Compare based on the second element (long double)
    return lhs.second < rhs.second;
  }
};

class Bucket {
 public:
  int bucket_id_{0};
  std::set<query_t> candidate_queries_;
  int cluster_number_per_bucket_{};
  std::vector<std::shared_ptr<Cluster>> cluster_vectors_;
  pm_quality_container_type_ pm_quality_bimap_{};
  Bucket() = default;
  Bucket(int id, std::set<query_t> query_ids_, int cluster_size = 1);
  // Sort the clusters by their contribution/consumption ratios
  auto SortClusters() -> void;
  auto GetLowestRatioCluster() -> std::shared_ptr<Cluster>;
  auto ShedLowestRatioPM() -> void;
  auto Shed(int shed_number) -> void;
};

// Track the contribution and consumption of each attr vector
class LoadSheddingManager {
 public:
  // bucket id: 0: q1; 1: q1 and q2; 2 q2; 3: none
  static std::vector<Bucket> all_buckets;
  static uint64_t num_shed_partial_match;
  explicit LoadSheddingManager(int bucket_num = 3);
  static long double total_consumption_q0;
  static long double total_consumption_q1;
  static std::map<std::vector<attr_t>, contribution_pair> contribution_booking;
  static std::map<std::vector<attr_t>, consumption_pair> consumption_booking;
  // flag = 0 => q1 violates latency bound
  // flag = 1 => q2 violates latency bound
  // flag = 2 => q1 and q2 both violate latency bound
  static auto LoadSheddingGreedy(long double extend1, long double extend2, int8_t flag);
  static auto LoadSheddingBimaps(long double extend1, long double extend2,
                                 std::vector<std::shared_ptr<EventVector>> &all, int invalid_op = 0,
                                 int ratio = 100) -> void;
  static auto LoadSheddingBimapsRandom(long double extend1, long double extend2,
                                       std::vector<std::shared_ptr<EventVector>> &all_pms, int invalid_op = 0,
                                       int ratio = 100) -> void;
  static auto LoadSheddingFraction(long double extend1, long double extend2,
                                   std::vector<std::shared_ptr<EventVector>> &all_pms, int invalid_op = 0,
                                   int ratio = 100) -> void;
  static auto LoadSheddingDarling(long double extend1, long double extend2,
                                  std::vector<std::shared_ptr<EventVector>> &all_pms, int invalid_op = 0,
                                  int ratio = 100) -> void;
  static auto LoadSheddingICDE(long double extend1, long double extend2,
                               std::vector<std::shared_ptr<EventVector>> &all_pms, int invalid_op = 0,
                               int ratio = 100) -> void;
  static auto LoadSheddingBookingDeletePM(const std::shared_ptr<EventVector> &pm_ptr) -> void;
  static auto UpdatePartialMatchPriority2Shed(const std::shared_ptr<EventVector> &pm_ptr) -> void;
  static auto CheckEmpty() -> bool {
    if (all_buckets.empty()) {
      return true;
    }
    bool ans = true;
    for (const auto &bucket : all_buckets) {
      ans = ans && bucket.pm_quality_bimap_.empty();
    }
    return ans;
  }
};
}  // namespace cep
