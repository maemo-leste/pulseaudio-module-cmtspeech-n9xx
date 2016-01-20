Name:       pulseaudio-module-cmtspeech-n9xx

%define pulseversion 5.0

Summary:    Cmtspeech module for PulseAudio on N9xx
Version:    5.0.4
Release:    1
Group:      Multimedia/PulseAudio
License:    LGPLv2.1+
ExclusiveArch:  %arm
URL:        https://github.com/nemomobile/pulseaudio-module-cmtspeech-n9xx
Source0:    %{name}-%{version}.tar.gz
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(pulsecore) >= 5.0
BuildRequires:  pkgconfig(libmeego-common) >= 5.0.15
BuildRequires:  libcmtspeechdata-devel
BuildRequires:  libtool-ltdl-devel

%description
Cmtspeech module for PulseAudio on N9xx device.

%prep
%setup -q -n %{name}-%{version}

%build
autoreconf -vfi

%reconfigure --disable-static
make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install

%files
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulseversion}/modules/module-meego-cmtspeech.so
