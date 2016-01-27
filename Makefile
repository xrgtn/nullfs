LDLIBS="-lfuse"
T=nul1fs nullfs nulnfs

all: $(T)
nullfs: nullfs.c++
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDLIBS) -o $@
clean:
	rm -f $(T) *.o
