# Remote Controller Demo

This is a minimal gRPC sandbox controller that matches the containerd v2.2.3 sandbox controller API used by CRI.

## Containerd Flow

In containerd v2.2.3, an external sandbox controller is wired through `proxy_plugins`. When CRI handles `RunPodSandbox`, it resolves the runtime handler's `sandboxer`, then calls the controller RPCs in this order:

1. `Create`: receives the sandbox metadata, rootfs, CRI `PodSandboxConfig` in `options`, annotations, and netns path.
2. `Start`: marks the sandbox as running and returns pid, labels, and optionally a task API endpoint.
3. `Wait`: CRI starts a background monitor after the sandbox has started.
4. `Status`, `Stop`, `Shutdown`, `Metrics`, and `Update`: used by later CRI lifecycle and inspection calls.

This demo keeps sandbox state in memory and returns no task API endpoint from `Start`, so normal container tasks can still use the configured runtime shim. It is meant to prove that containerd CRI can connect to a remote sandbox controller, not to replace a production runtime.

## Build

```sh
go test ./...
go build -o bin/remote-controller .
```

Or build in a container:

```sh
make container-build
```

If the container cannot reach `proxy.golang.org`, verify against a local containerd v2.2.3 source tree and its checked-in `vendor/` directory:

```sh
make container-verify CONTAINERD_SRC=/tmp/containerd-2.2.3
```

## Run

Start the controller before starting containerd:

```sh
sudo ./bin/remote-controller --address /run/containerd/remote-controller-demo.sock
```

Add this to containerd config:

```toml
[proxy_plugins.demo-remote]
  type = "sandbox"
  address = "/run/containerd/remote-controller-demo.sock"

[plugins."io.containerd.cri.v1.runtime".containerd.runtimes.runc]
  runtime_type = "io.containerd.runc.v2"
  sandboxer = "demo-remote"
```

Then restart containerd. A host-network sandbox is the smallest CRI request to try because it avoids requiring CNI setup:

```json
{
  "metadata": {
    "name": "demo",
    "namespace": "default",
    "uid": "demo",
    "attempt": 1
  },
  "linux": {
    "security_context": {
      "namespace_options": {
        "network": 2
      }
    }
  }
}
```

Use it with:

```sh
sudo crictl --runtime-endpoint unix:///run/containerd/containerd.sock runp pod.json
```
