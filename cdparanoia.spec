%define ver	9.7
%define realver	alpha%{ver}

Summary: A Compact Disc Digital Audio (CDDA) extraction tool (or ripper).
Name: cdparanoia
Version: %{realver}
Release: 7
License: GPL
Group: Applications/Multimedia
Source: http://www.xiph.org/paranoia/download/%{name}-III-%{realver}.src.tgz 
Url: http://www.xiph.org/paranoia/index.html
BuildRoot: %{_tmppath}/cdparanoia-%{version}-root

%description
Cdparanoia (Paranoia III) reads digital audio directly from a CD, then
writes the data to a file or pipe in WAV, AIFC or raw 16 bit linear
PCM format.  Cdparanoia doesn't contain any extra features (like the ones
included in the cdda2wav sampling utility).  Instead, cdparanoia's strength
lies in its ability to handle a variety of hardware, including inexpensive
drives prone to misalignment, frame jitter and loss of streaming during
atomic reads.  Cdparanoia is also good at reading and repairing data from
damaged CDs.

%package devel
Summary: Development tools for libcdda_paranoia (Paranoia III).
Group: Development/Libraries

%description devel
The cdparanoia-devel package contains the static libraries and header
files needed for developing applications to read CD Digital Audio disks.

%prep
%setup -q -n %{name}-III-%{realver}

%build
rm -rf $RPM_BUILD_ROOT
%configure
make  

%install
rm -rf $RPM_BUILD_ROOT

install -d $RPM_BUILD_ROOT%{_bindir}
install -d $RPM_BUILD_ROOT%{_includedir}
install -d $RPM_BUILD_ROOT%{_libdir}
install -d $RPM_BUILD_ROOT%{_mandir}/man1
install -m 0755 -s cdparanoia $RPM_BUILD_ROOT%{_bindir}
install -m 0644 cdparanoia.1 $RPM_BUILD_ROOT%{_mandir}/man1/ 
install -m 0644 paranoia/cdda_paranoia.h $RPM_BUILD_ROOT%{_includedir}
install -m 0755 paranoia/libcdda_paranoia.so.0.%{ver} \
	$RPM_BUILD_ROOT%{_libdir}
install -m 0755 paranoia/libcdda_paranoia.a $RPM_BUILD_ROOT%{_libdir}
install -m 0644 interface/cdda_interface.h $RPM_BUILD_ROOT%{_includedir}
install -m 0755 interface/libcdda_interface.so.0.%{ver} \
	$RPM_BUILD_ROOT%{_libdir}
install -m 0755 interface/libcdda_interface.a $RPM_BUILD_ROOT%{_libdir}

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%doc README GPL FAQ.txt
%{_bindir}/*
%{_mandir}/man1/*
%{_libdir}/libcdda_paranoia.so.*
%{_libdir}/libcdda_interface.so.*

%files devel
%defattr(-,root,root)
%{_includedir}/*
%{_libdir}/*.a

%changelog
* Tue Feb 27 2001 Karsten Hopp <karsten@redhat.de>
- fix spelling error in description

* Thu Dec  7 2000 Crutcher Dunnavant <crutcher@redhat.com>
- rebuild for new tree

* Fri Jul 21 2000 Trond Eivind Glomsr�d <teg@redhat.com>
- use %%{_tmppath}

* Wed Jul 12 2000 Prospector <bugzilla@redhat.com>
- automatic rebuild

* Wed Jun 06 2000 Preston Brown <pbrown@redhat.com>
- revert name change
- use new rpm macro paths

* Wed Apr 19 2000 Trond Eivind Glomsr�d <teg@redhat.com>
- Switched spec file from the one used in Red Hat Linux 6.2, which
  also changes the name
- gzip man page

* Thu Dec 23 1999 Peter Jones <pjones@redhat.com>
- update package to provide cdparanoia-alpha9.7-2.*.rpm and 
  cdparanoia-devel-alpha9.7-2.*.rpm.  Also, URLs point at xiph.org
  like they should.

* Wed Dec 22 1999 Peter Jones <pjones@redhat.com>
- updated package for alpha9.7, based on input from:
  Monty <xiphmont@xiph.org> 
  David Philippi <david@torangan.saar.de>

* Mon Apr 12 1999 Michael Maher <mike@redhat.com>
- updated pacakge

* Tue Oct 06 1998 Michael Maher <mike@redhat.com>
- updated package

* Mon Jun 29 1998 Michael Maher <mike@redhat.com>
- built package
