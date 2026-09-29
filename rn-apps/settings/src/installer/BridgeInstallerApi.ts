import {MatonOS} from '../bridge/MatonOS';
import type {InstallDrive, InstallerApi, OperationProgress, OperationRequestV1} from '../service/InstallerApi';

function decode<T>(value: string): T {
  try { return JSON.parse(value) as T; } catch { throw new Error('Install service returned an invalid response.'); }
}

export const bridgeInstallerApi: InstallerApi = {
  async listDrives(): Promise<InstallDrive[]> {
    const result = await MatonOS.call('install', 'list_drives');
    if (!result.available) throw new Error(result.reason || 'Install service is unavailable.');
    const payload = decode<{drives: InstallDrive[]}>(result.value);
    return Array.isArray(payload.drives) ? payload.drives : [];
  },
  async executeOperation(request: OperationRequestV1, onProgress: (progress: OperationProgress) => void) {
    const sub = MatonOS.subscribe('install', 'operation_progress', (event) => {
      try { onProgress(JSON.parse(event.json) as OperationProgress); } catch { /* Ignore malformed optional progress events. */ }
    });
    try {
      const result = await MatonOS.call('install', 'execute_operation', request);
      if (!result.available) return {ok:false as const, message: result.reason || 'Install service is unavailable.'};
      const value = decode<{ok:boolean; message?:string}>(result.value);
      return value.ok ? {ok:true as const} : {ok:false as const, message:value.message || 'The operation failed.'};
    } finally { sub.remove(); }
  },
  async cancelCurrentOperation() {
    const result = await MatonOS.call('install', 'cancel_operation');
    return result.available && result.value === 'true';
  },
};
