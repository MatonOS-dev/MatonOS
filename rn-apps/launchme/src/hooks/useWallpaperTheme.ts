import {useEffect, useMemo, useState} from 'react';
import {useColorScheme} from 'react-native';
import {createMatonTheme} from '@matonos/rn-common';
import {MatonShell} from '../MatonShellNative';

const FALLBACK_WALLPAPER_SEED = 0xff365d96;

export function useWallpaperTheme() {
  const systemScheme = useColorScheme();
  const [seedColor, setSeedColor] = useState(FALLBACK_WALLPAPER_SEED);

  useEffect(() => {
    let mounted = true;
    const refresh = () => {
      void MatonShell.getWallpaperSeedColor()
        .then((color) => {
          if (mounted && color !== 0) setSeedColor(color);
        })
        .catch(() => undefined);
    };
    refresh();
    const subscription = MatonShell.onWallpaperChanged(refresh);
    return () => {
      mounted = false;
      subscription.remove();
    };
  }, []);

  const isDark = systemScheme === 'dark';
  return useMemo(() => createMatonTheme(seedColor, isDark), [seedColor, isDark]);
}
