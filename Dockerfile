FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk

WORKDIR /app

RUN apt update && apt install -y gcc-10 g++-10
RUN ln -sf /usr/bin/gcc-10 /usr/bin/cc && ln -sf /usr/bin/g++-10 /usr/bin/c++
RUN git clone https://github.com/alliedmodders/ambuild
RUN cd ambuild && python setup.py install && cd ..
RUN git clone https://github.com/alliedmodders/metamod-source
RUN git config --global --add safe.directory /app

COPY ./docker-entrypoint.sh ./
ENV HL2SDKCS2=/app/source/vendor/hl2sdk-cs2
ENV MMSOURCE112=/app/source/vendor/metamod-source
WORKDIR /app/source
CMD ["/bin/bash", "./docker-entrypoint.sh"]