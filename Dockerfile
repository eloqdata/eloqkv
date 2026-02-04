# Stage 1: Builder
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
  sudo \
  curl \
  git \
  build-essential \
  pkg-config \
  && rm -rf /var/lib/apt/lists/*

COPY scripts /tmp/scripts
RUN chmod +x /tmp/scripts/*.sh /tmp/scripts/dep/ubuntu/*.sh

RUN /tmp/scripts/install_dependency_ubuntu2404.sh

WORKDIR /app

COPY . .

RUN mkdir build && cd build && \
  cmake .. -DCMAKE_BUILD_TYPE=Release && \
  make -j$(nproc)

RUN mkdir -p /so && \
  ldd build/eloqkv | grep "=> /" | awk '{print $3}' | xargs -I '{}' cp -v -L "{}" /so/

# Stage 2: Final Runtime Image
FROM ubuntu:24.04

LABEL description="EloqKV Runtime Image"

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
  sudo \
  curl \
  ca-certificates \
  iproute2 \
  vim \
  openssl \
  libgflags-dev \
  libleveldb-dev \
  libsnappy-dev \
  zlib1g \
  liblz4-1 \
  libzstd1 \
  && rm -rf /var/lib/apt/lists/*

RUN useradd -rm -s /bin/bash -g sudo eloquser

USER eloquser
WORKDIR /home/eloquser

RUN mkdir -p /home/eloquser/EloqKV/bin /home/eloquser/EloqKV/conf

COPY --from=builder /app/build/eloqkv /home/eloquser/EloqKV/bin/eloqkv
COPY --from=builder /so/* /usr/local/lib/
COPY ./eloqkv.ini /home/eloquser/EloqKV/conf/

USER root
RUN ldconfig
USER eloquser

COPY scripts/docker-entrypoint.sh /home/eloquser/EloqKV/bin/entrypoint.sh

ENV PATH="$PATH:/home/eloquser/EloqKV/bin"
EXPOSE 6379

ENTRYPOINT ["entrypoint.sh"]
