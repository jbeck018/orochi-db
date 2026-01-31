# Multi-stage Dockerfile for HowlerOps Dashboard
# Uses Node.js for both build and runtime (DO compatible)

# Stage 1: Build Stage with Node.js
FROM node:20-alpine AS builder

ARG VITE_API_URL
ENV VITE_API_URL=${VITE_API_URL}

WORKDIR /app

# Install bun for package management only
RUN npm install -g bun

# Copy package files
COPY package.json bun.lock* ./

# Install dependencies with bun (faster) or fallback to npm
RUN bun install --frozen-lockfile || npm install

# Copy source code
COPY . .

# Build with Node.js (not bun runtime to avoid AVX issues)
RUN npm run build:node

# Stage 2: Runtime Stage with Node.js
FROM node:20-alpine

WORKDIR /app

# Copy built assets from builder
COPY --from=builder /app/dist ./dist

# Copy server files
COPY --from=builder /app/server ./server

# Copy node_modules for runtime dependencies
COPY --from=builder /app/node_modules ./node_modules
COPY --from=builder /app/package.json ./package.json

# Create non-root user
RUN addgroup -g 1001 -S nodejs && \
    adduser -S nodejs -u 1001 -G nodejs && \
    chown -R nodejs:nodejs /app

USER nodejs

ENV NODE_ENV=production
ENV PORT=3000

EXPOSE 3000

# Run with Node.js
CMD ["npm", "run", "start:node"]
