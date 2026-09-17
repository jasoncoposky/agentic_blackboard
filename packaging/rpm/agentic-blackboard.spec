Name:           agentic-blackboard
Version:        0.4.0
Release:        1.el9
Summary:        Agentic Blackboard Server & Ambient Substrate Daemon
License:        Apache-2.0
URL:            https://github.com/agentic-blackboard/agentic-blackboard

Requires:         python3, zeromq, openssl
Requires(pre):    shadow-utils
Requires(post):   systemd
Requires(preun):  systemd
Requires(postun): systemd

AutoReqProv:    no

%define debug_package %{nil}
%define _build_id_links none

%description
Agentic Blackboard ambient substrate daemon, CLI control tool,
and semantic skills catalog.

%prep
cp %{_sourcedir}/LICENSE . 2>/dev/null || true

%build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/usr/bin
mkdir -p %{buildroot}/usr/lib/systemd/system
mkdir -p %{buildroot}/etc/agentic-blackboard
mkdir -p %{buildroot}/etc/security/limits.d
mkdir -p %{buildroot}/usr/share/agentic-blackboard/skills
mkdir -p %{buildroot}/var/lib/agentic-blackboard
mkdir -p %{buildroot}/var/log/agentic-blackboard
mkdir -p %{buildroot}/usr/include

SOURCE_DIR="%{_sourcedir}"
if [ ! -f "$SOURCE_DIR/build/agentic-blackboardd" ] && [ -f "$SOURCE_DIR/../build/agentic-blackboardd" ]; then
    SOURCE_DIR="$SOURCE_DIR/.."
fi

install -m 0755 $SOURCE_DIR/build/agentic-blackboardd %{buildroot}/usr/bin/agentic-blackboardd
install -m 0755 $SOURCE_DIR/src/ab-ctl.py %{buildroot}/usr/bin/ab-ctl
install -m 0644 $SOURCE_DIR/packaging/systemd/agentic-blackboard.service %{buildroot}/usr/lib/systemd/system/agentic-blackboard.service
install -m 0640 $SOURCE_DIR/packaging/config/blackboard.conf %{buildroot}/etc/agentic-blackboard/blackboard.conf
install -m 0644 $SOURCE_DIR/packaging/config/blackboard.conf.default %{buildroot}/etc/agentic-blackboard/blackboard.conf.default
install -m 0644 $SOURCE_DIR/packaging/limits/99-blackboard.conf %{buildroot}/etc/security/limits.d/99-blackboard.conf
cp -r $SOURCE_DIR/skills/* %{buildroot}/usr/share/agentic-blackboard/skills/
cp -r $SOURCE_DIR/include/agentic_blackboard %{buildroot}/usr/include/
cp -r $SOURCE_DIR/include/ab %{buildroot}/usr/include/

%pre
getent group blackboard >/dev/null || groupadd -r blackboard
getent passwd blackboard >/dev/null || \
    useradd -r -g blackboard -d /var/lib/agentic-blackboard -s /sbin/nologin \
    -c "Agentic Blackboard daemon" blackboard
exit 0

%post
%systemd_post agentic-blackboard.service
if [ ! -f /var/lib/agentic-blackboard/initialized ]; then
    mkdir -p /var/lib/agentic-blackboard /var/log/agentic-blackboard
    chown -R blackboard:blackboard /var/lib/agentic-blackboard /var/log/agentic-blackboard
    if [ -x /usr/bin/ab-ctl ]; then
        /usr/bin/ab-ctl init --bootstrap --data-dir=/var/lib/agentic-blackboard && \
            touch /var/lib/agentic-blackboard/initialized || true
    fi
    chown -R blackboard:blackboard /var/lib/agentic-blackboard
fi

%preun
%systemd_preun agentic-blackboard.service

%postun
%systemd_postun_with_restart agentic-blackboard.service

%files
%license LICENSE
%attr(0755, root, root) /usr/bin/agentic-blackboardd
%attr(0755, root, root) /usr/bin/ab-ctl
%attr(0644, root, root) /usr/lib/systemd/system/agentic-blackboard.service
%dir %attr(0750, root, blackboard) /etc/agentic-blackboard
%config(noreplace) %attr(0640, root, blackboard) /etc/agentic-blackboard/blackboard.conf
%config(noreplace) %attr(0644, root, root) /etc/agentic-blackboard/blackboard.conf.default
%config(noreplace) %attr(0644, root, root) /etc/security/limits.d/99-blackboard.conf
/usr/share/agentic-blackboard
/usr/include/agentic_blackboard
/usr/include/ab
%dir %attr(0750, blackboard, blackboard) /var/lib/agentic-blackboard
%dir %attr(0750, blackboard, blackboard) /var/log/agentic-blackboard
