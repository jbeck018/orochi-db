# Orochi Cloud CLI Makefile

# Build variables
VERSION ?= 1.0.0
GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)
LDFLAGS := -ldflags "-X github.com/orochi-db/orochi-cloud/cli/internal/cmd.Version=$(VERSION) \
	-X github.com/orochi-db/orochi-cloud/cli/internal/cmd.GitCommit=$(GIT_COMMIT) \
	-X github.com/orochi-db/orochi-cloud/cli/internal/cmd.BuildDate=$(BUILD_DATE)"

# Binary name
BINARY_NAME := orochi
OUTPUT_DIR := ./bin

# Go parameters
GOCMD := go
GOBUILD := $(GOCMD) build
GOTEST := $(GOCMD) test
GOMOD := $(GOCMD) mod
GOFMT := $(GOCMD) fmt
GOVET := $(GOCMD) vet

.PHONY: all build clean test fmt vet lint deps install help

all: build

## Build the CLI binary
build:
	@echo "Building $(BINARY_NAME)..."
	@mkdir -p $(OUTPUT_DIR)
	$(GOBUILD) $(LDFLAGS) -o $(OUTPUT_DIR)/$(BINARY_NAME) ./cmd/orochi/
	@echo "Built $(OUTPUT_DIR)/$(BINARY_NAME)"

## Build for multiple platforms
build-all: build-linux build-darwin build-windows

build-linux:
	@echo "Building for Linux..."
	@mkdir -p $(OUTPUT_DIR)
	GOOS=linux GOARCH=amd64 $(GOBUILD) $(LDFLAGS) -o $(OUTPUT_DIR)/$(BINARY_NAME)-linux-amd64 ./cmd/orochi/
	GOOS=linux GOARCH=arm64 $(GOBUILD) $(LDFLAGS) -o $(OUTPUT_DIR)/$(BINARY_NAME)-linux-arm64 ./cmd/orochi/

build-darwin:
	@echo "Building for macOS..."
	@mkdir -p $(OUTPUT_DIR)
	GOOS=darwin GOARCH=amd64 $(GOBUILD) $(LDFLAGS) -o $(OUTPUT_DIR)/$(BINARY_NAME)-darwin-amd64 ./cmd/orochi/
	GOOS=darwin GOARCH=arm64 $(GOBUILD) $(LDFLAGS) -o $(OUTPUT_DIR)/$(BINARY_NAME)-darwin-arm64 ./cmd/orochi/

build-windows:
	@echo "Building for Windows..."
	@mkdir -p $(OUTPUT_DIR)
	GOOS=windows GOARCH=amd64 $(GOBUILD) $(LDFLAGS) -o $(OUTPUT_DIR)/$(BINARY_NAME)-windows-amd64.exe ./cmd/orochi/

## Clean build artifacts
clean:
	@echo "Cleaning..."
	@rm -rf $(OUTPUT_DIR)
	@rm -f $(BINARY_NAME)
	@echo "Done"

## Run tests
test:
	@echo "Running tests..."
	$(GOTEST) -v ./...

## Run tests with coverage
test-coverage:
	@echo "Running tests with coverage..."
	$(GOTEST) -v -coverprofile=coverage.out ./...
	$(GOCMD) tool cover -html=coverage.out -o coverage.html
	@echo "Coverage report: coverage.html"

## Format code
fmt:
	@echo "Formatting code..."
	$(GOFMT) ./...

## Run go vet
vet:
	@echo "Running vet..."
	$(GOVET) ./...

## Run linter (requires golangci-lint)
lint:
	@echo "Running linter..."
	@if command -v golangci-lint >/dev/null 2>&1; then \
		golangci-lint run ./...; \
	else \
		echo "golangci-lint not installed, skipping..."; \
	fi

## Download dependencies
deps:
	@echo "Downloading dependencies..."
	$(GOMOD) download
	$(GOMOD) tidy

## Install binary to GOPATH/bin
install: build
	@echo "Installing $(BINARY_NAME)..."
	@cp $(OUTPUT_DIR)/$(BINARY_NAME) $(GOPATH)/bin/$(BINARY_NAME)
	@echo "Installed to $(GOPATH)/bin/$(BINARY_NAME)"

## Run the CLI (for development)
run: build
	$(OUTPUT_DIR)/$(BINARY_NAME)

## Show help
help:
	@echo "Orochi Cloud CLI - Makefile targets:"
	@echo ""
	@echo "  build          Build the CLI binary"
	@echo "  build-all      Build for Linux, macOS, and Windows"
	@echo "  clean          Remove build artifacts"
	@echo "  test           Run tests"
	@echo "  test-coverage  Run tests with coverage report"
	@echo "  fmt            Format source code"
	@echo "  vet            Run go vet"
	@echo "  lint           Run golangci-lint"
	@echo "  deps           Download and tidy dependencies"
	@echo "  install        Install binary to GOPATH/bin"
	@echo "  run            Build and run the CLI"
	@echo "  help           Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  VERSION        Version to embed (default: $(VERSION))"
