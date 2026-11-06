# In-memory Amp harnesses: test support for the tests here and for consumers' (not in pp_mesh).
pp_mesh_add_folder_library(testing
  SOURCES
    mesh_harness_support.cpp
    mesh_test_harness.cpp
    mesh_triple_harness.cpp
    delayed_datagram_io.cpp
    park_harness.cpp
  DEPS host shared reach)
