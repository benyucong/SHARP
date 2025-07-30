#include "shedder/shedder.h"
#include <algorithm>
#include <boost/bimap/support/lambda.hpp>
#include <cmath>
#include <cstdint>
#include <random>
#include <typeinfo>
#include <utility>

namespace cep {
// declaration of static elements
std::queue<std::shared_ptr<EventVector>> Cluster::event_vectors;
long double LoadSheddingManager::total_consumption_q0;
long double LoadSheddingManager::total_consumption_q1;
uint64_t LoadSheddingManager::num_shed_partial_match;
std::vector<Bucket> LoadSheddingManager::all_buckets;
std::map<std::vector<attr_t>, contribution_pair> LoadSheddingManager::contribution_booking;
std::map<std::vector<attr_t>, consumption_pair> LoadSheddingManager::consumption_booking;

Cluster::Cluster(std::vector<long double> vals, std::vector<long double> weights)
    : values_(std::move(vals)), weights_(std::move(weights)) {}

auto Cluster::SetRatio(long double w0, long double w1) {
  v_w_ratio_ = w0 * (values_[0] / weights_[0]) + w1 * (values_[1] / weights_[1]);
}

auto Cluster::ShedOne() -> void {
  if (!event_vectors.empty()) {
    auto event_vector_ptr = event_vectors.front();
    event_vector_ptr->SetInvalidity();
    event_vectors.pop();
  }
}

auto Cluster::ShedByNum(int shedding_number) -> void {
  int num = 0;
  while (num < shedding_number && !event_vectors.empty()) {
    auto event_vector_ptr = event_vectors.front();
    event_vector_ptr->SetInvalidity();
    event_vectors.pop();
    num++;
  }
}

auto Bucket::SortClusters() -> void {
  std::sort(cluster_vectors_.begin(), cluster_vectors_.end(),
            [](const std::shared_ptr<Cluster> &l, const std::shared_ptr<Cluster> &r) {
              return l->v_w_ratio_ < r->v_w_ratio_;
            });
}
Bucket::Bucket(int id, std::set<query_t> query_ids_, int cluster_size)
    : bucket_id_(id), candidate_queries_(std::move(query_ids_)), cluster_number_per_bucket_(cluster_size) {}

auto Bucket::GetLowestRatioCluster() -> std::shared_ptr<Cluster> {
  // after sort
  for (auto &cluster_vector : cluster_vectors_) {
    if (!cluster_vector->event_vectors.empty()) {
      return cluster_vector;
    }
  }
  return nullptr;
}

auto Bucket::ShedLowestRatioPM() -> void {
  // after sort
  for (auto &cluster_vector : cluster_vectors_) {
    if (!cluster_vector->event_vectors.empty()) {
      cluster_vector->ShedOne();
    }
  }
}

auto Bucket::Shed(int shed_number) -> void {
  int count = 0;
  auto it = pm_quality_bimap_.right.begin();
  while (count < shed_number && it != pm_quality_bimap_.right.end()) {
    // Set Invalidity of the PM having the greatest value
    it->second->SetInvalidity();
    pm_quality_bimap_.right.erase(it++);
    ++count;
  }
}

LoadSheddingManager::LoadSheddingManager(int bucket_num) {
  total_consumption_q0 = 0;
  total_consumption_q1 = 0;
  num_shed_partial_match = 0;

  for (int i = 0; i < bucket_num; ++i) {
    auto new_bucket = Bucket();
    all_buckets.push_back(new_bucket);
  }
  //  int cluster_num = 5;
  //  for (int id = 0; id < bucket_num; ++id) {
  //    auto new_cluster = Cluster();
  //    // to do
  //  }
}
auto LoadSheddingManager::LoadSheddingGreedy(long double extend1, long double extend2, int8_t flag) {
  auto expected_total_consumption_q1 = total_consumption_q0 * (1 - extend1);
  auto expected_total_consumption_q2 = total_consumption_q1 * (1 - extend2);
  if (flag == 0) {
    // bucket[0] and bucket[1] need to Shed
    while (total_consumption_q0 > expected_total_consumption_q1) {
      auto cluster_0 = all_buckets[0].GetLowestRatioCluster();
      auto cluster_1 = all_buckets[1].GetLowestRatioCluster();
      if (cluster_0->v_w_ratio_ < cluster_1->v_w_ratio_) {
        total_consumption_q0 -= cluster_0->weights_[0];
        cluster_0->ShedOne();
      } else {
        total_consumption_q0 -= cluster_1->weights_[0];
        cluster_1->ShedOne();
      }
    }
  } else if (flag == 1) {
    // bucket[1] and bucket[2] need to Shed
    while (total_consumption_q1 > expected_total_consumption_q2) {
      auto cluster1 = all_buckets[1].GetLowestRatioCluster();
      auto cluster2 = all_buckets[2].GetLowestRatioCluster();
      if (cluster1->v_w_ratio_ < cluster2->v_w_ratio_) {
        total_consumption_q1 -= cluster1->weights_[1];
        cluster1->ShedOne();
      } else {
        total_consumption_q1 -= cluster2->weights_[1];
        cluster2->ShedOne();
      }
    }
  } else {
    // all bucket need to Shed
    while (total_consumption_q0 > expected_total_consumption_q1 ||
           total_consumption_q1 > expected_total_consumption_q2) {
      auto cluster0 = all_buckets[0].GetLowestRatioCluster();
      auto cluster1 = all_buckets[1].GetLowestRatioCluster();
      auto cluster2 = all_buckets[2].GetLowestRatioCluster();
      if (cluster0->v_w_ratio_ <= cluster1->v_w_ratio_ && cluster0->v_w_ratio_ <= cluster2->v_w_ratio_) {
        total_consumption_q0 -= cluster0->weights_[0];
        cluster0->ShedOne();
      } else if (cluster1->v_w_ratio_ <= cluster0->v_w_ratio_ && cluster1->v_w_ratio_ <= cluster2->v_w_ratio_) {
        total_consumption_q1 -= cluster1->weights_[1];
        cluster1->ShedOne();
      } else {
        total_consumption_q0 -= cluster2->weights_[0];
        total_consumption_q1 -= cluster2->weights_[1];
        cluster2->ShedOne();
      }
    }
  }
}

auto LoadSheddingManager::UpdatePartialMatchPriority2Shed(const std::shared_ptr<EventVector> &pm_ptr) -> void {
  int8_t bucket_id;
  if (pm_ptr->candidate_queries_.size() > 1) {
    bucket_id = 2;
  } else {
    bucket_id = *(pm_ptr->candidate_queries_.begin());
  }
  auto attr_vector = pm_ptr->GetAttrVector("id");
  auto sum_contribution = contribution_booking[attr_vector].first + contribution_booking[attr_vector].second;
  auto sum_consumption = consumption_booking[attr_vector].first + consumption_booking[attr_vector].second;
  auto v_w_ratio = sum_contribution / sum_consumption;

  // update partial match - quality pair
  auto it = all_buckets[bucket_id].pm_quality_bimap_.left.find(pm_ptr);
  if (it != all_buckets[bucket_id].pm_quality_bimap_.left.end()) {
    //    all_buckets[bucket_id].pm_quality_bimap_.left.modify_data(it, boost::bimaps::_data = v_w_ratio);
    all_buckets[bucket_id].pm_quality_bimap_.left.erase(it);
  }
  all_buckets[bucket_id].pm_quality_bimap_.insert(pm_quality_container_type_::value_type(pm_ptr, v_w_ratio));
}

auto LoadSheddingManager::LoadSheddingBimaps(long double extend1, long double extend2,
                                             std::vector<std::shared_ptr<EventVector>> &all, int invalid_op, int ratio)
    -> void {
  std::vector<int> bucket_ids;
  if (extend1 > 5e-2F) {
    bucket_ids.push_back(0);
  }
  if (extend2 > 5e-2F) {
    bucket_ids.push_back(1);
  }

  auto shed_quota_q0 = static_cast<uint64_t>(all_buckets[0].pm_quality_bimap_.size() * extend1);
  auto shed_quota_q1 = static_cast<uint64_t>(all_buckets[1].pm_quality_bimap_.size() * extend2);

  shed_quota_q0 = shed_quota_q0 * ratio / 100;
  shed_quota_q1 = shed_quota_q1 * ratio / 100;

  uint64_t shed_q0 = 0;
  uint64_t shed_q1 = 0;
  if (bucket_ids.size() == 1) {
    // only one query violates bound
    int bucket_id = bucket_ids[0];
    bucket_id == 0 ? shed_quota_q1 = 0 : shed_quota_q0 = 0;
    auto it = all_buckets[bucket_id].pm_quality_bimap_.right.begin();
    auto it2 = all_buckets[2].pm_quality_bimap_.right.begin();
    while ((shed_q0 < shed_quota_q0 || shed_q1 < shed_quota_q1)) {
      if (it2 != all_buckets[2].pm_quality_bimap_.right.end() &&
          (it == all_buckets[bucket_id].pm_quality_bimap_.right.end() || it2->first <= it->first)) {
        invalid_op == 0 ? it2->second->SetInvalidity() : it2->second->DynamicSetInvalidity();
        all_buckets[2].pm_quality_bimap_.right.erase(it2++);
        shed_q0++;
        shed_q1++;
      } else if (it != all_buckets[bucket_id].pm_quality_bimap_.right.end()) {
        invalid_op == 0 ? it->second->SetInvalidity() : it->second->DynamicSetInvalidity();
        all_buckets[bucket_id].pm_quality_bimap_.right.erase(it++);
        bucket_id == 0 ? shed_q0++ : shed_q1++;
      } else {
        break;
      }
      num_shed_partial_match++;
    }
  } else {
    // two queries violates at the same time; use 3-pointer
    auto it0 = all_buckets[0].pm_quality_bimap_.right.begin();
    auto it1 = all_buckets[1].pm_quality_bimap_.right.begin();
    auto it2 = all_buckets[2].pm_quality_bimap_.right.begin();
    while ((shed_q0 < shed_quota_q0 || shed_q1 < shed_quota_q1)) {
      if ((it0 != all_buckets[0].pm_quality_bimap_.right.end()) &&
          (it1 == all_buckets[1].pm_quality_bimap_.right.end() || it0->first <= it1->first) &&
          (it2 == all_buckets[2].pm_quality_bimap_.right.end() || it0->first <= it2->first) &&
          it0 != all_buckets[0].pm_quality_bimap_.right.end()) {
        invalid_op == 0 ? it0->second->SetInvalidity() : it0->second->DynamicSetInvalidity();
        all_buckets[0].pm_quality_bimap_.right.erase(it0++);
        shed_q0++;
      } else if ((it0 == all_buckets[0].pm_quality_bimap_.right.end() || it1->first <= it0->first) &&
                 (it2 == all_buckets[2].pm_quality_bimap_.right.end() || it1->first <= it2->first) &&
                 it1 != all_buckets[1].pm_quality_bimap_.right.end()) {
        invalid_op == 0 ? it1->second->SetInvalidity() : it1->second->DynamicSetInvalidity();
        all_buckets[1].pm_quality_bimap_.right.erase(it1++);
        shed_q1++;
      } else if (it2 != all_buckets[2].pm_quality_bimap_.right.end() && it2->second->GetSize() > 0 &&
                 it2 != all_buckets[2].pm_quality_bimap_.right.end()) {
        // total_consumption_q0 -= consumption_booking[it2->second->GetAttrVector("id")].first;
        // total_consumption_q1 -= consumption_booking[it2->second->GetAttrVector("id")].second;

        invalid_op == 0 ? it2->second->SetInvalidity() : it2->second->DynamicSetInvalidity();
        all_buckets[2].pm_quality_bimap_.right.erase(it2++);
        shed_q0++;
        shed_q1++;
      } else {
        break;
      }
      num_shed_partial_match++;
    }
  }
}

auto LoadSheddingManager::LoadSheddingBimapsRandom(long double extend1, long double extend2,
                                                   std::vector<std::shared_ptr<EventVector>> &all_pms, int invalid_op,
                                                   int ratio) -> void {
  auto shed_count = static_cast<uint64_t>(all_pms.size() * (extend1 + extend2));
  shed_count = shed_count * 1.5 * ratio / 100;

  std::random_device rd;
  std::mt19937_64 rng(rd());

  uint64_t shed_num = 0;

  while (shed_num < shed_count && all_pms.size() > 2) {
    auto it = all_pms.begin();
    std::uniform_int_distribution<uint64_t> dist_idx(2, static_cast<int>(all_pms.size()) - 1);
    std::advance(it, dist_idx(rng));
    invalid_op == 0 ? (*it)->SetInvalidity() : (*it)->DynamicSetInvalidity();
    all_pms.erase(it);
    num_shed_partial_match++;
    shed_num++;
    // for (const auto &pm : all_pms) {
    //   if (pm->GetSize() == 0) {
    //     continue;
    //   }
    //   if (pm->valid_) {
    //     LoadSheddingManager::UpdatePartialMatchPriority2Shed(pm);
    //   }
    // }
  }
}
auto LoadSheddingManager::LoadSheddingFraction(long double extend1, long double extend2,
                                               std::vector<std::shared_ptr<EventVector>> &all_pms, int invalid_op,
                                               int ratio) -> void {
  auto shed_quota_q0 = static_cast<uint64_t>(all_buckets[0].pm_quality_bimap_.size() * extend1);
  auto shed_quota_q1 = static_cast<uint64_t>(all_buckets[1].pm_quality_bimap_.size() * extend2);
  shed_quota_q0 = shed_quota_q0 * ratio / 100;
  shed_quota_q1 = shed_quota_q1 * ratio / 100;
  uint64_t shed_count_q0 = 0;
  uint64_t shed_count_q1 = 0;
  std::random_device rd;
  std::mt19937_64 rng(rd());
  while (shed_count_q0 < shed_quota_q0) {
    auto it = all_buckets[0].pm_quality_bimap_.right.begin();
    if (it == all_buckets[0].pm_quality_bimap_.right.end()) {
      break;
    }
    std::uniform_int_distribution<uint64_t> dist_idx(0, static_cast<int>(all_buckets[0].pm_quality_bimap_.size()) - 1);
    std::advance(it, dist_idx(rng));
    invalid_op == 0 ? it->second->SetInvalidity() : it->second->DynamicSetInvalidity();
    all_buckets[0].pm_quality_bimap_.right.erase(it);
    shed_count_q0++;
  }

  while (shed_count_q1 < shed_quota_q1) {
    auto it = all_buckets[1].pm_quality_bimap_.right.begin();
    if (it == all_buckets[1].pm_quality_bimap_.right.end()) {
      break;
    }
    std::uniform_int_distribution<uint64_t> dist_idx(0, static_cast<int>(all_buckets[1].pm_quality_bimap_.size()) - 1);
    std::advance(it, dist_idx(rng));
    invalid_op == 0 ? it->second->SetInvalidity() : it->second->DynamicSetInvalidity();
    all_buckets[1].pm_quality_bimap_.right.erase(it);
    shed_count_q1++;
  }
}

auto LoadSheddingManager::LoadSheddingBookingDeletePM(const std::shared_ptr<EventVector> &pm_ptr) -> void {
  if (pm_ptr->GetSize() == 0) {
    return;
  }
  int8_t bucket_id;
  if (pm_ptr->candidate_queries_.size() > 1) {
    bucket_id = 2;
  } else {
    bucket_id = *(pm_ptr->candidate_queries_.begin());
  }
  auto it = all_buckets[bucket_id].pm_quality_bimap_.left.find(pm_ptr);
  if (it != all_buckets[bucket_id].pm_quality_bimap_.left.end()) {
    all_buckets[bucket_id].pm_quality_bimap_.left.erase(it);
  }
}

auto LoadSheddingManager::LoadSheddingDarling(long double extend1, long double extend2,
                                              std::vector<std::shared_ptr<EventVector>> &all_pms, int invalid_op,
                                              int ratio) -> void {}

auto LoadSheddingManager::LoadSheddingICDE(long double extend1, long double extend2,
                                           std::vector<std::shared_ptr<EventVector>> &all_pms, int invalid_op,
                                           int ratio) -> void {
  std::vector<int> bucket_ids;
  if (extend1 > 5e-2F) {
    bucket_ids.push_back(0);
  }
  if (extend2 > 5e-2F) {
    bucket_ids.push_back(1);
  }

  auto shed_quota_q0 = static_cast<uint64_t>(
      (all_buckets[0].pm_quality_bimap_.size() + all_buckets[2].pm_quality_bimap_.size()) * extend1);
  auto shed_quota_q1 = static_cast<uint64_t>(
      (all_buckets[1].pm_quality_bimap_.size() + all_buckets[0].pm_quality_bimap_.size()) * extend2);

  shed_quota_q0 = static_cast<uint64_t>(shed_quota_q0 * std::sqrt(ratio) / 100);
  shed_quota_q1 = static_cast<uint64_t>(shed_quota_q1 * std::sqrt(ratio) / 100);
  // to do
  uint64_t shed_q0 = 0;
  uint64_t shed_q1 = 0;
  if (bucket_ids.size() == 1) {
    // only one query violates bound
    int bucket_id = bucket_ids[0];
    bucket_id == 0 ? shed_quota_q1 = 0 : shed_quota_q0 = 0;
    auto it = all_buckets[bucket_id].pm_quality_bimap_.right.begin();
    auto it2 = all_buckets[2].pm_quality_bimap_.right.begin();
    shed_quota_q0 = static_cast<uint64_t>(std::sqrt(ratio) * shed_q0);
    shed_quota_q1 = static_cast<uint64_t>(std::sqrt(ratio) * shed_q1);
    while ((shed_q0 < shed_quota_q0 || shed_q1 < shed_quota_q1)) {
      if (it2 != all_buckets[2].pm_quality_bimap_.right.end() &&
          (it == all_buckets[bucket_id].pm_quality_bimap_.right.end() || it2->first <= it->first)) {
        invalid_op == 0 ? it2->second->SetInvalidity() : it2->second->DynamicSetInvalidity();
        all_buckets[2].pm_quality_bimap_.right.erase(it2++);
        shed_q0++;
        shed_q1++;
      } else if (it != all_buckets[bucket_id].pm_quality_bimap_.right.end()) {
        invalid_op == 0 ? it->second->SetInvalidity() : it->second->DynamicSetInvalidity();
        all_buckets[bucket_id].pm_quality_bimap_.right.erase(it++);
        bucket_id == 0 ? shed_q0++ : shed_q1++;
      } else {
        break;
      }
      num_shed_partial_match++;
    }
  } else {
    // two queries violates at the same time; use 3-pointer
    auto it0 = all_buckets[0].pm_quality_bimap_.right.begin();
    auto it1 = all_buckets[1].pm_quality_bimap_.right.begin();
    auto it2 = all_buckets[2].pm_quality_bimap_.right.begin();
    shed_quota_q0 = static_cast<uint64_t>(std::sqrt(ratio) * shed_q0);
    shed_quota_q1 = static_cast<uint64_t>(std::sqrt(ratio) * shed_q1);
    while ((shed_q0 < shed_quota_q0 || shed_q1 < shed_quota_q1)) {
      if ((it0 != all_buckets[0].pm_quality_bimap_.right.end()) &&
          (it1 == all_buckets[1].pm_quality_bimap_.right.end() || it0->first <= it1->first) &&
          (it2 == all_buckets[2].pm_quality_bimap_.right.end() || it0->first <= it2->first) &&
          it0 != all_buckets[0].pm_quality_bimap_.right.end()) {
        invalid_op == 0 ? it0->second->SetInvalidity() : it0->second->DynamicSetInvalidity();
        all_buckets[0].pm_quality_bimap_.right.erase(it0++);
        shed_q0++;
      } else if ((it0 == all_buckets[0].pm_quality_bimap_.right.end() || it1->first <= it0->first) &&
                 (it2 == all_buckets[2].pm_quality_bimap_.right.end() || it1->first <= it2->first) &&
                 it1 != all_buckets[1].pm_quality_bimap_.right.end()) {
        invalid_op == 0 ? it1->second->SetInvalidity() : it1->second->DynamicSetInvalidity();
        all_buckets[1].pm_quality_bimap_.right.erase(it1++);
        shed_q1++;
      } else if (it2 != all_buckets[2].pm_quality_bimap_.right.end() && it2->second->GetSize() > 0 &&
                 it2 != all_buckets[2].pm_quality_bimap_.right.end()) {
        total_consumption_q0 -= consumption_booking[it2->second->GetAttrVector("id")].first;
        total_consumption_q1 -= consumption_booking[it2->second->GetAttrVector("id")].second;

        invalid_op == 0 ? it2->second->SetInvalidity() : it2->second->DynamicSetInvalidity();
        all_buckets[2].pm_quality_bimap_.right.erase(it2++);
        shed_q0++;
        shed_q1++;
      } else {
        break;
      }
      num_shed_partial_match++;
    }
  }
}
}  // namespace cep
