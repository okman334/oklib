file(GLOB base_sources "${PROJECT_SOURCE_DIR}/src/base/*.cpp")
list(LENGTH base_sources base_source_count)

if(base_source_count EQUAL 0)
  message(FATAL_ERROR "src/base must contain base library implementation .cpp files")
endif()
