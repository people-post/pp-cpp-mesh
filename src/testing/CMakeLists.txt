# In-memory Amp harnesses: test support for the tests here and for consumers' (not in pp_mesh).
pp_mesh_add_folder_library(testing
  SOURCES
    mesh_harness_support.cpp
    mesh_test_harness.cpp
    mesh_triple_harness.cpp
  DEPS host shared)
