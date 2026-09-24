# All test wiring lives here; the top-level CMakeLists just adds this
# subdirectory when SQUEEZE2RAOP2_BUILD_TESTS is on.

# --- unit tests: one binary, cases self-registered via SQ2_TEST() ----------
add_executable(squeeze2raop2_tests
  unit/test_main.cpp
  unit/test_util.cpp
  unit/test_config.cpp
  unit/test_persistence.cpp
  unit/test_wav_sink.cpp
  unit/test_slimproto_wire.cpp
  unit/test_transport.cpp
  unit/test_volume_map.cpp
  unit/test_device_registry.cpp
  unit/test_byte_order.cpp
  unit/test_pcm_decoder.cpp
  unit/test_decoder.cpp
  unit/test_aac_decoder.cpp
  unit/test_ogg_decoder.cpp
  unit/test_opus_decoder.cpp
  unit/test_ring_telemetry.cpp
  unit/test_icy_meta.cpp
  unit/test_stream_counters.cpp
  unit/test_session_exit.cpp
  unit/test_mdns_names.cpp
  unit/test_gain.cpp
  unit/test_log.cpp)
target_link_libraries(squeeze2raop2_tests PRIVATE squeeze2raop2_core)
target_include_directories(squeeze2raop2_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/framework)
squeeze2raop2_target_settings(squeeze2raop2_tests)

add_test(NAME unit COMMAND squeeze2raop2_tests)
set_tests_properties(unit PROPERTIES LABELS unit)

# Per-suite cases for `ctest -j` / `-R`, all sharing the one binary.
foreach(s util config persistence wav wire transport volume registry
          byte_order pcm decoder ring_telemetry icy counters session_exit mdns_names gain log aac ogg opus)
  add_test(NAME unit_${s} COMMAND squeeze2raop2_tests --filter ${s})
  set_tests_properties(unit_${s} PROPERTIES LABELS unit)
endforeach()

# --- integration tests: spawn the bridge against the fake LMS ---------------
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  set(SQUEEZE2RAOP2_INTEGRATION_TESTS
    single_player_stream two_player_registration queue_advance
    skip_ahead replay_gain power_off codc_stream codc_unsupported
    watchdog fade fade_out pause_resume)
  # Codec-specific scenarios only when that decoder is built: they assert the
  # matching HELO cap and decode path, which do not exist in a feature-off build.
  if(SQUEEZE2RAOP2_WITH_AAC)
    list(APPEND SQUEEZE2RAOP2_INTEGRATION_TESTS aac_adts_stream aac_mp4_stream)
  endif()
  if(SQUEEZE2RAOP2_WITH_OGG)
    list(APPEND SQUEEZE2RAOP2_INTEGRATION_TESTS ogg_vorbis_stream)
  endif()
  if(SQUEEZE2RAOP2_WITH_OPUS)
    list(APPEND SQUEEZE2RAOP2_INTEGRATION_TESTS opus_stream)
  endif()
  # Direct HTTPS handover is TLS-specific: the bridge only advertises CanHTTPS
  # and the fake LMS only serves TLS when the feature is built in.
  if(SQUEEZE2RAOP2_WITH_HTTPS)
    list(APPEND SQUEEZE2RAOP2_INTEGRATION_TESTS https_stream)
  endif()
  foreach(scenario ${SQUEEZE2RAOP2_INTEGRATION_TESTS})
    add_test(NAME ${scenario}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/${scenario}.py
                     $<TARGET_FILE:squeeze2raop2>)
  endforeach()
  set_tests_properties(${SQUEEZE2RAOP2_INTEGRATION_TESTS}
    PROPERTIES LABELS integration TIMEOUT 60)
else()
  message(STATUS "Python3 not found; integration tests disabled")
endif()