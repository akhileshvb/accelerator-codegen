# CMake collector scaffold for {{engine_name}}
# Add to cmake/GeneratedSources.cmake (or equivalent):
#
# function({{collect_fn}})
#   set(srcs
#     backends/{{engine_name}}/engine/scan_adapter.cpp
#     backends/{{engine_name}}/state/{{engine_name}}_aux_backend.cpp
#     backends/{{engine_name}}/state/{{engine_name}}_state_store.cpp
#     backends/{{engine_name}}/engine/{{engine_name}}_mutation_capture.cpp
#   )
#   set(${ARGV0} "${srcs}" PARENT_SCOPE)
# endfunction()
#
# Optional: option({{build_flag}} "Build {{engine_name}} adapter" OFF)
