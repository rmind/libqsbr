all: rpm

rpm:
	mkdir -p SOURCES && tar czpvf SOURCES/libqsbr.tar.gz ../src
	rpmbuild -ba -v --define "_topdir ${PWD}" SPECS/libqsbr.spec

clean:
	rm -rf BUILD BUILDROOT RPMS SOURCES SRPMS

.PHONY: all rpm clean
