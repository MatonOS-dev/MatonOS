import React from 'react';
import {Button, type ButtonProps, useTheme} from 'react-native-paper';

/** A small shared Material 3 action style for Shell and MatonOS Settings. */
export function MatonButton(props: ButtonProps): React.JSX.Element {
  const theme = useTheme();
  return <Button mode={props.mode ?? 'contained'} buttonColor={theme.colors.primary}
    textColor={theme.colors.onPrimary} {...props} />;
}
