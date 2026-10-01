%global srcname linuxqq-wayland-screenshare-fix

Name:           linuxqq-wayland-native-screenshare-fix
Version:        %{_ver}
Release:        1%{?dist}
Summary:        Fix Linux QQ screen sharing on Wayland
License:        MIT
URL:            https://github.com/SHORiN-KiWATA/%{srcname}
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(libpulse)
BuildRequires:  pkgconfig(libpipewire-0.3)
Requires:       glib2
Recommends:     linuxqq
Recommends:     xdg-desktop-portal

%description
Lets Linux QQ use its own built-in xdg-desktop-portal + PipeWire screen capture
path on Wayland, and fixes device-audio sharing on sound cards whose native
sample format is not s16le/f32le. Start QQ with linuxqq-wayland-native-screenshare-fix.

%prep
%autosetup

%build
%make_build PREFIX=%{_prefix} LIBEXECDIR=%{_libdir}/%{srcname} VERSION=%{version} \
    CFLAGS="%{optflags}" LDFLAGS="%{build_ldflags}"

%install
%make_install PREFIX=%{_prefix} LIBEXECDIR=%{_libdir}/%{srcname} VERSION=%{version}

%files
%license %{_datadir}/licenses/%{srcname}/LICENSE
%doc %{_docdir}/%{srcname}/README.md
%{_bindir}/linuxqq-wayland-native-screenshare-fix
%{_libdir}/%{srcname}/
%{_datadir}/applications/linuxqq-wayland-native-screenshare-fix.desktop

%changelog
* Thu Oct 01 2026 Shorin <shorin@example.com> - %{version}-1
- See https://github.com/SHORiN-KiWATA/%{srcname}/releases
