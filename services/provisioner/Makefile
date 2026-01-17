# Orochi Cloud Provisioner Makefile

# Project settings
BINARY_NAME := provisioner
MODULE := github.com/orochi-db/orochi-cloud/provisioner
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_TIME := $(shell date -u +"%Y-%m-%dT%H:%M:%SZ")

# Go settings
GO := go
GOFLAGS := -v
LDFLAGS := -ldflags="-w -s -X main.version=$(VERSION) -X main.commit=$(COMMIT) -X main.buildTime=$(BUILD_TIME)"

# Docker settings
DOCKER_REGISTRY ?= ghcr.io/orochi-db
DOCKER_IMAGE := $(DOCKER_REGISTRY)/orochi-provisioner
DOCKER_TAG ?= $(VERSION)

# Proto settings
PROTO_DIR := api/proto
PROTO_OUT := api/proto

# Directories
BUILD_DIR := build
DIST_DIR := dist

.PHONY: all build clean test lint fmt proto docker docker-push help

## Default target
all: build

## Build the provisioner binary
build:
	@echo "Building $(BINARY_NAME)..."
	@mkdir -p $(BUILD_DIR)
	$(GO) build $(GOFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME) ./cmd/provisioner

## Build for Linux (for containers)
build-linux:
	@echo "Building $(BINARY_NAME) for Linux..."
	@mkdir -p $(BUILD_DIR)
	CGO_ENABLED=0 GOOS=linux GOARCH=amd64 $(GO) build $(GOFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME)-linux-amd64 ./cmd/provisioner

## Build for multiple platforms
build-all: build-linux
	@echo "Building $(BINARY_NAME) for multiple platforms..."
	CGO_ENABLED=0 GOOS=darwin GOARCH=amd64 $(GO) build $(GOFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME)-darwin-amd64 ./cmd/provisioner
	CGO_ENABLED=0 GOOS=darwin GOARCH=arm64 $(GO) build $(GOFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME)-darwin-arm64 ./cmd/provisioner
	CGO_ENABLED=0 GOOS=linux GOARCH=arm64 $(GO) build $(GOFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME)-linux-arm64 ./cmd/provisioner

## Clean build artifacts
clean:
	@echo "Cleaning..."
	@rm -rf $(BUILD_DIR) $(DIST_DIR)
	@$(GO) clean

## Run tests
test:
	@echo "Running tests..."
	$(GO) test -v -race -coverprofile=coverage.out ./...

## Run tests with coverage report
test-coverage: test
	@echo "Generating coverage report..."
	$(GO) tool cover -html=coverage.out -o coverage.html
	@echo "Coverage report: coverage.html"

## Run linter
lint:
	@echo "Running linter..."
	@if command -v golangci-lint >/dev/null 2>&1; then \
		golangci-lint run ./...; \
	else \
		echo "golangci-lint not installed. Install with: go install github.com/golangci/golangci-lint/cmd/golangci-lint@latest"; \
		exit 1; \
	fi

## Format code
fmt:
	@echo "Formatting code..."
	$(GO) fmt ./...
	@if command -v goimports >/dev/null 2>&1; then \
		goimports -w .; \
	fi

## Generate protobuf code
proto:
	@echo "Generating protobuf code..."
	@if command -v protoc >/dev/null 2>&1; then \
		protoc --go_out=$(PROTO_OUT) --go_opt=paths=source_relative \
			--go-grpc_out=$(PROTO_OUT) --go-grpc_opt=paths=source_relative \
			$(PROTO_DIR)/provisioner.proto; \
	else \
		echo "protoc not installed. Install from: https://grpc.io/docs/protoc-installation/"; \
		exit 1; \
	fi

## Download dependencies
deps:
	@echo "Downloading dependencies..."
	$(GO) mod download
	$(GO) mod tidy

## Verify dependencies
verify:
	@echo "Verifying dependencies..."
	$(GO) mod verify

## Build Docker image
docker:
	@echo "Building Docker image..."
	docker build \
		--build-arg VERSION=$(VERSION) \
		--build-arg COMMIT=$(COMMIT) \
		--build-arg BUILD_TIME=$(BUILD_TIME) \
		-t $(DOCKER_IMAGE):$(DOCKER_TAG) \
		-t $(DOCKER_IMAGE):latest \
		.

## Push Docker image
docker-push: docker
	@echo "Pushing Docker image..."
	docker push $(DOCKER_IMAGE):$(DOCKER_TAG)
	docker push $(DOCKER_IMAGE):latest

## Run locally
run: build
	@echo "Running $(BINARY_NAME)..."
	$(BUILD_DIR)/$(BINARY_NAME)

## Run with config file
run-config: build
	@echo "Running $(BINARY_NAME) with config..."
	$(BUILD_DIR)/$(BINARY_NAME) -config config.yaml

## Install to $GOPATH/bin
install:
	@echo "Installing $(BINARY_NAME)..."
	$(GO) install $(LDFLAGS) ./cmd/provisioner

## Create release tarball
dist: build-all
	@echo "Creating distribution..."
	@mkdir -p $(DIST_DIR)
	@for os in linux darwin; do \
		for arch in amd64 arm64; do \
			tar -czvf $(DIST_DIR)/$(BINARY_NAME)-$(VERSION)-$$os-$$arch.tar.gz \
				-C $(BUILD_DIR) $(BINARY_NAME)-$$os-$$arch; \
		done; \
	done

## Generate mocks for testing
mocks:
	@echo "Generating mocks..."
	@if command -v mockgen >/dev/null 2>&1; then \
		mockgen -source=internal/provisioner/service.go -destination=internal/provisioner/mocks/service_mock.go -package=mocks; \
	else \
		echo "mockgen not installed. Install with: go install github.com/golang/mock/mockgen@latest"; \
	fi

## Show version info
version:
	@echo "Version: $(VERSION)"
	@echo "Commit: $(COMMIT)"
	@echo "Build Time: $(BUILD_TIME)"

## Show help
help:
	@echo "Orochi Cloud Provisioner"
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@grep -E '^## ' Makefile | sed 's/## /  /'
	@echo ""
	@echo "Variables:"
	@echo "  VERSION       - Build version (default: git describe)"
	@echo "  DOCKER_TAG    - Docker image tag (default: VERSION)"
	@echo "  DOCKER_REGISTRY - Docker registry (default: ghcr.io/orochi-db)"
