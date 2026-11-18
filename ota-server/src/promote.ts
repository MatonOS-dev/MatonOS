import { promises as fs } from 'node:fs';
import { dirname, resolve } from 'node:path';

const [channel, updateId] = process.argv.slice(2);
const configPath = process.env.OTA_CONFIG;
const storageDir = process.env.OTA_STORAGE_DIR;
const channelPattern = /^[a-z0-9_]+$/;
const updatePattern = /^[1-9][0-9]{0,19}$/;

async function main() {
  if (!channel || !updateId || process.argv.length !== 4 || !channelPattern.test(channel) || !updatePattern.test(updateId)) {
    throw new Error('usage: node src/promote.ts <channel> <update_id>');
  }
  if (!configPath || !storageDir) throw new Error('OTA_CONFIG and OTA_STORAGE_DIR are required');
  const config = JSON.parse(await fs.readFile(configPath, 'utf8'));
  const manifest = JSON.parse(await fs.readFile(resolve(storageDir, updateId, 'manifest.json'), 'utf8'));
  if (String(manifest?.build?.id) !== updateId || !Number.isSafeInteger(manifest?.build?.android_base) || manifest.build.android_base <= 0) {
    throw new Error(`invalid manifest for update ${updateId}`);
  }
  const old = config.channels?.[channel];
  if (old) {
    const oldManifest = JSON.parse(await fs.readFile(resolve(storageDir, String(old.update_id), 'manifest.json'), 'utf8'));
    const oldBase = oldManifest?.build?.android_base;
    if (!Number.isSafeInteger(oldBase) || oldBase <= 0) throw new Error(`invalid current manifest for channel ${channel}`);
    if (manifest.build.android_base < oldBase) throw new Error(`refusing Android base downgrade of channel ${channel}: current ${oldBase}, requested ${manifest.build.android_base}`);
  }
  config.channels ??= {};
  config.channels[channel] = { update_id: Number(updateId), ...(old?.requires_license === true ? { requires_license: true } : {}) };
  const tempPath = `${configPath}.tmp`;
  await fs.writeFile(tempPath, `${JSON.stringify(config, null, 2)}\n`, { mode: 0o600 });
  await fs.rename(tempPath, configPath);
  process.stdout.write(`Promoted ${updateId} to ${channel} in ${configPath}\n`);
}

main().catch((error) => {
  process.stderr.write(`ota-server promote: ERROR: ${error.message}\n`);
  process.exitCode = 1;
});
