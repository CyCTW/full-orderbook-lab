#pragma once
// Stateless fixed-size node allocator for node-based containers (std::map).
// Single-object allocations come from a per-type free list carved out of
// 64 KiB chunks; memory is recycled LIFO and never returned to the OS, which
// is the usual trade-off in a market data handler. Not thread-safe.

#include <cstddef>
#include <new>
#include <vector>

namespace obl::book {

template <std::size_t Size, std::size_t Align>
class FixedArena {
 public:
  static FixedArena& instance() {
    static FixedArena arena;
    return arena;
  }

  void* allocate() {
    if (free_) {
      Node* n = free_;
      free_ = n->next;
      return n;
    }
    if (left_ == 0) refill();
    void* p = cursor_;
    cursor_ += kSlot;
    --left_;
    return p;
  }

  void deallocate(void* p) {
    Node* n = static_cast<Node*>(p);
    n->next = free_;
    free_ = n;
  }

  ~FixedArena() {
    for (void* c : chunks_) ::operator delete(c, std::align_val_t(kAlign));
  }

 private:
  struct Node {
    Node* next;
  };
  static constexpr std::size_t kAlign = Align < alignof(Node) ? alignof(Node) : Align;
  static constexpr std::size_t kSlot = ((Size < sizeof(Node) ? sizeof(Node) : Size) + kAlign - 1) / kAlign * kAlign;
  static constexpr std::size_t kChunk = 64 * 1024;

  void refill() {
    void* c = ::operator new(kChunk, std::align_val_t(kAlign));
    chunks_.push_back(c);
    cursor_ = static_cast<char*>(c);
    left_ = kChunk / kSlot;
  }

  Node* free_ = nullptr;
  char* cursor_ = nullptr;
  std::size_t left_ = 0;
  std::vector<void*> chunks_;
};

template <class T>
struct NodePoolAllocator {
  using value_type = T;

  NodePoolAllocator() noexcept = default;
  template <class U>
  NodePoolAllocator(const NodePoolAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
    if (n == 1) return static_cast<T*>(FixedArena<sizeof(T), alignof(T)>::instance().allocate());
    return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t(alignof(T))));
  }

  void deallocate(T* p, std::size_t n) noexcept {
    if (n == 1) FixedArena<sizeof(T), alignof(T)>::instance().deallocate(p);
    else ::operator delete(p, std::align_val_t(alignof(T)));
  }

  template <class U>
  bool operator==(const NodePoolAllocator<U>&) const noexcept { return true; }
};

}  // namespace obl::book
