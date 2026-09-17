FROM registry.access.redhat.com/ubi9/ubi-minimal:latest

COPY build/agentic-blackboard-*.rpm /tmp/
COPY packaging/container/entrypoint.sh /usr/local/bin/agentic-blackboard-entrypoint.sh

RUN rpm -Uvh https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm && \
    microdnf install -y shadow-utils systemd util-linux zeromq openssl python3 python3-pip python3.11 python3.11-pip && \
    ln -sf /usr/bin/python3.11 /usr/bin/python3 && \
    ln -sf /usr/bin/pip3.11 /usr/bin/pip3 && \
    pip3 install --no-cache-dir httpx mcp && \
    rpm -ivh /tmp/agentic-blackboard-*.rpm && \
    rm -f /tmp/agentic-blackboard-*.rpm && \
    mkdir -p /etc/agentic-blackboard.dist && \
    cp -a /etc/agentic-blackboard/* /etc/agentic-blackboard.dist/ && \
    chmod 0755 /usr/local/bin/agentic-blackboard-entrypoint.sh && \
    microdnf clean all

USER blackboard
EXPOSE 8085 8090
VOLUME ["/var/lib/agentic-blackboard", "/etc/agentic-blackboard"]
ENTRYPOINT ["/usr/local/bin/agentic-blackboard-entrypoint.sh"]
CMD ["--config=/etc/agentic-blackboard/blackboard.conf"]
