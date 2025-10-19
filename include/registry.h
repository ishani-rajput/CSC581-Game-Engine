#pragma once
#include "object_model.h"
#include <unordered_map>
#include <memory>

namespace Engine {

class Registry {
public:
    GameObject& upsert(const std::string& id) {
        auto& ptr = objs_[id];
        if (!ptr) ptr = std::make_unique<GameObject>(id);
        return *ptr;
    }
    void erase(const std::string& id) { objs_.erase(id); }
    bool exists(const std::string& id) const { return objs_.count(id) > 0; }

private:
    std::unordered_map<std::string, std::unique_ptr<GameObject>> objs_;
};

} // namespace Engine
