import {
  createContext,
  useContext,
  useEffect,
  useState,
  type ReactNode,
} from "react";
import { MatonOS } from "../bridge/MatonOS";

type SystemInfo = {
  version: string;
  build: string;
  device: string;
  model: string;
  sdk: number;
};
type MatonSettingsContextValue = { live: boolean; system: SystemInfo | null };
const MatonSettingsContext = createContext<MatonSettingsContextValue>({
  live: false,
  system: null,
});
export function MatonSettingsProvider({ children }: { children: ReactNode }) {
  const [live, setLive] = useState(false);
  const [system, setSystem] = useState<SystemInfo | null>(null);
  useEffect(() => {
    let active = true;
    void MatonOS.isLiveImage()
      .then((value) => {
        if (active) setLive(value);
      })
      .catch(() => {
        if (active) setLive(false);
      });
    void MatonOS.getSystemInfo()
      .then((value) => {
        if (active) setSystem(value);
      })
      .catch(() => {
        if (active) setSystem(null);
      });
    return () => {
      active = false;
    };
  }, []);
  return (
    <MatonSettingsContext.Provider value={{ live, system }}>
      {children}
    </MatonSettingsContext.Provider>
  );
}
export function useMatonSettings() {
  return useContext(MatonSettingsContext);
}
