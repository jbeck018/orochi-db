// Production server entry point for HowlerOps Dashboard (Node.js version)
// Compatible with Node.js runtime without Bun dependencies

import { createServer, IncomingMessage, ServerResponse } from 'node:http';
import { readdir, readFile, stat } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { resolve, join, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

// Get __dirname equivalent in ESM
const __dirname = fileURLToPath(new URL('.', import.meta.url));

// Configuration
const SERVER_PORT = Number(process.env.PORT ?? 3000);
const CLIENT_DIRECTORY = resolve(__dirname, '../dist/client');
const SERVER_ENTRY_POINT = resolve(__dirname, '../dist/server/server.js');

// MIME types for static assets
const MIME_TYPES: Record<string, string> = {
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
  '.webm': 'video/webm',
  '.mp4': 'video/mp4',
  '.txt': 'text/plain',
  '.xml': 'application/xml',
  '.map': 'application/json',
};

// Cache for preloaded static assets
interface CachedAsset {
  content: Buffer;
  contentType: string;
  etag: string;
}

const assetCache = new Map<string, CachedAsset>();

// Generate ETag from content using Node.js crypto
function generateETag(content: Buffer): string {
  const hash = createHash('md5').update(content).digest('hex');
  return `"${hash}"`;
}

// Preload static assets from dist/client
async function preloadStaticAssets(): Promise<void> {
  console.log(`Preloading static assets from ${CLIENT_DIRECTORY}...`);

  async function scanDirectory(dir: string, basePath: string = ''): Promise<void> {
    try {
      const entries = await readdir(dir, { withFileTypes: true });

      for (const entry of entries) {
        const fullPath = join(dir, entry.name);
        const urlPath = join(basePath, entry.name);

        if (entry.isDirectory()) {
          await scanDirectory(fullPath, urlPath);
        } else if (entry.isFile()) {
          try {
            const content = await readFile(fullPath);
            const ext = extname(entry.name).toLowerCase();
            const contentType = MIME_TYPES[ext] || 'application/octet-stream';
            const etag = generateETag(content);

            // Store with leading slash
            const cacheKey = '/' + urlPath.replace(/\\/g, '/');
            assetCache.set(cacheKey, { content, contentType, etag });
          } catch (err) {
            console.warn(`Failed to preload ${fullPath}:`, err);
          }
        }
      }
    } catch (err) {
      console.warn(`Failed to scan directory ${dir}:`, err);
    }
  }

  try {
    await stat(CLIENT_DIRECTORY);
    await scanDirectory(CLIENT_DIRECTORY);
    console.log(`Preloaded ${assetCache.size} static assets`);
  } catch {
    console.warn(`Client directory not found: ${CLIENT_DIRECTORY}`);
  }
}

// TanStack Start server handler type
interface StartHandler {
  fetch(request: Request): Promise<Response>;
}

// Load the TanStack Start handler
let startHandler: StartHandler | null = null;

async function loadHandler(): Promise<StartHandler | null> {
  if (startHandler === null) {
    try {
      const handlerModule = await import(SERVER_ENTRY_POINT);
      startHandler = handlerModule.default as StartHandler;
      console.log('TanStack Start handler loaded successfully');
    } catch (error) {
      console.warn('TanStack Start handler not available, serving static files only');
      return null;
    }
  }
  return startHandler;
}

// Convert Node.js IncomingMessage to Web API Request
function nodeRequestToWebRequest(req: IncomingMessage): Request {
  const protocol = 'http';
  const host = req.headers.host || 'localhost';
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
async function webResponseToNodeResponse(webResponse: Response, res: ServerResponse): Promise<void> {
  res.statusCode = webResponse.status;

  webResponse.headers.forEach((value, key) => {
    res.setHeader(key, value);
  });

  const body = await webResponse.arrayBuffer();
  res.end(Buffer.from(body));
}

// Serve a static asset from cache
function serveStaticAsset(pathname: string, req: IncomingMessage, res: ServerResponse): boolean {
  const cached = assetCache.get(pathname);
  if (!cached) {
    return false;
  }

  // Check If-None-Match for conditional request
  const ifNoneMatch = req.headers['if-none-match'];
  if (ifNoneMatch === cached.etag) {
    res.writeHead(304, {
      'ETag': cached.etag,
      'Cache-Control': 'public, max-age=31536000, immutable',
    });
    res.end();
    return true;
  }

  res.writeHead(200, {
    'Content-Type': cached.contentType,
    'Content-Length': cached.content.length,
    'ETag': cached.etag,
    'Cache-Control': 'public, max-age=31536000, immutable',
  });
  res.end(cached.content);
  return true;
}

// Find main entry files from cached assets
function findMainAssets(): { mainJs: string | null; mainCss: string | null } {
  let mainJs: string | null = null;
  let mainCss: string | null = null;

  for (const [path] of assetCache) {
    if (path.includes('/assets/main-') && path.endsWith('.js')) {
      mainJs = path;
    }
    if (path.includes('/assets/main-') && path.endsWith('.css')) {
      mainCss = path;
    }
  }

  return { mainJs, mainCss };
}

// Generate HTML shell for SPA mode
function generateHtmlShell(): string {
  const { mainJs, mainCss } = findMainAssets();

  return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>HowlerOps Dashboard</title>
  <meta name="description" content="HowlerOps - OrochiDB Management Platform" />
  ${mainCss ? `<link rel="stylesheet" href="${mainCss}" />` : ''}
</head>
<body>
  <div id="root"></div>
  ${mainJs ? `<script type="module" src="${mainJs}"></script>` : '<p>Application failed to load</p>'}
</body>
</html>`;
}

// Serve index.html for SPA fallback
function serveIndexHtml(res: ServerResponse): void {
  const indexAsset = assetCache.get('/index.html');
  if (indexAsset) {
    res.writeHead(200, {
      'Content-Type': 'text/html',
      'Content-Length': indexAsset.content.length,
    });
    res.end(indexAsset.content);
  } else {
    // Generate a basic HTML shell for SPA mode
    const html = generateHtmlShell();
    res.writeHead(200, {
      'Content-Type': 'text/html',
      'Content-Length': Buffer.byteLength(html),
    });
    res.end(html);
  }
}

// Request handler
async function handleRequest(req: IncomingMessage, res: ServerResponse): Promise<void> {
  const url = new URL(req.url || '/', `http://${req.headers.host || 'localhost'}`);
  const pathname = url.pathname;

  // Health check endpoint
  if (pathname === '/health' || pathname === '/healthz') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'healthy' }));
    return;
  }

  // Try to serve static asset first
  if (serveStaticAsset(pathname, req, res)) {
    return;
  }

  // Also check for /assets/ prefix paths
  if (pathname.startsWith('/assets/')) {
    if (serveStaticAsset(pathname, req, res)) {
      return;
    }
  }

  // Try TanStack Start handler for SSR
  const handler = await loadHandler();
  if (handler) {
    try {
      const webRequest = nodeRequestToWebRequest(req);
      const webResponse = await handler.fetch(webRequest);
      await webResponseToNodeResponse(webResponse, res);
      return;
    } catch (error) {
      console.error('Error handling SSR request:', error);
    }
  }

  // Fallback to SPA mode - serve index.html for all routes
  serveIndexHtml(res);
}

// Initialize and start server
async function main(): Promise<void> {
  // Preload static assets
  await preloadStaticAssets();

  // Try to load TanStack Start handler
  await loadHandler();

  // Create and start Node.js HTTP server
  const server = createServer((req, res) => {
    handleRequest(req, res).catch((error) => {
      console.error('Unhandled error:', error);
      res.writeHead(500);
      res.end('Internal Server Error');
    });
  });

  server.listen(SERVER_PORT, '0.0.0.0', () => {
    console.log(`HowlerOps Dashboard listening on http://0.0.0.0:${SERVER_PORT}`);
  });
}

// Run the server
main().catch((error) => {
  console.error('Failed to start server:', error);
  process.exit(1);
});
