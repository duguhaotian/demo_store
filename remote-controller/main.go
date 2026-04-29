package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	goruntime "runtime"
	"sync"
	"syscall"
	"time"

	sandboxapi "github.com/containerd/containerd/api/services/sandbox/v1"
	typesapi "github.com/containerd/containerd/api/types"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/types/known/timestamppb"
)

const (
	stateReady    = "SANDBOX_READY"
	stateNotReady = "SANDBOX_NOTREADY"
)

type sandboxRecord struct {
	id         string
	sandboxer  string
	pid        uint32
	state      string
	info       map[string]string
	labels     map[string]string
	createdAt  time.Time
	exitedAt   time.Time
	exitStatus uint32
	stopped    chan struct{}
	stopOnce   sync.Once
}

type controllerServer struct {
	sandboxapi.UnimplementedControllerServer

	mu        sync.Mutex
	sandboxes map[string]*sandboxRecord
}

func newControllerServer() *controllerServer {
	return &controllerServer{
		sandboxes: make(map[string]*sandboxRecord),
	}
}

func (c *controllerServer) Create(ctx context.Context, req *sandboxapi.ControllerCreateRequest) (*sandboxapi.ControllerCreateResponse, error) {
	id := req.GetSandboxID()
	if id == "" {
		return nil, status.Error(codes.InvalidArgument, "sandbox_id is required")
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	if _, ok := c.sandboxes[id]; ok {
		return nil, status.Errorf(codes.AlreadyExists, "sandbox %q already exists", id)
	}

	info := map[string]string{
		"sandboxer":   req.GetSandboxer(),
		"netns_path":  req.GetNetnsPath(),
		"rootfs":      fmt.Sprintf("%d mount(s)", len(req.GetRootfs())),
		"annotations": fmt.Sprintf("%d item(s)", len(req.GetAnnotations())),
	}
	if sb := req.GetSandbox(); sb != nil {
		info["sandbox_runtime"] = sb.GetRuntime().GetName()
	}

	c.sandboxes[id] = &sandboxRecord{
		id:        id,
		sandboxer: req.GetSandboxer(),
		state:     stateNotReady,
		info:      info,
		labels: map[string]string{
			"oci_runtime_type": "remote-controller-demo",
		},
		stopped: make(chan struct{}),
	}

	log.Printf("Create sandbox=%s sandboxer=%s netns=%q", id, req.GetSandboxer(), req.GetNetnsPath())
	return &sandboxapi.ControllerCreateResponse{SandboxID: id}, nil
}

func (c *controllerServer) Start(ctx context.Context, req *sandboxapi.ControllerStartRequest) (*sandboxapi.ControllerStartResponse, error) {
	rec, err := c.get(req.GetSandboxID())
	if err != nil {
		return nil, err
	}

	c.mu.Lock()
	if rec.createdAt.IsZero() {
		rec.createdAt = time.Now()
	}
	rec.pid = uint32(os.Getpid())
	rec.state = stateReady
	createdAt := rec.createdAt
	labels := cloneMap(rec.labels)
	c.mu.Unlock()

	log.Printf("Start sandbox=%s pid=%d", rec.id, rec.pid)
	return &sandboxapi.ControllerStartResponse{
		SandboxID: rec.id,
		Pid:       rec.pid,
		CreatedAt: timestamppb.New(createdAt),
		Labels:    labels,
	}, nil
}

func (c *controllerServer) Platform(ctx context.Context, req *sandboxapi.ControllerPlatformRequest) (*sandboxapi.ControllerPlatformResponse, error) {
	if _, err := c.get(req.GetSandboxID()); err != nil {
		return nil, err
	}
	return &sandboxapi.ControllerPlatformResponse{
		Platform: &typesapi.Platform{
			OS:           "linux",
			Architecture: runtimeArch(),
		},
	}, nil
}

func (c *controllerServer) Stop(ctx context.Context, req *sandboxapi.ControllerStopRequest) (*sandboxapi.ControllerStopResponse, error) {
	rec, err := c.get(req.GetSandboxID())
	if err != nil {
		return nil, err
	}

	c.stopRecord(rec, 0)
	log.Printf("Stop sandbox=%s timeout=%d", rec.id, req.GetTimeoutSecs())
	return &sandboxapi.ControllerStopResponse{}, nil
}

func (c *controllerServer) Wait(ctx context.Context, req *sandboxapi.ControllerWaitRequest) (*sandboxapi.ControllerWaitResponse, error) {
	rec, err := c.get(req.GetSandboxID())
	if err != nil {
		return nil, err
	}

	select {
	case <-rec.stopped:
	case <-ctx.Done():
		return nil, ctx.Err()
	}

	c.mu.Lock()
	exitStatus := rec.exitStatus
	exitedAt := rec.exitedAt
	c.mu.Unlock()

	log.Printf("Wait sandbox=%s exit=%d", rec.id, exitStatus)
	return &sandboxapi.ControllerWaitResponse{
		ExitStatus: exitStatus,
		ExitedAt:   timestamppb.New(exitedAt),
	}, nil
}

func (c *controllerServer) Status(ctx context.Context, req *sandboxapi.ControllerStatusRequest) (*sandboxapi.ControllerStatusResponse, error) {
	rec, err := c.get(req.GetSandboxID())
	if err != nil {
		return nil, err
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	resp := &sandboxapi.ControllerStatusResponse{
		SandboxID: rec.id,
		Pid:       rec.pid,
		State:     rec.state,
		CreatedAt: timestamppb.New(rec.createdAt),
		ExitedAt:  timestamppb.New(rec.exitedAt),
	}
	if req.GetVerbose() {
		resp.Info = cloneMap(rec.info)
	}
	return resp, nil
}

func (c *controllerServer) Shutdown(ctx context.Context, req *sandboxapi.ControllerShutdownRequest) (*sandboxapi.ControllerShutdownResponse, error) {
	rec, err := c.get(req.GetSandboxID())
	if err != nil {
		return nil, err
	}

	c.stopRecord(rec, 0)

	c.mu.Lock()
	delete(c.sandboxes, rec.id)
	c.mu.Unlock()

	log.Printf("Shutdown sandbox=%s", rec.id)
	return &sandboxapi.ControllerShutdownResponse{}, nil
}

func (c *controllerServer) Metrics(ctx context.Context, req *sandboxapi.ControllerMetricsRequest) (*sandboxapi.ControllerMetricsResponse, error) {
	if _, err := c.get(req.GetSandboxID()); err != nil {
		return nil, err
	}
	return &sandboxapi.ControllerMetricsResponse{}, nil
}

func (c *controllerServer) Update(ctx context.Context, req *sandboxapi.ControllerUpdateRequest) (*sandboxapi.ControllerUpdateResponse, error) {
	rec, err := c.get(req.GetSandboxID())
	if err != nil {
		return nil, err
	}

	c.mu.Lock()
	rec.info["updated_fields"] = fmt.Sprintf("%v", req.GetFields())
	c.mu.Unlock()

	log.Printf("Update sandbox=%s fields=%v", req.GetSandboxID(), req.GetFields())
	return &sandboxapi.ControllerUpdateResponse{}, nil
}

func (c *controllerServer) get(id string) (*sandboxRecord, error) {
	if id == "" {
		return nil, status.Error(codes.InvalidArgument, "sandbox_id is required")
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	rec, ok := c.sandboxes[id]
	if !ok {
		return nil, status.Errorf(codes.NotFound, "sandbox %q not found", id)
	}
	return rec, nil
}

func (c *controllerServer) stopRecord(rec *sandboxRecord, exitStatus uint32) {
	rec.stopOnce.Do(func() {
		c.mu.Lock()
		rec.state = stateNotReady
		rec.exitStatus = exitStatus
		rec.exitedAt = time.Now()
		c.mu.Unlock()
		close(rec.stopped)
	})
}

func cloneMap(in map[string]string) map[string]string {
	if len(in) == 0 {
		return nil
	}
	out := make(map[string]string, len(in))
	for k, v := range in {
		out[k] = v
	}
	return out
}

func runtimeArch() string {
	switch arch := goruntime.GOARCH; arch {
	case "":
		return "amd64"
	default:
		return arch
	}
}

func listen(address string) (net.Listener, func(), error) {
	if address == "" {
		return nil, nil, errors.New("address is required")
	}

	if hasTCPScheme(address) {
		l, err := net.Listen("tcp", address[len("tcp://"):])
		return l, func() {}, err
	}

	path := address
	if len(path) > len("unix://") && path[:len("unix://")] == "unix://" {
		path = path[len("unix://"):]
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return nil, nil, err
	}
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, nil, err
	}

	l, err := net.Listen("unix", path)
	if err != nil {
		return nil, nil, err
	}
	return l, func() { _ = os.Remove(path) }, nil
}

func hasTCPScheme(address string) bool {
	return len(address) > len("tcp://") && address[:len("tcp://")] == "tcp://"
}

func main() {
	address := flag.String("address", "/run/containerd/remote-controller-demo.sock", "unix socket path, unix:// path, or tcp://host:port")
	flag.Parse()

	l, cleanup, err := listen(*address)
	if err != nil {
		log.Fatalf("listen %q: %v", *address, err)
	}
	defer cleanup()

	grpcServer := grpc.NewServer()
	sandboxapi.RegisterControllerServer(grpcServer, newControllerServer())

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-stop
		log.Print("shutting down")
		grpcServer.GracefulStop()
	}()

	log.Printf("remote sandbox controller listening on %s", *address)
	if err := grpcServer.Serve(l); err != nil {
		log.Fatalf("serve: %v", err)
	}
}
