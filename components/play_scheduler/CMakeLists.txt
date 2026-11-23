# Layer: player. Decides what plays next: which channel of the active
# playset, and which of its artworks. So far the arithmetic (sched_core) and
# the playsets; the sources and the commands arrive with the rest of M2.3.
idf_component_register(
    SRCS "playset.c"
         "playset_store.c"
         "sched_core.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES p3a_storage p3a_util
)
