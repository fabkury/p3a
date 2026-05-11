import { test, expect } from '@playwright/test';
import { expectArtworkImage720, listCollectionsAndWait } from './helpers.js';

// Wellcome's aggregations come back in one shot for all four axes, so axis
// switching is fast. IIIF host is iiif.wellcomecollection.org.

test.describe('Wellcome Collection', () => {
  test('A → B: museum page, axis sub-selector, lists collections with counts', async ({ page }) => {
    await page.goto('/');
    await page.locator('a.card', { hasText: /Wellcome/i }).click();
    await expect(page).toHaveURL(/#\/source\/wellcome$/);
    await expect(page.locator('h1')).toContainText(/Wellcome/i);

    const axisSelect = page.locator('#axis');
    await expect(axisSelect).toBeVisible();
    const axisOptions = await axisSelect.locator('option').allTextContents();
    expect(axisOptions).toEqual(expect.arrayContaining(['workType', 'genres', 'subjects', 'contributors']));

    await listCollectionsAndWait(page);
    await expect(page.locator('ul.collections li .count').first()).toBeVisible();
  });

  test('B: switching to "genres" yields a different list', async ({ page }) => {
    await page.goto('/#/source/wellcome');
    await listCollectionsAndWait(page);
    const workTypeLabels = await page.locator('ul.collections li a').allTextContents();

    await page.selectOption('#axis', 'genres');
    await page.getByRole('button', { name: /list collections/i }).click();
    await expect(page.locator('ul.collections li').first()).toBeVisible();
    const genreLabels = await page.locator('ul.collections li a').allTextContents();
    expect(genreLabels.length).toBeGreaterThan(0);
    expect(genreLabels).not.toEqual(workTypeLabels);
  });

  test('C: collection link carries axis through to the listing', async ({ page }) => {
    await page.goto('/#/source/wellcome');
    await page.selectOption('#axis', 'genres');
    await page.getByRole('button', { name: /list collections/i }).click();
    await expect(page.locator('ul.collections li').first()).toBeVisible();

    await page.locator('ul.collections li a').first().click();
    await expect(page).toHaveURL(/axis=genres/);
    await expect(page.locator('ol.artworks li').first()).toBeVisible({ timeout: 60_000 });
    const count = await page.locator('ol.artworks li').count();
    expect(count).toBeGreaterThan(0);
  });

  test('D: artwork page renders IIIF image at !720,720', async ({ page }) => {
    await page.goto('/#/source/wellcome');
    await listCollectionsAndWait(page);
    await page.locator('ul.collections li a').first().click();
    await page.locator('ol.artworks li a').first().waitFor({ timeout: 60_000 });
    await page.locator('ol.artworks li a').first().click();
    await expect(page).toHaveURL(/\/artwork\//);

    const dims = await expectArtworkImage720(page);
    await expect(page.locator('.iiif-url a')).toHaveAttribute('href', /iiif\.wellcomecollection\.org\/image\/.*\/full\/!720,720\//);
    test.info().annotations.push({ type: 'image-dims', description: `${dims.w}×${dims.h}` });
  });
});
