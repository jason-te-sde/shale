#ifndef SHALE_IO_SKIPLIST_H_
#define SHALE_IO_SKIPLIST_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "util/arena.h"
#include "util/check.h"
#include "util/random.h"

namespace shale {

/// An ordered set of keys that one thread inserts into while any number of other threads read it,
/// with no lock anywhere. It is the index of the memtable.
///
/// Nothing is ever removed, and nodes live in the Arena until the arena is destroyed, so a reader
/// holding a node never has to ask whether it is still there. That is what makes the lock-free
/// design this small: the only thing a reader can race with is a node being linked in, and the
/// ordering below covers that.
///
/// Memory ordering. A node is built completely, key and links, with relaxed stores, and only then
/// linked into the list with a release store of the pointer that makes it reachable. A reader loads
/// every pointer it follows with acquire, so whichever node it arrives at, it sees that node's
/// fields as the writer left them. A node is linked one level at a time, bottom up, and each of its
/// own links is stored before the node is published at that level or any above it. A reader that
/// arrives at a node through a link of level L reads that node's links at level L and below and
/// no others, so every link it reads has been stored; the ones above L, which may not be yet, it
/// never looks at. `max_height_` is only a hint
/// for where a search starts: a reader that sees a stale value starts lower than it could, and one
/// that sees a new value before the node that raised it finds only null links at the new levels,
/// and drops down. So it needs no ordering at all.
///
/// Writers. At most one thread may be inside Insert() at a time, and Insert() aborts if two are:
/// the node heights come from one generator, and the writer reads links back with relaxed loads on
/// the assumption that nobody else changes them, so a second writer would corrupt the list without
/// any error. The writer may change from one thread to another, as long as the first has left
/// before the second enters. The guard's release on exit and acquire on entry hand the first
/// writer's state to the next, which matters because the arena's bookkeeping and the height
/// generator are plain memory that no link in the list publishes.
///
/// @tparam Key a type that is trivially destructible and default-constructible: nodes are never
///   destroyed, and the head node holds a default-constructed key that is never compared. It is
///   copied into the node, so a pointer is a good key and a large object is not.
/// @tparam Comparator a callable `int operator()(const Key&, const Key&) const` that returns a
///   negative, zero or positive value for less, equal or greater. It is called by readers and by
///   the writer, and must not depend on state that changes.
template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  /// Creates an empty list whose nodes are allocated from `*arena`, which must outlive the list.
  /// The arena is written by this constructor and by Insert() and must not be by anything else at
  /// the same time, since it is not safe for two threads to allocate from. `seed` seeds the
  /// generator that picks node heights, so a test that prints it can reproduce the shape of a list.
  SkipList(Comparator cmp, Arena* arena, uint64_t seed)
      : compare_(cmp), arena_(arena), head_(NewNode(Key(), kMaxHeight)), rnd_(seed) {}

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  /// Adds `key`. Requires that no key comparing equal to it is present, and aborts with
  /// "duplicate key" if there is: the memtable never writes one key twice, because the sequence
  /// number makes every internal key distinct, so a duplicate means the caller is broken. Aborts
  /// with "concurrent SkipList::Insert" if another call is in progress, on this thread or another.
  /// Safe to call while other threads read.
  void Insert(const Key& key) {
    const InsertScope scope(&inserting_);

    Node* prev[kMaxHeight];
    Node* const next = FindGreaterOrEqual(key, prev);
    SHALE_CHECK(next == nullptr || compare_(key, next->key) != 0, "duplicate key");

    const int height = RandomHeight();
    const int old_height = MaxHeight();
    if (height > old_height) {
      for (int level = old_height; level < height; level++) {
        prev[level] = head_;
      }
      // Readers need not see this before they see the node: until the node is linked at a level,
      // head's link there is null and a search that starts there drops down at once.
      max_height_.store(height, std::memory_order_relaxed);
    }

    Node* const node = NewNode(key, height);
    for (int level = 0; level < height; level++) {
      // The node's own link is not yet visible to anyone, so a relaxed store will do; the release
      // store into prev[level] that follows is what publishes it.
      node->SetNextRelaxed(level, prev[level]->NextRelaxed(level));
      prev[level]->SetNext(level, node);
    }
  }

  /// Returns true if a key comparing equal to `key` is in the list. Safe to call while another
  /// thread inserts.
  bool Contains(const Key& key) const {
    Node* const node = FindGreaterOrEqual(key, nullptr);
    return node != nullptr && compare_(key, node->key) == 0;
  }

  /// A cursor over a list, in key order. It reads while the list is being inserted into, and stays
  /// valid across inserts: it sees every key that was in the list when it moved onto it and may or
  /// may not see one inserted after.
  class Iterator {
   public:
    /// Creates a cursor over `*list`, which must outlive it. It starts invalid.
    explicit Iterator(const SkipList* list) : list_(list) {}

    /// Returns true if the cursor is on a key.
    bool Valid() const { return node_ != nullptr; }

    /// Returns the key the cursor is on. Requires Valid().
    const Key& key() const {
      SHALE_CHECK(Valid(), "SkipList::Iterator::key() on an invalid iterator");
      return node_->key;
    }

    /// Moves to the next key, or becomes invalid after the last. Requires Valid().
    void Next() {
      SHALE_CHECK(Valid(), "SkipList::Iterator::Next() on an invalid iterator");
      node_ = node_->Next(0);
    }

    /// Moves to the first key that is not less than `target`, or becomes invalid if there is none.
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

    /// Moves to the smallest key, or becomes invalid if the list is empty.
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* const list_;
    Node* node_ = nullptr;
  };

 private:
  /// The tallest a node can be. With a branching factor of 4, 12 levels index about 4^12, 16
  /// million, entries before the top level stops thinning: far more than a memtable holds.
  static constexpr int kMaxHeight = 12;

  /// A node is promoted to the next level with probability 1/kBranching. Four spends less memory
  /// on links than two and costs a search a few more comparisons.
  static constexpr uint32_t kBranching = 4;

  /// A node and its links. The links follow the struct in the same allocation, `height` of them,
  /// which saves the pointer to a separate array that a vector would cost each node.
  struct Node {
    explicit Node(const Key& k) : key(k) {}

    // The key never changes after the node is built, and is published by the release store that
    // links the node in.
    const Key key;

    Node* Next(int level) { return Links()[level].load(std::memory_order_acquire); }
    void SetNext(int level, Node* node) { Links()[level].store(node, std::memory_order_release); }

    // For the writer, on a node no reader can reach yet or on a link only the writer changes.
    Node* NextRelaxed(int level) { return Links()[level].load(std::memory_order_relaxed); }
    void SetNextRelaxed(int level, Node* node) {
      Links()[level].store(node, std::memory_order_relaxed);
    }

   private:
    std::atomic<Node*>* Links() {
      // NewNode() constructs the links in the bytes right after the Node, so one exists here.
      return std::launder(
          reinterpret_cast<std::atomic<Node*>*>(reinterpret_cast<char*>(this) + sizeof(Node)));
    }
  };

  static_assert(std::is_default_constructible_v<Key>,
                "the head node needs a key to hold, though it is never compared");
  static_assert(std::is_trivially_destructible_v<Key>,
                "the arena never runs destructors, so a key must not need one");
  static_assert(sizeof(Node) % alignof(std::atomic<Node*>) == 0,
                "the links must be aligned where they follow the node");
  static_assert(alignof(Node) <= alignof(std::max_align_t),
                "the arena aligns allocations to max_align_t, which must be enough for a node");

  /// Marks the list as being inserted into for as long as it lives, and aborts if it already was.
  class InsertScope {
   public:
    explicit InsertScope(std::atomic<bool>* flag) : flag_(flag) {
      SHALE_CHECK(!flag->exchange(true, std::memory_order_acquire), "concurrent SkipList::Insert");
    }

    InsertScope(const InsertScope&) = delete;
    InsertScope& operator=(const InsertScope&) = delete;

    ~InsertScope() { flag_->store(false, std::memory_order_release); }

   private:
    std::atomic<bool>* const flag_;
  };

  Node* NewNode(const Key& key, int height) {
    char* const memory = arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) *
                                                                    static_cast<size_t>(height));
    Node* const node = new (memory) Node(key);
    auto* const links = reinterpret_cast<std::atomic<Node*>*>(memory + sizeof(Node));
    for (int level = 0; level < height; level++) {
      new (links + level) std::atomic<Node*>(nullptr);
    }
    return node;
  }

  int RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && rnd_.OneIn(kBranching)) {
      height++;
    }
    return height;
  }

  int MaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  /// Returns the first node whose key is not less than `key`, or null if there is none. When `prev`
  /// is not null it is filled, at each level below the current height, with the last node at that
  /// level whose key is less than `key`: the nodes a new node is linked after.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* node = head_;
    int level = MaxHeight() - 1;
    while (true) {
      Node* const next = node->Next(level);
      if (next != nullptr && compare_(next->key, key) < 0) {
        node = next;
      } else {
        if (prev != nullptr) {
          prev[level] = node;
        }
        if (level == 0) {
          return next;
        }
        level--;
      }
    }
  }

  const Comparator compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_{1};
  std::atomic<bool> inserting_{false};
  Random rnd_;  // only Insert() touches it, under the guard
};

}  // namespace shale

#endif  // SHALE_IO_SKIPLIST_H_
