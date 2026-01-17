#!/bin/bash
# Orochi Cloud Uninstallation Script
# This script removes Orochi Cloud and optionally its dependencies from a Kubernetes cluster.

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
NAMESPACE="orochi-cloud"
RELEASE_NAME="orochi-cloud"
REMOVE_DEPS=false
REMOVE_NAMESPACE=false
REMOVE_PVCS=false
FORCE=false
DRY_RUN=false

# Print usage
usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Uninstall Orochi Cloud from a Kubernetes cluster.

OPTIONS:
    -n, --namespace NAME       Kubernetes namespace (default: orochi-cloud)
    -r, --release NAME         Helm release name (default: orochi-cloud)
    --remove-deps              Also remove dependencies (CloudNativePG, cert-manager, ingress-nginx)
    --remove-namespace         Remove the namespace after uninstallation
    --remove-pvcs              Remove persistent volume claims (DATA LOSS!)
    --force                    Skip confirmation prompts
    --dry-run                  Show what would be removed without making changes
    -h, --help                 Show this help message

EXAMPLES:
    # Basic uninstall (keeps namespace and PVCs)
    $0

    # Full cleanup including namespace
    $0 --remove-namespace

    # Complete removal including dependencies (CAUTION!)
    $0 --remove-deps --remove-namespace --remove-pvcs

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

# Confirm action
confirm() {
    if [ "$FORCE" = true ]; then
        return 0
    fi

    local message="$1"
    echo -e "${YELLOW}$message${NC}"
    read -p "Are you sure you want to continue? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        log_info "Operation cancelled."
        exit 0
    fi
}

# Check prerequisites
check_prerequisites() {
    log_info "Checking prerequisites..."

    # Check kubectl
    if ! command -v kubectl &>/dev/null; then
        log_error "kubectl is not installed."
        exit 1
    fi

    # Check helm
    if ! command -v helm &>/dev/null; then
        log_error "helm is not installed."
        exit 1
    fi

    # Check cluster connectivity
    if ! kubectl cluster-info &>/dev/null; then
        log_error "Cannot connect to Kubernetes cluster."
        exit 1
    fi

    log_success "Prerequisites check passed."
}

# Uninstall Orochi Cloud
uninstall_orochi_cloud() {
    log_info "Uninstalling Orochi Cloud..."

    if ! helm list -n "$NAMESPACE" | grep -q "$RELEASE_NAME"; then
        log_warn "Helm release $RELEASE_NAME not found in namespace $NAMESPACE."
        return
    fi

    local cmd="helm uninstall $RELEASE_NAME -n $NAMESPACE"

    if [ "$DRY_RUN" = true ]; then
        cmd="$cmd --dry-run"
    fi

    log_info "Running: $cmd"
    eval "$cmd"

    log_success "Orochi Cloud uninstalled."
}

# Remove PVCs
remove_pvcs() {
    if [ "$REMOVE_PVCS" != true ]; then
        return
    fi

    log_info "Removing persistent volume claims..."

    if [ "$DRY_RUN" = true ]; then
        log_info "[DRY RUN] Would delete PVCs in namespace $NAMESPACE:"
        kubectl get pvc -n "$NAMESPACE" 2>/dev/null || true
        return
    fi

    confirm "This will DELETE ALL DATA in persistent volume claims in namespace $NAMESPACE!"

    kubectl delete pvc --all -n "$NAMESPACE" 2>/dev/null || log_warn "No PVCs found."

    log_success "PVCs removed."
}

# Remove namespace
remove_namespace() {
    if [ "$REMOVE_NAMESPACE" != true ]; then
        return
    fi

    log_info "Removing namespace $NAMESPACE..."

    if [ "$DRY_RUN" = true ]; then
        log_info "[DRY RUN] Would delete namespace $NAMESPACE"
        return
    fi

    if ! kubectl get namespace "$NAMESPACE" &>/dev/null; then
        log_warn "Namespace $NAMESPACE does not exist."
        return
    fi

    kubectl delete namespace "$NAMESPACE" --timeout=5m || {
        log_warn "Namespace deletion timed out. There may be finalizers blocking deletion."
        log_info "You can force delete with: kubectl delete namespace $NAMESPACE --force --grace-period=0"
    }

    log_success "Namespace removed."
}

# Uninstall CloudNativePG
uninstall_cloudnativepg() {
    if [ "$REMOVE_DEPS" != true ]; then
        return
    fi

    log_info "Uninstalling CloudNativePG..."

    if ! helm list -n cnpg-system | grep -q cnpg; then
        log_warn "CloudNativePG not found."
        return
    fi

    if [ "$DRY_RUN" = true ]; then
        log_info "[DRY RUN] Would uninstall CloudNativePG"
        return
    fi

    confirm "This will remove CloudNativePG. Any PostgreSQL clusters managed by it will become orphaned!"

    helm uninstall cnpg -n cnpg-system || log_warn "Failed to uninstall CloudNativePG."
    kubectl delete namespace cnpg-system --timeout=2m 2>/dev/null || log_warn "Failed to delete cnpg-system namespace."

    log_success "CloudNativePG uninstalled."
}

# Uninstall cert-manager
uninstall_cert_manager() {
    if [ "$REMOVE_DEPS" != true ]; then
        return
    fi

    log_info "Uninstalling cert-manager..."

    if ! helm list -n cert-manager | grep -q cert-manager; then
        log_warn "cert-manager not found."
        return
    fi

    if [ "$DRY_RUN" = true ]; then
        log_info "[DRY RUN] Would uninstall cert-manager"
        return
    fi

    confirm "This will remove cert-manager. SSL certificates will no longer be managed!"

    helm uninstall cert-manager -n cert-manager || log_warn "Failed to uninstall cert-manager."
    kubectl delete namespace cert-manager --timeout=2m 2>/dev/null || log_warn "Failed to delete cert-manager namespace."

    log_success "cert-manager uninstalled."
}

# Uninstall ingress-nginx
uninstall_ingress_nginx() {
    if [ "$REMOVE_DEPS" != true ]; then
        return
    fi

    log_info "Uninstalling ingress-nginx..."

    if ! helm list -n ingress-nginx | grep -q ingress-nginx; then
        log_warn "ingress-nginx not found."
        return
    fi

    if [ "$DRY_RUN" = true ]; then
        log_info "[DRY RUN] Would uninstall ingress-nginx"
        return
    fi

    confirm "This will remove ingress-nginx. External access will be disrupted!"

    helm uninstall ingress-nginx -n ingress-nginx || log_warn "Failed to uninstall ingress-nginx."
    kubectl delete namespace ingress-nginx --timeout=2m 2>/dev/null || log_warn "Failed to delete ingress-nginx namespace."

    log_success "ingress-nginx uninstalled."
}

# Print summary
print_summary() {
    echo ""
    echo "=============================================="
    echo "     Orochi Cloud Uninstallation Summary"
    echo "=============================================="
    echo ""

    if [ "$DRY_RUN" = true ]; then
        echo "This was a DRY RUN. No changes were made."
        return
    fi

    echo "The following actions were performed:"
    echo "  - Helm release '$RELEASE_NAME' uninstalled"

    if [ "$REMOVE_PVCS" = true ]; then
        echo "  - Persistent volume claims removed"
    fi

    if [ "$REMOVE_NAMESPACE" = true ]; then
        echo "  - Namespace '$NAMESPACE' removed"
    fi

    if [ "$REMOVE_DEPS" = true ]; then
        echo "  - CloudNativePG operator removed"
        echo "  - cert-manager removed"
        echo "  - ingress-nginx removed"
    fi

    echo ""
    echo "Note: Some resources may still exist if they were created outside of Helm."
    echo "To verify cleanup, run:"
    echo "  kubectl get all -n $NAMESPACE"
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
            --remove-deps)
                REMOVE_DEPS=true
                shift
                ;;
            --remove-namespace)
                REMOVE_NAMESPACE=true
                shift
                ;;
            --remove-pvcs)
                REMOVE_PVCS=true
                shift
                ;;
            --force)
                FORCE=true
                shift
                ;;
            --dry-run)
                DRY_RUN=true
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
    echo "       Orochi Cloud Uninstallation"
    echo "=============================================="
    echo ""

    if [ "$DRY_RUN" != true ]; then
        confirm "This will uninstall Orochi Cloud from namespace '$NAMESPACE'."
    fi

    check_prerequisites

    # Uninstall Orochi Cloud
    uninstall_orochi_cloud

    # Remove PVCs if requested
    remove_pvcs

    # Remove namespace if requested
    remove_namespace

    # Remove dependencies if requested
    uninstall_cloudnativepg
    uninstall_cert_manager
    uninstall_ingress_nginx

    # Print summary
    print_summary
}

# Run main function
main "$@"
