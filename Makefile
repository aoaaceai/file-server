all: server.cpp client.cpp
	g++ server.cpp -o server
	g++ client.cpp -o client
clean:
	rm -rf server client
	# rm -rf server_dir client_dir