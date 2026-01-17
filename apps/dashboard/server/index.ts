// Production server entry point for Orochi Dashboard
// Uses Bun's native server and file APIs for optimal performance

import { readdir } from 'node:fs/promises';
import { resolve, join, extname } from 'node:path';

// Configuration
const SERVER_PORT = Number(process.env.PORT ?? 3000);
const CLIENT_DIRECTORY = resolve(import.meta.dirname ?? process.cwd(), '../dist/client');
const SERVER_ENTRY_POINT = resolve(import.meta.dirname ?? process.cwd(), '../dist/server/server.js');

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
  content: Uint8Array;
  contentType: string;
  etag: string;
}

const assetCache = new Map<string, CachedAsset>();

// Generate ETag from content
function generateETag(content: Uint8Array): string {
  const hash = Bun.hash(content);
  return `"${hash.toString(16)}"`;
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
            const file = Bun.file(fullPath);
            const content = new Uint8Array(await file.arrayBuffer());
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

  await scanDirectory(CLIENT_DIRECTORY);
  console.log(`Preloaded ${assetCache.size} static assets`);
}

// TanStack Start server handler type
interface StartHandler {
  fetch(request: Request): Promise<Response>;
}

// Load the TanStack Start handler
let startHandler: StartHandler | null = null;

async function loadHandler(): Promise<StartHandler> {
  if (!startHandler) {
    try {
      const handlerModule = await import(SERVER_ENTRY_POINT);
      startHandler = handlerModule.default as StartHandler;
      console.log('TanStack Start handler loaded successfully');
    } catch (error) {
      const errorMessage = error instanceof Error ? error.message : 'Unknown error';
      throw new Error(
        `TanStack Start handler not found: ${errorMessage}. Make sure to run \`bun run build\` first.`
      );
    }
  }
  return startHandler;
}

// Serve a static asset from cache
function serveStaticAsset(pathname: string, request: Request): Response | null {
  const cached = assetCache.get(pathname);
  if (!cached) {
    return null;
  }

  // Check If-None-Match for conditional request
  const ifNoneMatch = request.headers.get('if-none-match');
  if (ifNoneMatch === cached.etag) {
    return new Response(null, {
      status: 304,
      headers: {
        'ETag': cached.etag,
        'Cache-Control': 'public, max-age=31536000, immutable',
      },
    });
  }

  return new Response(cached.content.buffer as ArrayBuffer, {
    status: 200,
    headers: {
      'Content-Type': cached.contentType,
      'Content-Length': cached.content.length.toString(),
      'ETag': cached.etag,
      'Cache-Control': 'public, max-age=31536000, immutable',
    },
  });
}

// Initialize and start server
async function main(): Promise<void> {
  // Preload static assets
  await preloadStaticAssets();

  // Load TanStack Start handler
  await loadHandler();

  // Start Bun server
  const server = Bun.serve({
    port: SERVER_PORT,
    hostname: '0.0.0.0',

    async fetch(request: Request): Promise<Response> {
      const url = new URL(request.url);
      const pathname = url.pathname;

      // Try to serve static asset first
      const staticResponse = serveStaticAsset(pathname, request);
      if (staticResponse) {
        return staticResponse;
      }

      // Also check for /assets/ prefix paths
      if (pathname.startsWith('/assets/')) {
        const assetPath = pathname;
        const assetResponse = serveStaticAsset(assetPath, request);
        if (assetResponse) {
          return assetResponse;
        }
      }

      // Fall through to TanStack Start handler for SSR
      try {
        const handler = await loadHandler();
        return await handler.fetch(request);
      } catch (error) {
        console.error('Error handling request:', error);
        return new Response('Internal Server Error', { status: 500 });
      }
    },

    error(error: Error): Response {
      console.error('Server error:', error);
      return new Response('Internal Server Error', { status: 500 });
    },
  });

  console.log(`Orochi Dashboard listening on http://${server.hostname}:${server.port}`);
}

// Run the server
main().catch((error) => {
  console.error('Failed to start server:', error);
  process.exit(1);
});
