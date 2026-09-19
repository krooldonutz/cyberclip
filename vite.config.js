import { fileURLToPath, URL } from 'node:url';
import { cp, copyFile, mkdir } from 'node:fs/promises';
import { defineConfig } from 'vite';

const repositoryRoot = fileURLToPath(new URL('.', import.meta.url));

function emitPwaAssets() {
  const sourceRoot = fileURLToPath(new URL('./public', import.meta.url));
  const outputRoot = fileURLToPath(new URL('./dist', import.meta.url));
  return {
    name: 'emit-pwa-assets',
    enforce: 'post',
    transformIndexHtml(html) {
      return html
        .replace(/<link rel="manifest"[^>]*>/, '<link rel="manifest" href="/manifest.json">')
        .replace(
          /<link rel="icon"[^>]*>/,
          '<link rel="icon" href="/icons/icon.svg" type="image/svg+xml">',
        );
    },
    async closeBundle() {
      await mkdir(`${outputRoot}/icons`, { recursive: true });
      await Promise.all([
        copyFile(`${sourceRoot}/manifest.json`, `${outputRoot}/manifest.json`),
        copyFile(`${sourceRoot}/service-worker.js`, `${outputRoot}/service-worker.js`),
        cp(`${sourceRoot}/icons`, `${outputRoot}/icons`, { recursive: true }),
      ]);
    },
  };
}

export default defineConfig({
  root: 'public',
  publicDir: false,
  plugins: [emitPwaAssets()],
  resolve: {
    alias: {
      '/src': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  build: {
    outDir: '../dist',
    emptyOutDir: true,
  },
  test: {
    root: repositoryRoot,
    environment: 'jsdom',
    include: ['src/**/*.test.js'],
  },
});
