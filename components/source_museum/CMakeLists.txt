# Layer: sources. The museums: channels of artworks from public collections,
# over IIIF and similar image servers. One loop lists a channel for every
# museum (source_museum.c); what each museum's answers mean is in its own
# pure file.
idf_component_register(
    SRCS "museum_artic.c"
         "museum_cma.c"
         "museum_ham.c"
         "museum_mia.c"
         "museum_rijks.c"
         "museum_si.c"
         "museum_smk.c"
         "museum_util.c"
         "museum_vam.c"
         "museum_wellcome.c"
         "source_museum.c"
    INCLUDE_DIRS "include"
    REQUIRES p3a_library
    PRIV_REQUIRES esp_app_format json p3a_fetch p3a_settings p3a_util
)
