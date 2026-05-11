// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Adapter registry for the museum browse modal. Each adapter exposes the
// same surface (id, displayName, axes, listCollections, listArtworks,
// thumbnailUrl); the modal dispatches through it without per-museum
// branches.
//
// The id MUST match the corresponding entry in
// components/art_institution/art_institution.c's ART_INSTITUTION_MUSEUMS
// table, because the device parses the channel `name` field as
// `{museum_id}:{axis}`.

import { ArticAdapter } from './artic.js';
import { RijksmuseumAdapter } from './rijksmuseum.js';

const ADAPTERS = [
    new ArticAdapter(),
    new RijksmuseumAdapter(),
];

export function listAdapters() {
    return ADAPTERS;
}

export function getAdapter(museumId) {
    for (let i = 0; i < ADAPTERS.length; i++) {
        if (ADAPTERS[i].id === museumId) return ADAPTERS[i];
    }
    return null;
}
