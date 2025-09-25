import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import "./index.css";
import App from "./App.tsx";
import { registerSW } from "virtual:pwa-register";

(window as any).loading = false;

registerSW({
  onNeedRefresh() {
    setTimeout(() => {
      window.location.reload();
    }, 5000);
  },
  onOfflineReady() {},
  onRegisteredSW(_, registration) {
    registration?.addEventListener("updatefound", () => {
      (window as any).loading = true;
    });
  },
});

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
