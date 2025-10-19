#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <variant>

namespace Engine {

struct Vec2 { float x=0.f, y=0.f; };
using Property = std::variant<int,float,bool,Vec2>;

class GameObject {
public:
    using UpdateFn = std::function<void(GameObject&, float)>;

    explicit GameObject(std::string name) : name_(std::move(name)) {}

    const std::string& name() const { return name_; }

    template<typename T>
    void set(const std::string& key, const T& v) { props_[key] = v; }

    template<typename T>
    T get(const std::string& key, const T& def = {}) const {
        auto it = props_.find(key);
        if (it == props_.end()) return def;
        if (auto p = std::get_if<T>(&it->second)) return *p;
        return def;
    }

    void onUpdate(UpdateFn fn) { update_ = std::move(fn); }
    void tick(float dt) { if (update_) update_(*this, dt); }

private:
    std::string name_;
    std::unordered_map<std::string, Property> props_;
    UpdateFn update_{};
};

} // namespace Engine