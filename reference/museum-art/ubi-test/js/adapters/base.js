// Abstract Adapter — the unified browsing interface contract.
//
// Each museum subclass adapts its native API behind these methods so the
// rest of the app can treat all sources uniformly.

export class Adapter {
  static get id() { return 'unknown'; }
  static get displayName() { return 'Unknown'; }
  // null, OR an array of axis names. When set, the museum page renders a
  // sub-selector and `axis` is passed into listCollections / listArtworks.
  static get axes() { return null; }

  // List collections.  axis is consulted only when static axes !== null.
  // Returns: [{ id, label, count? }]
  async listCollections(/* { axis } = {} */) { throw new Error('abstract'); }

  // Range-style listing: items [offset, offset+rows).
  // Returns: { items: [{ id, title, artist?, date? }], total }
  async listArtworks(/* collectionId, { offset, rows, axis } = {} */) { throw new Error('abstract'); }

  // Resolve one artwork to its display payload.
  // Returns: { title, artist?, date?, imageUrl }
  // imageUrl is the IIIF /full/!720,720/0/default.jpg URL.
  async getArtwork(/* artworkId */) { throw new Error('abstract'); }
}
