#pragma once

#include <cstdlib>
#include <string>

namespace accel::mysql {

struct Tools {
  bool ok = true;
  std::string error;
  std::string host = "127.0.0.1";
  int port = 3306;
  std::string user = "root";
  std::string password = "mysql";
  std::string database = "accel_mysql_test";
  std::string plugin_so = "accelerator_codegen_mysql.so";
};

inline Tools DiscoverTools() {
  Tools tools;
  if (const char *host = std::getenv("MYSQL_HOST"); host && *host) {
    tools.host = host;
  }
  if (const char *port = std::getenv("MYSQL_PORT"); port && *port) {
    tools.port = std::atoi(port);
  }
  if (const char *user = std::getenv("MYSQL_USER"); user && *user) {
    tools.user = user;
  }
  if (const char *password = std::getenv("MYSQL_PWD"); password) {
    tools.password = password;
  } else if (const char *password = std::getenv("MYSQL_ROOT_PASSWORD");
             password && *password) {
    tools.password = password;
  }
  if (const char *db = std::getenv("TPCH_MYSQL_DB"); db && *db) {
    tools.database = db;
  } else if (const char *db = std::getenv("MYSQL_DATABASE"); db && *db) {
    tools.database = db;
  }
  return tools;
}

} // namespace accel::mysql
