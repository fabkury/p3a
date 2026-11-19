# Layer: player. Decides what plays next: which channel of the active
# playset, and which of its artworks. So far only the arithmetic
# (sched_core); the playsets, the sources and the commands arrive with the
# rest of M2.3.
idf_component_register(
    SRCS "sched_core.c"
)
