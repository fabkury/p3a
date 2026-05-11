import { test, expect } from '@playwright/test';
import { expectArtworkImage720, listCollectionsAndWait } from './helpers.js';

test.describe('V&A', () => {
  test('A → B: museum page renders axis sub-selector + lists collections', async ({ page }) => {
    await page.goto('/');
    await page.locator('a.card', { hasText: /V&A/i }).click();
    await expect(page).toHaveURL(/#\/source\/vam$/);
    await expect(page.locator('h1')).toContainText(/V&A/);

    // Sub-selector must exist with all three axes.
    const axisSelect = page.locator('#axis');
    await expect(axisSelect).toBeVisible();
    const axisOptions = await axisSelect.locator('option').allTextContents();
    expect(axisOptions).toEqual(expect.arrayContaining(['collection', 'category', 'venue']));

    // Default axis → list collections.
    await listCollectionsAndWait(page);
    const items = await page.locator('ul.collections li').count();
    expect(items).toBeGreaterThan(0);
  });

  test('B: switching axis to "category" produces a different list', async ({ page }) => {
    await page.goto('/#/source/vam');
    await listCollectionsAndWait(page);
    const collectionLabels = await page.locator('ul.collections li a').allTextContents();

    // Switch axis to "category" and re-list.
    await page.selectOption('#axis', 'category');
    await page.getByRole('button', { name: /list collections/i }).click();
    await expect(page.locator('ul.collections li').first()).toBeVisible();
    const categoryLabels = await page.locator('ul.collections li a').allTextContents();

    expect(categoryLabels.length).toBeGreaterThan(0);
    expect(categoryLabels).not.toEqual(collectionLabels);
  });

  test('C: collection link carries axis through to the listing', async ({ page }) => {
    await page.goto('/#/source/vam');
    await page.selectOption('#axis', 'category');
    await page.getByRole('button', { name: /list collections/i }).click();
    await expect(page.locator('ul.collections li').first()).toBeVisible();

    await page.locator('ul.collections li a').first().click();
    await expect(page).toHaveURL(/axis=category/);
    await expect(page.locator('ol.artworks li').first()).toBeVisible();
    const count = await page.locator('ol.artworks li').count();
    expect(count).toBeGreaterThan(0);
  });

  test('D: artwork page renders IIIF image at !720,720 (framemark host)', async ({ page }) => {
    await page.goto('/#/source/vam');
    await listCollectionsAndWait(page);
    await page.locator('ul.collections li a').first().click();
    await page.locator('ol.artworks li a').first().waitFor();
    await page.locator('ol.artworks li a').first().click();
    await expect(page).toHaveURL(/\/artwork\//);

    const dims = await expectArtworkImage720(page);
    await expect(page.locator('.iiif-url a')).toHaveAttribute('href', /framemark\.vam\.ac\.uk.*\/full\/!720,720\//);
    test.info().annotations.push({ type: 'image-dims', description: `${dims.w}×${dims.h}` });
  });
});
