# pp-specs (people-post/pp-specs, public; pp-browser capability-repos CR025): the frozen vectors of the contracts
# more than one repository implements, which this library's tests read at the pinned tag instead of keeping copies.
# Files only, fetched only with the tests. -DPP_SPECS_SOURCE_DIR=<checkout> reads a local one; -DPP_SPECS_GIT_TAG=<tag>
# tries another tag. Sets PP_SPECS_VECTORS_DIR. A new case starts here: this library's test writes the file
# (PP_WRITE_<NAME>_VECTORS=<path>), a pp-specs pull request adds it, and the pin below moves to that release.

set(_pp_specs_pin v1.0.0)
set(PP_SPECS_GIT_TAG "" CACHE STRING "pp-specs tag the tests read the vectors at (empty: the pin, ${_pp_specs_pin})")
set(PP_SPECS_SOURCE_DIR "" CACHE PATH "Optional local pp-specs checkout (overrides the tag)")

if(PP_SPECS_SOURCE_DIR)
  set(_pp_specs_src "${PP_SPECS_SOURCE_DIR}")
else()
  if(PP_SPECS_GIT_TAG)
    set(_pp_specs_tag "${PP_SPECS_GIT_TAG}")
  else()
    set(_pp_specs_tag "${_pp_specs_pin}")
  endif()
  include(FetchContent)
  FetchContent_Declare(pp_specs GIT_REPOSITORY https://github.com/people-post/pp-specs.git GIT_TAG ${_pp_specs_tag})
  # No CMakeLists.txt there: MakeAvailable only fetches it.
  FetchContent_MakeAvailable(pp_specs)
  FetchContent_GetProperties(pp_specs SOURCE_DIR _pp_specs_src)
endif()

set(PP_SPECS_VECTORS_DIR "${_pp_specs_src}/vectors")
if(NOT EXISTS "${PP_SPECS_VECTORS_DIR}/manifest.json")
  message(FATAL_ERROR "pp-specs at ${_pp_specs_src} has no vectors/manifest.json: not a pp-specs checkout?")
endif()
