FROM registry.access.redhat.com/ubi9/ubi-minimal:latest

COPY build/agentic-blackboard-*.rpm /tmp/
RUN rpm -Uvh https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm && \
    microdnf install -y shadow-utils systemd util-linux zeromq openssl python3 python3-pip python3.11 python3.11-pip && \
    ln -sf /usr/bin/python3.11 /usr/bin/python3 && \
    ln -sf /usr/bin/pip3.11 /usr/bin/pip3 && \
    pip3 install httpx mcp && \
    rpm -ivh /tmp/agentic-blackboard-*.rpm && \
    rm -f /tmp/agentic-blackboard-*.rpm && \
    mkdir -p /etc/agentic-blackboard.dist && \
    cp -a /etc/agentic-blackboard/* /etc/agentic-blackboard.dist/ && \
    mv /usr/bin/agentic-blackboardd /usr/bin/agentic-blackboardd.bin && \
    printf '#!/bin/bash\nset -e\nif [ ! -f /etc/agentic-blackboard/blackboard.conf ] && [ -f /etc/agentic-blackboard.dist/blackboard.conf ]; then\n    cp -a /etc/agentic-blackboard.dist/* /etc/agentic-blackboard/ 2>/dev/null || true\nfi\nif [ ! -f /var/lib/agentic-blackboard/admin.token ] && [ ! -f /var/lib/agentic-blackboard/credentials.db ]; then\n    /usr/bin/ab-ctl init --bootstrap --data-dir=/var/lib/agentic-blackboard\n    chmod 0644 /var/lib/agentic-blackboard/admin.token 2>/dev/null || true\nfi\nexec /usr/bin/agentic-blackboardd.bin "$@"\n' > /usr/bin/agentic-blackboardd && \
    chmod 0755 /usr/bin/agentic-blackboardd && \
    microdnf clean all

USER blackboard
EXPOSE 8085 8090
VOLUME ["/var/lib/agentic-blackboard", "/etc/agentic-blackboard"]
ENTRYPOINT ["/usr/bin/agentic-blackboardd", "--config=/etc/agentic-blackboard/blackboard.conf"]
