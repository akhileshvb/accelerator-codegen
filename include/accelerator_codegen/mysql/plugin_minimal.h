#pragma once
// Minimal MySQL 8.4 plugin ABI definitions for engine_backend layer.
// Avoids the full internal server header dependency chain.

#include <cstddef>
#include <cstdint>

extern "C++" {
typedef void *MYSQL_PLUGIN;
typedef void *MYSQL_THD;
}

// Item_result (matching mysql/udf_registration_types.h)
enum Item_result {
  INVALID_RESULT = -1,
  STRING_RESULT = 0,
  REAL_RESULT,
  INT_RESULT,
  ROW_RESULT,
  DECIMAL_RESULT
};

// UDF definitions (matching mysql/udf_registration_types.h)
typedef struct UDF_ARGS {
  unsigned int arg_count;
  enum Item_result *arg_type;
  char **args;
  unsigned long *lengths;
  char *maybe_null;
  char **attributes;
  unsigned long *attribute_lengths;
  void *extension;
} UDF_ARGS;

typedef struct UDF_INIT {
  bool maybe_null;
  unsigned int decimals;
  unsigned long max_length;
  char *ptr;
  bool const_item;
  void *extension;
} UDF_INIT;

// Plugin type enum (matching include/mysql/plugin.h)
enum {
  MYSQL_UDF_PLUGIN = 0,
  MYSQL_STORAGE_ENGINE_PLUGIN = 1,
  MYSQL_FTPARSER_PLUGIN = 2,
  MYSQL_DAEMON_PLUGIN = 3,
  MYSQL_INFORMATION_SCHEMA_PLUGIN = 4,
  MYSQL_AUDIT_PLUGIN = 5,
  MYSQL_REPLICATION_PLUGIN = 6,
  MYSQL_AUTHENTICATION_PLUGIN = 7,
  MYSQL_VALIDATE_PASSWORD_PLUGIN = 8,
  MYSQL_GROUP_REPLICATION_PLUGIN = 9,
  MYSQL_KEYRING_PLUGIN = 10,
  MYSQL_CLONE_PLUGIN = 11,
  MYSQL_MAX_PLUGIN_TYPE_NUM = 12
};

// Plugin interface version
#define MYSQL_PLUGIN_INTERFACE_VERSION 0x010B

// Plugin licenses
#define PLUGIN_LICENSE_PROPRIETARY 0
#define PLUGIN_LICENSE_GPL 1
#define PLUGIN_LICENSE_BSD 2

// Plugin declaration macros (MYSQL_DYNAMIC_PLUGIN path)
#define MYSQL_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#define MYSQL_DYNAMIC_PLUGIN

#define __MYSQL_DECLARE_PLUGIN(NAME, VERSION, PSIZE, DECLS)            MYSQL_PLUGIN_EXPORT int _mysql_plugin_interface_version_ =                MYSQL_PLUGIN_INTERFACE_VERSION;                                   MYSQL_PLUGIN_EXPORT int _mysql_sizeof_struct_st_plugin_ =                 sizeof(struct st_mysql_plugin);                                   MYSQL_PLUGIN_EXPORT struct st_mysql_plugin _mysql_plugin_declarations_[] = {

#define mysql_declare_plugin(NAME)                                          __MYSQL_DECLARE_PLUGIN(NAME, builtin_##NAME##_plugin_interface_version,                          builtin_##NAME##_sizeof_struct_st_plugin,                                 builtin_##NAME##_plugin)

#define mysql_declare_plugin_end                                            , {                                                                         0, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr, 0,         nullptr, nullptr, nullptr, 0                                          }                                                                         }

// Plugin flags
#define PLUGIN_OPT_NO_INSTALL 1UL
#define PLUGIN_OPT_NO_UNINSTALL 2UL
#define PLUGIN_OPT_ALLOW_EARLY 4UL
#define PLUGIN_OPT_DEFAULT_OFF 8UL
#define PLUGIN_OPT_DEPENDENT_EXTRA_PLUGINS 16UL

// Forward declarations for plugin.h
struct SHOW_VAR;
struct SYS_VAR;
struct st_mysql_plugin;

// st_mysql_plugin (matching include/mysql/plugin.h)
struct st_mysql_plugin {
  int type;
  void *info;
  const char *name;
  const char *author;
  const char *descr;
  int license;
  int (*init)(MYSQL_PLUGIN);
  int (*check_uninstall)(MYSQL_PLUGIN);
  int (*deinit)(MYSQL_PLUGIN);
  unsigned int version;
  SHOW_VAR *status_vars;
  SYS_VAR **system_vars;
  void *__reserved1;
  unsigned long flags;
};

// st_mysql_daemon
#define MYSQL_DAEMON_INTERFACE_VERSION (0)
struct st_mysql_daemon {
  int interface_version;
};

// Audit plugin definitions (matching include/mysql/plugin_audit.h)
#define MYSQL_AUDIT_INTERFACE_VERSION 0x0401

typedef enum {
  MYSQL_AUDIT_GENERAL_CLASS = 0,
  MYSQL_AUDIT_CONNECTION_CLASS = 1,
  MYSQL_AUDIT_PARSE_CLASS = 2,
  MYSQL_AUDIT_AUTHORIZATION_CLASS = 3,
  MYSQL_AUDIT_TABLE_ACCESS_CLASS = 4,
  MYSQL_AUDIT_GLOBAL_VARIABLE_CLASS = 5,
  MYSQL_AUDIT_SERVER_STARTUP_CLASS = 6,
  MYSQL_AUDIT_SERVER_SHUTDOWN_CLASS = 7,
  MYSQL_AUDIT_COMMAND_CLASS = 8,
  MYSQL_AUDIT_QUERY_CLASS = 9,
  MYSQL_AUDIT_STORED_PROGRAM_CLASS = 10,
  MYSQL_AUDIT_AUTHENTICATION_CLASS = 11,
  MYSQL_AUDIT_MESSAGE_CLASS = 12,
  MYSQL_AUDIT_CLASS_MASK_SIZE
} mysql_event_class_t;

// MYSQL_LEX_CSTRING for audit events
struct MYSQL_LEX_CSTRING {
  const char *str;
  size_t length;
};

struct CHARSET_INFO;

typedef enum {
  MYSQL_AUDIT_GENERAL_LOG = 1 << 0,
  MYSQL_AUDIT_GENERAL_ERROR = 1 << 1,
  MYSQL_AUDIT_GENERAL_RESULT = 1 << 2,
  MYSQL_AUDIT_GENERAL_STATUS = 1 << 3
} mysql_event_general_subclass_t;

struct mysql_event_general {
  mysql_event_general_subclass_t event_subclass;
  int general_error_code;
  unsigned long general_thread_id;
  MYSQL_LEX_CSTRING general_user;
  MYSQL_LEX_CSTRING general_command;
  MYSQL_LEX_CSTRING general_query;
  CHARSET_INFO *general_charset;
  unsigned long long general_time;
  unsigned long long general_rows;
  MYSQL_LEX_CSTRING general_host;
  MYSQL_LEX_CSTRING general_sql_command;
  MYSQL_LEX_CSTRING general_external_user;
  MYSQL_LEX_CSTRING general_ip;
};

struct st_mysql_audit {
  int interface_version;
  void (*release_thd)(MYSQL_THD);
  int (*event_notify)(MYSQL_THD, mysql_event_class_t, const void *);
  unsigned long class_mask[MYSQL_AUDIT_CLASS_MASK_SIZE];
};

// MYSQL_ERRMSG_SIZE
#define MYSQL_ERRMSG_SIZE 512

// Minimal component-registry / udf_registration ABI (MySQL 8.4).
// CREATE FUNCTION persists to mysql.func; Docker volume perms often reject that
// write. Plugin init can register the same symbols in-memory via this service.
typedef bool mysql_service_status_t;
typedef struct my_h_service_imp *my_h_service;

struct mysql_service_registry_t {
  mysql_service_status_t (*acquire)(const char *service_name,
                                    my_h_service *out_service);
  mysql_service_status_t (*acquire_related)(const char *service_name,
                                            my_h_service service,
                                            my_h_service *out_service);
  mysql_service_status_t (*release)(my_h_service service);
};

struct mysql_service_udf_registration_t {
  mysql_service_status_t (*udf_register)(
      const char *func_name, enum Item_result return_type, void (*func)(),
      bool (*init_func)(UDF_INIT *, UDF_ARGS *, char *),
      void (*deinit_func)(UDF_INIT *));
  mysql_service_status_t (*udf_unregister)(const char *name, int *was_present);
};

struct plugin_registry_service_st {
  const mysql_service_registry_t *(*mysql_plugin_registry_acquire_func)();
  int (*mysql_plugin_registry_release_func)(
      const mysql_service_registry_t *registry_ptr);
};

#ifdef MYSQL_DYNAMIC_PLUGIN
#define mysql_plugin_registry_acquire() \
  plugin_registry_service->mysql_plugin_registry_acquire_func()
#define mysql_plugin_registry_release(r) \
  plugin_registry_service->mysql_plugin_registry_release_func(r)
#endif
