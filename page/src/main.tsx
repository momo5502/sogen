import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import "./index.css";
import App from "./App.tsx";
import { registerSW } from "virtual:pwa-register";
import Loader from "./Loader";

registerSW({
  onNeedRefresh() {
    Loader.setLoading(false);
    window.location.reload();
  },
  onOfflineReady() {
    Loader.setLoading(false);
  },
  onRegisteredSW(_, registration) {
    registration?.addEventListener("updatefound", () => {
      Loader.setLoading(true);
    });
  },
});

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
