import {useEffect, useState} from 'react';
import {MatonOS, type BridgeSnapshot} from '../MatonOS';

export function useBridgeAvailable(accessTargets: string[], requiredChannels: string[] = []) {
  const [snapshot, setSnapshot] = useState<BridgeSnapshot | null>(null);

  useEffect(() => {
    let active = true;
    const targetsKey = [...accessTargets].sort().join(',');
    const channelsKey = [...requiredChannels].sort().join(',');
    const refresh = () => {
      void MatonOS.checkStartup(targetsKey ? targetsKey.split(',') : [],
        channelsKey ? channelsKey.split(',') : []).then(value => {
          if (active) setSnapshot(value);
        });
    };
    refresh();
    const subscription = MatonOS.onAvailabilityChanged(refresh);
    return () => {
      active = false;
      subscription.remove();
    };
  }, [accessTargets.join(','), requiredChannels.join(',')]);

  return {
    available: snapshot?.available ?? false,
    reason: snapshot?.reason ?? 'BRIDGE_CONNECTING',
    snapshot,
  };
}
