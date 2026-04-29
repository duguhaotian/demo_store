CONTAINERD_SRC ?= /tmp/containerd-2.2.3
GO_IMAGE ?= golang:1.24.3-bookworm

.PHONY: test build container-build container-verify

test:
	go test ./...

build:
	go build -o bin/remote-controller .

container-build:
	docker build -t remote-controller-demo:local .

container-verify:
	docker run --rm \
		-v $(CONTAINERD_SRC):/work \
		-v $(CURDIR):/src:ro \
		-w /work \
		$(GO_IMAGE) \
		sh -c 'trap "rm -rf demo_remote_controller" EXIT && rm -rf demo_remote_controller && mkdir -p demo_remote_controller && cp /src/*.go demo_remote_controller/ && go test -mod=vendor ./demo_remote_controller && go build -mod=vendor -o /tmp/remote-controller-demo ./demo_remote_controller'
