import {useEffect, useMemo, useState} from 'react';
import {useColorScheme} from 'react-native';
import {createMatonTheme} from '@matonos/rn-common';
import {MatonShelf} from '../ShelfNative';

const FALLBACK_WALLPAPER_SEED = 0xff365d96;

export function useWallpaperTheme() {
  const systemScheme = useColorScheme();
  const [seedColor, setSeedColor] = useState(FALLBACK_WALLPAPER_SEED);

  useEffect(() => {
    let mounted = true;
    const refresh = () => {
      void MatonShelf.getWallpaperSeedColor()
        .then((color) => {
          if (mounted && color !== 0) setSeedColor(color);
        })
        .catch(() => undefined);
    };
    refresh();
    const subscription = MatonShelf.onShelfState((event) => {
      if (event.type === 'wallpaperChanged') refresh();
    });
    return () => {
      mounted = false;
      subscription.remove();
    };
  }, []);

  const isDark = systemScheme === 'dark';
  return useMemo(() => createMatonTheme(seedColor, isDark), [seedColor, isDark]);
}
