# syntax=docker/dockerfile:1

FROM golang:1.22-alpine AS build
WORKDIR /src

# Dependencies layer — cached across code changes
COPY go.mod go.sum* ./
RUN go mod download

# Source + build
COPY *.go ./
RUN CGO_ENABLED=0 GOOS=linux go build \
    -ldflags="-s -w" \
    -trimpath \
    -o /out/swarmrt-feedback .

FROM alpine:3.19
RUN apk add --no-cache ca-certificates tzdata \
 && addgroup -S app && adduser -S app -G app \
 && mkdir -p /data && chown app:app /data
COPY --from=build /out/swarmrt-feedback /usr/local/bin/swarmrt-feedback
USER app
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/swarmrt-feedback"]
