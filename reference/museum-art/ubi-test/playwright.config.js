import { defineConfig } from '@playwright/test';

// Tests drive a real browser against the static UBI-test site, which then
// hits the live museum APIs. Network latency dominates; timeouts are
// generous, especially for Rijksmuseum (193 parallel set-count fetches +
// per-row HMO hydration on artwork listing pages).

export default defineConfig({
  testDir: './tests',
  timeout: 180_000,
  expect: { timeout: 60_000 },
  fullyParallel: false,        // serial by default — easier to read live-API output
  workers: 1,
  reporter: [['list']],
  use: {
    baseURL: 'http://localhost:8765',
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
    actionTimeout: 30_000,
    navigationTimeout: 30_000,
  },
  webServer: {
    command: 'python -m http.server 8765',
    url: 'http://localhost:8765/',
    reuseExistingServer: !process.env.CI,
    timeout: 30_000,
  },
  projects: [
    { name: 'chromium', use: { browserName: 'chromium' } },
  ],
});
