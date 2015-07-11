Name:		libqsbr
Version:	1.0
Release:	1%{?dist}
Summary:	QSBR-based reclamation library
Group:		System Environment/Libraries
License:	BSD
URL:		https://github.com/rmind/libqsbr
Source0:	libqsbr.tar.gz

BuildRequires:	make
BuildRequires:	libtool

%description
QSBR is a synchronisation mechanism which can be used for efficient
memory reclamation (garbage collection) in multi-threaded environment.
A typical use case of the QSBR mechanism would be together with lock-free
data structures.  This library provides a raw QSBR interface and a garbage
collection (GC) interface based on QSBR.

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
