pp_mesh_add_library(types
  SOURCES
    AddressDisclosure.cpp
    MeshHopDial.cpp
    PublicAddress.cpp
  LIBS pp_common)

if(PP_MESH_BUILD_TESTS)
  add_subdirectory(tests)
endif()
