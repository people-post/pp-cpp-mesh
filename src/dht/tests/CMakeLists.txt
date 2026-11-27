pp_mesh_add_tests(pp_mesh_dht LINK_LIBS pp_mesh_testing)
# Frozen vector file, in pp-specs (cmake/PpSpecs.cmake); PP_WRITE_DHT_RECORD_VECTORS=<path> writes a fresh one, for a
# pp-specs pull request (dht_record_vectors_test.cpp).
target_compile_definitions(pp_mesh_dht_test PRIVATE
  PP_DHT_RECORD_VECTORS_PATH="${PP_SPECS_VECTORS_DIR}/dht_record_vectors.json")
