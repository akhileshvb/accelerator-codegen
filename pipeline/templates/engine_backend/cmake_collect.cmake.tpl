# CMake collector scaffold for {{engine_name}}
# Add to cmake/GeneratedSources.cmake (or equivalent):
#
# function({{collect_fn}})
#   set(srcs
#     src/runtime/{{engine_name}}/scan_adapter.cpp
#     src/stateful/{{engine_name}}/{{engine_name}}_aux_backend.cpp
#     src/stateful/{{engine_name}}/{{engine_name}}_state_store.cpp
#     src/stateful/{{engine_name}}/{{engine_name}}_mutation_capture.cpp
#   )
#   set(${ARGV0} "${srcs}" PARENT_SCOPE)
# endfunction()
#
# Optional: option({{build_flag}} "Build {{engine_name}} adapter" OFF)
