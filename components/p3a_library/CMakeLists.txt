# Layer: library. What p3a knows about artworks: the record, the index of a
# channel's records, and (as M2.2 goes on) the blob store that holds their
# bytes. Everything here is logic over bytes and files; nothing draws and
# nothing talks to a network.
idf_component_register(
    SRCS "p3a_index.c"
         "p3a_record.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES p3a_util
)
