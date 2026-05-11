import { expect } from '@playwright/test';

// Wait for the displayed artwork's <img> to be fully decoded and assert
// that one of its dimensions is exactly 720px (the IIIF !720,720 contract).
export async function expectArtworkImage720(page) {
  const img = page.locator('.image img');
  await expect(img).toBeVisible();
  await page.waitForFunction(() => {
    const i = document.querySelector('.image img');
    return i && i.complete && i.naturalWidth > 0;
  }, null, { timeout: 60_000 });
  const { w, h } = await img.evaluate(el => ({ w: el.naturalWidth, h: el.naturalHeight }));
  expect.soft(Math.max(w, h)).toBe(720);
  return { w, h };
}

// Click the "List collections" button and wait for the collections list
// to populate (or for an error panel — fail loudly if so).
export async function listCollectionsAndWait(page, { timeout = 90_000 } = {}) {
  await page.getByRole('button', { name: /list collections/i }).click();
  await expect(page.locator('ul.collections li').first()).toBeVisible({ timeout });
}
