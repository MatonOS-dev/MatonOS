import React from 'react';
import {processColor, Text, type ColorValue, type StyleProp, type TextStyle, type ViewStyle} from 'react-native';
import {requireNativeView} from 'expo';

type NativeProps = {resource: string; tint?: number | null; style?: StyleProp<ViewStyle>};
const NativeSystemIcon = requireNativeView<NativeProps>('MatonSystemIcon');

/**
 * Logical names -> Android resources. SystemUI / com.android.internal
 * resource names are NOT stable API: verify them on every AOSP release and
 * update only this table. Anything not listed: use `resource` or <Symbol>.
 */
export const SYSTEM_ICONS = {
  back: 'com.android.systemui:drawable/ic_sysbar_back',
  home: 'com.android.systemui:drawable/ic_sysbar_home',
  recents: 'com.android.systemui:drawable/ic_sysbar_recent',
  settings: 'com.android.systemui:drawable/ic_settings_24dp',
  battery: 'android:drawable/ic_battery',
  notifications: 'android:drawable/ic_notifications',
  screenshot: 'android:drawable/ic_screenshot',
} as const;
export type SystemIconName = keyof typeof SYSTEM_ICONS;

type SystemIconProps = {
  /** Logical name from SYSTEM_ICONS, or a raw "pkg:type/name" resource. */
  name?: SystemIconName;
  resource?: string;
  size?: number;
  tint?: ColorValue;
  style?: StyleProp<ViewStyle>;
};

/** An Android system drawable (e.g. the real nav-bar glyphs), tinted. */
export function SystemIcon({name, resource, size = 24, tint, style}: SystemIconProps): React.JSX.Element {
  const spec = resource ?? (name ? SYSTEM_ICONS[name] : '');
  const color = tint === undefined ? null : (processColor(tint) as number | null);
  return <NativeSystemIcon resource={spec} tint={color} style={[{width: size, height: size}, style]} />;
}

/** The system icon font installed by MatonOS (/product/fonts). */
export const SYMBOL_FONT_FAMILY = 'material-symbols-outlined';

type SymbolProps = {
  /** Material Symbols ligature name, e.g. "home", "wifi", "volume_up". */
  name: string;
  size?: number;
  color?: ColorValue;
  style?: StyleProp<TextStyle>;
};

/**
 * A Material Symbols icon from the system font (ligatures: the name itself
 * renders as the icon). Names: fonts.google.com/icons or
 * device/maton/pc_x86_64/fonts/MaterialSymbolsOutlined.codepoints.
 */
export function Symbol({name, size = 24, color, style}: SymbolProps): React.JSX.Element {
  return (
    <Text
      accessibilityElementsHidden
      importantForAccessibility="no"
      allowFontScaling={false}
      style={[{fontFamily: SYMBOL_FONT_FAMILY, fontSize: size, lineHeight: size, color}, style]}>
      {name}
    </Text>
  );
}
