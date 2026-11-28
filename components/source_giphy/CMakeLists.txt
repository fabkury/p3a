# Layer: sources. Giphy: trending and search channels of GIFs. It lists a
# channel and says where each GIF's bytes are; the content task does the
# rest (when to list, the downloads, the waits after a refusal).
idf_component_register(
    SRCS "giphy_parse.c"
         "source_giphy.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "credit/powered-by-giphy-on-dark.p3aimg" "credit/powered-by-giphy-on-light.p3aimg"
    REQUIRES p3a_library
    PRIV_REQUIRES json nvs_flash p3a_board p3a_fetch p3a_settings p3a_util
)
