%global provider        github
%global provider_tld    com
%global project         projectatomic
%global repo            oci-systemd-hook
# https://github.com/projectatomic/oci-register-machine
%global provider_prefix %{provider}.%{provider_tld}/%{project}/%{repo}
%global import_path     %{provider_prefix}
%global commit          76d0b9c748f5f29e999a7fefd8c5bd588fda39de
%global shortcommit     %(c=%{commit}; echo ${c:0:7})

Name:           oci-systemd-hook
Version:        0.1.3
Release:        1.git%{shortcommit}%{?dist}
Summary:        OCI systemd hook for docker
Group:          Applications/Text
License:        GPLv3+
URL:            https://%{provider_prefix}
Source0:        https://%{provider_prefix}/archive/%{commit}/%{repo}-%{shortcommit}.tar.gz

BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  yajl-devel
BuildRequires:  libselinux-devel
BuildRequires:  libmount-devel

%description
OCI systemd hooks enable running systemd in a OCI runc/docker container.

%prep
%setup -q -n %{repo}-%{commit}

%build
autoreconf -i
%configure --libexecdir=/usr/libexec/oci/hooks.d/
make %{?_smp_mflags}

%install
%make_install

%files
%{_libexecdir}/oci/hooks.d/oci-systemd-hook
%{_mandir}/man1/oci-systemd-hook.1*
%doc README.md LICENSE

%changelog
* Mon Nov 23 2015 Mrunal Patel <mrunalp@gmail.com> - 0.1.3
- Fix bug in man page installation
* Mon Nov 23 2015 Mrunal Patel <mrunalp@gmail.com> - 0.1.2
- Add man pages
* Mon Nov 23 2015 Mrunal Patel <mrunalp@gmail.com> - 0.1.1
- Initial RPM release
