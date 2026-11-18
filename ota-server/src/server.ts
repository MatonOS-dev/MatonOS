import { createReadStream, promises as fs } from 'node:fs';
import { createServer } from 'node:http';
import { basename, extname, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const PORT = Number(process.env.OTA_PORT ?? '8080');
const STORAGE_DIR = process.env.OTA_STORAGE_DIR;
const CONFIG_PATH = process.env.OTA_CONFIG;
const MAX_BODY = 64 * 1024;
const CHANNEL_RE = /^[a-z0-9_]+$/;
const UPDATE_ID_RE = /^[1-9][0-9]{0,19}$/;

function log(level: string, event: string, fields: Record<string, unknown> = {}) {
  process.stderr.write(`${JSON.stringify({ time: new Date().toISOString(), level, event, ...fields })}\n`);
}

function fail(status: number, code: string, message: string): HttpError {
  return new HttpError(status, code, message);
}

class HttpError extends Error {
  status: number;
  code: string;
  constructor(status: number, code: string, message: string) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

function validUpdateId(value: unknown): value is string | number {
  return (typeof value === 'string' && UPDATE_ID_RE.test(value)) ||
    (Number.isSafeInteger(value) && Number(value) > 0);
}

function idString(value: string | number): string {
  return String(value);
}

function validateBody(body: any) {
  if (!body || typeof body !== 'object' || Array.isArray(body)) throw fail(400, 'invalid_body', 'Expected a JSON object');
  const required = ['update_id', 'channel', 'base_android', 'addon_list'];
  for (const key of required) if (!(key in body)) throw fail(400, 'missing_field', `Missing field: ${key}`);
  if (!validUpdateId(body.update_id)) throw fail(400, 'invalid_update_id', 'update_id must be a positive integer');
  if (!Number.isSafeInteger(body.base_android) || body.base_android <= 0) throw fail(400, 'invalid_base_android', 'base_android must be a positive integer');
  if (typeof body.channel !== 'string' || !CHANNEL_RE.test(body.channel)) throw fail(400, 'invalid_channel', 'channel has an invalid format');
  if (body.machine_id !== undefined && typeof body.machine_id !== 'string') throw fail(400, 'invalid_machine_id', 'machine_id must be a string');
  if (body.license_hash !== undefined && typeof body.license_hash !== 'string') {
    throw fail(400, 'invalid_license_hash', 'license_hash must be a string');
  }
  if (!Array.isArray(body.addon_list) || body.addon_list.some((id: unknown) => typeof id !== 'string')) {
    throw fail(400, 'invalid_addon_list', 'addon_list must contain add-on id strings');
  }
  return body as { update_id: string | number; channel: string; base_android: number; machine_id?: string; license_hash?: string; addon_list: string[] };
}

async function readConfig() {
  if (!CONFIG_PATH) throw new Error('OTA_CONFIG is not set');
  const config = JSON.parse(await fs.readFile(CONFIG_PATH, 'utf8'));
  if (!config || typeof config.channels !== 'object' || !config.channels) {
    throw new Error('config must contain a channels object');
  }
  for (const [name, channel] of Object.entries(config.channels) as [string, any][]) {
    if (!CHANNEL_RE.test(name) || !channel || !validUpdateId(channel.update_id) ||
      (channel.requires_license !== undefined && typeof channel.requires_license !== 'boolean')) {
      throw new Error(`invalid channel configuration: ${name}`);
    }
  }
  return config as { channels: Record<string, { update_id: string | number; requires_license?: boolean }> };
}

async function readManifest(id: string) {
  if (!STORAGE_DIR) throw new Error('OTA_STORAGE_DIR is not set');
  const path = resolve(STORAGE_DIR, id, 'manifest.json');
  try {
    const manifest = JSON.parse(await fs.readFile(path, 'utf8'));
    if (idString(manifest?.build?.id) !== id || !Number.isSafeInteger(manifest?.build?.android_base) ||
        manifest.build.android_base <= 0 || !Array.isArray(manifest.files) || !manifest.addons || typeof manifest.addons !== 'object') {
      throw new Error(`invalid manifest metadata for update ${id}`);
    }
    return manifest;
  } catch (error: any) {
    if (error?.code === 'ENOENT') throw fail(404, 'update_not_found', `Update ${id} was not found`);
    if (error instanceof HttpError) throw error;
    throw error;
  }
}

function fileRecord(id: string, entry: any, addonId?: string) {
  if (!entry || typeof entry.name !== 'string' || entry.name.length === 0 || entry.name.startsWith('/') ||
      entry.name.split(/[\\/]/).some((part: string) => !part || part === '.' || part === '..') ||
      !Number.isSafeInteger(entry.size) || entry.size < 0 || !/^[a-f0-9]{64}$/.test(entry.sha256)) {
    throw new Error(`invalid file entry in update ${id}`);
  }
  const pathName = addonId ? `${addonId}/${entry.name}` : entry.name;
  const name = pathName.split(/[\\/]/).map(encodeURIComponent).join('/');
  return { name: entry.name, url: `/storage/${encodeURIComponent(id)}/${name}`, size: entry.size, sha256: entry.sha256 };
}

function json(res: any, status: number, value: unknown) {
  const data = Buffer.from(`${JSON.stringify(value)}\n`);
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Content-Length': data.length, 'Cache-Control': 'no-store' });
  res.end(data);
}

async function requestBody(req: any): Promise<unknown> {
  const type = String(req.headers['content-type'] ?? '').split(';', 1)[0].trim().toLowerCase();
  if (type !== 'application/json') throw fail(415, 'unsupported_media_type', 'Content-Type must be application/json');
  let chunks: Buffer[] = [], length = 0;
  for await (const chunk of req) {
    length += chunk.length;
    if (length > MAX_BODY) throw fail(413, 'body_too_large', 'Request body exceeds 64 KiB');
    chunks.push(chunk);
  }
  try { return JSON.parse(Buffer.concat(chunks).toString('utf8')); }
  catch { throw fail(400, 'malformed_json', 'Request body is not valid JSON'); }
}

async function handleApi(req: any, res: any) {
  const path = new URL(req.url ?? '/', 'http://localhost').pathname;
  if (!['/api/channels', '/api/update', '/api/eol', '/api/license'].includes(path)) throw fail(404, 'not_found', 'API endpoint not found');
  if (req.method !== 'POST') throw fail(405, 'method_not_allowed', 'API endpoints require POST');
  const body = validateBody(await requestBody(req));
  const config = await readConfig();
  if (path === '/api/license') return json(res, 200, { status: 'none', license: null });
  if (path === '/api/channels') {
    const channels = [];
    for (const [name, entry] of Object.entries(config.channels)) {
      if (entry.requires_license) continue;
      const manifest = await readManifest(idString(entry.update_id));
      const missing = body.addon_list.filter((addonId: string) => !Array.isArray(manifest.addons[addonId]));
      if (manifest.build.android_base >= body.base_android && missing.length === 0) channels.push({ channel: name, update_id: entry.update_id, version: manifest.build.version, android_base: manifest.build.android_base });
    }
    return json(res, 200, { channels });
  }
  if (path === '/api/eol') {
    return json(res, 200, { eol: false });
  }
  const selected = config.channels[body.channel];
  if (!selected) throw fail(404, 'channel_not_found', `Channel ${body.channel} was not found`);
  if (selected.requires_license) throw fail(403, 'license_required', 'This channel requires a license');
  const updateId = idString(selected.update_id);
  const manifest = await readManifest(updateId);
  if (manifest.build.android_base < body.base_android) {
    return json(res, 200, { available: false, reason: 'android_base_too_old' });
  }
  const missing = [...new Set(body.addon_list)].filter((addonId) => !Array.isArray(manifest.addons[addonId]));
  if (missing.length > 0) return json(res, 200, { available: false, reason: 'missing_addons', missing_addons: missing });
  if (updateId === idString(body.update_id)) return json(res, 200, { available: false });
  const addons: Record<string, unknown[]> = {};
  for (const addonId of [...new Set(body.addon_list)]) {
    const entries = manifest.addons[addonId];
    addons[addonId] = entries.map((entry: any) => fileRecord(updateId, entry, addonId));
  }
  const files = manifest.files.map((entry: any) => fileRecord(updateId, entry));
  return json(res, 200, {
    available: true,
    update_id: selected.update_id,
    version: manifest.build.version,
    android_base: manifest.build.android_base,
    manifest_url: `/storage/${encodeURIComponent(updateId)}/manifest.json`,
    signature_url: `/storage/${encodeURIComponent(updateId)}/manifest.sig`,
    files,
    addons
  });
}

function contentType(path: string) {
  return extname(path) === '.json' ? 'application/json; charset=utf-8' : 'application/octet-stream';
}

async function handleStorage(req: any, res: any) {
  if (req.method !== 'GET' && req.method !== 'HEAD') throw fail(405, 'method_not_allowed', 'Storage endpoints require GET or HEAD');
  if (!STORAGE_DIR) throw new Error('OTA_STORAGE_DIR is not set');
  const raw = new URL(req.url ?? '/', 'http://localhost').pathname.slice('/storage/'.length);
  let decoded: string;
  try { decoded = decodeURIComponent(raw); } catch { throw fail(400, 'invalid_path', 'Malformed storage path'); }
  const parts = decoded.split('/');
  if (parts.length < 2 || !UPDATE_ID_RE.test(parts[0]) || parts.slice(1).some((part) => !part || part === '.' || part === '..' || part.includes('\\'))) {
    throw fail(404, 'not_found', 'Storage file not found');
  }
  const path = resolve(STORAGE_DIR, ...parts);
  const root = await fs.realpath(STORAGE_DIR);
  let canonical: string, stat;
  try {
    canonical = await fs.realpath(path);
    stat = await fs.stat(canonical);
  }
  catch (error: any) { if (error?.code === 'ENOENT') throw fail(404, 'not_found', 'Storage file not found'); throw error; }
  if (!canonical.startsWith(root + sep)) throw fail(404, 'not_found', 'Storage file not found');
  if (!stat.isFile()) throw fail(404, 'not_found', 'Storage file not found');
  let start = 0, end = stat.size - 1, status = 200;
  const range = req.headers.range;
  if (range !== undefined) {
    const match = /^bytes=(\d*)-(\d*)$/.exec(String(range));
    if (!match || (!match[1] && !match[2]) || stat.size === 0) {
      res.writeHead(416, { 'Content-Range': `bytes */${stat.size}`, 'Cache-Control': 'public, max-age=31536000, immutable' }); return res.end();
    }
    if (!match[1]) { const suffix = Number(match[2]); start = Math.max(0, stat.size - suffix); }
    else start = Number(match[1]);
    if (match[2] && match[1]) end = Math.min(Number(match[2]), stat.size - 1);
    if (start >= stat.size || end < start) {
      res.writeHead(416, { 'Content-Range': `bytes */${stat.size}`, 'Cache-Control': 'public, max-age=31536000, immutable' }); return res.end();
    }
    status = 206;
  }
  const headers: Record<string, string | number> = {
    'Content-Type': contentType(path), 'Accept-Ranges': 'bytes', 'Content-Length': end - start + 1,
    'Cache-Control': 'public, max-age=31536000, immutable'
  };
  if (status === 206) headers['Content-Range'] = `bytes ${start}-${end}/${stat.size}`;
  res.writeHead(status, headers);
  if (req.method === 'HEAD' || stat.size === 0) return res.end();
  const stream = createReadStream(canonical, { start, end });
  stream.on('error', (error) => {
    log('error', 'storage_stream_failed', { path: basename(path), error: String(error) });
    if (!res.headersSent) res.writeHead(500, { 'Cache-Control': 'no-store' });
    res.destroy(error);
  });
  stream.pipe(res);
}

export function createOtaServer() {
  return createServer(async (req, res) => {
    try {
      const path = new URL(req.url ?? '/', 'http://localhost').pathname;
      if (path.startsWith('/api/')) await handleApi(req, res);
      else if (path.startsWith('/storage/')) await handleStorage(req, res);
      else throw fail(404, 'not_found', 'Not found');
    } catch (error: any) {
      if (error instanceof HttpError) {
        log('warn', 'request_rejected', { method: req.method, path: new URL(req.url ?? '/', 'http://localhost').pathname, status: error.status, code: error.code });
        if (!res.headersSent) json(res, error.status, { error: error.code, message: error.message });
        else res.destroy();
      } else {
        log('error', 'request_failed', { method: req.method, path: new URL(req.url ?? '/', 'http://localhost').pathname, error: String(error) });
        if (!res.headersSent) json(res, 500, { error: 'internal_error', message: 'Request failed' });
        else res.destroy();
      }
    }
  });
}

const invoked = process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (invoked) {
  if (!Number.isInteger(PORT) || PORT < 1 || PORT > 65535) throw new Error('OTA_PORT must be a valid TCP port');
  if (!STORAGE_DIR || !CONFIG_PATH) throw new Error('OTA_STORAGE_DIR and OTA_CONFIG are required');
  createOtaServer().listen(PORT, '0.0.0.0', () => log('info', 'server_listening', { port: PORT }));
}
