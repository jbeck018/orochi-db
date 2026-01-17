# Orochi DB - Monorepo Makefile
# This Makefile delegates to subdirectory Makefiles for building different components

.PHONY: all extension services cli dashboard clean test lint format help
.PHONY: dev dev-all dev-dashboard dev-control-plane dev-services
.PHONY: deploy deploy-dashboard deploy-control-plane deploy-fks
.PHONY: docker docker-dashboard docker-control-plane docker-provisioner docker-autoscaler

# Default target - show help
all: help

# =============================================================================
# PostgreSQL Extension
# =============================================================================
extension:
	$(MAKE) -C extensions/postgres

extension-install:
	$(MAKE) -C extensions/postgres install

extension-clean:
	$(MAKE) -C extensions/postgres clean

extension-test:
	$(MAKE) -C extensions/postgres installcheck

extension-unit-test:
	cd extensions/postgres/test/unit && make && ./run_tests

# =============================================================================
# Go Services
# =============================================================================
services:
	$(MAKE) -C services

services-control-plane:
	cd services/control-plane && go build -o bin/control-plane ./cmd/server

services-provisioner:
	cd services/provisioner && go build -o bin/provisioner ./cmd/provisioner

services-autoscaler:
	cd services/autoscaler && go build -o bin/autoscaler ./cmd/autoscaler

services-clean:
	rm -rf services/control-plane/bin services/provisioner/bin services/autoscaler/bin

services-test:
	cd services/control-plane && go test ./...
	cd services/provisioner && go test ./...
	cd services/autoscaler && go test ./...

# =============================================================================
# CLI Tool
# =============================================================================
cli:
	cd tools/cli && go build -o bin/orochi ./cmd/orochi

cli-install: cli
	cp tools/cli/bin/orochi /usr/local/bin/orochi

cli-clean:
	rm -rf tools/cli/bin

cli-test:
	cd tools/cli && go test ./...

# =============================================================================
# Dashboard (Bun)
# =============================================================================
dashboard:
	cd apps/dashboard && bun install && bun run build

dashboard-dev:
	cd apps/dashboard && bun install && bun run dev

dashboard-clean:
	rm -rf apps/dashboard/dist apps/dashboard/node_modules

dashboard-type-check:
	cd apps/dashboard && bun run type-check

dashboard-lint:
	cd apps/dashboard && bun run lint

# =============================================================================
# All Components
# =============================================================================
build-all: extension services cli dashboard

clean: extension-clean services-clean cli-clean dashboard-clean
	rm -rf go.work.sum node_modules bun.lock

test: extension-unit-test services-test cli-test

# =============================================================================
# Code Quality
# =============================================================================
lint:
	$(MAKE) -C extensions/postgres lint
	cd services/control-plane && go vet ./...
	cd services/provisioner && go vet ./...
	cd services/autoscaler && go vet ./...
	cd tools/cli && go vet ./...
	cd apps/dashboard && bun run lint

format:
	$(MAKE) -C extensions/postgres format
	cd services/control-plane && go fmt ./...
	cd services/provisioner && go fmt ./...
	cd services/autoscaler && go fmt ./...
	cd tools/cli && go fmt ./...

# =============================================================================
# Go Workspace
# =============================================================================
go-sync:
	go work sync

go-tidy:
	cd services/control-plane && go mod tidy
	cd services/provisioner && go mod tidy
	cd services/autoscaler && go mod tidy
	cd tools/cli && go mod tidy
	cd packages/go/shared && go mod tidy

# =============================================================================
# Local Development (mirrors production setup)
# =============================================================================

# Install all dependencies
install:
	bun install
	cd apps/dashboard && bun install

# Run dashboard in dev mode (same as deployed on Fly.io)
dev: dev-dashboard

# Run all services locally
dev-all:
	@echo "Starting all services..."
	@echo "Dashboard:      http://localhost:3000"
	@echo "Control Plane:  http://localhost:8080"
	@echo ""
	@echo "Run each in separate terminals:"
	@echo "  make dev-dashboard"
	@echo "  make dev-control-plane"

dev-dashboard:
	cd apps/dashboard && bun run dev

dev-control-plane:
	cd services/control-plane && go run ./cmd/server

dev-provisioner:
	cd services/provisioner && go run ./cmd/provisioner

dev-autoscaler:
	cd services/autoscaler && go run ./cmd/autoscaler

# =============================================================================
# Docker Builds
# =============================================================================
REGISTRY ?= registry.fly.io

docker-dashboard:
	cd apps/dashboard && docker build \
		--build-arg VITE_API_URL=https://orochi-control-plane.fly.dev \
		-t $(REGISTRY)/orochi-dashboard:latest .

docker-control-plane:
	cd services/control-plane && docker build \
		-t $(REGISTRY)/orochi-control-plane:latest .

docker-provisioner:
	cd services/provisioner && docker build \
		-t $(REGISTRY)/orochi-provisioner:latest .

docker-autoscaler:
	cd services/autoscaler && docker build \
		-t $(REGISTRY)/orochi-autoscaler:latest .

docker: docker-dashboard docker-control-plane docker-provisioner docker-autoscaler

# =============================================================================
# Fly.io Deployment
# =============================================================================

# Deploy dashboard to Fly.io
deploy-dashboard:
	cd apps/dashboard && fly deploy

# Deploy control plane to Fly.io
deploy-control-plane:
	cd services/control-plane && fly deploy

# Deploy all Fly.io apps (Dashboard + Control Plane)
deploy-fly: deploy-dashboard deploy-control-plane

# =============================================================================
# FKS (Fly.io Kubernetes) Deployment
# =============================================================================

# Get FKS kubeconfig
fks-kubeconfig:
	fly ext k8s kubeconfig --name orochi-fks > ~/.kube/orochi-fks.yaml
	@echo "Run: export KUBECONFIG=~/.kube/orochi-fks.yaml"

# Install CloudNativePG operator
fks-install-cnpg:
	kubectl apply -f https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.21/releases/cnpg-1.21.0.yaml

# Deploy FKS namespace and secrets
fks-init:
	kubectl apply -f infrastructure/fks/namespace.yaml
	@echo "IMPORTANT: Update infrastructure/fks/secrets.yaml with actual secrets before applying"
	@echo "Run: kubectl apply -f infrastructure/fks/secrets.yaml"

# Deploy Provisioner to FKS
fks-provisioner:
	kubectl apply -f infrastructure/fks/provisioner/

# Deploy Autoscaler to FKS
fks-autoscaler:
	kubectl apply -f infrastructure/fks/autoscaler/

# Deploy all FKS services
deploy-fks: fks-provisioner fks-autoscaler
	@echo "FKS services deployed. Check status with:"
	@echo "  kubectl get pods -n orochi-cloud"

# Full FKS setup (assumes cluster exists and kubeconfig is set)
fks-setup: fks-install-cnpg fks-init
	@echo "FKS setup complete. Now run: make deploy-fks"

# Check FKS status
fks-status:
	@echo "=== Pods ==="
	kubectl get pods -n orochi-cloud
	@echo ""
	@echo "=== Services ==="
	kubectl get svc -n orochi-cloud
	@echo ""
	@echo "=== CloudNativePG Clusters ==="
	kubectl get clusters.postgresql.cnpg.io -n orochi-cloud 2>/dev/null || echo "No clusters found"

# View FKS logs
fks-logs-provisioner:
	kubectl logs -f deployment/provisioner -n orochi-cloud

fks-logs-autoscaler:
	kubectl logs -f deployment/autoscaler -n orochi-cloud

# =============================================================================
# Full Deployment (Fly.io + FKS)
# =============================================================================
deploy: deploy-fly deploy-fks
	@echo ""
	@echo "=== Deployment Complete ==="
	@echo "Dashboard:      https://orochi-dashboard.fly.dev"
	@echo "Control Plane:  https://orochi-control-plane.fly.dev"
	@echo "FKS Services:   kubectl get pods -n orochi-cloud"

# =============================================================================
# Help
# =============================================================================
help:
	@echo "Orochi DB - Monorepo Build System"
	@echo ""
	@echo "PostgreSQL Extension:"
	@echo "  make extension           - Build the PostgreSQL extension"
	@echo "  make extension-install   - Install extension to PostgreSQL"
	@echo "  make extension-test      - Run extension regression tests"
	@echo "  make extension-unit-test - Run extension unit tests"
	@echo "  make extension-clean     - Clean extension build artifacts"
	@echo ""
	@echo "Go Services:"
	@echo "  make services              - Build all Go services"
	@echo "  make services-control-plane - Build control plane service"
	@echo "  make services-provisioner   - Build provisioner service"
	@echo "  make services-autoscaler    - Build autoscaler service"
	@echo "  make services-test          - Run service tests"
	@echo "  make services-clean         - Clean service binaries"
	@echo ""
	@echo "CLI Tool:"
	@echo "  make cli          - Build CLI tool"
	@echo "  make cli-install  - Install CLI to /usr/local/bin"
	@echo "  make cli-test     - Run CLI tests"
	@echo "  make cli-clean    - Clean CLI binary"
	@echo ""
	@echo "Dashboard (Bun):"
	@echo "  make dashboard       - Build dashboard for production"
	@echo "  make dashboard-dev   - Run dashboard in development mode"
	@echo "  make dashboard-clean - Clean dashboard build artifacts"
	@echo ""
	@echo "Local Development:"
	@echo "  make install         - Install all dependencies (Bun)"
	@echo "  make dev             - Run dashboard dev server"
	@echo "  make dev-all         - Show how to run all services"
	@echo "  make dev-dashboard   - Run dashboard (port 3000)"
	@echo "  make dev-control-plane - Run control plane (port 8080)"
	@echo ""
	@echo "Docker:"
	@echo "  make docker            - Build all Docker images"
	@echo "  make docker-dashboard  - Build dashboard image"
	@echo "  make docker-control-plane - Build control plane image"
	@echo ""
	@echo "Fly.io Deployment:"
	@echo "  make deploy-dashboard     - Deploy dashboard to Fly.io"
	@echo "  make deploy-control-plane - Deploy control plane to Fly.io"
	@echo "  make deploy-fly           - Deploy all Fly.io apps"
	@echo ""
	@echo "FKS (Fly.io Kubernetes):"
	@echo "  make fks-kubeconfig    - Get FKS kubeconfig"
	@echo "  make fks-setup         - Install CNPG and init namespace"
	@echo "  make deploy-fks        - Deploy Provisioner + Autoscaler"
	@echo "  make fks-status        - Check FKS service status"
	@echo "  make fks-logs-provisioner - View Provisioner logs"
	@echo "  make fks-logs-autoscaler  - View Autoscaler logs"
	@echo ""
	@echo "Full Deployment:"
	@echo "  make deploy  - Deploy everything (Fly.io + FKS)"
	@echo ""
	@echo "All Components:"
	@echo "  make build-all  - Build all components"
	@echo "  make clean      - Clean all build artifacts"
	@echo "  make test       - Run all tests"
	@echo "  make lint       - Run linters on all code"
	@echo "  make format     - Format all code"
	@echo ""
	@echo "Go Workspace:"
	@echo "  make go-sync    - Sync go.work file"
	@echo "  make go-tidy    - Run go mod tidy on all modules"
