# Copyright (c) 2026 J.J. Boyd. Original diagnostic source, authored with Codex.
# SPDX-License-Identifier: MIT
FROM alpine:3.23@sha256:85fe1e81d6758c208f3e1eed4338a1997e19d4be002d4dd32d3100c9a8c010a0

# Packaging metadata documented by flexradio/waveform-example/Dockerfile.
# Original diagnostic code; no waveform or radio-control implementation.
LABEL com.flexradio.waveform.name="nereus-net-probe"
LABEL com.flexradio.waveform.version="0.1.0"

RUN apk add --no-cache openssh-server openssh-client iproute2 curl bind-tools tcpdump python3 ca-certificates \
    && mkdir -p /root/.ssh /run/sshd \
    && passwd -d root
COPY authorized_keys /root/.ssh/authorized_keys
COPY sshd_config /etc/ssh/sshd_config
COPY entrypoint.sh /usr/local/bin/probe-start
COPY net-report.sh /usr/local/bin/net-report
COPY rv-probe.py /usr/local/bin/rv-probe
RUN chmod 700 /root/.ssh \
    && chmod 600 /root/.ssh/authorized_keys \
    && chmod 755 /usr/local/bin/probe-start /usr/local/bin/net-report /usr/local/bin/rv-probe
EXPOSE 22222/tcp
ENTRYPOINT ["/usr/local/bin/probe-start"]
