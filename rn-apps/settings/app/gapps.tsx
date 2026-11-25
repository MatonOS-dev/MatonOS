import { useDeveloperOptions } from "../src/context/DeveloperOptionsContext";
import { AboutSection } from "../src/sections/AboutSection";
import { GappsSection } from "../src/sections/GappsSection";

export default function GappsRoute() {
  const { enabled, ready } = useDeveloperOptions();
  if (!ready || !enabled) return <AboutSection />;
  return <GappsSection />;
}
