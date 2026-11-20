pp_mesh_add_tests(pp_mesh_dht LINK_LIBS pp_mesh_testing)
# Frozen vector file, read in place; PP_WRITE_DHT_RECORD_VECTORS=<path> rewrites it (dht_record_vectors_test.cpp).
target_compile_definitions(pp_mesh_dht_test PRIVATE
  PP_DHT_RECORD_VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/dht_record_vectors.json")
