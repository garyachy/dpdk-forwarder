# Stage 1: Build DPDK from submodule
FROM ubuntu:22.04 AS dpdk-builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    meson \
    ninja-build \
    python3-pyelftools \
    libnuma-dev \
    libpcap-dev \
    pkg-config \
    git \
  && rm -rf /var/lib/apt/lists/*

COPY dpdk/ /build/dpdk/
RUN cd /build/dpdk && \
    meson setup build \
      --prefix=/usr/local \
      -Dplatform=generic \
      -Ddisable_drivers="crypto/*,compress/*,regex/*,baseband/*,dma/*,event/*,raw/*" \
      -Ddisable_libs="bbdev,bitratestats,bpf,cfgfile,efd,fib,flow_classify,gpudev,gro,gso,ipsec,jobstats,latencystats,member,metrics,mldev,pdcp,pdump,power,rcu,rib,sched,security" \
    && ninja -C build install \
    && ldconfig

# Stage 2: Build the forwarder
FROM dpdk-builder AS app-builder

COPY . /build/app/
WORKDIR /build/app
RUN meson setup build && ninja -C build

# Stage 3: Minimal runtime image (no system DPDK — we copy compiled libs from builder)
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libatomic1 \
    libnuma1 \
    libpcap0.8 \
    tmux \
    python3 \
  && rm -rf /var/lib/apt/lists/*

COPY --from=dpdk-builder /usr/local/lib /usr/local/lib
COPY --from=dpdk-builder /usr/local/bin/dpdk-testpmd /usr/local/bin/dpdk-testpmd
COPY --from=app-builder /build/app/build/dpdk-forwarder /usr/local/bin/dpdk-forwarder
COPY tests/ /tests/
RUN ldconfig

CMD ["/bin/bash"]
