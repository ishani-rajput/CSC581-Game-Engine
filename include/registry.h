#pragma once
#include "object_model.h"
#include <unordered_map>
#include <memory>
#include <vector>

namespace Engine {

class Registry {
public:
    GameObject& upsert(const std::string& id) {
        auto& ptr = objs_[id];
        if (!ptr) ptr = std::make_unique<GameObject>(id);
        return *ptr;
    }

    void erase(const std::string& id) {
        objs_.erase(id);
    }

    bool exists(const std::string& id) const {
        return objs_.count(id) > 0;
    }

    GameObject* get(const std::string& id) {
        auto it = objs_.find(id);
        return it != objs_.end() ? it->second.get() : nullptr;
    }

    const GameObject* get(const std::string& id) const {
        auto it = objs_.find(id);
        return it != objs_.end() ? it->second.get() : nullptr;
    }

    std::vector<std::string> getAllIds() const {
        std::vector<std::string> ids;
        ids.reserve(objs_.size());
        for (const auto& [id, _] : objs_)
            ids.push_back(id);
        return ids;
    }

    size_t size() const { return objs_.size(); }

    void clear() { objs_.clear(); }

    // --- new for range-based for loops ---
    auto begin()       { return objs_.begin(); }
    auto end()         { return objs_.end(); }
    auto begin() const { return objs_.begin(); }
    auto end()   const { return objs_.end(); }

private:
    std::unordered_map<std::string, std::unique_ptr<GameObject>> objs_;
};

} 