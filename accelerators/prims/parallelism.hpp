#pragma once
//
// parallelism (Parallelism)
// PURPOSE    How a prim fans out its own bulk work without owning threads: the
//            caller hands in its pool (e.g. AuxEngineServices::ParallelFor).
// THREADS    parallel_for(n, fn) runs fn(0..n-1), possibly concurrently, and
//            returns when all are done; `workers` bounds the useful fan-out.
//            Default-constructed: serial on the calling thread.
//

#include <cstddef>
#include <functional>

namespace accelerator_codegen::aux_state::core {

struct Parallelism {
  using Fn = std::function<void(std::size_t)>;
  std::size_t workers = 1;
  std::function<void(std::size_t, const Fn &)> parallel_for;

  void For(std::size_t n, const Fn &fn) const {
    if (!parallel_for || n <= 1) {
      for (std::size_t i = 0; i < n; ++i)
        fn(i);
      return;
    }
    parallel_for(n, fn);
  }

  // The pool of any services-like object with WorkerCount() and ParallelFor().
  template <class Services> static Parallelism OnPool(const Services &services) {
    return {services.WorkerCount(),
            [&services](std::size_t n, const Fn &fn) { services.ParallelFor(n, fn); }};
  }
};

} // namespace accelerator_codegen::aux_state::core
