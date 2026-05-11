import { test, expect } from '@playwright/test';
import { expectArtworkImage720, listCollectionsAndWait } from './helpers.js';

// Rijksmuseum is the slowest source: listCollections fires ~193 parallel
// count requests, and listArtworks hydrates per-row HMO metadata. Bumped
// timeouts accordingly.

test.describe('Rijksmuseum', () => {
  test('A → B: landing → museum page → list collections', async ({ page }) => {
    await page.goto('/');
    await page.locator('a.card', { hasText: /Rijksmuseum/i }).click();
    await expect(page).toHaveURL(/#\/source\/rijksmuseum$/);
    await expect(page.locator('h1')).toContainText(/Rijksmuseum/);

    await listCollectionsAndWait(page, { timeout: 120_000 });
    // Expect close to 193 sets (some get filtered out if their count is 0).
    const items = await page.locator('ul.collections li').count();
    expect(items).toBeGreaterThan(50);
  });

  test('C: pre-baked sets and artwork listing work', async ({ page }) => {
    await page.goto('/#/source/rijksmuseum');
    await listCollectionsAndWait(page, { timeout: 120_000 });

    // Pick a set known to have many items: prefer one with the largest count.
    await page.locator('ul.collections li a').first().click();
    await expect(page).toHaveURL(/\/collections\//);

    await expect(page.locator('ol.artworks li').first()).toBeVisible({ timeout: 90_000 });
    const count = await page.locator('ol.artworks li').count();
    expect(count).toBeGreaterThan(0);
    expect(count).toBeLessThanOrEqual(24);
  });

  test('D: IIIF chain resolves, image renders at !720,720', async ({ page }) => {
    await page.goto('/#/source/rijksmuseum');
    await listCollectionsAndWait(page, { timeout: 120_000 });
    await page.locator('ul.collections li a').first().click();
    await page.locator('ol.artworks li a').first().waitFor({ timeout: 90_000 });
    await page.locator('ol.artworks li a').first().click();
    await expect(page).toHaveURL(/\/artwork\//);

    const dims = await expectArtworkImage720(page);
    await expect(page.locator('.iiif-url a')).toHaveAttribute('href', /iiif\.micr\.io.*\/full\/!720,720\//);
    test.info().annotations.push({ type: 'image-dims', description: `${dims.w}×${dims.h}` });
  });
});
