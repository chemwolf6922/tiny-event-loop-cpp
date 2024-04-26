LIBTEV_VERSION_MAJOR = 1
LIBTEV_VERSION_MINOR = 0
LIBTEV_VERSION_REVISION = 0
LIBTEV_VERSION=$(LIBTEV_VERSION_MAJOR).$(LIBTEV_VERSION_MINOR).$(LIBTEV_VERSION_REVISION)

CXX = g++
CXXFLAGS ?= -O3
override CXXFLAGS += -std=c++20 -fPIC
override CXXFLAGS += -MMD -MP
LDFLAGS ?=

LIBSRCS=tev.cpp
STATIC_LIB = libtev-cpp.a
SHARED_LIB = libtev-cpp.so

TESTSRCS=test.cpp
TEST=test

.PHONY:all
all: $(STATIC_LIB) $(SHARED_LIB) $(TEST)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIBSRCS:.cpp=.o)
	ar rcs $@ $<

$(SHARED_LIB): $(LIBSRCS:.cpp=.o)
	$(CXX) $(LDFLAGS) -shared -Wl,-soname,$@.$(LIBTEV_VERSION_MAJOR) -o $@.$(LIBTEV_VERSION) $<
	ln -sf $@.$(LIBTEV_VERSION) $@.$(LIBTEV_VERSION_MAJOR)
	ln -sf $@.$(LIBTEV_VERSION) $@

$(TEST): $(LIBSRCS:.cpp=.o) $(TESTSRCS:.cpp=.o)
	$(CXX) $(LDFLAGS) $^ -o $@

-include $(LIBSRCS:.cpp=.d) $(TESTSRCS:.cpp=.d)

.PHONY:clean
clean:
	rm -f *.d *.o $(STATIC_LIB) $(SHARED_LIB)* $(TEST)