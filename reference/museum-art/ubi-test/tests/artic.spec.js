import { test, expect } from '@playwright/test';
import { expectArtworkImage720, listCollectionsAndWait } from './helpers.js';

// AIC has six axes; default lists `departments` (16 buckets, count probes
// fan out in parallel). Be a little more patient on the collections wait.

test.describe('Art Institute of Chicago', () => {
  test('A → B: museum page, axis sub-selector, lists collections', async ({ page }) => {
    await page.goto('/');
    await page.locator('a.card', { hasText: /Art Institute/i }).click();
    await expect(page).toHaveURL(/#\/source\/artic$/);
    await expect(page.locator('h1')).toContainText(/Art Institute/i);

    const axisSelect = page.locator('#axis');
    await expect(axisSelect).toBeVisible();
    const axisOptions = await axisSelect.locator('option').allTextContents();
    expect(axisOptions).toEqual(expect.arrayContaining([
      'departments', 'classifications', 'subjects', 'themes', 'galleries', 'artwork-types',
    ]));
    expect(axisOptions).not.toContain('exhibitions');  // intentionally hidden

    await listCollectionsAndWait(page, { timeout: 90_000 });
    await expect(page.locator('ul.collections li .count').first()).toBeVisible();
  });

  test('B: switching to "themes" yields a different list with counts', async ({ page }) => {
    await page.goto('/#/source/artic');
    await listCollectionsAndWait(page, { timeout: 90_000 });
    const departmentLabels = await page.locator('ul.collections li a').allTextContents();

    await page.selectOption('#axis', 'themes');
    await page.getByRole('button', { name: /list collections/i }).click();
    await expect(page.locator('ul.collections li').first()).toBeVisible({ timeout: 90_000 });
    const themeLabels = await page.locator('ul.collections li a').allTextContents();
    expect(themeLabels.length).toBeGreaterThan(0);
    expect(themeLabels).not.toEqual(departmentLabels);
  });

  test('C: collection link carries axis through to the listing', async ({ page }) => {
    await page.goto('/#/source/artic');
    await page.selectOption('#axis', 'themes');
    await page.getByRole('button', { name: /list collections/i }).click();
    await expect(page.locator('ul.collections li').first()).toBeVisible({ timeout: 90_000 });

    await page.locator('ul.collections li a').first().click();
    await expect(page).toHaveURL(/axis=themes/);
    await expect(page.locator('ol.artworks li').first()).toBeVisible();
    const count = await page.locator('ol.artworks li').count();
    expect(count).toBeGreaterThan(0);
  });

  test('D: artwork page renders IIIF image at !720,720', async ({ page }) => {
    await page.goto('/#/source/artic');
    await listCollectionsAndWait(page, { timeout: 90_000 });
    await page.locator('ul.collections li a').first().click();
    await page.locator('ol.artworks li a').first().waitFor();
    await page.locator('ol.artworks li a').first().click();
    await expect(page).toHaveURL(/\/artwork\//);

    const dims = await expectArtworkImage720(page);
    await expect(page.locator('.iiif-url a')).toHaveAttribute('href', /artic\.edu\/iiif\/2\/.*\/full\/!720,720\//);
    test.info().annotations.push({ type: 'image-dims', description: `${dims.w}×${dims.h}` });
  });
});
