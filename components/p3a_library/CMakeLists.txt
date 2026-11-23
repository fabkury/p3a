# Layer: library. What p3a knows about artworks: the record, the index of a
# channel's records, and the blob store that holds their bytes. Everything
# here is logic over bytes and files; nothing draws and nothing talks to a
# network.
idf_component_register(
    SRCS "p3a_blob.c"
         "p3a_blob_evict.c"
         "p3a_evict.c"
         "p3a_index.c"
         "p3a_index_file.c"
         "p3a_record.c"
         "p3a_source.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls p3a_storage p3a_util
)
