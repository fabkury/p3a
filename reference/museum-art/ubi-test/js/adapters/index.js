import { SmkAdapter } from './smk.js';
import { RijksmuseumAdapter } from './rijksmuseum.js';
import { VamAdapter } from './vam.js';
import { ArticAdapter } from './artic.js';
import { WellcomeAdapter } from './wellcome.js';

const REGISTRY = [SmkAdapter, RijksmuseumAdapter, VamAdapter, ArticAdapter, WellcomeAdapter];

const _instances = new Map();

export function getAdapter(id) {
  const Cls = REGISTRY.find(C => C.id === id);
  if (!Cls) throw new Error(`Unknown source: ${id}`);
  if (!_instances.has(id)) _instances.set(id, new Cls());
  return _instances.get(id);
}

export function listSources() {
  return REGISTRY.map(C => ({ id: C.id, displayName: C.displayName, axes: C.axes }));
}
