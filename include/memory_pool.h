#pragma once

#include <cstddef>
#include <vector>
#include <stdexcept>
#include <new>

namespace Engine {

template <typename T>
class PoolAllocator {
public:
    explicit PoolAllocator(std::size_t capacity)
        : m_capacity(capacity),
          m_storage(nullptr),
          m_freeIndices(),
          m_live()
    {
        if (capacity == 0) {
            throw std::invalid_argument("PoolAllocator capacity must be > 0");
        }

        m_storage = static_cast<T*>(::operator new[](capacity * sizeof(T)));

        m_freeIndices.reserve(capacity);
        m_live.assign(capacity, false);

        for (std::size_t i = 0; i < capacity; ++i) {
            m_freeIndices.push_back(capacity - 1 - i);
        }
    }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    ~PoolAllocator() {
        for (std::size_t i = 0; i < m_capacity; ++i) {
            if (m_live[i]) {
                T* ptr = m_storage + i;
                ptr->~T();
            }
        }
        ::operator delete[](m_storage);
    }

    std::size_t capacity() const { return m_capacity; }
    std::size_t used() const { return m_capacity - m_freeIndices.size(); }
    std::size_t available() const { return m_freeIndices.size(); }

    template <typename... Args>
    T* create(Args&&... args) {
        if (m_freeIndices.empty()) {
            return nullptr;
        }

        std::size_t index = m_freeIndices.back();
        m_freeIndices.pop_back();
        m_live[index] = true;

        T* ptr = m_storage + index;
        new (ptr) T(std::forward<Args>(args)...);
        return ptr;
    }

    void destroy(T* ptr) {
        if (!ptr) return;

        if (!owns(ptr)) {
            return;
        }

        std::size_t index = static_cast<std::size_t>(ptr - m_storage);
        if (!m_live[index]) {
            return;
        }

        ptr->~T();
        m_live[index] = false;
        m_freeIndices.push_back(index);
    }

    bool owns(const T* ptr) const {
        if (!ptr) return false;
        const T* begin = m_storage;
        const T* end   = m_storage + m_capacity;
        return (ptr >= begin) && (ptr < end);
    }

private:
    std::size_t m_capacity;
    T* m_storage;
    std::vector<std::size_t> m_freeIndices;
    std::vector<bool> m_live;
};

} 