add_library(ramulator-memorysystem OBJECT)

target_sources(
  ramulator-memorysystem PRIVATE
  memory_system.h

  impl/dummy_memory_system.cpp
  impl/generic_DRAM_system.cpp
  impl/PIM_DRAM_system.cpp
  impl/numeric_pair.cpp
  impl/numeric_resident.cpp
  impl/numeric_pim7.cpp
  impl/numeric_pim.cpp
)

target_link_libraries(
  ramulator
  PRIVATE
  ramulator-memorysystem
)