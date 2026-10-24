// vite.config.js
import { defineConfig, loadEnv } from 'vite'
import vue from '@vitejs/plugin-vue'
import { viteExternalsPlugin } from 'vite-plugin-externals'
import { compression } from 'vite-plugin-compression2'

export default defineConfig(({ mode }) => {
  // eslint-disable-next-line no-undef
  const env = loadEnv(mode, process.cwd(), '')
  return {
    plugins: [vue({ isProduction: true }), viteExternalsPlugin({ vue: 'Vue' }), compression({ deleteOriginalAssets: true })],
    define: {
      'process.env.NODE_ENV': '"production"'
    },
    build: {
      cssCodeSplit: false,
      lib: {
        entry: env.VITE_PLUGIN_ENTRY,
        fileName: env.VITE_PLUGIN_FILENAME,
        formats: ['es']
      },
      rollupOptions: {
        external: ['vue'],
        output: {
          globals: {
            vue: 'Vue'
          }
        }
      }
    }
  }
})
