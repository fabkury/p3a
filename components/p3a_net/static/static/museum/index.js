// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors
//
// Adapter registry for the museum browse modal. Each adapter exposes the
// same surface (id, displayName, axes, listCollections, listArtworks,
// thumbnailUrl); the modal dispatches through it without per-museum
// branches.
//
// An adapter's id and its axes' names MUST be those of the museum's table
// in components/source_museum (museum_<id>.c): a museum channel's kind is
// `{museum_id}:{axis}`. tests/web/test_museum.mjs compares the two.

import { ArticAdapter } from './artic.js';
import { RijksmuseumAdapter } from './rijksmuseum.js';
import { VamAdapter } from './vam.js';
import { WellcomeAdapter } from './wellcome.js';
import { SmkAdapter } from './smk.js';
import { HamAdapter } from './ham.js';
import { SmithsonianAdapter } from './smithsonian.js';
import { CmaAdapter } from './cma.js';
import { MiaAdapter } from './mia.js';

const ADAPTERS = [
    new ArticAdapter(),
    new RijksmuseumAdapter(),
    new VamAdapter(),
    new WellcomeAdapter(),
    new SmkAdapter(),
    new HamAdapter(),
    new SmithsonianAdapter(),
    new CmaAdapter(),
    new MiaAdapter(),
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
