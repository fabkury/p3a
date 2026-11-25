# Layer: sources. A pin list played as a channel: the list's records, each
# still its own source's artwork.
idf_component_register(
    SRCS "source_pinned.c"
    INCLUDE_DIRS "include"
    REQUIRES p3a_library
    PRIV_REQUIRES p3a_util
)
