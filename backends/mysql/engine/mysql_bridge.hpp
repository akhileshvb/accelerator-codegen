#pragma once
// Observer hook of the MySQL plugin bridge (mysql_bridge.cpp).
//
// The bridge forwards every trigger-captured row and every statement end to
// MysqlHost. An observer, if one is installed, is told about the same events
// afterwards. The production plugin installs none; the gate-test plugin
// variant (backends/mysql/tests/mysql_bridge_test_udfs.cpp, built with
// -DACCEL_MYSQL_TEST_UDFS=ON) installs one to record captured rows for its
// inspection UDFs.

#include "backends/mysql/engine/mysql_statement_capture.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

class MysqlBridgeObserver {
public:
  virtual ~MysqlBridgeObserver() = default;
  // A trigger-captured row was decoded and handed to the host.
  virtual void OnCapturedRow(const CapturedRow &row) = 0;
  // A COMMIT / ROLLBACK statement finished.
  virtual void OnCommit() = 0;
  virtual void OnRollback() = 0;
  // The audit plugin is being unloaded.
  virtual void OnPluginDeinit() = 0;
};

// Install (or clear, with nullptr) the bridge observer. The observer must
// outlive the plugin or be cleared first.
void SetMysqlBridgeObserver(MysqlBridgeObserver *observer);

} // namespace accelerator_codegen::aux_state::engine::mysql
