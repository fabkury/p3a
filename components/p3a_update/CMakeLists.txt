# Layer: net. Firmware updates from GitHub Releases: looking for one,
# installing it into the other app slot when the owner asks, the trial of a
# newly installed firmware, going back.
#
# update_release.c and update_policy.c have no IDF includes and are also
# compiled by the host tests.
idf_component_register(
    SRCS "p3a_update.c"
         "update_api.c"
         "update_install.c"
         "update_policy.c"
         "update_release.c"
    INCLUDE_DIRS "include"
    REQUIRES p3a_util
    PRIV_REQUIRES app_update esp_app_format esp_partition esp_timer json
                  p3a_fetch p3a_net p3a_settings p3a_storage
)
