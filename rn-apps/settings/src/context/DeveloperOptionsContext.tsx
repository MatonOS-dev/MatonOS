import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from "react";
import { MatonOS } from "../bridge/MatonOS";

type DeveloperOptions = {
  enabled: boolean;
  ready: boolean;
  enable: () => Promise<void>;
};

const Context = createContext<DeveloperOptions | null>(null);

export function DeveloperOptionsProvider({ children }: { children: ReactNode }) {
  const [enabled, setEnabled] = useState(false);
  const [ready, setReady] = useState(false);

  useEffect(() => {
    let active = true;
    void MatonOS.isDeveloperOptionsEnabled().then((value) => {
      if (active) setEnabled(value);
    }).catch((error: unknown) => {
      console.error("Could not read developer options state", error);
    }).finally(() => {
      if (active) setReady(true);
    });
    return () => { active = false; };
  }, []);

  const enable = useCallback(async () => {
    const saved = await MatonOS.enableDeveloperOptions();
    if (!saved) throw new Error("Could not save developer options state");
    setEnabled(true);
  }, []);
  const value = useMemo(() => ({ enabled, ready, enable }), [enabled, ready, enable]);

  return <Context.Provider value={value}>{children}</Context.Provider>;
}

export function useDeveloperOptions() {
  const value = useContext(Context);
  if (!value) throw new Error("DeveloperOptionsProvider is missing");
  return value;
}
