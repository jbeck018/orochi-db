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
# Docker Builds (DigitalOcean Container Registry)
# =============================================================================
DO_REGISTRY ?= registry.digitalocean.com/orochi-registry
K8S_NAMESPACE ?= orochi-cloud
K8S_CONTEXT ?= do-sfo3-orochi-cloud

# Build dashboard image (using Node.js Dockerfile due to Bun AVX compatibility issues)
docker-dashboard:
	cd apps/dashboard && docker build --platform linux/amd64 \
		-f Dockerfile.node \
		-t $(DO_REGISTRY)/dashboard:latest .

# Build control plane image
docker-control-plane:
	cd services/control-plane && docker build --platform linux/amd64 \
		-t $(DO_REGISTRY)/control-plane:latest .

# Build provisioner image
docker-provisioner:
	cd services/provisioner && docker build --platform linux/amd64 \
		-t $(DO_REGISTRY)/provisioner:latest .

# Build autoscaler image
docker-autoscaler:
	cd services/autoscaler && docker build --platform linux/amd64 \
		-t $(DO_REGISTRY)/autoscaler:latest .

# Build all Docker images
docker: docker-dashboard docker-control-plane docker-provisioner docker-autoscaler

# =============================================================================
# DigitalOcean Kubernetes Deployment
# =============================================================================

# Login to DigitalOcean Container Registry
do-login:
	doctl registry login

# Push dashboard image to registry
push-dashboard: docker-dashboard
	docker push $(DO_REGISTRY)/dashboard:latest

# Push control plane image to registry
push-control-plane: docker-control-plane
	docker push $(DO_REGISTRY)/control-plane:latest

# Push all images
push-all: push-dashboard push-control-plane

# Deploy dashboard (build, push, restart)
deploy-dashboard: do-login push-dashboard
	kubectl config use-context $(K8S_CONTEXT)
	kubectl rollout restart deployment/dashboard -n $(K8S_NAMESPACE)
	kubectl rollout status deployment/dashboard -n $(K8S_NAMESPACE)

# Deploy control plane (build, push, restart)
deploy-api: do-login push-control-plane
	kubectl config use-context $(K8S_CONTEXT)
	kubectl rollout restart deployment/control-plane -n $(K8S_NAMESPACE)
	kubectl rollout status deployment/control-plane -n $(K8S_NAMESPACE)

# Alias for deploy-api
deploy-control-plane: deploy-api

# Deploy all services
deploy-all: do-login push-all
	kubectl config use-context $(K8S_CONTEXT)
	kubectl rollout restart deployment/dashboard -n $(K8S_NAMESPACE)
	kubectl rollout restart deployment/control-plane -n $(K8S_NAMESPACE)
	kubectl rollout status deployment/dashboard -n $(K8S_NAMESPACE)
	kubectl rollout status deployment/control-plane -n $(K8S_NAMESPACE)

# Quick deploy (skip docker build, just restart with existing images)
deploy-quick:
	kubectl config use-context $(K8S_CONTEXT)
	kubectl rollout restart deployment/dashboard -n $(K8S_NAMESPACE)
	kubectl rollout restart deployment/control-plane -n $(K8S_NAMESPACE)

# Full deployment alias
deploy: deploy-all
	@echo ""
	@echo "=== Deployment Complete ==="
	@echo "Dashboard:      http://24.199.68.43 (dashboard.orochi.cloud)"
	@echo "API:            http://134.199.141.125 (api.orochi.cloud)"
	@echo "Check status:   make do-status"

# =============================================================================
# DigitalOcean Kubernetes Operations
# =============================================================================

# Set kubectl context to DigitalOcean
do-context:
	kubectl config use-context $(K8S_CONTEXT)

# Get kubeconfig from DigitalOcean
do-kubeconfig:
	doctl kubernetes cluster kubeconfig save orochi-cloud

# Install CloudNativePG operator
do-install-cnpg:
	kubectl apply -f https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.21/releases/cnpg-1.21.0.yaml

# Check deployment status
do-status:
	@kubectl config use-context $(K8S_CONTEXT)
	@echo "=== Pods ==="
	@kubectl get pods -n $(K8S_NAMESPACE)
	@echo ""
	@echo "=== Services ==="
	@kubectl get svc -n $(K8S_NAMESPACE)
	@echo ""
	@echo "=== Deployments ==="
	@kubectl get deployments -n $(K8S_NAMESPACE)

# View dashboard logs
do-logs-dashboard:
	kubectl config use-context $(K8S_CONTEXT)
	kubectl logs -f deployment/dashboard -n $(K8S_NAMESPACE)

# View control plane logs
do-logs-api:
	kubectl config use-context $(K8S_CONTEXT)
	kubectl logs -f deployment/control-plane -n $(K8S_NAMESPACE)

# Port forward dashboard for local testing
do-forward-dashboard:
	kubectl config use-context $(K8S_CONTEXT)
	kubectl port-forward svc/dashboard 3000:80 -n $(K8S_NAMESPACE)

# Port forward API for local testing
do-forward-api:
	kubectl config use-context $(K8S_CONTEXT)
	kubectl port-forward svc/control-plane 8080:8080 -n $(K8S_NAMESPACE)

# Apply Kubernetes manifests
do-apply:
	kubectl config use-context $(K8S_CONTEXT)
	kubectl apply -f infrastructure/digitalocean/

# Describe pods (for debugging)
do-describe:
	kubectl config use-context $(K8S_CONTEXT)
	kubectl describe pods -n $(K8S_NAMESPACE)

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
	@echo "Dashboard:"
	@echo "  make dashboard       - Build dashboard for production"
	@echo "  make dashboard-dev   - Run dashboard in development mode"
	@echo "  make dashboard-clean - Clean dashboard build artifacts"
	@echo ""
	@echo "Local Development:"
	@echo "  make install         - Install all dependencies"
	@echo "  make dev             - Run dashboard dev server"
	@echo "  make dev-all         - Show how to run all services"
	@echo "  make dev-dashboard   - Run dashboard (port 3000)"
	@echo "  make dev-control-plane - Run control plane (port 8080)"
	@echo ""
	@echo "Docker (DigitalOcean Registry):"
	@echo "  make docker              - Build all Docker images"
	@echo "  make docker-dashboard    - Build dashboard image (Node.js)"
	@echo "  make docker-control-plane - Build control plane image"
	@echo "  make push-all            - Push all images to DO registry"
	@echo ""
	@echo "DigitalOcean Kubernetes Deployment:"
	@echo "  make deploy              - Deploy all services (build, push, restart)"
	@echo "  make deploy-dashboard    - Deploy dashboard only"
	@echo "  make deploy-api          - Deploy control plane only"
	@echo "  make deploy-quick        - Restart deployments (no rebuild)"
	@echo ""
	@echo "DigitalOcean Operations:"
	@echo "  make do-login            - Login to DO container registry"
	@echo "  make do-kubeconfig       - Fetch kubeconfig from DO"
	@echo "  make do-context          - Switch kubectl to DO context"
	@echo "  make do-status           - Check deployment status"
	@echo "  make do-logs-dashboard   - View dashboard logs"
	@echo "  make do-logs-api         - View control plane logs"
	@echo "  make do-forward-dashboard - Port forward dashboard to localhost:3000"
	@echo "  make do-forward-api      - Port forward API to localhost:8080"
	@echo "  make do-apply            - Apply Kubernetes manifests"
	@echo "  make do-install-cnpg     - Install CloudNativePG operator"
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
	@echo ""
	@echo "Production URLs:"
	@echo "  Dashboard: http://24.199.68.43 (dashboard.orochi.cloud)"
	@echo "  API:       http://134.199.141.125 (api.orochi.cloud)"
