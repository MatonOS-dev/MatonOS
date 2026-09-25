import {useBridgeAvailable} from './useBridgeAvailable';
import {MatonOS} from '../MatonOS';

/** Shared React view of bridge readiness and the typed MatonOS client API. */
export function useMatonOS(accessTargets: string[] = []) {
  const bridge = useBridgeAvailable(accessTargets);
  return {
    ...bridge,
    call: MatonOS.call,
    subscribe: MatonOS.subscribe,
    getBridgeSnapshot: MatonOS.getBridgeSnapshot,
    checkStartup: MatonOS.checkStartup,
  };
}
