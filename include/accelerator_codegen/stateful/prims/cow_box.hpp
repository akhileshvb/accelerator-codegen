#pragma once
//
// CowBox<T>
// PURPOSE    Clone-when-shared holder: makes any copyable value cheap to copy by holding it
//            behind a shared_ptr and cloning it only on the first mutable access while
//            another copy still shares it. Typical use: wrap a COW container whose
//            directory copy is not free (hundreds of shared_ptr entries) so that a
//            versioned state struct containing it copies one pointer.
// COMPLEXITY Copy O(1) (one refcount increment). Const access O(1). The first Mutable()
//            on a shared box copies T once (T's own copy cost, e.g. a directory of
//            shared shard pointers); later Mutable() calls on the private box are O(1).
// MEMORY     16 B per box plus one T per distinct version that wrote to it.
// COW/THREADS Mutate only a private copy of the owning state, concurrent const reads of a
//            published copy are safe. use_count() is only a hint under races: do not
//            call Mutable() on an object another thread is copying at the same time.
// USE WHEN   a state struct is copied once per version and holds containers whose copy
//            constructors are O(directory). Not needed for containers that are already
//            held by shared_ptr or are a single pointer.
//

#include <memory>
#include <utility>

namespace accelerator_codegen::aux_state::core {

template <typename T> class CowBox {
public:
  CowBox() : p_(std::make_shared<T>()) {}
  explicit CowBox(T value) : p_(std::make_shared<T>(std::move(value))) {}

  const T &operator*() const noexcept { return *p_; }
  const T *operator->() const noexcept { return p_.get(); }
  // The value, privately owned: clones it first when another copy shares it.
  T &Mutable() {
    if (p_.use_count() > 1) p_ = std::make_shared<T>(*p_);
    return *p_;
  }
  // True when this box is the only owner (a write would not clone).
  bool unique() const noexcept { return p_.use_count() == 1; }

private:
  std::shared_ptr<T> p_;
};

} // namespace accelerator_codegen::aux_state::core
