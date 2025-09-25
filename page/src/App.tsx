import { ThemeProvider } from "@/components/theme-provider";
import { TooltipProvider } from "@/components/ui/tooltip";
import { HashRouter, Route, Routes, Navigate } from "react-router-dom";
import { Playground, storeEmulateData } from "./playground";
import { LandingPage } from "./landing-page";
import { useEffect, useState } from "react";
import { useParams } from "react-router-dom";

import "@fontsource/inter/latin.css";

import "./App.css";
import "./animation.css";

function EmulateFile() {
  const { encodedData } = useParams();
  storeEmulateData(encodedData);
  return <Navigate to="/playground" replace />;
}

function isWindowLoading() {
  return !!(window as any).loading;
}

function useLoader() {
  const [isLoading, setIsLoading] = useState(isWindowLoading());

  useEffect(() => {
    const id = setInterval(() => {
      setIsLoading(isWindowLoading());
    }, 60);

    return () => {
      clearInterval(id);
    };
  });

  return isLoading;
}

function Loader() {
  const loading = useLoader();

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
        <Loader />
      </TooltipProvider>
    </ThemeProvider>
  );
}

export default App;
