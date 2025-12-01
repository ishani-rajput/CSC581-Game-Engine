#pragma once

#include "object_model.h"
#include "memory_pool.h"   // <-- NEW include for pool allocator

#include <unordered_map>
#include <memory>
#include <vector>
#include <string>

namespace Engine {

// Deleter that will either return objects to a pool, or delete them normally.
template<typename T>
struct PoolDeleter {
    PoolAllocator<T>* pool = nullptr;

    void operator()(T* ptr) const noexcept {
        if (!ptr) return;
        if (pool) {
            pool->destroy(ptr);
        } else {
            delete ptr;
        }
    }
};

class Registry {
public:
    using GameObjectDeleter = PoolDeleter<GameObject>;
    using GameObjectPtr     = std::unique_ptr<GameObject, GameObjectDeleter>;

    // Default: no pool, use dynamic allocation (same behavior as before).
    Registry()
        : m_pool(nullptr)
    {}

    // Optional: construct with a pool allocator for GameObject.
    explicit Registry(PoolAllocator<GameObject>* pool)
        : m_pool(pool)
    {}

    // Allow changing the pool at runtime if desired.
    void setPool(PoolAllocator<GameObject>* pool) {
        m_pool = pool;
    }

    GameObject& upsert(const std::string& id) {
        auto it = objs_.find(id);
        if (it != objs_.end() && it->second) {
            return *(it->second);
        }

        // Need to create a new GameObject.
        GameObject* raw = nullptr;
        GameObjectDeleter del;
        del.pool = m_pool;

        if (m_pool) {
            raw = m_pool->create(id);
            if (!raw) {
                // Pool exhausted: fall back to heap allocation.
                raw = new GameObject(id);
                del.pool = nullptr; // so destructor will use delete, not pool->destroy
            }
        } else {
            raw = new GameObject(id);
        }

        auto [insertedIt, _] = objs_.emplace(id, GameObjectPtr(raw, del));
        return *(insertedIt->second);
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
        for (const auto& kv : objs_) {
            ids.push_back(kv.first);
        }
        return ids;
    }

    std::size_t size() const { return objs_.size(); }

    void clear() { objs_.clear(); }

    // --- range-based for loops (unchanged interface) ---
    auto begin()       { return objs_.begin(); }
    auto end()         { return objs_.end(); }
    auto begin() const { return objs_.begin(); }
    auto end()   const { return objs_.end(); }

private:
    std::unordered_map<std::string, GameObjectPtr> objs_;
    PoolAllocator<GameObject>* m_pool; // nullptr = no custom allocator
};

} // namespace Engine