%define version	%(cat %{_topdir}/version.txt)

Name:		libqsbr
Version:	%{version}
Release:	1%{?dist}
Summary:	EBR and QSBR based reclamation library
Group:		System Environment/Libraries
License:	BSD
URL:		https://github.com/rmind/libqsbr
Source0:	libqsbr.tar.gz

BuildRequires:	make
BuildRequires:	libtool

%description
Epoch-Based Reclamation (EBR) and Quiescent-State-Based Reclamation (QSBR)
are synchronisation mechanisms which can be used for efficient memory/object
reclamation (garbage collection) in concurrent environment.  Conceptually
they are very similar to the read-copy-update (RCU) mechanism.

%prep
%setup -q -n src

%build
make %{?_smp_mflags} LIBDIR=%{_libdir}

%install
make install \
    DESTDIR=%{buildroot} \
    LIBDIR=%{_libdir} \
    INCDIR=%{_includedir} \
    MANDIR=%{_mandir}

%files
%{_libdir}/*
%{_includedir}/*
#%{_mandir}/*

%changelog
