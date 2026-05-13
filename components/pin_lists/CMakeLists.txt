idf_component_register(
    SRCS
        "pin_lists.c"
        "pin_lists_state.c"
        "pin_lists_manifest.c"
        "pin_lists_order.c"
        "pin_lists_entry.c"
        "pin_lists_copy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        esp_common
        p3a_core
        json
)
