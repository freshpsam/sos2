# Native SOS2: GNU cross compiler, static archives, native pthreads.
GEN_DEPS.c = $(CC) -E -MM $(DEFS) $(CPPFLAGS) $(CFLAGS)
GEN_DEPS.cc = $(CXX) -E -MM $(DEFS) $(CPPFLAGS) $(CXXFLAGS)
THREADSCPPFLAGS = -D_REENTRANT
STATIC_O = ao
SO = so
STATIC_PREFIX =
STATIC_PREFIX_WHEN_USED =

%.$(STATIC_O): $(srcdir)/%.c
	$(call SILENT_COMPILE,$(strip $(COMPILE.c) $(STATICCPPFLAGS) $(STATICCFLAGS)) -o $@ $<)
%.$(STATIC_O): $(srcdir)/%.cpp
	$(call SILENT_COMPILE,$(strip $(COMPILE.cc) $(STATICCPPFLAGS) $(STATICCXXFLAGS)) -o $@ $<)
%.d: $(srcdir)/%.c
	@$(SHELL) -ec '$(GEN_DEPS.c) $< | sed '\''s%\($*\)\.o[ :]*%\1.o $@ : %g'\'' > $@; [ -s $@ ] || rm -f $@'
%.d: $(srcdir)/%.cpp
	@$(SHELL) -ec '$(GEN_DEPS.cc) $< | sed '\''s%\($*\)\.o[ :]*%\1.o $@ : %g'\'' > $@; [ -s $@ ] || rm -f $@'
