// GENERATED BACKEND
#pragma once

#include <cstdint>

namespace accelerator_codegen::aux_state::engine::mysql {

// A session is one server connection (one THD). The server exports
// thd_get_current_thd() and thd_in_active_multi_stmt_transaction(); both are
// looked up at run time so the plugin still loads on a server that lacks them
// (one session per OS thread, i.e. per connection under the default
// thread-per-connection model, and autocommit semantics).
std::uintptr_t CurrentMysqlSession();
// The session key of a server THD handle (an audit event's).
std::uintptr_t MysqlSessionFromThd(const void *thd);
// Whether that connection is inside an explicit (multi-statement)
// transaction (false if the server does not export the query).
bool MysqlThdInActiveTransaction(const void *thd);

} // namespace accelerator_codegen::aux_state::engine::mysql
