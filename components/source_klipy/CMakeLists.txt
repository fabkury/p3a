# Layer: sources. Klipy: channels of GIFs and stickers. It lists a channel
# and says where each item's bytes are; the content task does the rest.
idf_component_register(
    SRCS "klipy_parse.c"
         "source_klipy.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "credit/powered-by-klipy-on-dark.p3aimg" "credit/powered-by-klipy-on-light.p3aimg"
    REQUIRES p3a_library
    PRIV_REQUIRES json p3a_board p3a_fetch p3a_settings p3a_util
)
