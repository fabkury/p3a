# Layer: util. No dependencies on other project components.
#
# Files with no IDF includes (p3a_crc32, p3a_err, p3a_event_table, p3a_job_table,
# p3a_mem_pressure, p3a_task_policy, p3a_version) are also compiled by the
# host tests in tests/host.
idf_component_register(
    SRCS "p3a_clock.c"
         "p3a_crc32.c"
         "p3a_err.c"
         "p3a_event.c" "p3a_event_table.c"
         "p3a_job.c" "p3a_job_table.c"
         "p3a_mem.c" "p3a_mem_pressure.c"
         "p3a_task.c" "p3a_task_policy.c"
         "p3a_version.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer
)
