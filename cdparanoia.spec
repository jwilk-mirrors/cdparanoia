%define ver	9.7
%define realver	alpha%{ver}

Summary: A Compact Disc Digital Audio (CDDA) extraction tool (or ripper).
Name: cdparanoia-III
Version: %{realver}
Release: 1
Copyright: GPL
Group: Applications/Multimedia
Source: http://www.xiph.org/paranoia/download/%{name}-%{realver}.src.tgz 
Url: http://www.xiph.org/paranoia/index.html
BuildRoot: /var/tmp/cdparanoia-root

%description
Cdparanoia (Paranoia III) reads digital audio directly from a CD, then
writes the data to a file or pipe in WAV, AIFC or raw 16 bit linear
PCM format.  Cdparanoia doesn't contain any extra features (like the ones
included in the cdda2wav sampling utility).  Instead, cdparanoia's strength
lies in its ability to handle a variety of hardware, including inexpensive
drives prone to misalignment, frame jitter and loss of streaming during
atomic reads.  Cdparanoia is also good at reading and repairing data from
damaged CDs.

%prep
%setup -q

%build
rm -rf $RPM_BUILD_ROOT
CFLAGS="${RPM_OPT_FLAGS}" ./configure --prefix=/usr
make  

%install
rm -rf $RPM_BUILD_ROOT

install -d $RPM_BUILD_ROOT/usr/{include,lib,bin,man/man1}
install -m 0755 -s cdparanoia $RPM_BUILD_ROOT/usr/bin/
install -m 0644 cdparanoia.1 $RPM_BUILD_ROOT/usr/man/man1/ 
install -m 0644 paranoia/cdda_paranoia.h $RPM_BUILD_ROOT/usr/include
install -m 0755 paranoia/libcdda_paranoia.so.0.%{ver} \
	$RPM_BUILD_ROOT/usr/lib
install -m 0755 paranoia/libcdda_paranoia.a $RPM_BUILD_ROOT/usr/lib
install -m 0644 interface/cdda_interface.h $RPM_BUILD_ROOT/usr/include
install -m 0755 interface/libcdda_interface.so.0.%{ver} \
	$RPM_BUILD_ROOT/usr/lib
install -m 0755 interface/libcdda_interface.a $RPM_BUILD_ROOT/usr/lib

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)

%doc README GPL
/usr/bin/cdparanoia
/usr/man/man1/cdparanoia.1
/usr/include/cdda_paranoia.h
/usr/lib/libcdda_paranoia.*
/usr/include/cdda_interface.h
/usr/lib/libcdda_interface.*

%changelog
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
