import { useCallback, useState } from "react";
import { useFocusEffect } from "expo-router";
import { Column } from "@expo/ui/jetpack-compose";
import { fillMaxWidth } from "@expo/ui/jetpack-compose/modifiers";
import { Security, type SecurityStatus } from "../security/Security";
import { SectionHeader, SettingsCard, StatusRow } from "../components/SettingsUI";
import { copy } from "../strings";

export function SecuritySection() {
  const [status, setStatus] = useState<SecurityStatus | null>(null);
  const refresh = useCallback(async () => {
    try {
      setStatus(await Security.getStatus());
    } catch (error) {
      console.error("security status read failed", error);
      setStatus(null);
    }
  }, []);
  useFocusEffect(useCallback(() => { void refresh(); }, [refresh]));

  return (
    <>
      <SectionHeader title={copy.security} summary={copy.securitySummary} />
      <Column modifiers={[fillMaxWidth()]} verticalArrangement={{ spacedBy: 16 }} horizontalAlignment="start">
        <SettingsCard title={copy.security}>
          <StatusRow label={copy.selinux} value={status?.selinux ?? copy.unknown} />
          <StatusRow label={copy.encryption} value={status?.encryption ?? copy.unknown} />
          <StatusRow label={copy.securityPatch} value={status?.securityPatch ?? copy.unknown} />
          <StatusRow label={copy.vendorSecurityPatch} value={status?.vendorSecurityPatch ?? copy.unknown} />
          <StatusRow label={copy.verifiedBoot} value={status?.verifiedBoot ?? copy.unknown} />
          <StatusRow label={copy.buildKeys} value={status?.buildKeys ?? copy.unknown} />
          <StatusRow label={copy.adb} value={status?.adb ?? copy.unknown} />
        </SettingsCard>
      </Column>
    </>
  );
}
