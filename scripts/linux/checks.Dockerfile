# The game-free checks' environment (tests/run_all.py --strict), for running them on any distribution: the system CI runs
# them in (.forgejo/workflows/ci.yml, Ubuntu 24.04) with the same packages, Python packages, glslang and headers.
# tests/test_checks_container.py checks that the versions here are ci.yml's and mise.toml's.
#
# The image holds the tools only. The checkout is mounted at its own path when it runs, with no network.
# scripts/linux/checks.sh builds the image and runs it (tests/run_all.py --container).
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
# (g++ clang lld spirv-tools ccache libdav1d-dev are ci.yml's, with what they recommend: clang's sanitizer libraries among
# it. The rest is what its runner has: Python 3.12, git, curl, .NET 8)
RUN apt-get update \
 && apt-get install -y g++ clang lld spirv-tools ccache libdav1d-dev \
 && apt-get install -y --no-install-recommends python3 python3-venv git ca-certificates curl file make cmake pkg-config \
      zip unzip xz-utils procps dotnet-sdk-8.0 \
 && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/python \
 && /opt/python/bin/pip install --no-cache-dir --disable-pip-version-check --no-deps \
      zstandard==0.25.0 pillow==13.0.0 numpy==2.5.4 lz4==4.4.5 unicorn==2.1.4
RUN mkdir /opt/glslang \
 && curl -fL --retry 3 -o /opt/glslang.tar.gz \
      https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-linux-x86_64-release.tar.gz \
 && echo '2c34071f56ecf39d16233294d3739cf43f981fd39080f2fd9dccb782101191ce  /opt/glslang.tar.gz' | sha256sum --check --strict \
 && tar -xzf /opt/glslang.tar.gz -C /opt/glslang \
 && rm /opt/glslang.tar.gz
COPY fetch_commit.sh /opt/
RUN /opt/fetch_commit.sh https://github.com/KhronosGroup/SPIRV-Headers 04f10f650d514df88b76d25e83db360142c7b174 /opt/spirv-headers \
 && /opt/fetch_commit.sh https://github.com/KhronosGroup/Vulkan-Headers 74d8a6cb930c68ef617b202c3ff3c59d919e086b /opt/vulkan-headers

# (run as the caller's user, whose home is not in the image: ccache, .NET and git want a writable HOME)
ENV HOME=/tmp DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
    LONGSHOT_VULKAN_HEADERS=/opt/vulkan-headers/include LONGSHOT_SPIRV_HEADERS=/opt/spirv-headers/include \
    PATH=/opt/python/bin:/opt/glslang/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
# (the checkout belongs to the caller, not to the container's user under every runtime)
RUN git config --system --add safe.directory '*'
