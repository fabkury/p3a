// SPDX-License-Identifier: Apache-2.0
#include "p3a_err.h"

#include <stddef.h>

static const char *const s_names[] = {
    [P3A_ERR_NOT_PLAYABLE - P3A_ERR_BASE] = "NOT_PLAYABLE",
    [P3A_ERR_CORRUPT - P3A_ERR_BASE] = "CORRUPT",
    [P3A_ERR_MISSING - P3A_ERR_BASE] = "MISSING",
    [P3A_ERR_RATE_LIMITED - P3A_ERR_BASE] = "RATE_LIMITED",
    [P3A_ERR_AUTH - P3A_ERR_BASE] = "AUTH",
    [P3A_ERR_NO_KEY - P3A_ERR_BASE] = "NO_KEY",
    [P3A_ERR_SD_FAILED - P3A_ERR_BASE] = "SD_FAILED",
};

// A code added to the enum without a name here fails to compile.
_Static_assert(sizeof(s_names) / sizeof(s_names[0]) == P3A_ERR_END_ - P3A_ERR_BASE,
               "p3a_err: every code needs a name");

bool p3a_err_is_p3a(esp_err_t err)
{
    return err > P3A_ERR_BASE && err < P3A_ERR_END_;
}

const char *p3a_err_name(esp_err_t err)
{
    return p3a_err_is_p3a(err) ? s_names[err - P3A_ERR_BASE] : NULL;
}

const char *p3a_err_to_name(esp_err_t err)
{
    const char *name = p3a_err_name(err);
    return name ? name : esp_err_to_name(err);
}
