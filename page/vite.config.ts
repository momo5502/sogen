import path from "path";
import tailwindcss from "@tailwindcss/vite";
import { defineConfig } from "vite";
import { VitePWA } from "vite-plugin-pwa";
import react from "@vitejs/plugin-react";
import { RuntimeCaching } from "workbox-build";

const mb = 1024 ** 2;

function generateExternalCache(
  pattern: string | RegExp,
  name: string,
): RuntimeCaching {
  return {
    urlPattern: pattern,
    handler: "CacheFirst",
    options: {
      cacheName: name,
      expiration: {
        maxEntries: 10,
        maxAgeSeconds: 60 * 60 * 24 * 365, // <== 365 days
      },
      cacheableResponse: {
        statuses: [0, 200],
      },
    },
  };
}

// https://vite.dev/config/
export default defineConfig({
  plugins: [
    react(),
    tailwindcss(),
    VitePWA({
      registerType: "autoUpdate",
      manifest: {
        theme_color: "#0279E8",
        background_color: "#141416",
      },
      workbox: {
        maximumFileSizeToCacheInBytes: 100 * mb,
        globPatterns: ["**/*.{js,css,html,woff,woff2}"],
        runtimeCaching: [
          generateExternalCache(
            /^https:\/\/momo5502\.com\/.*/i,
            "momo5502-cache",
          ),
          generateExternalCache(
            /^https:\/\/img\.youtube\.com\/.*/i,
            "youtube-img-cache",
          ),
        ],
      },
    }),
  ],
  resolve: {
    alias: {
      "@": path.resolve(__dirname, "./src"),
    },
  },
  define: {
    "import.meta.env.VITE_BUILD_TIME": JSON.stringify(Date.now()),
  },
});
