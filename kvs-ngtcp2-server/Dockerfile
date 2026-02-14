# kvs-ngtcp2 relay server (TCP+TLS). Requires OpenSSL.
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libssl-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN mkdir build && cd build && cmake .. && make
RUN cp build/kvs-ngtcp2-server /usr/local/bin/

# Default port
EXPOSE 4433
ENTRYPOINT ["/usr/local/bin/kvs-ngtcp2-server"]
CMD ["--bind", "0.0.0.0", "--port", "4433"]
