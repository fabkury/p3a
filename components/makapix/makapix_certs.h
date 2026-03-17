// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file makapix_certs.h
 * @brief Makapix provisioning CA certificate accessor
 */

#pragma once

/**
 * @brief Get CA certificate for HTTPS provisioning endpoint
 * 
 * @return Pointer to PEM-formatted certificate string
 */
const char* makapix_get_provision_ca_cert(void);

