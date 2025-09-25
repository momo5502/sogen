import { ThemeProvider } from "@/components/theme-provider";
import { TooltipProvider } from "@/components/ui/tooltip";
import { HashRouter, Route, Routes, Navigate } from "react-router-dom";
import { Playground, storeEmulateData } from "./playground";
import { LandingPage } from "./landing-page";
import { useParams } from "react-router-dom";
import Loader from "./Loader";

import "@fontsource/inter/latin.css";

import "./App.css";
import "./animation.css";

function EmulateFile() {
  const { encodedData } = useParams();
  storeEmulateData(encodedData);
  return <Navigate to="/playground" replace />;
}

function Spinner() {
  const loading = Loader.useLoader();

  if (!loading) {
    return <></>;
  }

  return (
    <div className="fixed z-9999 top-10 right-10 p-8 rounded-2xl ring-of-dots bg-[#00000081]"></div>
  );
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
        <Spinner />
      </TooltipProvider>
    </ThemeProvider>
  );
}

export default App;
