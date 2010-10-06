# use GNU Make to run tests in parallel, and without depending on RubyGems
all::
RUBY = ruby
RAKE = rake
RSYNC = rsync
GIT_URL = git://git.bogomips.org/kgio.git

GIT-VERSION-FILE: .FORCE-GIT-VERSION-FILE
	@./GIT-VERSION-GEN
-include GIT-VERSION-FILE
-include local.mk
ifeq ($(DLEXT),) # "so" for Linux
  DLEXT := $(shell $(RUBY) -rrbconfig -e 'puts Config::CONFIG["DLEXT"]')
endif
ifeq ($(RUBY_VERSION),)
  RUBY_VERSION := $(shell $(RUBY) -e 'puts RUBY_VERSION')
endif

install:
	$(prep_setup_rb)
	$(RM) -r .install-tmp
	mkdir .install-tmp
	$(RUBY) setup.rb all
	$(RM) $^
	$(RM) -r .install-tmp
	$(prep_setup_rb)

setup_rb_files := .config InstalledFiles
prep_setup_rb := @-$(RM) $(setup_rb_files);$(MAKE) -C $(ext) clean

clean:
	-$(MAKE) -C ext/kgio clean
	$(RM) $(setup_rb_files) ext/kgio/Makefile

pkg_extra := GIT-VERSION-FILE NEWS ChangeLog
manifest: $(pkg_extra)
	$(RM) .manifest
	$(MAKE) .manifest

.manifest:
	(git ls-files && \
         for i in $@ $(pkg_extra) $(man1_paths); \
	 do echo $$i; done) | LC_ALL=C sort > $@+
	cmp $@+ $@ || mv $@+ $@
	$(RM) $@+

NEWS: GIT-VERSION-FILE
	$(RAKE) -s news_rdoc > $@+
	mv $@+ $@

latest: NEWS
	@awk 'BEGIN{RS="=== ";ORS=""}NR==2{sub(/\n$$/,"");print RS""$$0 }' $<

SINCE =
ChangeLog: LOG_VERSION = \
  $(shell git rev-parse -q "$(GIT_VERSION)" >/dev/null 2>&1 && \
          echo $(GIT_VERSION) || git describe)
ifneq ($(SINCE),)
ChangeLog: log_range = v$(SINCE)..$(LOG_VERSION)
endif
ChangeLog: GIT-VERSION-FILE
	@echo "ChangeLog from $(GIT_URL) ($(log_range))" > $@+
	@echo >> $@+
	git log $(log_range) | sed -e 's/^/    /' >> $@+
	mv $@+ $@

news_atom := http://unicorn.bogomips.org/kgio/NEWS.atom.xml
cgit_atom := http://git.bogomips.org/cgit/kgio.git/atom/?h=master
atom = <link rel="alternate" title="Atom feed" href="$(1)" \
             type="application/atom+xml"/>

# using rdoc 2.5.x
doc: .document NEWS ChangeLog
	rdoc -t "$(shell sed -ne '1s/^= //p' README)"
	install -m644 COPYING doc/COPYING
	install -m644 $(shell grep '^[A-Z]' .document) doc/
	$(RUBY) -i -p -e \
	  '$$_.gsub!("</title>",%q{\&$(call atom,$(cgit_atom))})' \
	  doc/ChangeLog.html
	$(RUBY) -i -p -e \
	  '$$_.gsub!("</title>",%q{\&$(call atom,$(news_atom))})' \
	  doc/NEWS.html doc/README.html
	$(RAKE) -s news_atom > doc/NEWS.atom.xml
	cd doc && ln README.html tmp && mv tmp index.html

ifneq ($(VERSION),)
rfproject := rainbows
rfpackage := kgio
pkggem := pkg/$(rfpackage)-$(VERSION).gem
pkgtgz := pkg/$(rfpackage)-$(VERSION).tgz
release_notes := release_notes-$(VERSION)
release_changes := release_changes-$(VERSION)

release-notes: $(release_notes)
release-changes: $(release_changes)
$(release_changes):
	$(RAKE) -s release_changes > $@+
	$(VISUAL) $@+ && test -s $@+ && mv $@+ $@
$(release_notes):
	GIT_URL=$(GIT_URL) $(RAKE) -s release_notes > $@+
	$(VISUAL) $@+ && test -s $@+ && mv $@+ $@

# ensures we're actually on the tagged $(VERSION), only used for release
verify:
	test x"$(shell umask)" = x0022
	git rev-parse --verify refs/tags/v$(VERSION)^{}
	git diff-index --quiet HEAD^0
	test `git rev-parse --verify HEAD^0` = \
	     `git rev-parse --verify refs/tags/v$(VERSION)^{}`

fix-perms:
	-git ls-tree -r HEAD | awk '/^100644 / {print $$NF}' | xargs chmod 644
	-git ls-tree -r HEAD | awk '/^100755 / {print $$NF}' | xargs chmod 755

gem: $(pkggem)

install-gem: $(pkggem)
	gem install $(CURDIR)/$<

$(pkggem): manifest fix-perms
	gem build $(rfpackage).gemspec
	mkdir -p pkg
	mv $(@F) $@

$(pkgtgz): distdir = $(basename $@)
$(pkgtgz): HEAD = v$(VERSION)
$(pkgtgz): manifest fix-perms
	@test -n "$(distdir)"
	$(RM) -r $(distdir)
	mkdir -p $(distdir)
	tar cf - `cat .manifest` | (cd $(distdir) && tar xf -)
	cd pkg && tar cf - $(basename $(@F)) | gzip -9 > $(@F)+
	mv $@+ $@

package: $(pkgtgz) $(pkggem)

test-release: verify package $(release_notes) $(release_changes)
release: verify package $(release_notes) $(release_changes)
	# make tgz release on RubyForge
	rubyforge add_release -f -n $(release_notes) -a $(release_changes) \
	  $(rfproject) $(rfpackage) $(VERSION) $(pkgtgz)
	# push gem to Gemcutter
	gem push $(pkggem)
	# in case of gem downloads from RubyForge releases page
	-rubyforge add_file \
	  $(rfproject) $(rfpackage) $(VERSION) $(pkggem)
	$(RAKE) raa_update VERSION=$(VERSION)
	$(RAKE) publish_news VERSION=$(VERSION)
else
gem install-gem: GIT-VERSION-FILE
	$(MAKE) $@ VERSION=$(GIT_VERSION)
endif

ext := ext/kgio/kgio_ext.$(DLEXT)
ext/kgio/Makefile: ext/kgio/extconf.rb
	cd $(@D) && $(RUBY) extconf.rb

$(ext): $(wildcard ext/kgio/*.[ch] ext/kgio/*/*.h) ext/kgio/Makefile
	$(MAKE) -C $(@D)

all:: test

build: $(ext)
test_units := $(wildcard test/test_*.rb)
test: test-unit
test-unit: $(test_units)
$(test_units): build
	$(RUBY) -I lib:ext/kgio $@

# this requires GNU coreutils variants
publish_doc:
	-git set-file-times
	$(RM) -r doc ChangeLog NEWS
	$(MAKE) doc LOG_VERSION=$(shell git tag -l | tail -1)
	$(MAKE) -s latest > doc/LATEST
	find doc/images doc/js -type f | \
		TZ=UTC xargs touch -d '1970-01-01 00:00:00' doc/rdoc.css
	$(MAKE) doc_gz
	chmod 644 $$(find doc -type f)
	$(RSYNC) -av doc/ unicorn.bogomips.org:/srv/unicorn/kgio/
	git ls-files | xargs touch

# Create gzip variants of the same timestamp as the original so nginx
# "gzip_static on" can serve the gzipped versions directly.
doc_gz: docs = $(shell find doc -type f ! -regex '^.*\.\(gif\|jpg\|png\|gz\)$$')
doc_gz:
	touch doc/NEWS.atom.xml -d "$$(awk 'NR==1{print $$4,$$5,$$6}' NEWS)"
	for i in $(docs); do \
	  gzip --rsyncable -9 < $$i > $$i.gz; touch -r $$i $$i.gz; done

.PHONY: .FORCE-GIT-VERSION-FILE doc manifest man test $(test_units)
