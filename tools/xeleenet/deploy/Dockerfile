# The lobby service as a container, for hosts that run images (README.md).
# Build from tools/xeleenet: docker build -f deploy/Dockerfile .
FROM golang:1.27 AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY *.go ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags=-s -o /xeleenet .

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /xeleenet /xeleenet
EXPOSE 8787 3478/udp
ENTRYPOINT ["/xeleenet", "-listen", ":8787", "-stun", ":3478"]
