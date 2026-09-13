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
      injectRegister: false,
      registerType: "autoUpdate",
      manifest: {
        theme_color: "#0279E8",
        background_color: "#141416",
      },
      workbox: {
        // Only one registration can exist per scope, so a second worker at "/" does not coexist with
        // this one -- it replaces it. page/public/macos-root-sw.js therefore ships as part of this
        // worker rather than as its own registration; it must stay a standalone service worker script
        // (its own skipWaiting/clients.claim included) because page/test/root-cache-adversarial.mjs
        // drives it directly.
        importScripts: ["macos-root-sw.js"],
        skipWaiting: true,
        clientsClaim: true,
        maximumFileSizeToCacheInBytes: 100 * mb,
        cleanupOutdatedCaches: true,
        globPatterns: ["**/*.{js,css,html,woff,woff2,wasm}"],
        // The capability probe is a diagnostic page almost no visitor opens, and precaching its
        // worker script would be actively wrong: the probe range-requests that very file to measure
        // whether the origin honours Range headers, but the precache route Workbox installs for a
        // precached URL answers from Cache Storage with a full 200 regardless of the request's Range
        // header, which would make the probe misreport the server's caching as the browser's Range
        // support. Excluding it keeps the request on the network path the probe is meant to measure.
        globIgnores: ["root.zip", "capability-probe.html", "capability-probe-worker.js", "app.css"],
        navigateFallbackDenylist: [/^\/root\.zip$/, /^\/capability-probe\.html$/],
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
