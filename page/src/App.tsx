import { ThemeProvider } from "@/components/theme-provider";
import { HashRouter, Route, Routes } from "react-router-dom";
import { Playground } from "./Playground";
import { LandingPage } from "./LandingPage";

import "./App.css";

function App() {
  return (
      <ThemeProvider defaultTheme="dark" storageKey="vite-ui-theme">
        <HashRouter>
          <Routes>
            <Route path="/" element={<LandingPage />} />
            <Route path="/playground" element={<Playground />} />
          </Routes>
        </HashRouter>
      </ThemeProvider>
  );
}

export default App;
