#!/bin/bash
# Orochi Cloud Installation Script
# This script installs Orochi Cloud and its dependencies on a Kubernetes cluster.

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INFRASTRUCTURE_DIR="$(dirname "$SCRIPT_DIR")"

# Default values
NAMESPACE="orochi-cloud"
RELEASE_NAME="orochi-cloud"
ENVIRONMENT="production"
VALUES_FILE=""
DRY_RUN=false
SKIP_CNPG=false
SKIP_CERT_MANAGER=false
SKIP_INGRESS=false
TIMEOUT="10m"

# Print usage
usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Install Orochi Cloud on a Kubernetes cluster.

OPTIONS:
    -n, --namespace NAME       Kubernetes namespace (default: orochi-cloud)
    -r, --release NAME         Helm release name (default: orochi-cloud)
    -e, --environment ENV      Environment: development, staging, production (default: production)
    -f, --values FILE          Additional values file to use
    -t, --timeout DURATION     Helm timeout (default: 10m)
    --dry-run                  Perform a dry run without making changes
    --skip-cnpg                Skip CloudNativePG operator installation
    --skip-cert-manager        Skip cert-manager installation
    --skip-ingress             Skip ingress-nginx installation
    -h, --help                 Show this help message

EXAMPLES:
    # Install with default settings (production)
    $0

    # Install in development mode
    $0 -e development -n orochi-cloud-dev

    # Install with custom values file
    $0 -f custom-values.yaml

    # Dry run to see what would be installed
    $0 --dry-run

EOF
}

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check prerequisites
check_prerequisites() {
    log_info "Checking prerequisites..."

    # Check kubectl
    if ! command -v kubectl &>/dev/null; then
        log_error "kubectl is not installed. Please install kubectl first."
        exit 1
    fi

    # Check helm
    if ! command -v helm &>/dev/null; then
        log_error "helm is not installed. Please install helm first."
        exit 1
    fi

    # Check cluster connectivity
    if ! kubectl cluster-info &>/dev/null; then
        log_error "Cannot connect to Kubernetes cluster. Please check your kubeconfig."
        exit 1
    fi

    log_success "Prerequisites check passed."
}

# Add Helm repositories
add_helm_repos() {
    log_info "Adding Helm repositories..."

    helm repo add cnpg https://cloudnative-pg.github.io/charts 2>/dev/null || true
    helm repo add jetstack https://charts.jetstack.io 2>/dev/null || true
    helm repo add ingress-nginx https://kubernetes.github.io/ingress-nginx 2>/dev/null || true
    helm repo update

    log_success "Helm repositories added."
}

# Install cert-manager
install_cert_manager() {
    if [ "$SKIP_CERT_MANAGER" = true ]; then
        log_info "Skipping cert-manager installation."
        return
    fi

    log_info "Installing cert-manager..."

    if kubectl get namespace cert-manager &>/dev/null; then
        log_warn "cert-manager namespace already exists. Skipping installation."
        return
    fi

    local cmd="helm upgrade --install cert-manager jetstack/cert-manager \
        --namespace cert-manager \
        --create-namespace \
        --version v1.14.0 \
        --set installCRDs=true \
        --wait \
        --timeout $TIMEOUT"

    if [ "$DRY_RUN" = true ]; then
        cmd="$cmd --dry-run"
    fi

    eval "$cmd"

    log_success "cert-manager installed."
}

# Install ingress-nginx
install_ingress_nginx() {
    if [ "$SKIP_INGRESS" = true ]; then
        log_info "Skipping ingress-nginx installation."
        return
    fi

    log_info "Installing ingress-nginx..."

    if kubectl get namespace ingress-nginx &>/dev/null; then
        log_warn "ingress-nginx namespace already exists. Skipping installation."
        return
    fi

    local cmd="helm upgrade --install ingress-nginx ingress-nginx/ingress-nginx \
        --namespace ingress-nginx \
        --create-namespace \
        --set controller.metrics.enabled=true \
        --set controller.podAnnotations.\"prometheus\.io/scrape\"=true \
        --set controller.podAnnotations.\"prometheus\.io/port\"=10254 \
        --wait \
        --timeout $TIMEOUT"

    if [ "$DRY_RUN" = true ]; then
        cmd="$cmd --dry-run"
    fi

    eval "$cmd"

    log_success "ingress-nginx installed."
}

# Install CloudNativePG operator
install_cloudnativepg() {
    if [ "$SKIP_CNPG" = true ]; then
        log_info "Skipping CloudNativePG installation."
        return
    fi

    log_info "Installing CloudNativePG operator..."

    if kubectl get namespace cnpg-system &>/dev/null; then
        log_warn "CloudNativePG namespace already exists. Checking for existing installation..."
        if kubectl get deployment -n cnpg-system cnpg-controller-manager &>/dev/null; then
            log_warn "CloudNativePG is already installed. Skipping."
            return
        fi
    fi

    local cmd="helm upgrade --install cnpg cnpg/cloudnative-pg \
        --namespace cnpg-system \
        --create-namespace \
        --set monitoring.podMonitorEnabled=true \
        --wait \
        --timeout $TIMEOUT"

    if [ "$DRY_RUN" = true ]; then
        cmd="$cmd --dry-run"
    fi

    eval "$cmd"

    log_success "CloudNativePG operator installed."
}

# Install Orochi Cloud
install_orochi_cloud() {
    log_info "Installing Orochi Cloud..."

    local chart_dir="$INFRASTRUCTURE_DIR/helm/orochi-cloud"
    local values_args=""

    # Select environment-specific values
    case "$ENVIRONMENT" in
        production)
            values_args="-f $chart_dir/values-production.yaml"
            ;;
        staging|development)
            # Use default values.yaml for staging and development
            ;;
        *)
            log_error "Unknown environment: $ENVIRONMENT"
            exit 1
            ;;
    esac

    # Add custom values file if specified
    if [ -n "$VALUES_FILE" ]; then
        if [ -f "$VALUES_FILE" ]; then
            values_args="$values_args -f $VALUES_FILE"
        else
            log_error "Values file not found: $VALUES_FILE"
            exit 1
        fi
    fi

    local cmd="helm upgrade --install $RELEASE_NAME $chart_dir \
        --namespace $NAMESPACE \
        --create-namespace \
        $values_args \
        --wait \
        --timeout $TIMEOUT"

    if [ "$DRY_RUN" = true ]; then
        cmd="$cmd --dry-run --debug"
    fi

    log_info "Running: $cmd"
    eval "$cmd"

    log_success "Orochi Cloud installed."
}

# Verify installation
verify_installation() {
    if [ "$DRY_RUN" = true ]; then
        log_info "Dry run completed. No verification needed."
        return
    fi

    log_info "Verifying installation..."

    # Wait for deployments to be ready
    log_info "Waiting for deployments to be ready..."

    local deployments=("control-plane" "dashboard" "autoscaler" "provisioner")
    for deployment in "${deployments[@]}"; do
        local full_name="$RELEASE_NAME-$deployment"
        if kubectl get deployment "$full_name" -n "$NAMESPACE" &>/dev/null; then
            kubectl rollout status deployment/"$full_name" -n "$NAMESPACE" --timeout=5m || {
                log_warn "Deployment $full_name is not ready yet."
            }
        fi
    done

    log_success "Installation verification completed."
}

# Print installation summary
print_summary() {
    echo ""
    echo "=============================================="
    echo "       Orochi Cloud Installation Summary"
    echo "=============================================="
    echo ""
    echo "Namespace:   $NAMESPACE"
    echo "Release:     $RELEASE_NAME"
    echo "Environment: $ENVIRONMENT"
    echo ""

    if [ "$DRY_RUN" = true ]; then
        echo "This was a DRY RUN. No changes were made."
        return
    fi

    echo "Services:"
    kubectl get svc -n "$NAMESPACE" 2>/dev/null || true
    echo ""

    echo "Pods:"
    kubectl get pods -n "$NAMESPACE" 2>/dev/null || true
    echo ""

    echo "Next steps:"
    echo "  1. Configure DNS for your ingress hosts"
    echo "  2. Set up SSL certificates with cert-manager"
    echo "  3. Configure secrets for production use"
    echo "  4. Review and customize values as needed"
    echo ""
    echo "To access the dashboard:"
    echo "  kubectl port-forward svc/$RELEASE_NAME-dashboard 3000:3000 -n $NAMESPACE"
    echo ""
    echo "To access the API:"
    echo "  kubectl port-forward svc/$RELEASE_NAME-control-plane 8080:8080 -n $NAMESPACE"
    echo ""
}

# Parse arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -n|--namespace)
                NAMESPACE="$2"
                shift 2
                ;;
            -r|--release)
                RELEASE_NAME="$2"
                shift 2
                ;;
            -e|--environment)
                ENVIRONMENT="$2"
                shift 2
                ;;
            -f|--values)
                VALUES_FILE="$2"
                shift 2
                ;;
            -t|--timeout)
                TIMEOUT="$2"
                shift 2
                ;;
            --dry-run)
                DRY_RUN=true
                shift
                ;;
            --skip-cnpg)
                SKIP_CNPG=true
                shift
                ;;
            --skip-cert-manager)
                SKIP_CERT_MANAGER=true
                shift
                ;;
            --skip-ingress)
                SKIP_INGRESS=true
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done
}

# Main function
main() {
    parse_args "$@"

    echo ""
    echo "=============================================="
    echo "        Orochi Cloud Installation"
    echo "=============================================="
    echo ""

    check_prerequisites
    add_helm_repos

    # Install dependencies
    install_cert_manager
    install_ingress_nginx
    install_cloudnativepg

    # Install Orochi Cloud
    install_orochi_cloud

    # Verify installation
    verify_installation

    # Print summary
    print_summary
}

# Run main function
main "$@"
