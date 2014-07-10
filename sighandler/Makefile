SOURCES+=game.cpp
SOURCES+=sighandler.cpp
SOURCES+=watchdog.cpp
HEADERS+=sighandler.h
HEADERS+=watchdog.h
OUTPUT=sighandler

OBJECTS=$(SOURCES:.cpp=.cpp.o)

LDFLAGS+=-lpthread
CXXFLAGS+=-rdynamic -Wfatal-errors

all: $(OUTPUT)

$(OUTPUT): $(OBJECTS)
	g++ $^ -o $@ $(CXXFLAGS) $(LDFLAGS)

%.cpp.o: %.cpp $(HEADERS)
	g++ $< -o $@ -c $(CXXFLAGS)

clean:
	rm -f $(OBJECTS) $(OUTPUT)
