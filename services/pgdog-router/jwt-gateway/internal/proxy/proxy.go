// Package proxy provides a TCP proxy with PostgreSQL protocol handling.
package proxy

import (
	"context"
	"crypto/hmac"
	"crypto/md5"
	cryptorand "crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"github.com/orochi-db/pgdog-jwt-gateway/internal/auth"
	"github.com/orochi-db/pgdog-jwt-gateway/internal/config"
	"github.com/orochi-db/pgdog-jwt-gateway/internal/routing"
	"github.com/orochi-db/pgdog-jwt-gateway/internal/session"
	"github.com/orochi-db/pgdog-jwt-gateway/internal/wakeup"
)

// PostgreSQL protocol constants
const (
	// Protocol version 3.0
	protocolVersion30 = 196608

	// SSL request magic number
	sslRequestCode = 80877103

	// Cancel request magic number
	cancelRequestCode = 80877102

	// Message types
	msgTypeAuthentication  = 'R'
	msgTypeBackendKeyData  = 'K'
	msgTypeParameterStatus = 'S'
	msgTypeReadyForQuery   = 'Z'
	msgTypeError           = 'E'
	msgTypePasswordMessage = 'p'
	msgTypeQuery           = 'Q'
	msgTypeTerminate       = 'X'
	msgTypeCommandComplete = 'C'
	msgTypeNoticeResponse  = 'N'

	// Authentication request types
	authTypeOK                = 0
	authTypeCleartextPassword = 3
	authTypeMD5Password       = 5
	authTypeSASL              = 10
)

var (
	// ErrMaxConnectionsReached is returned when connection limit is hit.
	ErrMaxConnectionsReached = errors.New("maximum connections reached")

	// ErrAuthenticationFailed is returned when JWT validation fails.
	ErrAuthenticationFailed = errors.New("authentication failed")

	// ErrBackendConnectionFailed is returned when unable to connect to backend.
	ErrBackendConnectionFailed = errors.New("backend connection failed")

	// ErrProtocolError is returned for invalid PostgreSQL protocol data.
	ErrProtocolError = errors.New("protocol error")

	// ErrSNIRequired is returned when SNI routing is enabled but no SNI was provided.
	ErrSNIRequired = errors.New("SNI hostname required")

	// ErrClusterNotFound is returned when the cluster cannot be found.
	ErrClusterNotFound = errors.New("cluster not found")

	// ErrTLSConfigError is returned when TLS configuration fails.
	ErrTLSConfigError = errors.New("TLS configuration error")

	// ErrClusterSuspended is returned when the cluster is suspended.
	ErrClusterSuspended = errors.New("cluster is suspended")

	// ErrWakeTimeout is returned when cluster wake times out.
	ErrWakeTimeout = errors.New("cluster wake timeout")

	// ErrMaxQueuedConnections is returned when too many connections are waiting for wake.
	ErrMaxQueuedConnections = errors.New("too many connections waiting for cluster wake")
)

// Proxy handles PostgreSQL connections with JWT authentication.
type Proxy struct {
	config       *config.Config
	jwtValidator *auth.Validator
	injector     *session.Injector
	logger       *slog.Logger

	// TLS configuration for secure connections.
	tlsConfig *tls.Config

	// SNI router for multi-tenant routing.
	sniRouter *routing.SNIRouter
	registry  routing.ClusterRegistry

	// Wake handler for scale-to-zero wake-on-connect.
	wakeHandler *wakeup.Handler

	listener    net.Listener
	connections sync.Map // map[uint64]*Connection
	connCounter atomic.Uint64
	activeConns atomic.Int64

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

// Connection represents a proxied connection.
type Connection struct {
	ID          uint64
	ClientConn  net.Conn
	BackendConn net.Conn
	Claims      *auth.OrochiClaims
	Database    string
	Username    string
	StartTime   time.Time

	// SNI routing information.
	SNIHostname    string
	RouteInfo      *routing.RouteInfo
	BackendAddress string
}

// NewProxy creates a new proxy instance.
func NewProxy(cfg *config.Config, jwtValidator *auth.Validator, logger *slog.Logger) *Proxy {
	ctx, cancel := context.WithCancel(context.Background())

	return &Proxy{
		config:       cfg,
		jwtValidator: jwtValidator,
		injector:     session.NewInjector(cfg.Session.InjectionTimeout),
		logger:       logger,
		ctx:          ctx,
		cancel:       cancel,
	}
}

// WithSNIRouter sets the SNI router for multi-tenant routing.
func (p *Proxy) WithSNIRouter(router *routing.SNIRouter, registry routing.ClusterRegistry) *Proxy {
	p.sniRouter = router
	p.registry = registry
	return p
}

// WithWakeHandler sets the wake handler for scale-to-zero wake-on-connect.
func (p *Proxy) WithWakeHandler(handler *wakeup.Handler) *Proxy {
	p.wakeHandler = handler
	return p
}

// SetupTLS configures TLS for the proxy.
func (p *Proxy) SetupTLS() error {
	if !p.config.TLS.Enabled {
		return nil
	}

	// Load server certificate and key.
	cert, err := tls.LoadX509KeyPair(p.config.TLS.CertFile, p.config.TLS.KeyFile)
	if err != nil {
		return fmt.Errorf("%w: failed to load certificate: %v", ErrTLSConfigError, err)
	}

	tlsConfig := &tls.Config{
		Certificates: []tls.Certificate{cert},
	}

	// Set minimum TLS version.
	switch p.config.TLS.MinVersion {
	case "1.3":
		tlsConfig.MinVersion = tls.VersionTLS13
	case "1.2":
		tlsConfig.MinVersion = tls.VersionTLS12
	default:
		tlsConfig.MinVersion = tls.VersionTLS12
	}

	// Load CA certificate for client verification if required.
	if p.config.TLS.RequireClientCert {
		if p.config.TLS.CAFile != "" {
			caCert, err := os.ReadFile(p.config.TLS.CAFile)
			if err != nil {
				return fmt.Errorf("%w: failed to load CA certificate: %v", ErrTLSConfigError, err)
			}

			caCertPool := x509.NewCertPool()
			if !caCertPool.AppendCertsFromPEM(caCert) {
				return fmt.Errorf("%w: failed to parse CA certificate", ErrTLSConfigError)
			}

			tlsConfig.ClientCAs = caCertPool
			tlsConfig.ClientAuth = tls.RequireAndVerifyClientCert
		} else {
			tlsConfig.ClientAuth = tls.RequireAnyClientCert
		}
	}

	// If SNI routing is enabled, we need to use GetConfigForClient
	// to extract SNI before completing the handshake.
	if p.config.SNI.Enabled {
		tlsConfig.GetConfigForClient = p.getConfigForClient
	}

	p.tlsConfig = tlsConfig
	return nil
}

// getConfigForClient is called during TLS handshake to extract SNI.
func (p *Proxy) getConfigForClient(hello *tls.ClientHelloInfo) (*tls.Config, error) {
	// Log the SNI hostname for debugging.
	p.logger.Debug("TLS ClientHello received", "sni", hello.ServerName)

	// Store the SNI in the connection context if needed.
	// The actual routing decision happens after the handshake in handleConnection.

	// Return nil to use the default config.
	return nil, nil
}

// Start starts the proxy server.
func (p *Proxy) Start() error {
	// Set up TLS if configured.
	if err := p.SetupTLS(); err != nil {
		return err
	}

	// Create listener.
	var listener net.Listener
	var err error

	if p.tlsConfig != nil {
		listener, err = tls.Listen("tcp", p.config.Server.ListenAddr, p.tlsConfig)
		if err != nil {
			return fmt.Errorf("failed to listen on %s with TLS: %w", p.config.Server.ListenAddr, err)
		}
		p.logger.Info("JWT gateway started with TLS",
			"address", p.config.Server.ListenAddr,
			"sni_routing", p.config.SNI.Enabled)
	} else {
		listener, err = net.Listen("tcp", p.config.Server.ListenAddr)
		if err != nil {
			return fmt.Errorf("failed to listen on %s: %w", p.config.Server.ListenAddr, err)
		}
		p.logger.Info("JWT gateway started",
			"address", p.config.Server.ListenAddr,
			"backend", p.config.BackendAddress())
	}

	p.listener = listener

	p.wg.Add(1)
	go p.acceptLoop()

	return nil
}

// Stop gracefully stops the proxy server.
func (p *Proxy) Stop() error {
	p.logger.Info("Shutting down JWT gateway")
	p.cancel()

	// Close listener to stop accepting new connections
	if p.listener != nil {
		p.listener.Close()
	}

	// Wait for connections to drain with timeout
	done := make(chan struct{})
	go func() {
		p.wg.Wait()
		close(done)
	}()

	select {
	case <-done:
		p.logger.Info("All connections closed")
	case <-time.After(p.config.Server.ShutdownTimeout):
		p.logger.Warn("Shutdown timeout, forcing close")
		// Force close all connections
		p.connections.Range(func(key, value any) bool {
			if conn, ok := value.(*Connection); ok {
				conn.ClientConn.Close()
				if conn.BackendConn != nil {
					conn.BackendConn.Close()
				}
			}
			return true
		})
	}

	// Close SNI router and registry.
	if p.sniRouter != nil {
		p.sniRouter.Close()
	}
	if p.registry != nil {
		p.registry.Close()
	}

	return nil
}

// acceptLoop accepts new connections.
func (p *Proxy) acceptLoop() {
	defer p.wg.Done()

	for {
		conn, err := p.listener.Accept()
		if err != nil {
			select {
			case <-p.ctx.Done():
				return
			default:
				p.logger.Error("Accept error", "error", err)
				continue
			}
		}

		// Check connection limit
		if p.config.Server.MaxConnections > 0 &&
			int(p.activeConns.Load()) >= p.config.Server.MaxConnections {
			p.logger.Warn("Max connections reached, rejecting", "client", conn.RemoteAddr())
			p.sendErrorAndClose(conn, "too many connections")
			continue
		}

		p.wg.Add(1)
		go p.handleConnection(conn)
	}
}

// handleConnection handles a single client connection.
func (p *Proxy) handleConnection(clientConn net.Conn) {
	defer p.wg.Done()

	connID := p.connCounter.Add(1)
	p.activeConns.Add(1)
	defer p.activeConns.Add(-1)

	connection := &Connection{
		ID:         connID,
		ClientConn: clientConn,
		StartTime:  time.Now(),
	}
	p.connections.Store(connID, connection)
	defer p.connections.Delete(connID)

	defer func() {
		clientConn.Close()
		if connection.BackendConn != nil {
			connection.BackendConn.Close()
		}
	}()

	logger := p.logger.With("conn_id", connID, "client", clientConn.RemoteAddr())
	logger.Debug("New connection")

	// Extract SNI hostname from TLS connection if applicable.
	if err := p.extractSNIAndRoute(connection, logger); err != nil {
		logger.Error("SNI routing failed", "error", err)
		p.sendErrorAndClose(clientConn, err.Error())
		return
	}

	// Set connection timeout for startup phase
	clientConn.SetDeadline(time.Now().Add(p.config.Server.ConnectionTimeout))

	// Handle PostgreSQL startup
	claims, err := p.handleStartup(connection, logger)
	if err != nil {
		logger.Error("Startup failed", "error", err)
		p.sendErrorAndClose(clientConn, err.Error())
		return
	}

	connection.Claims = claims
	logger = logger.With("user_id", claims.UserID, "tenant_id", claims.TenantID)
	if connection.RouteInfo != nil {
		logger = logger.With("cluster_id", connection.RouteInfo.ClusterID, "branch", connection.RouteInfo.Branch)
	}
	logger.Info("Connection authenticated")

	// Clear deadline for normal operation
	clientConn.SetDeadline(time.Time{})

	// Start bidirectional forwarding
	p.proxyConnection(connection, logger)
}

// extractSNIAndRoute extracts SNI from TLS connection and resolves the backend route.
func (p *Proxy) extractSNIAndRoute(conn *Connection, logger *slog.Logger) error {
	// If SNI routing is not enabled, use the default backend.
	if !p.config.SNI.Enabled || p.sniRouter == nil {
		conn.BackendAddress = p.config.BackendAddress()
		return nil
	}

	// Extract SNI from TLS connection.
	tlsConn, ok := conn.ClientConn.(*tls.Conn)
	if !ok {
		// Not a TLS connection, this shouldn't happen if SNI is enabled.
		return ErrSNIRequired
	}

	// The TLS handshake should already be complete at this point,
	// so we can access the connection state.
	state := tlsConn.ConnectionState()
	sni := state.ServerName

	if sni == "" {
		// No SNI provided.
		if p.sniRouter.AllowsUnknownClusters() && p.sniRouter.GetDefaultBackend() != "" {
			conn.BackendAddress = p.sniRouter.GetDefaultBackend()
			logger.Debug("No SNI provided, using default backend", "backend", conn.BackendAddress)
			return nil
		}
		return ErrSNIRequired
	}

	conn.SNIHostname = sni
	logger.Debug("SNI hostname extracted", "sni", sni)

	// Route based on SNI.
	routeInfo, err := p.sniRouter.Route(sni)
	if err != nil {
		// Check if we should fall back to default backend.
		if errors.Is(err, routing.ErrClusterNotFound) || errors.Is(err, routing.ErrBranchNotFound) {
			if p.sniRouter.AllowsUnknownClusters() && p.sniRouter.GetDefaultBackend() != "" {
				conn.BackendAddress = p.sniRouter.GetDefaultBackend()
				logger.Warn("Cluster not found, using default backend",
					"sni", sni,
					"backend", conn.BackendAddress)
				return nil
			}
		}
		return fmt.Errorf("%w: %v", ErrClusterNotFound, err)
	}

	conn.RouteInfo = routeInfo
	conn.BackendAddress = routeInfo.BackendAddress
	logger.Debug("Route resolved",
		"cluster_id", routeInfo.ClusterID,
		"branch", routeInfo.Branch,
		"backend", routeInfo.BackendAddress)

	// Check if cluster is suspended and handle wake-on-connect
	if p.wakeHandler != nil && routeInfo.ClusterID != "" {
		if err := p.handleWakeOnConnect(conn, routeInfo.ClusterID, logger); err != nil {
			return err
		}
	}

	return nil
}

// handleWakeOnConnect checks if a cluster is suspended and triggers wake if needed.
func (p *Proxy) handleWakeOnConnect(conn *Connection, clusterID string, logger *slog.Logger) error {
	ctx, cancel := context.WithTimeout(p.ctx, p.config.ScaleToZero.WakeTimeout)
	defer cancel()

	// Check cluster state
	state, err := p.wakeHandler.CheckClusterState(ctx, clusterID)
	if err != nil {
		if errors.Is(err, wakeup.ErrClusterNotFound) {
			// Cluster not found in control plane, continue with connection attempt
			logger.Debug("Cluster not found in control plane, continuing",
				"cluster_id", clusterID)
			return nil
		}
		logger.Warn("Failed to check cluster state",
			"cluster_id", clusterID,
			"error", err)
		// Continue with connection attempt - control plane might be unavailable
		return nil
	}

	// If cluster is active, proceed normally
	if state.IsReady {
		logger.Debug("Cluster is active",
			"cluster_id", clusterID,
			"status", state.Status)
		return nil
	}

	// Handle different cluster states
	switch state.Status {
	case wakeup.StateSuspended:
		logger.Info("Cluster is suspended, triggering wake-on-connect",
			"cluster_id", clusterID)

		// Handle the suspended cluster - this will queue the connection and wait for wake
		if err := p.wakeHandler.HandleSuspendedCluster(ctx, clusterID, conn.ClientConn); err != nil {
			if errors.Is(err, wakeup.ErrWakeTimeout) {
				return ErrWakeTimeout
			}
			if errors.Is(err, wakeup.ErrMaxQueuedConnections) {
				return ErrMaxQueuedConnections
			}
			return fmt.Errorf("wake-on-connect failed: %w", err)
		}

		logger.Info("Cluster woke successfully, proceeding with connection",
			"cluster_id", clusterID)
		return nil

	case wakeup.StateWaking:
		logger.Info("Cluster is already waking, waiting for ready",
			"cluster_id", clusterID)

		// Handle the waking cluster - queue the connection
		if err := p.wakeHandler.HandleSuspendedCluster(ctx, clusterID, conn.ClientConn); err != nil {
			if errors.Is(err, wakeup.ErrWakeTimeout) {
				return ErrWakeTimeout
			}
			if errors.Is(err, wakeup.ErrMaxQueuedConnections) {
				return ErrMaxQueuedConnections
			}
			return fmt.Errorf("wait for wake failed: %w", err)
		}

		logger.Info("Cluster is now ready, proceeding with connection",
			"cluster_id", clusterID)
		return nil

	case wakeup.StateSuspending:
		// Cluster is going to sleep - reject connection
		return fmt.Errorf("%w: cluster is suspending", ErrClusterSuspended)

	default:
		// Unknown state, proceed with connection attempt
		logger.Warn("Unknown cluster state, proceeding with connection attempt",
			"cluster_id", clusterID,
			"status", state.Status)
		return nil
	}
}

// handleStartup handles the PostgreSQL startup sequence with JWT authentication.
func (p *Proxy) handleStartup(conn *Connection, logger *slog.Logger) (*auth.OrochiClaims, error) {
	// Read startup message
	startup, err := p.readStartupMessage(conn.ClientConn)
	if err != nil {
		return nil, fmt.Errorf("failed to read startup: %w", err)
	}

	// Handle SSL request
	if startup.isSSLRequest {
		// Send 'N' to decline SSL (PgDog handles SSL)
		conn.ClientConn.Write([]byte{'N'})

		// Read actual startup message
		startup, err = p.readStartupMessage(conn.ClientConn)
		if err != nil {
			return nil, fmt.Errorf("failed to read startup after SSL: %w", err)
		}
	}

	// Handle cancel request (just forward to backend)
	if startup.isCancelRequest {
		return nil, p.handleCancelRequest(conn, startup, logger)
	}

	conn.Username = startup.params["user"]
	conn.Database = startup.params["database"]

	// Request cleartext password (which will be the JWT)
	if err := p.requestCleartextPassword(conn.ClientConn); err != nil {
		return nil, fmt.Errorf("failed to request password: %w", err)
	}

	// Read password (JWT token)
	token, err := p.readPasswordMessage(conn.ClientConn)
	if err != nil {
		return nil, fmt.Errorf("failed to read password: %w", err)
	}

	// Validate JWT
	claims, err := p.jwtValidator.Validate(token)
	if err != nil {
		logger.Warn("JWT validation failed", "error", err)
		return nil, ErrAuthenticationFailed
	}

	// Connect to backend (PgDog)
	backendConn, err := p.connectToBackend(conn, startup, logger)
	if err != nil {
		return nil, err
	}
	conn.BackendConn = backendConn

	// Inject session variables
	if p.config.Session.InjectClaims {
		if err := p.injector.InjectClaims(backendConn, claims); err != nil {
			logger.Warn("Failed to inject claims", "error", err)
			// Continue anyway - this is not fatal
		}
	}

	// Send authentication OK to client
	if err := p.sendAuthOK(conn.ClientConn); err != nil {
		return nil, fmt.Errorf("failed to send auth OK: %w", err)
	}

	// Forward backend ready messages to client
	if err := p.forwardBackendReady(conn, logger); err != nil {
		return nil, fmt.Errorf("failed to forward backend ready: %w", err)
	}

	return claims, nil
}

// startupMessage holds parsed startup message data.
type startupMessage struct {
	length          int
	protocolVersion int
	params          map[string]string
	isSSLRequest    bool
	isCancelRequest bool
	cancelPID       int32
	cancelKey       int32
	rawData         []byte
}

// readStartupMessage reads and parses a PostgreSQL startup message.
func (p *Proxy) readStartupMessage(conn net.Conn) (*startupMessage, error) {
	// Read length (4 bytes)
	lenBuf := make([]byte, 4)
	if _, err := io.ReadFull(conn, lenBuf); err != nil {
		return nil, err
	}
	length := int(binary.BigEndian.Uint32(lenBuf))

	if length < 8 || length > 10000 {
		return nil, fmt.Errorf("%w: invalid startup message length: %d", ErrProtocolError, length)
	}

	// Read rest of message
	data := make([]byte, length-4)
	if _, err := io.ReadFull(conn, data); err != nil {
		return nil, err
	}

	// Parse protocol version / request type
	version := int(binary.BigEndian.Uint32(data[0:4]))

	msg := &startupMessage{
		length:          length,
		protocolVersion: version,
		params:          make(map[string]string),
		rawData:         append(lenBuf, data...),
	}

	switch version {
	case sslRequestCode:
		msg.isSSLRequest = true
		return msg, nil

	case cancelRequestCode:
		msg.isCancelRequest = true
		msg.cancelPID = int32(binary.BigEndian.Uint32(data[4:8]))
		msg.cancelKey = int32(binary.BigEndian.Uint32(data[8:12]))
		return msg, nil

	case protocolVersion30:
		// Parse parameters
		paramData := data[4:]
		for len(paramData) > 1 {
			// Find key
			keyEnd := 0
			for keyEnd < len(paramData) && paramData[keyEnd] != 0 {
				keyEnd++
			}
			if keyEnd >= len(paramData) {
				break
			}
			key := string(paramData[:keyEnd])
			paramData = paramData[keyEnd+1:]

			if key == "" {
				break
			}

			// Find value
			valueEnd := 0
			for valueEnd < len(paramData) && paramData[valueEnd] != 0 {
				valueEnd++
			}
			if valueEnd > len(paramData) {
				break
			}
			value := string(paramData[:valueEnd])
			paramData = paramData[valueEnd+1:]

			msg.params[key] = value
		}
		return msg, nil

	default:
		return nil, fmt.Errorf("%w: unsupported protocol version: %d", ErrProtocolError, version)
	}
}

// requestCleartextPassword sends an authentication cleartext password request.
func (p *Proxy) requestCleartextPassword(conn net.Conn) error {
	// AuthenticationCleartextPassword message:
	// - 1 byte: 'R'
	// - 4 bytes: length (8)
	// - 4 bytes: auth type (3)
	msg := []byte{'R', 0, 0, 0, 8, 0, 0, 0, 3}
	_, err := conn.Write(msg)
	return err
}

// readPasswordMessage reads a password message from the client.
func (p *Proxy) readPasswordMessage(conn net.Conn) (string, error) {
	// Read message type
	msgType := make([]byte, 1)
	if _, err := io.ReadFull(conn, msgType); err != nil {
		return "", err
	}

	if msgType[0] != msgTypePasswordMessage {
		return "", fmt.Errorf("%w: expected password message, got %c", ErrProtocolError, msgType[0])
	}

	// Read length
	lenBuf := make([]byte, 4)
	if _, err := io.ReadFull(conn, lenBuf); err != nil {
		return "", err
	}
	length := int(binary.BigEndian.Uint32(lenBuf)) - 4

	if length < 1 || length > 65535 {
		return "", fmt.Errorf("%w: invalid password length: %d", ErrProtocolError, length)
	}

	// Read password
	password := make([]byte, length)
	if _, err := io.ReadFull(conn, password); err != nil {
		return "", err
	}

	// Remove null terminator
	if len(password) > 0 && password[len(password)-1] == 0 {
		password = password[:len(password)-1]
	}

	return string(password), nil
}

// connectToBackend establishes a connection to PgDog.
func (p *Proxy) connectToBackend(conn *Connection, startup *startupMessage, logger *slog.Logger) (net.Conn, error) {
	// Use the pre-resolved backend address from SNI routing, or fall back to config.
	backendAddr := conn.BackendAddress
	if backendAddr == "" {
		backendAddr = p.config.BackendAddress()
	}

	logger.Debug("Connecting to backend", "address", backendAddr)

	dialer := net.Dialer{Timeout: p.config.Backend.ConnectTimeout}
	backendConn, err := dialer.DialContext(p.ctx, "tcp", backendAddr)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrBackendConnectionFailed, err)
	}

	// Send startup message to backend
	// Use configured credentials or original startup params
	username := p.config.Backend.Username
	if username == "" {
		username = startup.params["user"]
	}

	database := startup.params["database"]

	startupMsg := p.buildStartupMessage(username, database, startup.params)
	if _, err := backendConn.Write(startupMsg); err != nil {
		backendConn.Close()
		return nil, fmt.Errorf("failed to send startup to backend: %w", err)
	}

	// Handle backend authentication
	if err := p.handleBackendAuth(backendConn, logger); err != nil {
		backendConn.Close()
		return nil, fmt.Errorf("backend authentication failed: %w", err)
	}

	return backendConn, nil
}

// buildStartupMessage builds a PostgreSQL startup message.
func (p *Proxy) buildStartupMessage(username, database string, extraParams map[string]string) []byte {
	// Build parameter list
	params := make([]byte, 0, 256)

	// Add user
	params = append(params, []byte("user")...)
	params = append(params, 0)
	params = append(params, []byte(username)...)
	params = append(params, 0)

	// Add database
	if database != "" {
		params = append(params, []byte("database")...)
		params = append(params, 0)
		params = append(params, []byte(database)...)
		params = append(params, 0)
	}

	// Add application_name if present
	if appName, ok := extraParams["application_name"]; ok {
		params = append(params, []byte("application_name")...)
		params = append(params, 0)
		params = append(params, []byte(appName)...)
		params = append(params, 0)
	}

	// Add client_encoding if present
	if encoding, ok := extraParams["client_encoding"]; ok {
		params = append(params, []byte("client_encoding")...)
		params = append(params, 0)
		params = append(params, []byte(encoding)...)
		params = append(params, 0)
	}

	// Terminator
	params = append(params, 0)

	// Build message: length (4) + protocol version (4) + params
	length := 4 + 4 + len(params)
	msg := make([]byte, length)
	binary.BigEndian.PutUint32(msg[0:4], uint32(length))
	binary.BigEndian.PutUint32(msg[4:8], uint32(protocolVersion30))
	copy(msg[8:], params)

	return msg
}

// handleBackendAuth handles authentication with the backend.
func (p *Proxy) handleBackendAuth(conn net.Conn, logger *slog.Logger) error {
	for {
		// Read message type
		msgType := make([]byte, 1)
		if _, err := io.ReadFull(conn, msgType); err != nil {
			return err
		}

		// Read length
		lenBuf := make([]byte, 4)
		if _, err := io.ReadFull(conn, lenBuf); err != nil {
			return err
		}
		length := int(binary.BigEndian.Uint32(lenBuf)) - 4

		// Read body
		body := make([]byte, length)
		if length > 0 {
			if _, err := io.ReadFull(conn, body); err != nil {
				return err
			}
		}

		switch msgType[0] {
		case msgTypeAuthentication:
			authType := int(binary.BigEndian.Uint32(body[0:4]))
			switch authType {
			case authTypeOK:
				// Authentication successful, continue reading until ReadyForQuery
				continue

			case authTypeCleartextPassword:
				// Send password
				password := p.config.Backend.Password
				if err := p.sendPassword(conn, password); err != nil {
					return err
				}

			case authTypeMD5Password:
				// MD5 auth: send md5(md5(password + user) + salt)
				if len(body) < 8 {
					return errors.New("MD5 auth: invalid salt length")
				}
				salt := body[4:8]
				password := p.config.Backend.Password
				user := p.config.Backend.Username

				// Step 1: md5(password + user)
				inner := md5.Sum([]byte(password + user))
				innerHex := hex.EncodeToString(inner[:])

				// Step 2: md5(inner_hex + salt)
				outer := md5.Sum(append([]byte(innerHex), salt...))
				md5Password := "md5" + hex.EncodeToString(outer[:])

				if err := p.sendPassword(conn, md5Password); err != nil {
					return err
				}

			case authTypeSASL:
				// SASL/SCRAM-SHA-256 authentication
				if err := p.handleSASLAuth(conn, body); err != nil {
					return fmt.Errorf("SASL auth failed: %w", err)
				}

			default:
				return fmt.Errorf("unsupported auth type: %d", authType)
			}

		case msgTypeError:
			errMsg := parseErrorMessage(body)
			return fmt.Errorf("backend error: %s", errMsg)

		case msgTypeReadyForQuery:
			// Backend is ready
			return nil

		case msgTypeBackendKeyData, msgTypeParameterStatus, msgTypeNoticeResponse:
			// Ignore these during auth phase - we'll forward them later
			continue

		default:
			logger.Debug("Unexpected message during auth", "type", string(msgType))
		}
	}
}

// sendPassword sends a password message.
func (p *Proxy) sendPassword(conn net.Conn, password string) error {
	// Password message: 'p' + length + password + null
	pwdBytes := []byte(password)
	length := 4 + len(pwdBytes) + 1
	msg := make([]byte, 1+length)
	msg[0] = msgTypePasswordMessage
	binary.BigEndian.PutUint32(msg[1:5], uint32(length))
	copy(msg[5:], pwdBytes)
	msg[len(msg)-1] = 0

	_, err := conn.Write(msg)
	return err
}

// handleSASLAuth implements SCRAM-SHA-256 SASL authentication with the backend.
func (p *Proxy) handleSASLAuth(conn net.Conn, initialBody []byte) error {
	password := p.config.Backend.Password

	// Parse supported mechanisms from the initial SASL message
	mechanisms := parseSASLMechanisms(initialBody[4:])
	if !containsMechanism(mechanisms, "SCRAM-SHA-256") {
		return errors.New("server does not support SCRAM-SHA-256")
	}

	// Generate client nonce
	nonce := make([]byte, 18)
	if _, err := cryptorand.Read(nonce); err != nil {
		return fmt.Errorf("failed to generate nonce: %w", err)
	}
	clientNonce := hex.EncodeToString(nonce)

	// Send SASLInitialResponse
	clientFirstBare := fmt.Sprintf("n=,r=%s", clientNonce)
	clientFirstMsg := fmt.Sprintf("n,,%s", clientFirstBare)

	if err := p.sendSASLInitialResponse(conn, "SCRAM-SHA-256", []byte(clientFirstMsg)); err != nil {
		return err
	}

	// Read SASLContinue (AuthenticationSASLContinue = 11)
	serverFirstMsg, err := p.readSASLResponse(conn, 11)
	if err != nil {
		return fmt.Errorf("reading SASL continue: %w", err)
	}

	// Parse server-first-message: r=<nonce>,s=<salt>,i=<iterations>
	serverParams := parseSASLServerFirst(string(serverFirstMsg))
	if serverParams.nonce == "" || serverParams.salt == nil || serverParams.iterations == 0 {
		return errors.New("invalid server-first-message")
	}

	// Verify server nonce starts with our client nonce
	if len(serverParams.nonce) < len(clientNonce) || serverParams.nonce[:len(clientNonce)] != clientNonce {
		return errors.New("server nonce does not start with client nonce")
	}

	// Compute SCRAM-SHA-256 proof
	saltedPassword := derivePBKDF2([]byte(password), serverParams.salt, serverParams.iterations, 32)

	clientKey := computeHMAC(saltedPassword, []byte("Client Key"))
	storedKey := sha256.Sum256(clientKey)

	channelBinding := "biws" // base64("n,,")
	clientFinalNoProof := fmt.Sprintf("c=%s,r=%s", channelBinding, serverParams.nonce)

	authMessage := fmt.Sprintf("%s,%s,%s", clientFirstBare, string(serverFirstMsg), clientFinalNoProof)

	clientSignature := computeHMAC(storedKey[:], []byte(authMessage))

	clientProof := make([]byte, len(clientKey))
	for i := range clientKey {
		clientProof[i] = clientKey[i] ^ clientSignature[i]
	}

	// Send client-final-message
	clientFinalMsg := fmt.Sprintf("%s,p=%s", clientFinalNoProof, base64Encode(clientProof))

	if err := p.sendSASLResponse(conn, []byte(clientFinalMsg)); err != nil {
		return err
	}

	// Read SASLFinal (AuthenticationSASLFinal = 12)
	_, err = p.readSASLResponse(conn, 12)
	if err != nil {
		return fmt.Errorf("reading SASL final: %w", err)
	}

	return nil
}

// sendSASLInitialResponse sends a SASLInitialResponse message.
func (p *Proxy) sendSASLInitialResponse(conn net.Conn, mechanism string, data []byte) error {
	mechBytes := []byte(mechanism)
	// 'p' + length + mechanism + null + data_length + data
	totalLen := 4 + len(mechBytes) + 1 + 4 + len(data)
	msg := make([]byte, 1+totalLen)
	msg[0] = msgTypePasswordMessage
	binary.BigEndian.PutUint32(msg[1:5], uint32(totalLen))
	copy(msg[5:], mechBytes)
	msg[5+len(mechBytes)] = 0
	binary.BigEndian.PutUint32(msg[5+len(mechBytes)+1:], uint32(len(data)))
	copy(msg[5+len(mechBytes)+1+4:], data)
	_, err := conn.Write(msg)
	return err
}

// sendSASLResponse sends a SASLResponse message.
func (p *Proxy) sendSASLResponse(conn net.Conn, data []byte) error {
	totalLen := 4 + len(data)
	msg := make([]byte, 1+totalLen)
	msg[0] = msgTypePasswordMessage
	binary.BigEndian.PutUint32(msg[1:5], uint32(totalLen))
	copy(msg[5:], data)
	_, err := conn.Write(msg)
	return err
}

// readSASLResponse reads a SASL response message from the backend.
func (p *Proxy) readSASLResponse(conn net.Conn, expectedAuthType int) ([]byte, error) {
	msgType := make([]byte, 1)
	if _, err := io.ReadFull(conn, msgType); err != nil {
		return nil, err
	}

	lenBuf := make([]byte, 4)
	if _, err := io.ReadFull(conn, lenBuf); err != nil {
		return nil, err
	}
	length := int(binary.BigEndian.Uint32(lenBuf)) - 4

	body := make([]byte, length)
	if length > 0 {
		if _, err := io.ReadFull(conn, body); err != nil {
			return nil, err
		}
	}

	if msgType[0] == msgTypeError {
		return nil, fmt.Errorf("backend error: %s", parseErrorMessage(body))
	}

	if msgType[0] != msgTypeAuthentication {
		return nil, fmt.Errorf("expected authentication message, got %c", msgType[0])
	}

	authType := int(binary.BigEndian.Uint32(body[0:4]))
	if authType != expectedAuthType {
		if authType == authTypeOK {
			return nil, nil // auth completed early
		}
		return nil, fmt.Errorf("expected auth type %d, got %d", expectedAuthType, authType)
	}

	return body[4:], nil
}

type saslServerParams struct {
	nonce      string
	salt       []byte
	iterations int
}

func parseSASLMechanisms(data []byte) []string {
	var mechanisms []string
	for len(data) > 0 {
		idx := 0
		for idx < len(data) && data[idx] != 0 {
			idx++
		}
		if idx > 0 {
			mechanisms = append(mechanisms, string(data[:idx]))
		}
		if idx < len(data) {
			data = data[idx+1:]
		} else {
			break
		}
	}
	return mechanisms
}

func containsMechanism(mechanisms []string, target string) bool {
	for _, m := range mechanisms {
		if m == target {
			return true
		}
	}
	return false
}

func parseSASLServerFirst(msg string) saslServerParams {
	var params saslServerParams
	for _, part := range splitSASL(msg) {
		if len(part) < 2 {
			continue
		}
		switch part[0] {
		case 'r':
			params.nonce = part[2:]
		case 's':
			params.salt, _ = base64Decode(part[2:])
		case 'i':
			fmt.Sscanf(part[2:], "%d", &params.iterations)
		}
	}
	return params
}

func splitSASL(s string) []string {
	var parts []string
	start := 0
	for i := 0; i < len(s); i++ {
		if s[i] == ',' {
			parts = append(parts, s[start:i])
			start = i + 1
		}
	}
	if start < len(s) {
		parts = append(parts, s[start:])
	}
	return parts
}

// derivePBKDF2 implements PBKDF2 with HMAC-SHA256 (RFC 2898).
func derivePBKDF2(password, salt []byte, iterations, keyLen int) []byte {
	dk := make([]byte, 0, keyLen)
	block := 1
	for len(dk) < keyLen {
		// U1 = PRF(password, salt || INT_32_BE(block))
		u := make([]byte, len(salt)+4)
		copy(u, salt)
		u[len(salt)] = byte(block >> 24)
		u[len(salt)+1] = byte(block >> 16)
		u[len(salt)+2] = byte(block >> 8)
		u[len(salt)+3] = byte(block)

		prev := computeHMAC(password, u)
		result := make([]byte, len(prev))
		copy(result, prev)

		for i := 1; i < iterations; i++ {
			prev = computeHMAC(password, prev)
			for j := range result {
				result[j] ^= prev[j]
			}
		}

		dk = append(dk, result...)
		block++
	}
	return dk[:keyLen]
}

func computeHMAC(key, data []byte) []byte {
	mac := hmac.New(sha256.New, key)
	mac.Write(data)
	return mac.Sum(nil)
}

func base64Encode(data []byte) string {
	const encoder = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
	result := make([]byte, 0, (len(data)+2)/3*4)
	for i := 0; i < len(data); i += 3 {
		var n uint32
		remaining := len(data) - i
		switch remaining {
		default:
			n = uint32(data[i])<<16 | uint32(data[i+1])<<8 | uint32(data[i+2])
			result = append(result, encoder[n>>18&0x3F], encoder[n>>12&0x3F], encoder[n>>6&0x3F], encoder[n&0x3F])
		case 2:
			n = uint32(data[i])<<16 | uint32(data[i+1])<<8
			result = append(result, encoder[n>>18&0x3F], encoder[n>>12&0x3F], encoder[n>>6&0x3F], '=')
		case 1:
			n = uint32(data[i]) << 16
			result = append(result, encoder[n>>18&0x3F], encoder[n>>12&0x3F], '=', '=')
		}
	}
	return string(result)
}

func base64Decode(s string) ([]byte, error) {
	const decoder = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
	lookup := [256]byte{}
	for i := range lookup {
		lookup[i] = 0xFF
	}
	for i, c := range decoder {
		lookup[c] = byte(i)
	}

	// Remove padding
	for len(s) > 0 && s[len(s)-1] == '=' {
		s = s[:len(s)-1]
	}

	result := make([]byte, 0, len(s)*3/4)
	buf := uint32(0)
	bits := 0
	for _, c := range []byte(s) {
		val := lookup[c]
		if val == 0xFF {
			return nil, fmt.Errorf("invalid base64 character: %c", c)
		}
		buf = buf<<6 | uint32(val)
		bits += 6
		if bits >= 8 {
			bits -= 8
			result = append(result, byte(buf>>bits))
			buf &= (1 << bits) - 1
		}
	}
	return result, nil
}

// sendAuthOK sends authentication OK to the client.
func (p *Proxy) sendAuthOK(conn net.Conn) error {
	// AuthenticationOk: 'R' + length(8) + type(0)
	msg := []byte{'R', 0, 0, 0, 8, 0, 0, 0, 0}
	_, err := conn.Write(msg)
	return err
}

// forwardBackendReady forwards backend ready messages to the client.
func (p *Proxy) forwardBackendReady(conn *Connection, logger *slog.Logger) error {
	// We need to read from backend and forward ParameterStatus, BackendKeyData,
	// and ReadyForQuery to the client.
	// Note: The backend auth phase already consumed these, so we need to
	// re-query the backend to get current state.

	// Send a simple query to sync state
	query := "SELECT 1"
	if err := p.sendSimpleQuery(conn.BackendConn, query); err != nil {
		return err
	}

	// Forward all responses until ReadyForQuery
	for {
		// Read message from backend
		msgType := make([]byte, 1)
		if _, err := io.ReadFull(conn.BackendConn, msgType); err != nil {
			return err
		}

		lenBuf := make([]byte, 4)
		if _, err := io.ReadFull(conn.BackendConn, lenBuf); err != nil {
			return err
		}
		length := int(binary.BigEndian.Uint32(lenBuf)) - 4

		body := make([]byte, length)
		if length > 0 {
			if _, err := io.ReadFull(conn.BackendConn, body); err != nil {
				return err
			}
		}

		// We don't forward the SELECT 1 results to the client
		// Just wait for ReadyForQuery
		if msgType[0] == msgTypeReadyForQuery {
			// Send ReadyForQuery to client
			msg := make([]byte, 1+4+length)
			msg[0] = msgType[0]
			copy(msg[1:5], lenBuf)
			copy(msg[5:], body)
			if _, err := conn.ClientConn.Write(msg); err != nil {
				return err
			}
			return nil
		}
	}
}

// sendSimpleQuery sends a simple query to the connection.
func (p *Proxy) sendSimpleQuery(conn net.Conn, query string) error {
	queryBytes := []byte(query)
	length := 4 + len(queryBytes) + 1
	msg := make([]byte, 1+length)
	msg[0] = msgTypeQuery
	binary.BigEndian.PutUint32(msg[1:5], uint32(length))
	copy(msg[5:], queryBytes)
	msg[len(msg)-1] = 0

	_, err := conn.Write(msg)
	return err
}

// handleCancelRequest forwards a cancel request to the backend.
func (p *Proxy) handleCancelRequest(conn *Connection, startup *startupMessage, logger *slog.Logger) error {
	// Use the pre-resolved backend address from SNI routing, or fall back to config.
	backendAddr := conn.BackendAddress
	if backendAddr == "" {
		backendAddr = p.config.BackendAddress()
	}

	// Connect to backend and forward the cancel request
	backendConn, err := net.DialTimeout("tcp", backendAddr, p.config.Backend.ConnectTimeout)
	if err != nil {
		return err
	}
	defer backendConn.Close()

	// Forward the cancel request
	_, err = backendConn.Write(startup.rawData)
	return err
}

// proxyConnection handles bidirectional data forwarding.
func (p *Proxy) proxyConnection(conn *Connection, logger *slog.Logger) {
	errChan := make(chan error, 2)

	// Client -> Backend
	go func() {
		_, err := io.Copy(conn.BackendConn, conn.ClientConn)
		errChan <- err
	}()

	// Backend -> Client
	go func() {
		_, err := io.Copy(conn.ClientConn, conn.BackendConn)
		errChan <- err
	}()

	// Wait for either direction to close
	select {
	case err := <-errChan:
		if err != nil && !errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) {
			logger.Debug("Connection closed", "error", err)
		}
	case <-p.ctx.Done():
		logger.Debug("Connection closed due to shutdown")
	}

	logger.Debug("Connection ended", "duration", time.Since(conn.StartTime))
}

// sendErrorAndClose sends a PostgreSQL error message and closes the connection.
func (p *Proxy) sendErrorAndClose(conn net.Conn, message string) {
	// Build error response
	// Error message format: 'E' + length + fields
	// Fields: 'S' + severity + 'M' + message + 'C' + code + 0
	var body []byte
	body = append(body, 'S')
	body = append(body, []byte("FATAL")...)
	body = append(body, 0)
	body = append(body, 'V')
	body = append(body, []byte("FATAL")...)
	body = append(body, 0)
	body = append(body, 'C')
	body = append(body, []byte("28000")...) // Invalid authorization specification
	body = append(body, 0)
	body = append(body, 'M')
	body = append(body, []byte(message)...)
	body = append(body, 0)
	body = append(body, 0) // End of fields

	length := 4 + len(body)
	msg := make([]byte, 1+length)
	msg[0] = msgTypeError
	binary.BigEndian.PutUint32(msg[1:5], uint32(length))
	copy(msg[5:], body)

	conn.Write(msg)
	conn.Close()
}

// parseErrorMessage extracts the message from an error response body.
func parseErrorMessage(body []byte) string {
	i := 0
	for i < len(body) {
		if body[i] == 0 {
			break
		}
		fieldType := body[i]
		i++

		end := i
		for end < len(body) && body[end] != 0 {
			end++
		}
		if fieldType == 'M' {
			return string(body[i:end])
		}
		i = end + 1
	}
	return "unknown error"
}

// Stats returns current proxy statistics.
func (p *Proxy) Stats() ProxyStats {
	return ProxyStats{
		ActiveConnections: int(p.activeConns.Load()),
		TotalConnections:  p.connCounter.Load(),
	}
}

// ProxyStats holds proxy statistics.
type ProxyStats struct {
	ActiveConnections int
	TotalConnections  uint64
}
