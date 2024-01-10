.SILENT: default

INCLUDE = -I/usr/include
LDFLAGS = -lPocoUtil -lPocoJSON -lPocoNet -lPocoXML -lPocoFoundation 

default: EchoApp

EchoApp : Main.o
	g++ -o EchoApp Main.o $(LDFLAGS)

Main.o : Main.cpp
	g++ $(INCLUDE) -c Main.cpp -o Main.o

