import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Proxy /webui/api requests to the running aviand node during development.
// Set AVIAN_NODE_URL in your environment or .env.local, e.g.:
//   AVIAN_NODE_URL=http://127.0.0.1:23000
const nodeUrl = process.env.AVIAN_NODE_URL ?? 'http://127.0.0.1:23000'

export default defineConfig({
  plugins: [react()],
  base: '/webui/',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    rollupOptions: {
      output: {
        // Stable filenames for embedding (no content hashes needed)
        entryFileNames: 'assets/main.js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name][extname]',
      },
    },
  },
  server: {
    proxy: {
      '/webui/api': {
        target: nodeUrl,
        changeOrigin: true,
      },
    },
  },
})
