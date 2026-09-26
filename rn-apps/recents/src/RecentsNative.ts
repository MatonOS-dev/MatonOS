import {requireNativeModule} from 'expo-modules-core';

type MatonRecentsModule = {
  goHome(): void;
  reportSurfaceFailure(error: string): void;
};
const NativeRecents = requireNativeModule<MatonRecentsModule>('MatonRecents');
export const MatonRecents = {
  goHome: () => NativeRecents.goHome(),
  reportSurfaceFailure: (error: string) => NativeRecents.reportSurfaceFailure(error),
};
