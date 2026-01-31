# PgDog Router Service Makefile
# Orochi DB Connection Pooler and Query Router

.PHONY: all build test docker-build docker-push clean help lint format dev run

# Configuration
REGISTRY ?= registry.digitalocean.com/orochi-registry
IMAGE_NAME ?= pgdog-router
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
DOCKER_TAG ?= $(REGISTRY)/$(IMAGE_NAME):$(VERSION)
DOCKER_LATEST ?= $(REGISTRY)/$(IMAGE_NAME):latest
PLATFORM ?= linux/amd64

# PgDog version to build
PGDOG_VERSION ?= latest

# Go configuration (for config generator)
GO ?= go
GOFLAGS ?= -mod=readonly

# Build directories
BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin

# Default target
all: build

# Build the config generator tool
build: $(BIN_DIR)/pgdog-config-gen

$(BIN_DIR)/pgdog-config-gen: cmd/pgdog-config-gen/main.go
	@echo "==> Building pgdog-config-gen..."
	@mkdir -p $(BIN_DIR)
	$(GO) build $(GOFLAGS) -o $(BIN_DIR)/pgdog-config-gen ./cmd/pgdog-config-gen

# Run tests
test:
	@echo "==> Running tests..."
	$(GO) test -v -race -cover ./...

# Lint the codebase
lint:
	@echo "==> Running linters..."
	@if command -v golangci-lint >/dev/null 2>&1; then \
		golangci-lint run ./...; \
	else \
		echo "golangci-lint not installed, skipping..."; \
	fi
	@echo "==> Checking shell scripts..."
	@if command -v shellcheck >/dev/null 2>&1; then \
		shellcheck scripts/*.sh; \
	else \
		echo "shellcheck not installed, skipping..."; \
	fi

# Format code
format:
	@echo "==> Formatting Go code..."
	$(GO) fmt ./...
	@echo "==> Formatting shell scripts..."
	@if command -v shfmt >/dev/null 2>&1; then \
		shfmt -w scripts/*.sh; \
	else \
		echo "shfmt not installed, skipping..."; \
	fi

# Build Docker image
docker-build:
	@echo "==> Building Docker image $(DOCKER_TAG)..."
	docker build \
		--platform $(PLATFORM) \
		--build-arg PGDOG_VERSION=$(PGDOG_VERSION) \
		-t $(DOCKER_TAG) \
		-t $(DOCKER_LATEST) \
		-f Dockerfile \
		.

# Build development Docker image
docker-build-dev:
	@echo "==> Building development Docker image..."
	docker build \
		--platform $(PLATFORM) \
		-t $(IMAGE_NAME):dev \
		-f Dockerfile.dev \
		.

# Push Docker image to registry
docker-push: docker-build
	@echo "==> Pushing Docker image to registry..."
	docker push $(DOCKER_TAG)
	docker push $(DOCKER_LATEST)

# Run locally with Docker Compose
dev:
	@echo "==> Starting development environment..."
	docker-compose -f docker-compose.dev.yml up --build

# Run PgDog locally (requires Docker)
run: docker-build-dev
	@echo "==> Running PgDog router..."
	docker run --rm -it \
		-p 5432:5432 \
		-p 6432:6432 \
		-p 9090:9090 \
		-v $(PWD)/config:/etc/pgdog:ro \
		$(IMAGE_NAME):dev

# Generate configuration from template
config-gen: build
	@echo "==> Generating configuration..."
	$(BIN_DIR)/pgdog-config-gen \
		-template config/pgdog.toml.tmpl \
		-output config/pgdog.toml \
		-cluster-name default \
		-namespace orochi-cloud \
		-database-name postgres \
		-shard-count 32

# Clean build artifacts
clean:
	@echo "==> Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)
	rm -f config/pgdog.toml config/users.toml
	docker rmi $(DOCKER_TAG) $(DOCKER_LATEST) $(IMAGE_NAME):dev 2>/dev/null || true

# Deploy to Kubernetes
deploy:
	@echo "==> Deploying to Kubernetes..."
	kubectl apply -f ../../infrastructure/digitalocean/pgdog-router.yaml

# Show help
help:
	@echo "PgDog Router Service Makefile"
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  all           Build the config generator (default)"
	@echo "  build         Build the pgdog-config-gen tool"
	@echo "  test          Run tests"
	@echo "  lint          Run linters"
	@echo "  format        Format code"
	@echo "  docker-build  Build the production Docker image"
	@echo "  docker-build-dev  Build the development Docker image"
	@echo "  docker-push   Push Docker image to registry"
	@echo "  dev           Start development environment"
	@echo "  run           Run PgDog locally with Docker"
	@echo "  config-gen    Generate config from template"
	@echo "  clean         Clean build artifacts"
	@echo "  deploy        Deploy to Kubernetes"
	@echo "  help          Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  REGISTRY      Container registry (default: registry.digitalocean.com/orochi-registry)"
	@echo "  IMAGE_NAME    Image name (default: pgdog-router)"
	@echo "  VERSION       Image version (default: git describe)"
	@echo "  PGDOG_VERSION PgDog version to build (default: latest)"
	@echo "  PLATFORM      Build platform (default: linux/amd64)"
