main: src/main.cpp
	g++ src/main.cpp -std=c++20 -O2 -pedantic -Wall -o server -lpqxx -lpq

prod: src/main.cpp
	g++ -Isrc/libs/libpqxx/include src/main.cpp -std=c++20 -O2 -pedantic -Wall -o server -lpqxx -lpq
