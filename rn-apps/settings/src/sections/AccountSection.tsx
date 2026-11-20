import { useCallback, useEffect, useState } from "react";
import { Alert } from "react-native";
import { Button, Column, OutlinedTextField, Row, Text, useMaterialColors, useNativeState } from "@expo/ui/jetpack-compose";
import { fillMaxWidth, paddingAll, weight } from "@expo/ui/jetpack-compose/modifiers";
import { useLocalSearchParams } from "expo-router";
import { MatonOS } from "../bridge/MatonOS";
import { SettingsCard } from "../components/SettingsUI";

const API = "https://maton.hanro50.net.za/api";
type Account = { username:string; email:string; verified:boolean };
type Device = { id:string; name:string; created:number; last_refreshed:number; current:boolean };
type Mode = "signin" | "register" | "forgot" | "reset" | "account" | "approve";

function useField(initial = "") {
  const state = useNativeState(initial);
  const set = (next:string) => state.set(next);
  return { state, get:() => state.get(), set };
}

function errorMessage(error:unknown) { return error instanceof Error ? error.message : "The account request failed."; }

export function AccountSection() {
  const params = useLocalSearchParams<{flow?:string; caller?:string}>();
  const colors = useMaterialColors();
  const username = useField(); const email = useField(); const password = useField();
  const resetToken = useField(); const verifyToken = useField(); const nextPassword = useField(); const deletePassword = useField();
  const [mode, setMode] = useState<Mode>(params.flow === "approve" ? "approve" : "signin");
  const [account, setAccount] = useState<Account | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState("");

  const loadAccount = useCallback(async () => {
    const session = await MatonOS.accountStoredSession();
    if (!session) { setAccount(null); setDevices([]); setMode("signin"); return; }
    const info = await MatonOS.accountMachineInfo();
    const headers = { cookie:`maton_session=${session.token}`, "X-Maton-Machine-Id":info.machineId };
    const response = await fetch(`${API}/account`, { headers });
    if (response.status === 401) {
      await MatonOS.accountInvalidate(session.token); setAccount(null); setDevices([]); setMessage("Your session expired. Sign in again."); return;
    }
    if (!response.ok) throw new Error(`Account details could not be loaded (${response.status})`);
    setAccount(await response.json() as Account);
    if (params.flow !== "approve") setMode("account");
    const deviceResponse = await fetch(`${API}/account/devices`, { headers });
    if (!deviceResponse.ok) throw new Error(`Signed-in devices could not be loaded (${deviceResponse.status})`);
    setDevices(await deviceResponse.json() as Device[]);
  }, [params.flow]);

  useEffect(() => {
    void Promise.resolve().then(loadAccount).catch((error) => setMessage(errorMessage(error)));
  }, [loadAccount, params.flow]);

  async function post(path:string, body:Record<string,unknown>) {
    const info = await MatonOS.accountMachineInfo();
    const response = await fetch(`${API}${path}`, { method:"POST", headers:{ "content-type":"application/json", "X-Maton-Machine-Id":info.machineId }, body:JSON.stringify({ ...body, machine_id:info.machineId, device_name:info.deviceName }) });
    const value = await response.json().catch(() => ({})) as Record<string,unknown>;
    if (!response.ok) throw new Error(typeof value.message === "string" ? value.message : `Request failed (${response.status})`);
    return value;
  }

  async function submitAccount() {
    setBusy(true); setMessage("");
    try {
      const created = mode === "register";
      const result = await post(created ? "/account/register" : "/account/login", {
        username:username.get(), ...(created ? { email:email.get() } : {}), password:password.get(),
      });
      if (typeof result.token !== "string" || typeof result.refresh_token !== "string" || typeof result.username !== "string") throw new Error("The account service returned no session tokens.");
      const machine = await MatonOS.accountMachineInfo();
      await MatonOS.accountComplete(result.username, typeof result.email === "string" ? result.email : email.get(), result.token, result.refresh_token, machine.machineId);
      setAccount({ username:result.username, email:String(result.email ?? email.get()), verified:Boolean(result.verified) });
      password.set(""); setMode("account"); await loadAccount();
    } catch (error) { setMessage(errorMessage(error)); }
    finally { setBusy(false); }
  }

  async function requestReset() {
    setBusy(true); setMessage("");
    try { await post("/account/reset", { email:email.get() }); setMode("reset"); setMessage("If that address has an account, a reset token has been sent."); }
    catch (error) { setMessage(errorMessage(error)); }
    finally { setBusy(false); }
  }

  async function confirmReset() {
    setBusy(true); setMessage("");
    try { await post("/account/reset/confirm", { token:resetToken.get(), password:nextPassword.get() }); setMode("signin"); setMessage("Password changed. Sign in with your new password."); }
    catch (error) { setMessage(errorMessage(error)); }
    finally { setBusy(false); }
  }

  async function verifyEmail() {
    setBusy(true); setMessage("");
    try { await post("/account/verify", { token:verifyToken.get() }); verifyToken.set(""); await loadAccount(); setMessage("E-mail address verified."); }
    catch (error) { setMessage(errorMessage(error)); }
    finally { setBusy(false); }
  }

  async function signOut() {
    if (!account) return;
    setBusy(true);
    try {
      const session = await MatonOS.accountStoredSession(); const info = await MatonOS.accountMachineInfo();
      if (session) await fetch(`${API}/account/logout`, { method:"POST", headers:{ cookie:`maton_session=${session.token}`, "X-Maton-Machine-Id":info.machineId } });
      await MatonOS.accountRemoveLocal(account.username); setAccount(null); setDevices([]); setMode("signin");
    } catch (error) { setMessage(errorMessage(error)); }
    finally { setBusy(false); }
  }

  async function approveCaller() {
    setBusy(true); setMessage("");
    try {
      const session = await MatonOS.accountStoredSession();
      if (!session) { setMode("signin"); throw new Error("Sign in to continue."); }
      const info = await MatonOS.accountMachineInfo();
      const response = await fetch(`${API}/account`, { headers:{ cookie:`maton_session=${session.token}`, "X-Maton-Machine-Id":info.machineId } });
      if (!response.ok) {
        if (response.status === 401) await MatonOS.accountInvalidate(session.token);
        setMode("signin"); throw new Error("Sign in again to approve access.");
      }
      const value = await response.json() as Account;
      await MatonOS.accountComplete(value.username, value.email, session.token, session.refreshToken, info.machineId);
    } catch (error) { setMessage(errorMessage(error)); }
    finally { setBusy(false); }
  }

  async function revoke(device:Device) {
    Alert.alert("Remove signed-in device?", `${device.name} will need to sign in again.`, [
      { text:"Cancel", style:"cancel" },
      { text:"Remove", style:"destructive", onPress:() => { void (async () => {
        const session = await MatonOS.accountStoredSession(); const info = await MatonOS.accountMachineInfo();
        if (!session) throw new Error("Sign in again to manage devices.");
        const response = await fetch(`${API}/account/devices/${encodeURIComponent(device.id)}`, { method:"DELETE", headers:{ cookie:`maton_session=${session.token}`, "X-Maton-Machine-Id":info.machineId } });
        if (!response.ok) throw new Error(`Device could not be removed (${response.status})`);
        await loadAccount();
      })().catch((error) => setMessage(errorMessage(error))); } },
    ]);
  }

  async function deleteAccount() {
    if (!account) return;
    Alert.alert("Delete MatonOS account?", "This permanently removes the account, e-mail address and all written reviews.", [
      { text:"Cancel", style:"cancel" },
      { text:"Delete account", style:"destructive", onPress:() => { void (async () => {
        const session = await MatonOS.accountStoredSession(); const info = await MatonOS.accountMachineInfo();
        if (!session) throw new Error("Sign in again to delete your account.");
        const response = await fetch(`${API}/account`, { method:"DELETE", headers:{ "content-type":"application/json", cookie:`maton_session=${session.token}`, "X-Maton-Machine-Id":info.machineId }, body:JSON.stringify({password:deletePassword.get()}) });
        const result = await response.json().catch(() => ({})) as {message?:string};
        if (!response.ok) throw new Error(result.message ?? `Account could not be deleted (${response.status})`);
        await MatonOS.accountRemoveLocal(account.username); setAccount(null); setDevices([]); deletePassword.set(""); setMode("signin");
      })().catch((error) => setMessage(errorMessage(error))); } },
    ]);
  }

  const deviceRows = devices.map((device) => <Row key={device.id} verticalAlignment="center" horizontalArrangement="start" modifiers={[fillMaxWidth(),paddingAll(8)]}>
    <Column modifiers={[weight(1)]} verticalArrangement={{spacedBy:2}}>
      <Text style={{typography:"titleSmall"}}>{device.name}{device.current ? " · This device" : ""}</Text>
      <Text style={{typography:"bodySmall"}}>Last active {new Date(device.last_refreshed).toLocaleString()}</Text>
    </Column>
    <Button enabled={!busy} onClick={() => revoke(device)}><Text>Remove</Text></Button>
  </Row>);

  const field = (value:ReturnType<typeof useField>, label:string, secret=false) => <OutlinedTextField value={value.state} onValueChange={value.set} singleLine visualTransformation={secret ? "password" : "none"}>
    <OutlinedTextField.Label><Text>{label}</Text></OutlinedTextField.Label>
  </OutlinedTextField>;

  if (account && mode === "account") return <Column verticalArrangement={{spacedBy:16}}>
    <Text style={{typography:"headlineMedium"}}>MatonOS account</Text>
    <SettingsCard title={account.username}>
      <Text>{account.email}</Text>
      <Text color={account.verified ? colors.primary : colors.onSurfaceVariant}>{account.verified ? "E-mail verified" : "E-mail not verified"}</Text>
      {!account.verified ? <>
        <Text>Enter the one-time token sent to your e-mail address.</Text>
        {field(verifyToken,"Verification token")}
        <Button enabled={!busy && verifyToken.get().length > 0} onClick={() => void verifyEmail()}><Text>Verify e-mail</Text></Button>
      </> : null}
      <Button enabled={!busy} onClick={() => void signOut()}><Text>Sign out</Text></Button>
    </SettingsCard>
    <SettingsCard title="Signed-in devices">
      {deviceRows.length ? deviceRows : <Text>No signed-in devices.</Text>}
    </SettingsCard>
    <SettingsCard title="Delete account">
      <Text>This removes your account and written reviews from MatonOS services.</Text>
      {field(deletePassword,"Confirm with password",true)}
      <Button enabled={!busy && deletePassword.get().length > 0} onClick={() => void deleteAccount()}><Text>Delete account</Text></Button>
    </SettingsCard>
    {message ? <Text color={colors.error}>{message}</Text> : null}
  </Column>;

  const title = mode === "register" ? "Create MatonOS account" : mode === "forgot" ? "Forgot password" : mode === "reset" ? "Reset password" : "Sign in";
  if (mode === "approve") return <Column verticalArrangement={{spacedBy:14}}>
    <Text style={{typography:"headlineMedium"}}>Share your MatonOS account?</Text>
    <SettingsCard title="Account access">
      <Text>{params.caller ?? "This app"} is requesting a MatonOS session token for this device.</Text>
      <Text>Only approve apps you trust. The token is bound to this PC.</Text>
      {message ? <Text color={colors.error}>{message}</Text> : null}
      <Button enabled={!busy && Boolean(account)} onClick={() => void approveCaller()}><Text>{busy ? "Please wait…" : "Approve"}</Text></Button>
      <Button enabled={!busy} onClick={() => void MatonOS.accountCancel()}><Text>Deny</Text></Button>
    </SettingsCard>
  </Column>;
  return <Column verticalArrangement={{spacedBy:14}}>
    <Text style={{typography:"headlineMedium"}}>MatonOS account</Text>
    <SettingsCard title={title}>
      {mode === "register" || mode === "signin" ? field(username,"Username") : null}
      {mode === "register" || mode === "forgot" ? field(email,"E-mail") : null}
      {mode === "register" || mode === "signin" ? field(password,"Password",true) : null}
      {mode === "reset" ? field(resetToken,"E-mail reset token") : null}
      {mode === "reset" ? field(nextPassword,"New password",true) : null}
      <Row horizontalArrangement={{spacedBy:8}} verticalAlignment="center">
        {mode === "signin" || mode === "register" ? <Button enabled={!busy} onClick={() => void submitAccount()}><Text>{busy ? "Please wait…" : mode === "register" ? "Create account" : "Sign in"}</Text></Button> : null}
        {mode === "forgot" ? <Button enabled={!busy} onClick={() => void requestReset()}><Text>Send reset instructions</Text></Button> : null}
        {mode === "reset" ? <Button enabled={!busy} onClick={() => void confirmReset()}><Text>Set new password</Text></Button> : null}
        {mode === "signin" ? <Button onClick={() => { setMode("register"); setMessage(""); }}><Text>Create account</Text></Button> : null}
        {mode === "signin" ? <Button onClick={() => { setMode("forgot"); setMessage(""); }}><Text>Forgot password?</Text></Button> : null}
        {mode !== "signin" ? <Button onClick={() => { setMode("signin"); setMessage(""); }}><Text>Back to sign in</Text></Button> : null}
      </Row>
      {message ? <Text color={colors.error}>{message}</Text> : null}
    </SettingsCard>
  </Column>;
}
