FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk

WORKDIR /app

RUN apt update && apt install -y wget gnupg lsb-release curl
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add -
RUN echo "deb http://apt.llvm.org/bullseye/ llvm-toolchain-bullseye-14 main" > /etc/apt/sources.list.d/llvm.list
RUN apt update && apt install -y clang-14
RUN ln -sf /usr/bin/clang-14 /usr/bin/clang && ln -sf /usr/bin/clang-14 /usr/bin/clang++

RUN git clone https://github.com/alliedmodders/ambuild
RUN cd ambuild && python setup.py install && cd ..
RUN git clone https://github.com/alliedmodders/metamod-source
RUN git config --global --add safe.directory /app

COPY ./docker-entrypoint.sh ./
ENV HL2SDKCS2=/app/source/vendor/hl2sdk-cs2
ENV MMSOURCE112=/app/source/vendor/metamod-source
WORKDIR /app/source
CMD ["/bin/bash", "./docker-entrypoint.sh"]