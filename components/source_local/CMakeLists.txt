# Layer: sources. The user's own files in the animations folder of the SD
# card, listed in place.
idf_component_register(
    SRCS "source_local.c"
         "source_local_names.c"
    INCLUDE_DIRS "include"
    REQUIRES p3a_library
    PRIV_REQUIRES esp_timer p3a_storage p3a_util
)
