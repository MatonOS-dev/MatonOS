import { useDeveloperOptions } from "../src/context/DeveloperOptionsContext";
import { AboutSection } from "../src/sections/AboutSection";
import { DeveloperSection } from "../src/sections/DeveloperSection";

export default function DeveloperRoute() {
  const { enabled, ready } = useDeveloperOptions();
  if (!ready || !enabled) return <AboutSection />;
  return <DeveloperSection />;
}
