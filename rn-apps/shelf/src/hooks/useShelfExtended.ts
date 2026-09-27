import {useEffect, useState} from 'react';
import {MatonShelf} from '../ShelfNative';

export function useShelfExpanded() {
  const [expanded, setExpanded] = useState(true);
  const [hovered, setHovered] = useState(false);
  useEffect(() => {
    void MatonShelf.getShelfState().then((s) => setExpanded(s.expanded));
    const sub = MatonShelf.onShelfState((e) => {
      if (e.type === 'hover') setHovered(!!e.hovered); // true = mouse over the bar
      if (e.expanded !== undefined) setExpanded(e.expanded);
    });
    return () => sub.remove();
  }, []);
  return {expanded, hovered};
}
