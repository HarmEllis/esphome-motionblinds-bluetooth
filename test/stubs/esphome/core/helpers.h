#pragma once
#include <functional>
#include <utility>
#include <vector>
namespace esphome {
template<typename Signature> class CallbackManager;
template<typename... Args> class CallbackManager<void(Args...)> {
 public:
  void add(std::function<void(Args...)> callback) { callbacks_.push_back(std::move(callback)); }
  void call(Args... args) { for (auto &callback : callbacks_) callback(args...); }
 private:
  std::vector<std::function<void(Args...)>> callbacks_;
};
}
