import {
  argbFromHex,
  hexFromArgb,
  themeFromSourceColor,
} from '@material/material-color-utilities';
import {
  MD3DarkTheme,
  MD3LightTheme,
  type MD3Theme,
} from 'react-native-paper';

function seedHex(seedColor: number): string {
  const rgb = (seedColor >>> 0) & 0x00ffffff;
  return `#${rgb.toString(16).padStart(6, '0')}`;
}

function applyScheme(base: MD3Theme, scheme: Record<string, number>): MD3Theme {
  const colorNames = Object.keys(base.colors) as Array<keyof MD3Theme['colors']>;
  const colors: Record<string, unknown> = {...base.colors};
  for (const name of colorNames) {
    const value = scheme[String(name)];
    if (typeof value === 'number') colors[name] = hexFromArgb(value);
  }
  return {...base, colors: colors as unknown as MD3Theme['colors']};
}

export function createMatonTheme(seedColor: number, isDark: boolean): MD3Theme {
  const source = argbFromHex(seedHex(seedColor));
  const dynamic = themeFromSourceColor(source).schemes;
  return applyScheme(isDark ? MD3DarkTheme : MD3LightTheme,
    (isDark ? dynamic.dark : dynamic.light).toJSON() as Record<string, number>);
}

export {MD3DarkTheme, MD3LightTheme};
