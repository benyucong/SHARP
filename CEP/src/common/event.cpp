#include "common/event.h"
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace cep {
Event::Event(std::string name, const std::vector<Attribute> &attrs, time_t time, time_t ttl, time_t interval)
    : event_name_(std::move(name)), arrival_time_(time) {
  std::set<std::string> attr_names;
  for (const auto &ele : attrs) {
    attributes_.insert(ele);
    attr_names.insert(ele.GetName());
  }
  type_ = EventType(event_name_, attr_names, interval, ttl);
}

Event::Event(const EventType &type, const std::vector<Attribute> &attrs, time_t time) : arrival_time_(time) {
  type_ = type;
  for (const auto &ele : attrs) {
    attributes_.insert(ele);
  }
}

auto Event::IfTimeout(time_t current_time) -> bool {
  if (is_timed_out_) {
    return true;
  }
  if (current_time - arrival_time_ > type_.timeout_duration_) {
    is_timed_out_ = true;
  }
  return is_timed_out_;
}

auto Event::GetAttributeValue(const std::string &attr_name) const -> attr_t {
  for (const auto &attr : attributes_) {
    if (attr.GetName() == attr_name) {
      return attr.GetValue();
    }
  }
  return INT64_MAX;
}

auto Event::GetAllAttributeValues() const -> std::vector<attr_t> {
  std::vector<attr_t> ans;
  // std::cout << "-------------------" << std::endl;
  // for (auto it : attributes_) {
  //   std::cout << it.GetName() << std::endl;
  // }
  // std::cout << "-------------------" << std::endl;
  // ans.reserve(attributes_.size());
  for (const auto &attr : attributes_) {
    auto val = attr.GetValue();
    ans.push_back(val);
  }
  return ans;
}
}  // namespace cep
