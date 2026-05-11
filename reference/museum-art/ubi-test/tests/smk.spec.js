import { test, expect } from '@playwright/test';
import { expectArtworkImage720, listCollectionsAndWait } from './helpers.js';

test.describe('SMK', () => {
  test('A → B: landing → museum page → list collections', async ({ page }) => {
    await page.goto('/');
    await page.locator('a.card', { hasText: /SMK/i }).click();
    await expect(page).toHaveURL(/#\/source\/smk$/);
    await expect(page.locator('h1')).toContainText(/SMK/);

    await listCollectionsAndWait(page);
    const firstCount = page.locator('ul.collections li .count').first();
    await expect(firstCount).toBeVisible();
    const items = await page.locator('ul.collections li').count();
    expect(items).toBeGreaterThan(0);
  });

  test('C: collection page lists artworks with pagination', async ({ page }) => {
    await page.goto('/#/source/smk');
    await listCollectionsAndWait(page);
    await page.locator('ul.collections li a').first().click();
    await expect(page).toHaveURL(/\/collections\//);

    await expect(page.locator('ol.artworks li').first()).toBeVisible();
    const count = await page.locator('ol.artworks li').count();
    expect(count).toBeGreaterThan(0);
    expect(count).toBeLessThanOrEqual(24);

    // Pager next link should exist (top SMK collections all have >24 items).
    await expect(page.locator('.pager a', { hasText: /next/i })).toBeVisible();
  });

  test('D: artwork page renders IIIF image at !720,720', async ({ page }) => {
    await page.goto('/#/source/smk');
    await listCollectionsAndWait(page);
    await page.locator('ul.collections li a').first().click();
    await page.locator('ol.artworks li a').first().waitFor();
    await page.locator('ol.artworks li a').first().click();
    await expect(page).toHaveURL(/\/artwork\//);

    const dims = await expectArtworkImage720(page);
    // IIIF URL link should point to iip.smk.dk
    await expect(page.locator('.iiif-url a')).toHaveAttribute('href', /iip\.smk\.dk.*\/full\/!720,720\//);
    test.info().annotations.push({ type: 'image-dims', description: `${dims.w}×${dims.h}` });
  });
});
