import {useCallback} from 'react';
import {Animated, Text, TouchableOpacity, View, Image} from 'react-native';
import {MatonOS, SystemIcon, SystemIconName} from '@matonos/rn-common';
import {useShelfExpanded} from '../hooks/useShelfExtended';

function Action({onPress, name}: {onPress: () => void; name: SystemIconName}) {
  return (
    <TouchableOpacity
      onPress={onPress}
      style={{
        width: 50,
        height: 50,
        justifyContent: 'center',
        alignItems: 'center',
      }}
    >
      <SystemIcon name={name} size={30} tint="#fff" />
    </TouchableOpacity>
  );
}

export function Actions() {
  const onBack = useCallback(() => {
    MatonOS.navigate('back', false);
  }, []);
  const onHome = useCallback(() => {
    MatonOS.navigate('home', false);
  }, []);
  const onRecents = useCallback(() => {
    MatonOS.navigate('recents', false);
  }, []);
  return (
    <>
      <View
        style={{
          flexDirection: 'row',
          gap: 2,
          alignItems: 'center',
        }}
      >
        <Action onPress={onBack} name="back" />
        <Action onPress={onHome} name="home" />
        <Action onPress={onRecents} name="recents" />
      </View>
    </>
  );
}
