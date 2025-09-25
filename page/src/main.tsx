import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import "./index.css";
import App from "./App.tsx";
import { registerSW } from "virtual:pwa-register";
import Loader from "./Loader";

registerSW({
  onNeedRefresh() {
    window.location.reload();
  },
  onOfflineReady() {},
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
