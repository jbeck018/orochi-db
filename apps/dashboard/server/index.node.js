// Production server entry point for Orochi Dashboard (Node.js compatible with SSR)
import { createServer } from 'node:http';
import { readFileSync, readdirSync, statSync, createReadStream } from 'node:fs';
import { resolve, join, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = fileURLToPath(new URL('.', import.meta.url));

// Configuration
const SERVER_PORT = Number(process.env.PORT ?? 3000);
const CLIENT_DIRECTORY = resolve(__dirname, '../dist/client');
const SERVER_HANDLER_PATH = resolve(__dirname, '../dist/server/server.js');

// MIME types for static assets
const MIME_TYPES = {
  '.html': 'text/html',
  '.js': 'application/javascript',
  '.mjs': 'application/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.ttf': 'font/ttf',
  '.eot': 'application/vnd.ms-fontobject',
  '.webp': 'image/webp',
  '.avif': 'image/avif',
  '.txt': 'text/plain',
  '.xml': 'application/xml',
  '.map': 'application/json',
};

// Build file index for static assets
const fileIndex = new Map();

function scanDirectory(dir, basePath = '') {
  try {
    const entries = readdirSync(dir, { withFileTypes: true });
    for (const entry of entries) {
      const fullPath = join(dir, entry.name);
      const urlPath = join(basePath, entry.name).replace(/\\/g, '/');

      if (entry.isDirectory()) {
        scanDirectory(fullPath, urlPath);
      } else if (entry.isFile()) {
        const stat = statSync(fullPath);
        fileIndex.set('/' + urlPath, {
          path: fullPath,
          size: stat.size,
          ext: extname(entry.name).toLowerCase()
        });
      }
    }
  } catch (err) {
    console.warn(`Failed to scan ${dir}:`, err.message);
  }
}

// Scan static files
console.log(`Scanning static assets from ${CLIENT_DIRECTORY}...`);
scanDirectory(CLIENT_DIRECTORY);
console.log(`Found ${fileIndex.size} static assets`);

// Load TanStack Start SSR handler
let ssrHandler = null;

async function loadSSRHandler() {
  try {
    const module = await import(SERVER_HANDLER_PATH);
    ssrHandler = module.default;
    console.log('TanStack Start SSR handler loaded successfully');
    return true;
  } catch (err) {
    console.warn('Failed to load SSR handler:', err.message);
    console.warn('Falling back to static file serving only');
    return false;
  }
}

// Convert Node.js request to Web API Request
function toWebRequest(req) {
  const protocol = req.headers['x-forwarded-proto'] || 'http';
  const host = req.headers['x-forwarded-host'] || req.headers.host || 'localhost';
  const url = `${protocol}://${host}${req.url}`;

  const headers = new Headers();
  for (const [key, value] of Object.entries(req.headers)) {
    if (value) {
      if (Array.isArray(value)) {
        value.forEach(v => headers.append(key, v));
      } else {
        headers.set(key, value);
      }
    }
  }

  return new Request(url, {
    method: req.method || 'GET',
    headers,
  });
}

// Convert Web API Response to Node.js response
async function sendWebResponse(webResponse, res) {
  res.statusCode = webResponse.status;

  webResponse.headers.forEach((value, key) => {
    // Skip content-encoding as we're handling the body directly
    if (key.toLowerCase() !== 'content-encoding') {
      res.setHeader(key, value);
    }
  });

  const body = await webResponse.arrayBuffer();
  res.end(Buffer.from(body));
}

// Serve static file
function serveStaticFile(pathname, res) {
  const fileInfo = fileIndex.get(pathname);
  if (!fileInfo) return false;

  const contentType = MIME_TYPES[fileInfo.ext] || 'application/octet-stream';
  res.writeHead(200, {
    'Content-Type': contentType,
    'Content-Length': fileInfo.size,
    'Cache-Control': 'public, max-age=31536000, immutable'
  });
  createReadStream(fileInfo.path).pipe(res);
  return true;
}

// Create HTTP server
const server = createServer(async (req, res) => {
  const url = new URL(req.url || '/', `http://${req.headers.host || 'localhost'}`);
  const pathname = url.pathname;

  // Health check endpoint
  if (pathname === '/health' || pathname === '/healthz') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok', timestamp: new Date().toISOString() }));
    return;
  }

  // Try to serve static files first (assets, etc.)
  if (serveStaticFile(pathname, res)) {
    return;
  }

  // Use SSR handler for all other routes
  if (ssrHandler) {
    try {
      const webRequest = toWebRequest(req);
      const webResponse = await ssrHandler.fetch(webRequest);
      await sendWebResponse(webResponse, res);
      return;
    } catch (err) {
      console.error('SSR error:', err);
      res.writeHead(500, { 'Content-Type': 'text/plain' });
      res.end('Internal Server Error');
      return;
    }
  }

  // Fallback: 404
  res.writeHead(404, { 'Content-Type': 'text/plain' });
  res.end('Not Found');
});

// Start server
async function main() {
  await loadSSRHandler();

  server.listen(SERVER_PORT, '0.0.0.0', () => {
    console.log(`Orochi Dashboard listening on http://0.0.0.0:${SERVER_PORT}`);
  });
}

main().catch(err => {
  console.error('Failed to start server:', err);
  process.exit(1);
});
