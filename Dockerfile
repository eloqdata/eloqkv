FROM ubuntu:24.04

RUN set -eux; \
    apt update; \
    export DEBIAN_FRONTEND=noninteractive; \
    apt install -y --no-install-recommends sudo curl ca-certificates iproute2 vim; \
    rm -rf /var/lib/apt/lists/*;

RUN useradd -rm -s /bin/bash -g sudo eloquser && \
    echo '%sudo ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers
USER eloquser
WORKDIR /home/eloquser

ARG ELOQKV
RUN set -eux; \
    case $(uname -m) in amd64 | x86_64) ARCH=amd64 ;; arm64 | aarch64) ARCH=arm64 ;; *) ARCH= $(uname -m) ;; esac; \
    curl "https://download.eloqdata.com/eloqkv/rocksdb/eloqkv-${ELOQKV}-ubuntu24-${ARCH}.tar.gz" | tar xvz

ENV PATH="$PATH:/home/eloquser/EloqKV/bin"
EXPOSE 6379
COPY scripts/docker-entrypoint.sh /home/eloquser/EloqKV/bin/entrypoint.sh
ENTRYPOINT ["entrypoint.sh"]
