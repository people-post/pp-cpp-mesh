pp_browser_add_domain_folder_tests(pp_domain_mesh pp_browser_mesh_test
  EXTRA_SOURCES
    ${CMAKE_SOURCE_DIR}/src/domain/mesh/tests/support/mesh_harness_support.cpp
    ${CMAKE_SOURCE_DIR}/src/domain/mesh/tests/support/mesh_test_harness.cpp
    ${CMAKE_SOURCE_DIR}/src/domain/mesh/tests/support/mesh_triple_harness.cpp
  LINK_LIBS pp_domain_people pp_foundation_identity pp_domain_net)
