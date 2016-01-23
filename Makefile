LDFLAGS="-lfuse"
T=nul1fs nullfs nulnfs

all: $(T)
nullfs: nullfs.c++
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $<
clean:
	rm -f $(T) *.o
