#pragma once

#include <cstddef>
#include <queue>
#include <utility>
#include <vector>

namespace spacelens {

/// Bounded collector for the K largest items.
///
/// Uses a min-heap of size at most K so N observations cost O(N log K)
/// instead of sorting all N records.
///
/// CompareKey(a, b) must return true when a ranks worse than b
/// (i.e. a has the smaller size / should be discarded first).
template <typename T, typename CompareKey>
class TopKCollector {
public:
    explicit TopKCollector(std::size_t k, CompareKey compareKey = CompareKey{})
        : m_k(k)
        , m_compareKey(std::move(compareKey))
        , m_heap(MinHeapOrder{&m_compareKey})
    {
    }

    void consider(T value)
    {
        if (m_k == 0) {
            return;
        }
        if (m_heap.size() < m_k) {
            m_heap.push(std::move(value));
            return;
        }
        // Top of the min-heap is the worst retained item.
        if (m_compareKey(m_heap.top(), value)) {
            m_heap.pop();
            m_heap.push(std::move(value));
        }
    }

    /// Best-first order (largest / highest rank first).
    [[nodiscard]] std::vector<T> sortedDescending() const
    {
        auto copy = m_heap;
        std::vector<T> ascending;
        ascending.reserve(copy.size());
        while (!copy.empty()) {
            ascending.push_back(copy.top());
            copy.pop();
        }
        // Min-heap pop order is worst→best; reverse for best→worst.
        return std::vector<T>(ascending.rbegin(), ascending.rend());
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_heap.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_k; }

private:
    /// priority_queue is a max-heap: Compare(a,b)==true means a has lower priority.
    /// For a min-heap of keys, lower priority = better rank = larger key.
    struct MinHeapOrder {
        CompareKey* key = nullptr;
        bool operator()(const T& a, const T& b) const
        {
            // If b is worse than a (b < a), then a has lower priority than b.
            return (*key)(b, a);
        }
    };

    std::size_t m_k = 0;
    CompareKey m_compareKey;
    std::priority_queue<T, std::vector<T>, MinHeapOrder> m_heap;
};

}  // namespace spacelens
