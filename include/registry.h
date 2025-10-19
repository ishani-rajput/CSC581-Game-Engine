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
    
    // NEW: Get GameObject by ID (returns nullptr if not found)
    GameObject* get(const std::string& id) {
        auto it = objs_.find(id);
        return it != objs_.end() ? it->second.get() : nullptr;
    }
    
    const GameObject* get(const std::string& id) const {
        auto it = objs_.find(id);
        return it != objs_.end() ? it->second.get() : nullptr;
    }
    
    // NEW: Get all IDs
    std::vector<std::string> getAllIds() const {
        std::vector<std::string> ids;
        ids.reserve(objs_.size());
        for (const auto& [id, _] : objs_) {
            ids.push_back(id);
        }
        return ids;
    }
    
    // NEW: Get count
    size_t size() const {
        return objs_.size();
    }
    
    // NEW: Clear all objects
    void clear() {
        objs_.clear();
    }

private:
    std::unordered_map<std::string, std::unique_ptr<GameObject>> objs_;
};

} // namespace Engine