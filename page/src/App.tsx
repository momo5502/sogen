import { ThemeProvider } from "@/components/theme-provider";
import { TooltipProvider } from "@/components/ui/tooltip";
import { HashRouter, Route, Routes, Navigate } from "react-router-dom";
import { Playground, storeEmulateData } from "./playground";
import { LandingPage } from "./landing-page";

import { useParams } from "react-router-dom";

import "@fontsource/inter/latin.css";

import "./App.css";

function EmulateFile() {
  const { encodedData } = useParams();
  storeEmulateData(encodedData);
  return <Navigate to="/playground" replace />;
}

function App() {
  return (
    <ThemeProvider defaultTheme="dark" storageKey="vite-ui-theme">
      <TooltipProvider>
        <HashRouter>
          <Routes>
            <Route path="*" element={<Navigate to="/" replace />} />
            <Route path="/" element={<LandingPage />} />
            <Route path="/playground" element={<Playground />} />
            <Route path="/emulate/:encodedData?" element={<EmulateFile />} />
          </Routes>
        </HashRouter>
      </TooltipProvider>
    </ThemeProvider>
  );
}

export default App;
