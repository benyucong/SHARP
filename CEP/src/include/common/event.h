#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cep {
using attr_t = long double;
using time_t = uint64_t;
/**
 * Attribute of event
 */
class Attribute {
 private:
  std::string name_{};
  attr_t value_;

 public:
  Attribute(std::string name, attr_t value) : name_(std::move(name)), value_(value) {}
  [[nodiscard]] auto GetName() const -> std::string { return name_; }
  [[nodiscard]] auto GetValue() const -> attr_t { return value_; }
  auto SetValue(attr_t val) -> void { value_ = val; }
  auto operator<(const Attribute &other) const -> bool { return name_ < other.name_; }
};

/**
 * an event type
 * contains the event type name, a list of attributes
 */
class EventType {
 private:
  // attribute names
  std::set<std::string> all_attributes_;
  bool is_kleene_{false};

 public:
  EventType() = default;
  virtual ~EventType() = default;
  EventType(std::string name, std::set<std::string> attrs, time_t interval = 10000, time_t ttl = 1000,
            bool kleene_flag = false)
      : all_attributes_(std::move(attrs)),
        is_kleene_(kleene_flag),
        name_(std::move(name)),
        arrival_interval_(interval),
        timeout_duration_(ttl) {}
  explicit EventType(std::string name, bool kleene_flag = false) : is_kleene_(kleene_flag), name_(std::move(name)) {}
  auto operator==(const EventType &other) const -> bool { return (name_ == other.name_); }
  auto operator<(const EventType &other) const -> bool { return name_ < other.name_; }

  auto Contain(const std::string &attr) -> bool { return all_attributes_.find(attr) != all_attributes_.end(); }
  auto SetKleene(bool flag) -> void { is_kleene_ = flag; }
  std::string name_{};
  time_t arrival_interval_{0};
  time_t timeout_duration_{0};
};

/**
 * Real event
 */
class Event {
 private:
  bool is_timed_out_{false};
  std::set<Attribute> attributes_;
  EventType type_;
  std::string event_name_{};

 public:
  Event(std::string name, const std::vector<Attribute> &attrs, time_t time, time_t ttl = 1000, time_t interval = 10000);
  Event(const EventType &type, const std::vector<Attribute> &attrs, time_t time);

  auto IfTimeout(time_t current_time) -> bool;

  auto GetAttributeValue(const std::string &attr_name) const -> attr_t;
  auto GetAllAttributeValues() const -> std::vector<attr_t>;
  [[nodiscard]] auto GetEventName() const -> std::string { return type_.name_; }
  [[nodiscard]] auto GetEventType() const -> EventType { return type_; }
  time_t arrival_time_;
  time_t out_queue_time_;
};
}  // namespace cep
