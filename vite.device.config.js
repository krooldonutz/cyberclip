import { fileURLToPath, URL } from 'node:url';
import { defineConfig } from 'vite';

export default defineConfig({
  root: 'public',
  publicDir: false,
  resolve: {
    alias: {
      '/src': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  build: {
    outDir: '../dist-device',
    emptyOutDir: true,
    rollupOptions: {
      input: fileURLToPath(new URL('./public/device.html', import.meta.url)),
    },
  },
});
