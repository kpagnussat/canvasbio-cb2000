#
# The module only: this package never replaces or rebuilds libfprint. Where
# the module is installed comes from the tod_driversdir of the libfprint-tod
# development package present at build time, which is why the module is built
# on each distribution instead of being copied from the .deb.
#
# Built with packaging/build-rpm.sh, which builds it in a container of the
# target distribution.
#
Name:           libfprint-2-tod1-canvasbio-cb2000
Version:        1.0.0
Release:        1%{?dist}
Summary:        CanvasBio CB2000 fingerprint sensor driver (libfprint TOD module)

License:        LGPL-2.1-or-later
URL:            https://github.com/kpagnussat/canvasbio-cb2000
Source0:        %{url}/releases/download/v%{version}/canvasbio-cb2000-%{version}.tar.gz

ExclusiveArch:  x86_64

BuildRequires:  gcc
BuildRequires:  meson >= 0.61.0
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gusb)
BuildRequires:  pkgconfig(libfprint-2-tod-1)

# The library itself comes in as an automatic dependency on
# libfprint-2-tod.so.1, which is the real requirement: any libfprint built
# with TOD satisfies it, whatever the distribution calls the package.
Recommends:     fprintd

%if 0%{?fedora}
# Fedora's own libfprint has no TOD support, so the library comes from a
# third party repository, and with the default SELinux policy fprintd refuses
# to load any TOD module at all. Both packages live in the same COPR, and
# without the policy the driver installs and silently never runs.
Requires:       fprintd_tod_selinux
%endif

%description
Driver for the CanvasBio CB2000 fingerprint sensor (USB 2df0:0003), the
reader in the power button of the Samsung Galaxy Book2 360 and Book3 360.
It is loaded by libfprint through TOD, so the system libfprint is not
replaced and other fingerprint readers keep working.

The 80x64 frame the sensor returns is too small for libfprint's minutiae
matcher, so the driver matches with code written independently for this
project, using documented behavior of the matching engine of the sensor's
Windows driver as its compatibility target.

%prep
%autosetup -n canvasbio-cb2000-%{version}

%build
%meson
%meson_build

%install
%meson_install

%post
# fprintd loads TOD modules when it starts, so an installed module is only
# seen after a restart. It is socket activated, so try-restart is enough.
if [ -d /run/systemd/system ]; then
    systemctl try-restart fprintd.service >/dev/null 2>&1 || :
fi

%postun
if [ "$1" = 0 ] && [ -d /run/systemd/system ]; then
    systemctl try-restart fprintd.service >/dev/null 2>&1 || :
fi

%files
%license LICENSE
%doc README.md CHANGELOG.md
%{_libdir}/libfprint-2/tod-1/libfprint-tod-canvasbio-cb2000.so

%changelog
* Thu Sep 17 2026 Kristofer Pagnussat <kristofer.pagnussat@gmail.com> - 1.0.0-1
- First release: libfprint TOD module for the CanvasBio CB2000 (2df0:0003),
  matching with an independently written engine that uses the sensor's
  Windows engine as its compatibility target.
