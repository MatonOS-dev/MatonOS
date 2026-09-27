import { useEffect, useMemo, useRef, useState } from "react";
import { SafeAreaView, StyleSheet } from "react-native";
import {
  Button,
  Card,
  Column,
  Host,
  LinearProgressIndicator,
  Row,
  Text,
} from "@expo/ui/jetpack-compose";
import type { InstallDrive, OperationProgress } from "./service/InstallerApi";
import { stubInstallerApi } from "./service/StubInstallerApi";
import { createV1Plan, V1_MINIMUM_DISK_BYTES } from "./installer/createV1Plan";

type Screen = "drives" | "confirm" | "progress" | "done";

function formatSize(bytes: number): string {
  const gib = bytes / 1024 ** 3;
  return `${gib.toFixed(gib >= 100 ? 0 : 1)} GB`;
}

function stageLabel(progress: OperationProgress | null): string {
  return progress?.message ?? "Preparing install request";
}

export default function App(): React.JSX.Element {
  const [screen, setScreen] = useState<Screen>("drives");
  const [drives, setDrives] = useState<InstallDrive[]>([]);
  const [selected, setSelected] = useState<InstallDrive | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [progress, setProgress] = useState<OperationProgress | null>(null);
  const [outcome, setOutcome] = useState("");
  const running = useRef(false);
  const listedDrives = useMemo(() => drives, [drives]);

  const refreshDrives = async () => {
    setLoading(true);
    setError("");
    try {
      setDrives(await stubInstallerApi.listDrives());
    } catch {
      setError("The install service is unavailable. Try again in a moment.");
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    let mounted = true;
    void stubInstallerApi
      .listDrives()
      .then(
        (found) => {
          if (mounted) setDrives(found);
        },
        () => {
          if (mounted)
            setError(
              "The install service is unavailable. Try again in a moment.",
            );
        },
      )
      .finally(() => {
        if (mounted) setLoading(false);
      });
    return () => {
      mounted = false;
    };
  }, []);

  const install = () => {
    if (!selected) return;
    let plan;
    try {
      plan = createV1Plan(selected.id, selected.sizeBytes);
    } catch (problem) {
      setError(
        problem instanceof Error
          ? problem.message
          : "Cannot build the install plan.",
      );
      return;
    }
    setScreen("progress");
    setProgress(null);
    setOutcome("");
    running.current = true;
    void (async () => {
      for (let index = 0; index < plan.operations.length; index += 1) {
        if (!running.current) return;
        const result = await stubInstallerApi.executeOperation(
          plan.operations[index],
          (update) => {
            setProgress({
              ...update,
              message: `${index + 1}/${plan.operations.length}: ${update.message}`,
            });
          },
        );
        if (!result.ok) {
          running.current = false;
          setError(result.message);
          setScreen("drives");
          return;
        }
      }
      running.current = false;
      setOutcome(
        "The service stub completed the app-authored operation plan. No drive was changed.",
      );
      setScreen("done");
    })();
  };

  const cancel = async () => {
    running.current = false;
    const cancelled = await stubInstallerApi.cancelCurrentOperation();
    if (!cancelled)
      setError(
        "Partitioning has started; this install can no longer be cancelled.",
      );
  };

  return (
    <SafeAreaView style={styles.root}>
      <Host style={styles.host} colorScheme="light">
        <Column
          modifiers={[]}
          verticalArrangement={{ spacedBy: 18 }}
          horizontalAlignment="start"
        >
          <Text style={{ typography: "headlineLarge" }}>Install MatonOS</Text>
          <Text style={{ typography: "bodyLarge" }}>
            Install the live system to a whole drive. The target is erased and
            prepared with two bootable system slots.
          </Text>

          <Card border={{ width: 1, color: "#8A8174" }}>
            <Column verticalArrangement={{ spacedBy: 8 }}>
              <Text style={{ typography: "titleMedium" }}>Service preview</Text>
              <Text style={{ typography: "bodyMedium" }}>
                This screen currently uses a stub. It lists a demo drive and
                never writes to storage.
              </Text>
            </Column>
          </Card>

          {error ? <Text color="#B3261E">{error}</Text> : null}

          {screen === "drives" ? (
            <Column
              verticalArrangement={{ spacedBy: 12 }}
              horizontalAlignment="start"
            >
              <Row
                verticalAlignment="center"
                horizontalArrangement="spaceBetween"
              >
                <Text style={{ typography: "titleLarge" }}>Choose a drive</Text>
                <Button onClick={() => void refreshDrives()} enabled={!loading}>
                  Refresh
                </Button>
              </Row>
              {loading ? <Text>Finding available drives…</Text> : null}
              {!loading && listedDrives.length === 0 ? (
                <Text>No safe target drives are available.</Text>
              ) : null}
              {listedDrives.map((drive) => (
                <Card key={drive.id} border={{ width: 1, color: "#79716B" }}>
                  <Column verticalArrangement={{ spacedBy: 8 }}>
                    <Text style={{ typography: "titleMedium" }}>
                      {drive.model}
                    </Text>
                    <Text style={{ typography: "bodyMedium" }}>
                      {formatSize(drive.sizeBytes)} · {drive.transport}
                      {drive.removable ? " · removable" : ""}
                    </Text>
                    <Text style={{ typography: "bodySmall" }}>
                      {drive.path}
                    </Text>
                    {!drive.safe && drive.reason ? (
                      <Text color="#B3261E">Unavailable: {drive.reason}</Text>
                    ) : null}
                    <Button
                      enabled={drive.safe}
                      onClick={() => {
                        setSelected(drive);
                        setError("");
                        setScreen("confirm");
                      }}
                    >
                      Select drive
                    </Button>
                  </Column>
                </Card>
              ))}
            </Column>
          ) : null}

          {screen === "confirm" && selected ? (
            <Column verticalArrangement={{ spacedBy: 14 }}>
              <Text style={{ typography: "titleLarge" }}>Confirm erase</Text>
              <Card border={{ width: 1, color: "#79716B" }}>
                <Column verticalArrangement={{ spacedBy: 8 }}>
                  <Text style={{ typography: "titleMedium" }}>
                    {selected.model}
                  </Text>
                  <Text style={{ typography: "bodyLarge" }}>
                    {formatSize(selected.sizeBytes)} · {selected.transport}
                  </Text>
                  <Text style={{ typography: "bodySmall" }}>
                    {selected.path}
                  </Text>
                </Column>
              </Card>
              <Text color="#B3261E" style={{ typography: "titleMedium" }}>
                Everything on this drive will be erased.
              </Text>
              <Text style={{ typography: "bodyMedium" }}>
                This app will create two 5 GiB system groups in a 10.5 GiB super
                partition and copy supported files from this live session. The
                installer app's own data is excluded.
              </Text>
              <Text style={{ typography: "bodySmall" }}>
                Minimum drive: {formatSize(V1_MINIMUM_DISK_BYTES)} · Estimated
                userdata:{" "}
                {formatSize(Math.max(0, selected.sizeBytes - 12.2 * 1024 ** 3))}
              </Text>
              <Row horizontalArrangement="spaceBetween">
                <Button onClick={() => setScreen("drives")}>Back</Button>
                <Button onClick={install}>Erase and install</Button>
              </Row>
            </Column>
          ) : null}

          {screen === "progress" ? (
            <Column verticalArrangement={{ spacedBy: 14 }}>
              <Text style={{ typography: "titleLarge" }}>
                Installing MatonOS
              </Text>
              <Text style={{ typography: "bodyLarge" }}>
                {stageLabel(progress)}
              </Text>
              <LinearProgressIndicator
                progress={
                  progress && progress.bytesTotal > 0
                    ? progress.bytesDone / progress.bytesTotal
                    : null
                }
              />
              <Button onClick={() => void cancel()}>Cancel operation</Button>
            </Column>
          ) : null}

          {screen === "done" ? (
            <Column verticalArrangement={{ spacedBy: 14 }}>
              <Text style={{ typography: "headlineMedium" }}>
                Ready to reboot
              </Text>
              <Text style={{ typography: "bodyLarge" }}>{outcome}</Text>
              <Button onClick={() => setScreen("drives")}>
                Back to drive list
              </Button>
            </Column>
          ) : null}
        </Column>
      </Host>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: "#F7F4EF" },
  host: { flex: 1, paddingHorizontal: 24, paddingVertical: 20 },
});
