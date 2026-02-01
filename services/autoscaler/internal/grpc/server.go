// Package grpc provides the gRPC server for the autoscaler.
package grpc

import (
	"context"
	"fmt"
	"net"
	"sync"

	"google.golang.org/grpc"
	"google.golang.org/grpc/health"
	"google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/reflection"

	"github.com/orochi-db/orochi-db/services/autoscaler/internal/k8s"
	"github.com/orochi-db/orochi-db/services/autoscaler/internal/metrics"
	"github.com/orochi-db/orochi-db/services/autoscaler/internal/scaler"
)

// Server represents the gRPC server for the autoscaler service.
type Server struct {
	mu           sync.RWMutex
	grpcServer   *grpc.Server
	handler      *Handler
	healthServer *health.Server
	port         int
	listener     net.Listener
	running      bool
}

// ServerConfig holds configuration for the gRPC server.
type ServerConfig struct {
	Port int
}

// ServerDependencies holds the dependencies for the gRPC server.
type ServerDependencies struct {
	K8sClient        *k8s.Client
	MetricsCollector *metrics.MetricsCollector
	HorizontalScaler *scaler.HorizontalScaler
	VerticalScaler   *scaler.VerticalScaler
	PolicyEngine     *scaler.PolicyEngine
	EventRecorder    *scaler.ScalingEventRecorder
}

// NewServer creates a new gRPC server.
func NewServer(cfg ServerConfig, deps ServerDependencies) *Server {
	// Create gRPC server with options
	opts := []grpc.ServerOption{
		grpc.MaxRecvMsgSize(4 * 1024 * 1024), // 4MB
		grpc.MaxSendMsgSize(4 * 1024 * 1024), // 4MB
	}

	grpcServer := grpc.NewServer(opts...)

	// Create handler
	handler := NewHandler(HandlerDependencies{
		K8sClient:        deps.K8sClient,
		MetricsCollector: deps.MetricsCollector,
		HorizontalScaler: deps.HorizontalScaler,
		VerticalScaler:   deps.VerticalScaler,
		PolicyEngine:     deps.PolicyEngine,
		EventRecorder:    deps.EventRecorder,
	})

	// Register the autoscaler service
	RegisterAutoscalerServiceServer(grpcServer, handler)

	// Register health service
	healthServer := health.NewServer()
	grpc_health_v1.RegisterHealthServer(grpcServer, healthServer)
	healthServer.SetServingStatus("autoscaler.AutoscalerService", grpc_health_v1.HealthCheckResponse_SERVING)

	// Enable reflection for debugging
	reflection.Register(grpcServer)

	return &Server{
		grpcServer:   grpcServer,
		handler:      handler,
		healthServer: healthServer,
		port:         cfg.Port,
	}
}

// Start starts the gRPC server.
func (s *Server) Start() error {
	s.mu.Lock()
	if s.running {
		s.mu.Unlock()
		return fmt.Errorf("server already running")
	}

	listener, err := net.Listen("tcp", fmt.Sprintf(":%d", s.port))
	if err != nil {
		s.mu.Unlock()
		return fmt.Errorf("failed to listen on port %d: %w", s.port, err)
	}

	s.listener = listener
	s.running = true
	s.mu.Unlock()

	fmt.Printf("gRPC server listening on port %d\n", s.port)

	if err := s.grpcServer.Serve(listener); err != nil {
		return fmt.Errorf("failed to serve: %w", err)
	}

	return nil
}

// Stop gracefully stops the gRPC server.
func (s *Server) Stop() {
	s.mu.Lock()
	defer s.mu.Unlock()

	if !s.running {
		return
	}

	s.healthServer.SetServingStatus("autoscaler.AutoscalerService", grpc_health_v1.HealthCheckResponse_NOT_SERVING)
	s.grpcServer.GracefulStop()
	s.running = false

	fmt.Println("gRPC server stopped")
}

// ForceStop immediately stops the gRPC server.
func (s *Server) ForceStop() {
	s.mu.Lock()
	defer s.mu.Unlock()

	if !s.running {
		return
	}

	s.grpcServer.Stop()
	s.running = false

	fmt.Println("gRPC server force stopped")
}

// IsRunning returns whether the server is running.
func (s *Server) IsRunning() bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.running
}

// Port returns the server port.
func (s *Server) Port() int {
	return s.port
}

// UnaryServerInterceptor provides logging and metrics for unary calls.
func UnaryServerInterceptor() grpc.UnaryServerInterceptor {
	return func(
		ctx context.Context,
		req interface{},
		info *grpc.UnaryServerInfo,
		handler grpc.UnaryHandler,
	) (interface{}, error) {
		// Log the method being called
		fmt.Printf("gRPC call: %s\n", info.FullMethod)

		// Call the handler
		resp, err := handler(ctx, req)

		if err != nil {
			fmt.Printf("gRPC error: %s - %v\n", info.FullMethod, err)
		}

		return resp, err
	}
}

// StreamServerInterceptor provides logging and metrics for stream calls.
func StreamServerInterceptor() grpc.StreamServerInterceptor {
	return func(
		srv interface{},
		ss grpc.ServerStream,
		info *grpc.StreamServerInfo,
		handler grpc.StreamHandler,
	) error {
		fmt.Printf("gRPC stream: %s\n", info.FullMethod)

		err := handler(srv, ss)

		if err != nil {
			fmt.Printf("gRPC stream error: %s - %v\n", info.FullMethod, err)
		}

		return err
	}
}

// AutoscalerServiceServer is the interface for the autoscaler service.
// This would normally be generated from the proto file.
type AutoscalerServiceServer interface {
	GetClusterStatus(context.Context, *GetClusterStatusRequest) (*ClusterStatus, error)
	ScaleCluster(context.Context, *ScaleClusterRequest) (*ScaleClusterResponse, error)
	UpdateScalingPolicy(context.Context, *UpdateScalingPolicyRequest) (*UpdateScalingPolicyResponse, error)
	GetScalingPolicy(context.Context, *GetScalingPolicyRequest) (*ScalingPolicy, error)
	ListScalingEvents(context.Context, *ListScalingEventsRequest) (*ListScalingEventsResponse, error)
	StreamMetrics(*StreamMetricsRequest, AutoscalerService_StreamMetricsServer) error
}

// AutoscalerService_StreamMetricsServer is the stream server interface.
type AutoscalerService_StreamMetricsServer interface {
	Send(*ClusterMetrics) error
	grpc.ServerStream
}

// RegisterAutoscalerServiceServer registers the autoscaler service.
func RegisterAutoscalerServiceServer(s *grpc.Server, srv AutoscalerServiceServer) {
	s.RegisterService(&_AutoscalerService_serviceDesc, srv)
}

// Service descriptor for registration.
var _AutoscalerService_serviceDesc = grpc.ServiceDesc{
	ServiceName: "autoscaler.AutoscalerService",
	HandlerType: (*AutoscalerServiceServer)(nil),
	Methods: []grpc.MethodDesc{
		{
			MethodName: "GetClusterStatus",
			Handler:    _AutoscalerService_GetClusterStatus_Handler,
		},
		{
			MethodName: "ScaleCluster",
			Handler:    _AutoscalerService_ScaleCluster_Handler,
		},
		{
			MethodName: "UpdateScalingPolicy",
			Handler:    _AutoscalerService_UpdateScalingPolicy_Handler,
		},
		{
			MethodName: "GetScalingPolicy",
			Handler:    _AutoscalerService_GetScalingPolicy_Handler,
		},
		{
			MethodName: "ListScalingEvents",
			Handler:    _AutoscalerService_ListScalingEvents_Handler,
		},
	},
	Streams: []grpc.StreamDesc{
		{
			StreamName:    "StreamMetrics",
			Handler:       _AutoscalerService_StreamMetrics_Handler,
			ServerStreams: true,
		},
	},
	Metadata: "autoscaler.proto",
}

func _AutoscalerService_GetClusterStatus_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(GetClusterStatusRequest)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(AutoscalerServiceServer).GetClusterStatus(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: "/autoscaler.AutoscalerService/GetClusterStatus",
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(AutoscalerServiceServer).GetClusterStatus(ctx, req.(*GetClusterStatusRequest))
	}
	return interceptor(ctx, in, info, handler)
}

func _AutoscalerService_ScaleCluster_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(ScaleClusterRequest)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(AutoscalerServiceServer).ScaleCluster(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: "/autoscaler.AutoscalerService/ScaleCluster",
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(AutoscalerServiceServer).ScaleCluster(ctx, req.(*ScaleClusterRequest))
	}
	return interceptor(ctx, in, info, handler)
}

func _AutoscalerService_UpdateScalingPolicy_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(UpdateScalingPolicyRequest)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(AutoscalerServiceServer).UpdateScalingPolicy(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: "/autoscaler.AutoscalerService/UpdateScalingPolicy",
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(AutoscalerServiceServer).UpdateScalingPolicy(ctx, req.(*UpdateScalingPolicyRequest))
	}
	return interceptor(ctx, in, info, handler)
}

func _AutoscalerService_GetScalingPolicy_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(GetScalingPolicyRequest)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(AutoscalerServiceServer).GetScalingPolicy(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: "/autoscaler.AutoscalerService/GetScalingPolicy",
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(AutoscalerServiceServer).GetScalingPolicy(ctx, req.(*GetScalingPolicyRequest))
	}
	return interceptor(ctx, in, info, handler)
}

func _AutoscalerService_ListScalingEvents_Handler(srv interface{}, ctx context.Context, dec func(interface{}) error, interceptor grpc.UnaryServerInterceptor) (interface{}, error) {
	in := new(ListScalingEventsRequest)
	if err := dec(in); err != nil {
		return nil, err
	}
	if interceptor == nil {
		return srv.(AutoscalerServiceServer).ListScalingEvents(ctx, in)
	}
	info := &grpc.UnaryServerInfo{
		Server:     srv,
		FullMethod: "/autoscaler.AutoscalerService/ListScalingEvents",
	}
	handler := func(ctx context.Context, req interface{}) (interface{}, error) {
		return srv.(AutoscalerServiceServer).ListScalingEvents(ctx, req.(*ListScalingEventsRequest))
	}
	return interceptor(ctx, in, info, handler)
}

func _AutoscalerService_StreamMetrics_Handler(srv interface{}, stream grpc.ServerStream) error {
	m := new(StreamMetricsRequest)
	if err := stream.RecvMsg(m); err != nil {
		return err
	}
	return srv.(AutoscalerServiceServer).StreamMetrics(m, &autoscalerServiceStreamMetricsServer{stream})
}

type autoscalerServiceStreamMetricsServer struct {
	grpc.ServerStream
}

func (x *autoscalerServiceStreamMetricsServer) Send(m *ClusterMetrics) error {
	return x.ServerStream.SendMsg(m)
}
