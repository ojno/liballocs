DEPS := 

ALLOCSTOOL ?= ../../../

CXXFLAGS += -std=c++0x -g -O0 -I$(ALLOCSTOOL)
CXXFLAGS += -Wno-deprecated-declarations # while we're using old libdwarfpp -- FIXME
LDFLAGS += -L`pwd` -Wl,-R`pwd` -L$(ALLOCSTOOL) -Wl,-R$(realpath $(ALLOCSTOOL))
LDLIBS += -lallocstool -ldwarfidl -lcxxgen -ldwarfpp -ldwarf -lboost_regex -lboost_filesystem -lboost_system -lboost_serialization -lcxxfileno -lsrk31cxx -lelf

CPP_SRC := $(wildcard *.cpp)

default: base-types-translation #lower-typenames 

.PHONY: clean
clean:
	rm -f *.o .*.d lower-typenames base-types-translation

CPP_DEPS := $(patsubst %.cpp,.%.d,$(CPP_SRC))
DEPS := $(CPP_DEPS)

$(CPP_DEPS): .%.d : %.cpp
	$(CXX) -MM $(CXXFLAGS) "$<"  > "$@"

ifneq ($(MAKECMDGOALS),clean)
-include $(DEPS)
endif

# GAH... for some reason GNU Make needs this rule to avoid trying 
# to link the .o using $(CC) instead of $(CXX). 
%: %.cpp
	$(CXX) $(LDFLAGS) $(CXXFLAGS) -o "$@" "$<" $(LDLIBS)

lower-typenames: lower-typenames.cpp $(ALLOCSTOOL)/liballocstool.a
base-types-translation: base-types-translation.cpp $(ALLOCSTOOL)/liballocstool.a
