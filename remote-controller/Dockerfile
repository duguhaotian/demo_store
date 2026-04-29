FROM golang:1.24.3-bookworm

WORKDIR /src
COPY go.mod go.sum* ./
RUN go mod download
COPY . .
RUN go test ./...
RUN go build -o /out/remote-controller .
