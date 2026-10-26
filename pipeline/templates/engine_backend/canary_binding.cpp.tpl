// GENERATED BACKEND: binds the pipeline's canary suite (pipeline/canary) to {{engine_name}}.
// Spec: pipeline/prompts/engine_backend/docs/canary_contract.md. A pass-through: no checks live here.
// Build: test_{{engine_name}}_canary   Gate: engine_canary, engine_canary_asan, engine_canary_tsan
#include <memory>
#include <stdexcept>

#include "pipeline/canary/canary_main.hpp"

namespace {

namespace canary = accelerator_codegen::canary;

class {{EngineName}}Session final : public canary::Session {
public:
  canary::Rows Query(const std::string &) override { throw std::logic_error("canary binding: Query not implemented"); }
};

class {{EngineName}}Engine final : public canary::Engine {
public:
  std::unique_ptr<canary::Session> Connect() override { return std::make_unique<{{EngineName}}Session>(); }
  void Register(const canary::AcceleratorSpec &) override { throw std::logic_error("canary binding: Register not implemented"); }
  std::string ReadSql(const std::string &) override { throw std::logic_error("canary binding: ReadSql not implemented"); }
  canary::Counters ReadCounters(const std::string &) override { throw std::logic_error("canary binding: ReadCounters not implemented"); }
  void Invalidate(const std::string &) override { throw std::logic_error("canary binding: Invalidate not implemented"); }
  void ForceFallback(bool) override { throw std::logic_error("canary binding: ForceFallback not implemented"); }
  void TrapFallback(bool) override { throw std::logic_error("canary binding: TrapFallback not implemented"); }
  void Settle() override { throw std::logic_error("canary binding: Settle not implemented"); }
};

} // namespace

int main(int argc, char **argv) {
  return canary::Main([] { return std::make_unique<{{EngineName}}Engine>(); }, argc, argv);
}
