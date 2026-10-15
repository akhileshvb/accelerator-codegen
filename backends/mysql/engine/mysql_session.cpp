// GENERATED BACKEND
#include "backends/mysql/engine/mysql_session.hpp"

#include <dlfcn.h>

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

struct ServerHooks {
  void *(*current_thd)() = nullptr;
  bool (*in_transaction)(const void *) = nullptr;
};

const ServerHooks &Hooks() {
  static const ServerHooks hooks = [] {
    ServerHooks h;
    h.current_thd = reinterpret_cast<void *(*)()>(dlsym(RTLD_DEFAULT, "_Z19thd_get_current_thdv"));
    h.in_transaction = reinterpret_cast<bool (*)(const void *)>(
        dlsym(RTLD_DEFAULT, "_Z36thd_in_active_multi_stmt_transactionPK3THD"));
    return h;
  }();
  return hooks;
}

thread_local char t_thread_marker;

std::uintptr_t ThreadSession() { return reinterpret_cast<std::uintptr_t>(&t_thread_marker); }

} // namespace

std::uintptr_t MysqlSessionFromThd(const void *thd) {
  return thd && Hooks().current_thd ? reinterpret_cast<std::uintptr_t>(thd) : ThreadSession();
}

std::uintptr_t CurrentMysqlSession() {
  if (Hooks().current_thd) {
    if (void *thd = Hooks().current_thd()) {
      return reinterpret_cast<std::uintptr_t>(thd);
    }
  }
  return ThreadSession();
}

bool MysqlThdInActiveTransaction(const void *thd) {
  return thd && Hooks().in_transaction && Hooks().in_transaction(thd);
}

} // namespace accelerator_codegen::aux_state::engine::mysql
