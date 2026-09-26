import {useEffect, useMemo, useState} from 'react';
import {useColorScheme} from 'react-native';
import {MatonOS} from '../MatonOS';
import {createMatonTheme} from '../theme/matonTheme';

const FALLBACK_SEED = 0xff365d96;

export function useMatonTheme() {
  const systemScheme = useColorScheme();
  const [seedColor, setSeedColor] = useState(FALLBACK_SEED);

  useEffect(() => {
    let mounted = true;
    void MatonOS.getWallpaperSeedColor()
      .then((seed) => {
        if (mounted && seed !== 0) setSeedColor(seed);
      })
      .catch(() => undefined);
    return () => {
      mounted = false;
    };
  }, []);

  return useMemo(
    () => createMatonTheme(seedColor, systemScheme === 'dark'),
    [seedColor, systemScheme],
  );
}
