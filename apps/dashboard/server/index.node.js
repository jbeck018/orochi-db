// Production server entry point for Orochi Dashboard (Node.js compatible)
const { createReadStream, readdirSync, statSync } = require('fs');
const { resolve, join, extname } = require('path');
const http = require('http');
const crypto = require('crypto');

// Configuration
const SERVER_PORT = Number(process.env.PORT ?? 3000);
const CLIENT_DIRECTORY = resolve(__dirname, '../dist/client');

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

// Build file index
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

// Create HTTP server
const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const pathname = url.pathname;

  // Health check endpoint for Kubernetes probes
  if (pathname === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok', timestamp: new Date().toISOString() }));
    return;
  }

  // Try to serve static file
  const fileInfo = fileIndex.get(pathname);
  if (fileInfo) {
    const contentType = MIME_TYPES[fileInfo.ext] || 'application/octet-stream';
    res.writeHead(200, {
      'Content-Type': contentType,
      'Content-Length': fileInfo.size,
      'Cache-Control': 'public, max-age=31536000, immutable'
    });
    createReadStream(fileInfo.path).pipe(res);
    return;
  }

  // Serve index.html for SPA routes
  const indexInfo = fileIndex.get('/index.html');
  if (indexInfo) {
    res.writeHead(200, {
      'Content-Type': 'text/html',
      'Content-Length': indexInfo.size
    });
    createReadStream(indexInfo.path).pipe(res);
    return;
  }

  // No index.html - generate HTML shell for SPA
  // Find the main JS and CSS bundles
  let mainJs = null;
  let mainCss = null;
  for (const [path] of fileIndex) {
    if (path.includes('/assets/main-') && path.endsWith('.js')) {
      mainJs = path;
    }
    if (path.includes('/assets/main-') && path.endsWith('.css')) {
      mainCss = path;
    }
  }

  if (mainJs) {
    const html = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>HowlerOps Dashboard</title>
  <meta name="description" content="HowlerOps - OrochiDB Management Platform" />
  <link rel="icon" type="image/png" href="/assets/howlerops-icon-qz7Q_0BK.png" />
  ${mainCss ? `<link rel="stylesheet" href="${mainCss}" />` : ''}
</head>
<body>
  <div id="root"></div>
  <script type="module" src="${mainJs}"></script>
</body>
</html>`;
    res.writeHead(200, {
      'Content-Type': 'text/html',
      'Content-Length': Buffer.byteLength(html)
    });
    res.end(html);
    return;
  }

  // 404 - no index.html and no main.js found
  res.writeHead(404);
  res.end('Not Found');
});

server.listen(SERVER_PORT, '0.0.0.0', () => {
  console.log(`Orochi Dashboard listening on http://0.0.0.0:${SERVER_PORT}`);
});
