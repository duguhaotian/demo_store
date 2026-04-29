package main

import (
	"context"
	"net"
	"testing"

	sandboxapi "github.com/containerd/containerd/api/services/sandbox/v1"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

func TestSandboxLifecycle(t *testing.T) {
	ctx := context.Background()
	srv := newControllerServer()

	if _, err := srv.Create(ctx, &sandboxapi.ControllerCreateRequest{
		SandboxID: "sandbox-1",
		Sandboxer: "demo-remote",
	}); err != nil {
		t.Fatalf("Create failed: %v", err)
	}

	start, err := srv.Start(ctx, &sandboxapi.ControllerStartRequest{SandboxID: "sandbox-1"})
	if err != nil {
		t.Fatalf("Start failed: %v", err)
	}
	if start.GetPid() == 0 {
		t.Fatal("Start returned empty pid")
	}

	status, err := srv.Status(ctx, &sandboxapi.ControllerStatusRequest{
		SandboxID: "sandbox-1",
		Verbose:   true,
	})
	if err != nil {
		t.Fatalf("Status failed: %v", err)
	}
	if status.GetState() != stateReady {
		t.Fatalf("unexpected state %q", status.GetState())
	}

	if _, err := srv.Stop(ctx, &sandboxapi.ControllerStopRequest{SandboxID: "sandbox-1"}); err != nil {
		t.Fatalf("Stop failed: %v", err)
	}
	wait, err := srv.Wait(ctx, &sandboxapi.ControllerWaitRequest{SandboxID: "sandbox-1"})
	if err != nil {
		t.Fatalf("Wait failed: %v", err)
	}
	if wait.GetExitStatus() != 0 {
		t.Fatalf("unexpected exit status %d", wait.GetExitStatus())
	}
}

func TestGRPCSandboxLifecycle(t *testing.T) {
	ctx := context.Background()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen failed: %v", err)
	}

	grpcServer := grpc.NewServer()
	sandboxapi.RegisterControllerServer(grpcServer, newControllerServer())
	go func() {
		_ = grpcServer.Serve(lis)
	}()
	defer grpcServer.Stop()

	conn, err := grpc.DialContext(ctx, lis.Addr().String(), grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("dial failed: %v", err)
	}
	defer conn.Close()

	client := sandboxapi.NewControllerClient(conn)
	if _, err := client.Create(ctx, &sandboxapi.ControllerCreateRequest{
		SandboxID: "sandbox-rpc",
		Sandboxer: "demo-remote",
	}); err != nil {
		t.Fatalf("Create RPC failed: %v", err)
	}
	if _, err := client.Start(ctx, &sandboxapi.ControllerStartRequest{SandboxID: "sandbox-rpc"}); err != nil {
		t.Fatalf("Start RPC failed: %v", err)
	}
	status, err := client.Status(ctx, &sandboxapi.ControllerStatusRequest{SandboxID: "sandbox-rpc"})
	if err != nil {
		t.Fatalf("Status RPC failed: %v", err)
	}
	if status.GetState() != stateReady {
		t.Fatalf("unexpected RPC state %q", status.GetState())
	}
}
