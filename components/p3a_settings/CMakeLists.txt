# Layer: storage. The settings store (everything a user can change, typed,
# generated from settings.json) and the counted reboot-recovery helper.
#
# p3a_settings_model.c and the generated table have no IDF includes and are
# also compiled by the host tests in tests/host.

# The table and the typed struct are generated at configure time, so the
# header exists before any component that includes it is compiled. Editing
# the schema or the generator re-runs the configure step.
set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(schema "${CMAKE_CURRENT_LIST_DIR}/settings.json")
set(generator "${CMAKE_CURRENT_LIST_DIR}/../../tools/gen_settings.py")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    execute_process(
        COMMAND ${python} "${generator}" "${schema}" "${gen_dir}"
        RESULT_VARIABLE gen_result)
    if(NOT gen_result EQUAL 0)
        message(FATAL_ERROR "settings.json was rejected; see the message above")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${schema}" "${generator}")
endif()

idf_component_register(
    SRCS "p3a_recovery.c"
         "p3a_settings.c"
         "p3a_settings_model.c"
         "${gen_dir}/p3a_settings_gen.c"
    INCLUDE_DIRS "include" "${gen_dir}"
    REQUIRES json
    PRIV_REQUIRES nvs_flash p3a_storage p3a_util
)
