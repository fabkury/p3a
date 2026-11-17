# Layer: util. No dependencies on other project components.
#
# Files with no IDF includes (p3a_version, p3a_err, p3a_task_policy) are also
# compiled by the host tests in tests/host.
idf_component_register(
    SRCS "p3a_err.c" "p3a_task.c" "p3a_task_policy.c" "p3a_version.c"
    INCLUDE_DIRS "include"
)
