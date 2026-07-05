add_subdirectory(tools)

# ── Harness E2E (tarea A1 de la Fase A) ──────────────────────────────
# Escenario real relay -> caster -> rover -> compare, 100% local (sin
# depender de red externa/credenciales, a diferencia de la validacion
# manual que se hizo contra SNIP). Ver test/cases/t_relay_rover_compare.sh
# y docs/FEATURE_improvements_FASE_A_20260704.md §C.1.
add_test(
  NAME relay_rover_compare_e2e
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cases/t_relay_rover_compare.sh
)
set_tests_properties(relay_rover_compare_e2e PROPERTIES
  TIMEOUT 60
  ENVIRONMENT
    "NTRIPCASTER_BIN=$<TARGET_FILE:ntripcaster>;RELAY_BIN=$<TARGET_FILE:ntrip_source_relay>;ROVER_BIN=$<TARGET_FILE:ntrip_rover_client>;COMPARE_BIN=$<TARGET_FILE:ntrip_capture_compare>;MK_RTCM_PY=${CMAKE_CURRENT_SOURCE_DIR}/helpers/mk_rtcm.py"
)
