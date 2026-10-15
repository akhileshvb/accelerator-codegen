# CMake collector scaffold for {{engine_name}}
# Add to cmake/GeneratedSources.cmake (or equivalent):
#
# function({{collect_fn}})
#   set(srcs
#     backends/{{engine_name}}/api/{{engine_name}}_host.cpp          # EngineSlots (S2-S6, S8)
#     backends/{{engine_name}}/engine/capture.cpp                    # S1
#     backends/{{engine_name}}/engine/scan.cpp                       # S5/S8 scans
#   )
#   set(${ARGV0} "${srcs}" PARENT_SCOPE)
# endfunction()
#
# Optional: option({{build_flag}} "Build {{engine_name}} backend" OFF)
